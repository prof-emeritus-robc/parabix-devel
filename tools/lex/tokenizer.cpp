/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/core/idisa_target.h>
#include <boost/filesystem.hpp>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/adt.h>
#include <re/parse/parser.h>
#include <re/unicode/resolve_properties.h>
#include <re/cc/cc_kernel.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/core/streamset.h>
#include <kernel/unicode/utf8_support.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/pablo_toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <grep/grep_kernel.h>
#include <toolchain/toolchain.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <map>
#include <grep/regex_passes.h>
#include <re/compile/re_compiler.h>
#include <kernel/unicode/UCD_property_kernel.h>

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;
using namespace kernel;
using namespace pablo;
using namespace re;

static cl::OptionCategory wordBreakerFlags("Command Flags", "Unicode word breaker options");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(wordBreakerFlags));


// word boundary detection kernel using Unicode word characters
class UnicodeWordBoundaryDetector : public PabloKernel {
public:
    // constructor argument: LLVMTypeSystemInterface --> to handle types for LLVM IR generation
    UnicodeWordBoundaryDetector(LLVMTypeSystemInterface & ts,
                                StreamSet * WordStream,
                                StreamSet * WordBoundaryMask)
    : PabloKernel(ts, "unicodeWordBoundaryDetector",
                  {Binding{"WordStream", WordStream}},
                  {Binding{"wordBoundaries", WordBoundaryMask}}) {}

    // the algorithm
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());

        RE_Compiler re_compiler(getEntryScope(), nullptr);

        PabloAST * wordChars = getInputStreamSet("WordStream")[0];


        // "Previous" bit relative to current position:
        // advance(s, 1) shifts forward so the previous char aligns with current bit.
        PabloAST * prevWordChars = pb.createAdvance(wordChars, 1);


        //word → non-word, mark an end boundary
        //non-word → word, mark a start boundary.
        // 1) End of word: previous was word, current is not
        PabloAST * wordToNonWord = pb.createAnd(prevWordChars, pb.createNot(wordChars));


        // 2) Start of word: previous was not word, current is
        //PabloAST * nonWordToWord = pb.createAnd(pb.createNot(prevWordChars), wordChars);

        PabloAST * allBoundaries = wordToNonWord;

        writeOutputStreamSet("wordBoundaries", std::vector<PabloAST*>{ allBoundaries });

    }
};

/** Unicode line separator insertion kernel: writes LF (0x0A) at mask positions */
class AddUnicodeLineSeparators : public PabloKernel {
public:
    AddUnicodeLineSeparators(LLVMTypeSystemInterface & ts,
                             StreamSet * insertMask,
                             StreamSet * spreadBasis,
                             StreamSet * finalBasis)
    : PabloKernel(ts, "addUnicodeLineSeparators",
                  {Binding{"insertMask", insertMask}, Binding{"spreadBasis", spreadBasis}},
                  {Binding{"finalBasis", finalBasis}}) {}


protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        // The insertMask has 0 bits at inserted positions, 1s everywhere else.
        PabloAST * insert = getInputStreamSet("insertMask")[0];
        std::vector<PabloAST *> basis = getInputStreamSet("spreadBasis");

        // Starting with the spread basis bits
        std::vector<PabloAST *> out(basis.size());
        for (unsigned i = 0; i < basis.size(); ++i) out[i] = basis[i];

        // This bit clearing clears the bits at all positions, not just
        // at the places to insert LFs.   But the spreadBasis will have
        // all bits cleared at the insert positions,
        // Clearing all bits where we'll insert LF: AND with NOT(insert)
        //PabloAST * keep = pb.createNot(insert);
        //for (unsigned i = 0; i < out.size(); ++i) {
        //    out[i] = pb.createAnd(out[i], keep);
        //}

        // We need to 1 bits at the positions for insertion of LFs.
        PabloAST * insertMark = pb.createNot(insert);
        // LF = 0x0A = b00001010 -> set bit1 and bit3 where insertMark=1
        if (out.size() >= 4) {
            out[1] = pb.createOr(out[1], insertMark);
            out[3] = pb.createOr(out[3], insertMark);
        }

        writeOutputStreamSet("finalBasis", out);
    }
};

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name)  if (codegen::EnableIllustrator) P.captureByteData(#name, name)

using WordBreakerFunctionType = void (*)(uint32_t fd);

WordBreakerFunctionType wordBreakerPipeline(CPUDriver & driver) {

    auto P = CreatePipeline(driver, Input<uint32_t>{"fileDescriptor"});

    Scalar * const fileDescriptor = P.getInputScalar("fileDescriptor");

    // Byte stream and 8 basis bit streams
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);

    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);

    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    // === Word boundary detection ===
    StreamSet * wordBoundaryMask = P.CreateStreamSet(1, 1);

    re::RE * wordProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "word");
    wordProp = UCD::linkAndResolve(wordProp);
    re::Name * word = re::makeName("word");
    word->setDefinition(wordProp);
    StreamSet * WordStream = P.CreateStreamSet(1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(word, BasisBits, WordStream);
    SHOW_STREAM(WordStream);

    StreamSet * WordSpans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(WordStream, u8index, WordSpans);
    SHOW_STREAM(WordSpans);


    // P.CreateKernelCall<BoundaryKernel>(WordStream, U8index, wordBoundary_stream);
    P.CreateKernelCall<UnicodeWordBoundaryDetector>(WordSpans, wordBoundaryMask);
    SHOW_STREAM(wordBoundaryMask);

    // === Insertion & spreading ===
    // Insert BEFORE boundaries (line break in front of word starts/ends)
    StreamSet * lineInsertMask = UnitInsertionSpreadMask(P, wordBoundaryMask, kernel::InsertPosition::Before);
    SHOW_STREAM(lineInsertMask);

    StreamSet * spreadBasis = P.CreateStreamSet(8);
    SpreadByMask(P, lineInsertMask, BasisBits, spreadBasis);
    SHOW_BIXNUM(spreadBasis);

    // === Insert LF at boundary positions ===
    StreamSet * tokenBasis = P.CreateStreamSet(8);
    P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    SHOW_BIXNUM(tokenBasis);

    // Back to bytes and out
    StreamSet * tokenizedWords = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(tokenBasis, tokenizedWords);
    SHOW_BYTES(tokenizedWords);

    P.CreateKernelCall<StdOutKernel>(tokenizedWords);

    return reinterpret_cast<WordBreakerFunctionType>(P.compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&wordBreakerFlags, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});
    CPUDriver driver("unicode_word_tokenizer");

    WordBreakerFunctionType wordBreakerFn = wordBreakerPipeline(driver);

    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
        return 1;
    } else {
        wordBreakerFn(fd);
        close(fd);
    }
    return 0;
}
