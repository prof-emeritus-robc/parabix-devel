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


#include <kernel/unicode/UCD_property_kernel.h>
#include <re/unicode/boundaries.h>
#include <re/analysis/collect_ccs.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/re_multiplex.h>
#include <kernel/unicode/charclasses.h>

// ICU boundary provider
#include "ICU_Boundaries.h"

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;
using namespace kernel;
using namespace pablo;
using namespace re;

static cl::OptionCategory wordBreakerFlags("Command Flags", "Unicode word breaker options");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(wordBreakerFlags));

// ICU locale for word boundaries, make this the only option
// the tokenizer will use this locale to build word boundaries, tokenizer callls buildWordBoundaryMaskFromICU 
// when we selecet a locale other than the default
static cl::opt<std::string> Locale("locale",
    cl::desc("ICU locale for word boundaries (e.g., en_US, fr_FR, ja_JP). Empty = default UAX#29"),
    cl::init(""),
    cl::cat(wordBreakerFlags));

// Pre-tokenizer selection: uax29 (default Unicode word boundaries), icu (ICU BreakIterator),
// whitespace (split on whitespace), bytelevel, punctuation, metaspace, etc.
static cl::opt<std::string> PreTokenizer("pretokenizer",
    cl::desc("Pre-tokenizer to use: icu|gpt2|whitespace|whitespacesplit|digits|punctuation"),
    cl::init(""),
    cl::cat(wordBreakerFlags));

// proper Unicode word boundary rules (WB1, WB2, WB3) implemented in 
// generateWordBoundaryRule() function.

// Unicode line separator insertion kernel: writes LF (0x0A) at token boundaries
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
        
        // insertMask has 0 bits where we want to insert LF, 1s elsewhere
        PabloAST * insert = getInputStreamSet("insertMask")[0];
        std::vector<PabloAST *> basis = getInputStreamSet("spreadBasis");
        
        // UTF-8 boundary
        //PabloAST * u8First = pb.createAdvance(u8index, 1);

        // Copy the spread basis bits
        std::vector<PabloAST *> out(basis.size());
        for (unsigned i = 0; i < basis.size(); ++i) {
            out[i] = basis[i];
        }

        // Create insertion mark (1s where we want to insert LF)
        PabloAST * insertMark = pb.createNot(insert);
        
        // Insert LF character (0x0A = 00001010) at marked positions
        // LF has bit 1 and bit 3 set
        if (out.size() >= 4) {
            out[1] = pb.createOr(out[1], insertMark); // Set bit 1
            out[3] = pb.createOr(out[3], insertMark); // Set bit 3
        }

        writeOutputStreamSet("finalBasis", out);
    }
};


// AND kernel to ensure boundaries only occur at UTF-8 character starts
class AndKernel : public PabloKernel {
public:
    AndKernel(LLVMTypeSystemInterface & ts,
              StreamSet * input1,  // WordBoundaries
              StreamSet * input2, // u8index
              StreamSet * output)
    : PabloKernel(ts, "andKernel",
                  {Binding{"input1", input1}, Binding{"input2", input2}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream1 = getInputStreamSet("input1")[0];
        PabloAST * u8index = getInputStreamSet("input2")[0];
        PabloAST * u8First = pb.createNot(pb.createAdvance(pb.createNot(u8index), 1));
        PabloAST * result = pb.createAnd(stream1, u8First);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// Debug macros for visualization
#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name)  if (codegen::EnableIllustrator) P.captureByteData(#name, name)

using WordBreakerFunctionType = void (*)(uint32_t fd);

WordBreakerFunctionType wordBreakerPipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>{"fileDescriptor"});
    
    Scalar * const fileDescriptor = P.getInputScalar("fileDescriptor");
    
    // Input Processing
    // Read file into byte stream
    StreamSet * const ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    
    // Convert serial bytes to 8 parallel bit streams
    StreamSet * const BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);
    SHOW_BIXNUM(BasisBits);
    
