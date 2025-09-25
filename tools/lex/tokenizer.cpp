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

// word boundary detection kernel
// Spaces attach to following tokens, creating clean word boundaries
class UnicodeWordBoundaryDetector : public PabloKernel {
public:
    // Fixed constructor: Both inputs need LookAhead(1) for the lookahead operations
    UnicodeWordBoundaryDetector(LLVMTypeSystemInterface & ts,
                                StreamSet * u8index,
                                StreamSet * WordSpans,
                                StreamSet * SpaceSpans,
                                StreamSet * SymbolSpans,
                                StreamSet * TokenBoundaries)
    : PabloKernel(ts, "tiktokenStyleTokenizer",
                  {Binding{"u8index", u8index, FixedRate(), LookAhead(1)},
                   Binding{"WordSpans", WordSpans, FixedRate(), LookAhead(1)},
                   Binding{"SpaceSpans", SpaceSpans, FixedRate(), LookAhead(1)},
                   Binding{"SymbolStream", SymbolSpans, FixedRate(), LookAhead(1)}}, // Added LookAhead(1)
                  {Binding{"tokenBoundaries", TokenBoundaries}})
    {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());

        // Get input streams
        PabloAST * u8index = getInputStreamSet("u8index")[0];
        PabloAST * wordSpans = getInputStreamSet("WordSpans")[0];
        PabloAST * spaceSpans = getInputStreamSet("SpaceSpans")[0];
        PabloAST * symbolSpans = getInputStreamSet("SymbolStream")[0];
        
        
        // Previous character positions (shift forward by 1)
        PabloAST * prevWordSpans = pb.createAdvance(wordSpans, 1);
        PabloAST * prevSpaceSpans = pb.createAdvance(spaceSpans, 1);
        PabloAST * prevsymbolSpans = pb.createAdvance(symbolSpans, 1);
        PabloAST * u8First = pb.createAdvance(u8index, 1);
        
        
        
        
        // Next character positions (lookahead by 1)
        PabloAST * nextWordSpans = pb.createLookahead(wordSpans, 1);
        PabloAST * nextsymbolSpans = pb.createLookahead(symbolSpans, 1);
        
        // Non-word, non-space characters, puntuations
        PabloAST * otherSpans = pb.createNot(pb.createOr3(wordSpans, spaceSpans, symbolSpans));
        PabloAST * prevOtherSpans = pb.createAdvance(otherSpans, 1);
        
        // Basic transitions
        PabloAST * wordToNonWord = pb.createAnd(prevWordSpans, pb.createNot(wordSpans));
        PabloAST * nonWordToWord = pb.createAnd(prevOtherSpans, wordSpans);  //??
        
        // Symbol transitions - each symbol should be a token
        PabloAST * symbolToAny = pb.createAnd(prevsymbolSpans, u8First);
        PabloAST * anyToSymbol = pb.createAnd(u8First, symbolSpans);
        
        
        
        // Space sequence handling
        
        // Start of space sequence: non-space to space
        PabloAST * startOfSpaceSequence = pb.createAnd(pb.createNot(prevSpaceSpans), spaceSpans);
        
        // Last space before word: current=space AND next=word
        PabloAST * nextTokenSpans = pb.createOr(nextWordSpans, nextsymbolSpans);
        PabloAST * lastSpaceBeforeToken = pb.createAnd(spaceSpans, nextTokenSpans);
        
        PabloAST * boundaryBeforeLastSpace = pb.createAnd(
                    prevSpaceSpans,     // previous was space
                    lastSpaceBeforeToken // current is last space before word
                );
        
        // Word boundaries
        PabloAST * wordBoundaries = pb.createOr(wordToNonWord, nonWordToWord);
        
        // Space boundaries
        PabloAST * spaceBoundaries = pb.createOr(startOfSpaceSequence, boundaryBeforeLastSpace);
        
        // Other character boundaries
        PabloAST * otherCharBoundaries = pb.createOr(otherSpans, prevOtherSpans);
        
        // Symbiol character
        PabloAST * symbolBoundaries = pb.createOr(symbolToAny, anyToSymbol);
        
        // Token boundaries occur at
        PabloAST * tokenBoundaries = pb.createOr3(
            wordBoundaries,
            spaceBoundaries,
            pb.createOr(symbolBoundaries, otherCharBoundaries)
        );
        tokenBoundaries = pb.createAnd(tokenBoundaries, u8First); //boundary shouldn't be at anywhere except at the frst char
        
        // Use the refined boundaries approach
        writeOutputStreamSet("tokenBoundaries", std::vector<PabloAST*>{ tokenBoundaries });
    }
};

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
    
    // Unicode Property Detection
    
    // Detect Unicode word characters (letters, digits, etc.)
    re::RE * wordProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "word");
    wordProp = UCD::linkAndResolve(wordProp);
    re::Name * word = re::makeName("word");
    word->setDefinition(wordProp);
    StreamSet * WordStream = P.CreateStreamSet(1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(word, BasisBits, WordStream);
    SHOW_STREAM(WordStream);
    
    // Detect Unicode space characters
    re::RE * spaceProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "space");
    spaceProp = UCD::linkAndResolve(spaceProp);
    re::Name * space = re::makeName("space");
    space->setDefinition(spaceProp);
    StreamSet * SpaceStream = P.CreateStreamSet(1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(space, BasisBits, SpaceStream);
    SHOW_STREAM(SpaceStream);
    
    // Detect symbol boundries
    re::RE * symbolProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "symbol");
    symbolProp = UCD::linkAndResolve(symbolProp);
    re::Name * symbol = re::makeName("symbol");
    symbol->setDefinition(symbolProp);
    StreamSet * SymbolStream = P.CreateStreamSet(1);
    P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(symbol, BasisBits, SymbolStream);
    SHOW_STREAM(SymbolStream);


    // Convert character-level properties to UTF-8 byte spans
    StreamSet * WordSpans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(WordStream, u8index, WordSpans);
    SHOW_STREAM(WordSpans);
    
    StreamSet * SpaceSpans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(SpaceStream, u8index, SpaceSpans);
    SHOW_STREAM(SpaceSpans);
    
    // NEW: Convert symbol properties to UTF-8 byte spans
    StreamSet * SymbolSpans = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<U8Spans>(SymbolStream, u8index, SymbolSpans);
    SHOW_STREAM(SymbolSpans);
    
   
    StreamSet * TokenBoundaries = P.CreateStreamSet(1, 1);
    
    // Fixed parameter order: WordSpans, SpaceSpans, TokenBoundaries (inputs first, output last)
    P.CreateKernelCall<UnicodeWordBoundaryDetector>(u8index, WordSpans, SpaceSpans,SymbolSpans, TokenBoundaries);
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