    // Create UTF-8 character boundary index
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);
    
    // Unicode Word Boundary Rules
    StreamSet * WordBoundaries = nullptr;
    
    (void)PreTokenizer; // PreTokenizer variable used later; silence unused-warning if any
    (void)Locale;

    if (PreTokenizer == "icu" || !Locale.empty()) {
        // Use ICU locale-aware word boundaries
        WordBoundaries = buildWordBoundaryMaskFromICU(P, BasisBits, u8index, Locale);

    }else if (PreTokenizer == "gpt2") {
        // Use GPT-2 r50k regex pretokenizer (centralized in boundaries.cpp)
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * parsedRE = re::generateGPT2R50KRule();
        if (!parsedRE) {
            llvm::errs() << "Warning: failed to obtain GPT-2 pretokenizer regex. Falling back to UAX#29.\n";
            parsedRE = re::generateWordBoundaryRule();
        }
        const auto Sets = re::collectCCs(parsedRE, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto mpx = cc::makeMultiplexedAlphabet("GPT2_mpx", Sets);
        parsedRE = transformCCs(mpx, parsedRE, re::NameTransformationMode::TransformDefinition);
        auto basis = mpx->getMultiplexedCCs();
        StreamSet * const Classes = P.CreateStreamSet(basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(basis, BasisBits, Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(parsedRE);
        options->addAlphabet(mpx, Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));

    }
    else if (PreTokenizer == "whitespace"){
        // Use whitespace pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * wsRule = re::generateWhitespaceBoundaryRule();
        const auto WS_Sets = re::collectCCs(wsRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto WS_mpx = cc::makeMultiplexedAlphabet("WS_mpx", WS_Sets);
        wsRule = transformCCs(WS_mpx, wsRule, re::NameTransformationMode::TransformDefinition);
        auto WS_basis = WS_mpx->getMultiplexedCCs();
        StreamSet * const WS_Classes = P.CreateStreamSet(WS_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(WS_basis, BasisBits, WS_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(wsRule);
        options->addAlphabet(WS_mpx, WS_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    }
    else if(PreTokenizer == "whitespacesplit"){
        // Use WhitespaceSplit pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * wssRule = re::generateWhitespaceSplitBoundaryRule();
        const auto WSS_Sets = re::collectCCs(wssRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto WSS_mpx = cc::makeMultiplexedAlphabet("WSS_mpx", WSS_Sets);
        wssRule = transformCCs(WSS_mpx, wssRule, re::NameTransformationMode::TransformDefinition);
        auto WSS_basis = WSS_mpx->getMultiplexedCCs();
        StreamSet * const WSS_Classes = P.CreateStreamSet(WSS_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(WSS_basis, BasisBits, WSS_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(wssRule);
        options->addAlphabet(WSS_mpx, WSS_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    }
    else if (PreTokenizer == "punctuation"){
    // Use punctuation pre-tokenizer
    WordBoundaries = P.CreateStreamSet(1, 1);
    re::RE * punctRule = re::generatePunctuationBoundaryRule();
    const auto PC_Sets = re::collectCCs(punctRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
    auto PC_mpx = cc::makeMultiplexedAlphabet("PC_mpx", PC_Sets);
    punctRule = transformCCs(PC_mpx, punctRule, re::NameTransformationMode::TransformDefinition);
    auto PC_basis = PC_mpx->getMultiplexedCCs();
    StreamSet * const PC_Classes = P.CreateStreamSet(PC_basis.size());
    P.CreateKernelFamilyCall<CharClassesKernel>(PC_basis, BasisBits, PC_Classes);
    auto options = std::make_unique<GrepKernelOptions>();
    options->setIndexing(u8index);
    options->setRE(punctRule);
    options->addAlphabet(PC_mpx, PC_Classes);
    options->setResults(WordBoundaries);
    options->addExternal("UTF8_index", u8index);
    P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));

   }
   else if (PreTokenizer == "digits"){
    // Use digits pre-tokenizer
    WordBoundaries = P.CreateStreamSet(1, 1);
    re::RE * digitsRule = re::generateDigitBoundaryRule();
    const auto DG_Sets = re::collectCCs(digitsRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
    auto DG_mpx = cc::makeMultiplexedAlphabet("DG_mpx", DG_Sets);
    digitsRule = transformCCs(DG_mpx, digitsRule, re::NameTransformationMode::TransformDefinition);
    auto DG_basis = DG_mpx->getMultiplexedCCs();
    StreamSet * const DG_Classes = P.CreateStreamSet(DG_basis.size());
    P.CreateKernelFamilyCall<CharClassesKernel>(DG_basis, BasisBits, DG_Classes);
    auto options = std::make_unique<GrepKernelOptions>();
    options->setIndexing(u8index);
    options->setRE(digitsRule);
    options->addAlphabet(DG_mpx, DG_Classes);
    options->setResults(WordBoundaries);
    options->addExternal("UTF8_index", u8index);
    P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    }
    
    /*else if(PreTokenizer == "byte-level"){
        // Use byte-level pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * byteRule = re::generateByteLevelRule();
        const auto BL_Sets = re::collectCCs(byteRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition
    }else if (PreTokenizer == "punctuation"){
        // Use punctuation pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * puncRule = re::generatePunctuationRule();
        const auto PC_Sets = re::collectCCs(puncRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto PC_mpx = cc::makeMultiplexedAlphabet("PC_mpx", PC_Sets);
        puncRule = transformCCs(PC_mpx, puncRule, re::NameTransformationMode::TransformDefinition);
        auto PC_basis = PC_mpx->getMultiplexedCCs();
        StreamSet * const PC_Classes = P.CreateStreamSet(PC_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(PC_basis, BasisBits, PC_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(puncRule);
        options->addAlphabet(PC_mpx, PC_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    }else if (PreTokenizer == "metaspace"){
        // Use metaspace pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * msRule = re::generateMetaspaceRule();
        const auto MS_Sets = re::collectCCs(msRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto MS_mpx = cc::makeMultiplexedAlphabet("MS_mpx", MS_Sets);
        msRule = transformCCs(MS_mpx, msRule, re::NameTransformationMode::TransformDefinition);
        auto MS_basis = MS_mpx->getMultiplexedCCs();
        StreamSet * const MS_Classes = P.CreateStreamSet(MS_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(MS_basis, BasisBits, MS_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(msRule);
        options->addAlphabet(MS_mpx, MS_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    }else if (PreTokenizer == "punctuation"){
        // use punctuation pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * puncRule = re::generatePunctuationRule();
        const auto PC_Sets = re::collectCCs(puncRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto PC_mpx = cc::makeMultiplexedAlphabet("PC_mpx", PC_Sets);
        puncRule = transformCCs(PC_mpx, puncRule, re::NameTransformationMode::TransformDefinition);
        auto PC_basis = PC_mpx->getMultiplexedCCs();
        StreamSet * const PC_Classes = P.CreateStreamSet(PC_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(PC_basis, BasisBits, PC_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(puncRule);
        options->addAlphabet(PC_mpx, PC_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));  
    }*/
    else {
        // Use default UAX#29 word boundaries
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * wordBoundaryRule = re::generateWordBoundaryRule();
        const auto WB_Sets = re::collectCCs(wordBoundaryRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto WB_mpx = cc::makeMultiplexedAlphabet("WB_mpx", WB_Sets);
        wordBoundaryRule = transformCCs(WB_mpx, wordBoundaryRule, re::NameTransformationMode::TransformDefinition);
        auto WB_basis = WB_mpx->getMultiplexedCCs();
        StreamSet * const WB_Classes = P.CreateStreamSet(WB_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(WB_basis, BasisBits, WB_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(wordBoundaryRule);
        options->addAlphabet(WB_mpx, WB_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    }
    SHOW_STREAM(WordBoundaries);
    
    // Using WordBoundaries as TokenBoundaries
    //StreamSet * TokenBoundaries = WordBoundaries;
    
    // UTF-8
    StreamSet * TokenBoundaries = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<AndKernel>(WordBoundaries, u8index, TokenBoundaries);
    SHOW_STREAM(TokenBoundaries);
    
    // Create insertion mask and spread original data
    StreamSet * lineInsertMask = UnitInsertionSpreadMask(P, TokenBoundaries, kernel::InsertPosition::Before);
    SHOW_STREAM(lineInsertMask);
    
    StreamSet * spreadBasis = P.CreateStreamSet(8);
    SpreadByMask(P, lineInsertMask, BasisBits, spreadBasis);
    SHOW_BIXNUM(spreadBasis);
    
    // Insert Token Separators
    StreamSet * tokenBasis = P.CreateStreamSet(8);
    P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    SHOW_BIXNUM(tokenBasis);
    
    // Output Processing
    // Convert parallel bit streams back to serial bytes
    StreamSet * tokenizedOutput = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(tokenBasis, tokenizedOutput);
    SHOW_BYTES(tokenizedOutput);
    
    // Write to standard output
    P.CreateKernelCall<StdOutKernel>(tokenizedOutput);
    
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
    
    }    return 0;
}
