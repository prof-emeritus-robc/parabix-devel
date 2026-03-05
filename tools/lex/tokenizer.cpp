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
#include <pablo/pe_ones.h>
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
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/UCD_property_kernel.h>
#include <re/unicode/boundaries.h>
#include <re/analysis/collect_ccs.h>
#include <re/transforms/re_transformer.h>
#include <re/transforms/re_multiplex.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/boundary_kernels.h>

namespace fs = boost::filesystem;

using namespace llvm;
using namespace codegen;
using namespace kernel;
using namespace pablo;
using namespace re;

static cl::OptionCategory wordBreakerFlags("Command Flags", "Unicode word breaker options");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(wordBreakerFlags));

// Enum for pre-tokenizer selection
enum PreTokenizerMode {
  uax29,
  whitespace,
  whitespacesplit,
  digits,
  punctuation,
  simpleWordBoundaries,
  bytelevel,
  chardelimiter,
  bert,
  sequence_whitespace_punctuation 
};

// Enum for split behavior
enum SplitBehaviorMode {
  nosplitbehavior,
  removed,
  isolated,
  mergedwithprevious,
  mergedwithnext,
  contiguous
};

std::string SplitCode(SplitBehaviorMode s) {
    if (s == isolated) return "i";
    if (s == contiguous) return "c";
    if (s == mergedwithprevious) return "p";
    if (s == mergedwithnext) return "n";
    if (s == removed) return "r";
    return "d";
}

// pretokenizer selection as named alternative 
static cl::opt<PreTokenizerMode> PreTokenizer(
    "pretokenizer",
    cl::desc("Pre-tokenizer mode:"),
    cl::init(uax29),      // DEFAULT VALUE
    cl::values(
        clEnumValN(uax29, "uax29", "Unicode UAX#29 word boundaries (default)"),
        clEnumValN(whitespace, "whitespace", "Split on whitespace characters"),
        clEnumValN(whitespacesplit, "whitespacesplit", "Split on whitespace and output delimiters as separate tokens"),
        clEnumValN(digits, "digits", "Split on digit sequences"),
        clEnumValN(punctuation, "punctuation", "Split on punctuation characters"),
        clEnumValN(simpleWordBoundaries, "simplewordboundaries", "Simple word boundaries based on alphanumeric characters"),
        clEnumValN(bytelevel, "bytelevel", "ByteLevel tokenization: split on whitespace with byte remapping"),
        clEnumValN(chardelimiter, "chardelimiter", "Split on a specific character delimiter"),
        clEnumValN(bert, "bert", "BERT pre-tokenizer: separates punctuation and words"),
        clEnumValN(sequence_whitespace_punctuation, "sequence_whitespace_punctuation", 
                   "Sequence pre-tokenizer: Whitespace then Punctuation")
    ),
    cl::cat(wordBreakerFlags)
);

// Split behavior as named alternative
static cl::opt<SplitBehaviorMode> SplitBehavior("behavior",
    cl::desc("Split delimiter behavior:"),
    cl::init(isolated),  // DEFAULT VALUE
    cl::values(
        clEnumValN(removed, "removed", "only keep word/punctuation boundaries, exclude whitespace"),
        clEnumValN(isolated, "isolated", "keep token boundaries AND add space boundaries"),
        clEnumValN(mergedwithprevious, "mergedwithprevious", "attach whitespace to previous word"),
        clEnumValN(mergedwithnext, "mergedwithnext", "attach whitespace to next word"),
        clEnumValN(contiguous, "contiguous", "keep punctuation with words, separate spaces")),
    cl::cat(wordBreakerFlags));

// Remove first position mark from insertion mask
class RemoveFirstMarkKernel : public PabloKernel {
public:
    RemoveFirstMarkKernel(LLVMTypeSystemInterface & ts,
                          StreamSet * inputMask,
                          StreamSet * outputMask)
    : PabloKernel(ts, "removeFirstMarkKernel",
                  {Binding{"inputMask", inputMask}},
                  {Binding{"outputMask", outputMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * mask = getInputStreamSet("inputMask")[0];
        
        // Create a stream that is 0 at position 0 and 1 everywhere else
        PabloAST * notAtFirst = pb.createAdvance(pb.createOnes(), 1);

        // AND the mask with notAtFirst to exclude position 0
        PabloAST * result = pb.createAnd(mask, notAtFirst);
        writeOutputStreamSet("outputMask", std::vector<PabloAST*>{result});
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
        PabloAST * result = pb.createAnd(stream1, u8index);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// NOT kernel to invert a bit stream (1→0, 0→1)
class NotKernel : public PabloKernel {
public:
    NotKernel(LLVMTypeSystemInterface & ts,
              StreamSet * input,
              StreamSet * output)
    : PabloKernel(ts, "notKernel",
                  {Binding{"input", input}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream = getInputStreamSet("input")[0];
        PabloAST * result = pb.createNot(stream);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// OR kernel to combine two boundary streams (union)
class OrKernel : public PabloKernel {
public:
    OrKernel(LLVMTypeSystemInterface & ts,
             StreamSet * input1,
             StreamSet * input2,
             StreamSet * output)
    : PabloKernel(ts, "orKernel",
                  {Binding{"input1", input1}, Binding{"input2", input2}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream1 = getInputStreamSet("input1")[0];
        PabloAST * stream2 = getInputStreamSet("input2")[0];
        PabloAST * result = pb.createOr(stream1, stream2);
        writeOutputStreamSet("output", std::vector<PabloAST*>{result});
    }
};

// Unicode Alphanumeric Detection kernel using Unicode properties
// Combines (L*), Mark (M*), and Number (N*) Unicode categories
// to detect alphanumeric characters properly according to Unicode standard
class UnicodeAlphanumericDetector : public PabloKernel {
public:
    UnicodeAlphanumericDetector(LLVMTypeSystemInterface & ts,
                                StreamSet * BasisBits,
                                StreamSet * AlphanumericMask,
                                StreamSet * LetterStream,
                                //StreamSet * MarkStream,
                                StreamSet * NumberStream)
    : PabloKernel(ts, "unicodeAlphanumericDetector",
                  {Binding{"LetterStream", LetterStream}, 
                   Binding{"NumberStream", NumberStream}},
                  {Binding{"AlphanumericMask", AlphanumericMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        
        // Get the Unicode property streams for letters, marks, and numbers
        PabloAST * letter = getInputStreamSet("LetterStream")[0];
        PabloAST * number = getInputStreamSet("NumberStream")[0];
        
        // Alphanumeric = Letter OR Number (excluding marks)
        // L* categories include: Lu (Uppercase), Ll (Lowercase), Lt (Titlecase), 
        //                        Lm (Modifier), Lo (Other Letter)
        // N* categories include: Nd (Decimal Digit), Nl (Letter Number), No (Other Number)
        PabloAST * alphanumeric = pb.createOr(letter, number);
        
        writeOutputStreamSet("AlphanumericMask", std::vector<PabloAST*>{alphanumeric});
    }
};

// Detect whitespace/delimiter positions (ASCII space 0x20 = 00100000)
class WhitespaceDetector : public PabloKernel {
public:
    WhitespaceDetector(LLVMTypeSystemInterface & ts,
                       StreamSet * BasisBits,
                       StreamSet * WhitespaceMask)
    : PabloKernel(ts, "whitespaceDetector" + BasisBits -> shapeString(),
                  {Binding{"BasisBits", BasisBits}},
                  {Binding{"WhitespaceMask", WhitespaceMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("BasisBits");
        
        // ASCII space is 0x20 = 00100000 (bit 5 set, all others clear) ? unicdoe space?
        PabloAST * isSpace = basis[5];
        for (unsigned i = 0; i < basis.size(); ++i) {
            if (i == 5) continue;
            isSpace = pb.createAnd(isSpace, pb.createNot(basis[i]));
        }
        // WhitespaceMask is a binary stream that marks every position in the input 
        //where an ASCII space character (0x20) occurs.
        writeOutputStreamSet("WhitespaceMask", std::vector<PabloAST*>{isSpace});
    }
};


// 
//  Given SplitMarks marking characters that are "split" characters,
//  produce output token marks according to a given split behaviour
//  mode.   The result is a correct token mark stream, but an additional
//  step is required when the behaviour is "removed" in which case all
//  the split characters must be deleted.
//
class SplitMarksToTokens : public PabloKernel {
public:
    SplitMarksToTokens(LLVMTypeSystemInterface & ts,
                  SplitBehaviorMode b, StreamSet * SplitMarks, StreamSet * ResultBoundaries)
    : PabloKernel(ts, "SplitMarksToTokens:" + SplitCode(b) ,
                  {Binding{"SplitMarks", SplitMarks}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}), mBehavior(b) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * SplitMarks = getInputStreamSet("SplitMarks")[0];
        //
        PabloAST * SplitRun1 = pb.createAnd(pb.createAdvance(pb.createNot(SplitMarks), 1), SplitMarks);
        PabloAST * SplitRunFollow = pb.createAnd(pb.createAdvance(SplitMarks, 1), pb.createNot(SplitMarks));
        PabloAST * result = nullptr;
        if (mBehavior == contiguous) {
            result = pb.createOr(SplitRun1, SplitRunFollow);
        } else if (mBehavior == mergedwithprevious) {
            result = SplitRunFollow;
        } else if (mBehavior == removed) {
            result = SplitRunFollow;
        } else if (mBehavior == mergedwithnext) {
            result = SplitRun1;
        } else { // isolated
            result = pb.createOr(SplitMarks, SplitRunFollow);
        }
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
private:
    SplitBehaviorMode mBehavior;
};

// Behavior mode 1: Isolated - keep token boundaries AND add space boundaries
class IsolatedBehavior : public PabloKernel {
public:
    IsolatedBehavior(LLVMTypeSystemInterface & ts,
                     StreamSet * TokenBoundaries,
                     StreamSet * WhitespaceMask,
                     StreamSet * ResultBoundaries)
    : PabloKernel(ts, "isolatedBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        
        // Find start of whitespace runs (first space in consecutive spaces)
        PabloAST * spaceStart = pb.createAnd(whitespace, pb.createNot(pb.createAdvance(whitespace, 1)));
        
        // Result = token boundaries OR space starts(space boundries, word/punctuation positions kept, spaces isolated)
        PabloAST * result = pb.createOr(boundaries, spaceStart);
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 3: MergedWithPrevious - attach whitespace to previous word
class MergedWithPreviousBehavior : public PabloKernel {
public:
    MergedWithPreviousBehavior(LLVMTypeSystemInterface & ts,
                               StreamSet * TokenBoundaries,
                               StreamSet * WhitespaceMask,
                               StreamSet * ResultBoundaries)
    : PabloKernel(ts, "mergedWithPreviousBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        // Shift right by 1: moves boundaries to merge whitespace with previous token
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(whitespace));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 4: MergedWithNext - attach whitespace to next word
class MergedWithNextBehavior : public PabloKernel {
public:
    MergedWithNextBehavior(LLVMTypeSystemInterface & ts,
                           StreamSet * TokenBoundaries,
                           StreamSet * WhitespaceMask,
                           StreamSet * ResultBoundaries)
    : PabloKernel(ts, "mergedWithNextBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        // Shift left by 1: moves boundaries to merge whitespace with next token
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(pb.createAdvance(whitespace, 1)));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 5: Contiguous - keep punctuation with words, separate spaces
class ContiguousBehavior : public PabloKernel {
public:
    ContiguousBehavior(LLVMTypeSystemInterface & ts,
                       StreamSet * TokenBoundaries,
                       StreamSet * WhitespaceMask,
                       StreamSet * AlphanumericMask, // use a unicode property? or of letter and numeric ?
                       StreamSet * PunctuationStream,
                       StreamSet * ResultBoundaries)
    : PabloKernel(ts, "contiguousBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, 
                   Binding{"WhitespaceMask", WhitespaceMask}, 
                   Binding{"AlphanumericMask", AlphanumericMask, FixedRate(), LookAhead(1)},
                   Binding{"PunctuationStream", PunctuationStream}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * alphanumeric = getInputStreamSet("AlphanumericMask")[0];
        PabloAST * punctuation = getInputStreamSet("PunctuationStream")[0];

        // Punctuation immediately follows alphanumeric should not have boundaries
        PabloAST * currentIsPunctuation = punctuation;
        PabloAST * previousIsAlpha = pb.createAdvance(alphanumeric, 1); // looks BEHIND by 1 character position
        PabloAST * punctAfterAlpha = pb.createAnd(currentIsPunctuation, previousIsAlpha);
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(punctAfterAlpha));
       
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Debug macros for visualization
#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name)  if (codegen::EnableIllustrator) P.captureByteData(#name, name)

using WordBreakerFunctionType = void (*)(uint32_t fd);


// make a new function here 
void whiteSpaceLogic (PipelineBuilder & P, StreamSet * BasisBits , StreamSet * u8index, StreamSet * results) {
        
    re::RE * rule1 = re::generateRE_TokenizerRule(re::WhitespaceBoundary);
        const auto WS_Sets = re::collectCCs(rule1, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto WS_mpx = cc::makeMultiplexedAlphabet("WS_mpx", WS_Sets);
        rule1 = transformCCs(WS_mpx, rule1, re::NameTransformationMode::TransformDefinition);
        auto WS_basis = WS_mpx->getMultiplexedCCs();
        StreamSet * const WS_Classes = P.CreateStreamSet(WS_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(WS_basis, BasisBits, WS_Classes);
        auto ws_options = std::make_unique<GrepKernelOptions>();
        ws_options->setIndexing(u8index);
        ws_options->setRE(rule1);
        ws_options->addAlphabet(WS_mpx, WS_Classes);
        ws_options->setResults(results);
        ws_options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(ws_options));
        SHOW_STREAM(results);
};
 
void applyRemovedWhitespaceFilter(PipelineBuilder & P,
                                                          StreamSet * WhitespaceMask,
                                                          StreamSet * U21codepoints,
                                                          StreamSet * U21_tokenBoundaries,
                                                          StreamSet * finalU21codepoints,
                                                          StreamSet * finalU21_tokenBoundaries) {
    StreamSet * keepMask = P.CreateStreamSet(1);
    P.CreateKernelCall<NotKernel>(WhitespaceMask, keepMask);

    StreamSet * keepMask2 = P.CreateStreamSet(1);
    P.CreateKernelCall<OrKernel>(keepMask,U21_tokenBoundaries, keepMask2);

    FilterByMask(P, keepMask2, U21codepoints, finalU21codepoints);

    FilterByMask(P, keepMask2, U21_tokenBoundaries, finalU21_tokenBoundaries);
}
// Function to apply split behavior transformation based on split behavior mode
void applySplitBehaviorTransformation(
    PipelineBuilder & P,
    SplitBehaviorMode effectiveBehavior,
    StreamSet * spreadBasis,
    StreamSet * spreadWhitespaceMask,
    StreamSet * spreadTokenBoundaries,
    StreamSet * spreadAlphanumericMask,
    StreamSet * spreadPunctuationStream,
    StreamSet *& finalU21codepoints,
    StreamSet *& TransformedBoundaries)
{
    // All behavior transformations are applied at the U21 level (before LF insertion)
    // via the insertionBoundaries block in wordBreakerPipeline. This is a pass-through.
    finalU21codepoints = spreadBasis;
    TransformedBoundaries = spreadTokenBoundaries;
    SHOW_STREAM(TransformedBoundaries);
}
// Function Declaration
// Helper function to build RE-based tokenizer boundaries
// Centralizes the common pattern: generate RE → collect CCs → multiplex → create kernels
StreamSet* buildREBasedTokenizer(
    PipelineBuilder& P,
    const std::string& prefixName,  // unique identifier for this tokenizer e.g., "PC", "WS"
    re::RE* rule,                  // the regex pattern to use
    StreamSet* BasisBits,          // the basis bits representing the input characters
    StreamSet* u8index             // UTF-8 character boundary index for correct token boundary alignment
) {
    if (!rule) {
        llvm::errs() << "Error: null RE rule for " << prefixName << "\n";
        return nullptr;
    }
    // empty stream to hold the boundary results
    StreamSet* WordBoundaries = P.CreateStreamSet(1, 1);
    
    const auto Sets = re::collectCCs(rule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
    auto mpx = cc::makeMultiplexedAlphabet(prefixName + "_mpx", Sets);
    rule = transformCCs(mpx, rule, re::NameTransformationMode::TransformDefinition);
    auto basis = mpx->getMultiplexedCCs();
    
    StreamSet* const Classes = P.CreateStreamSet(basis.size());
    P.CreateKernelFamilyCall<CharClassesKernel>(basis, BasisBits, Classes);
    
    auto options = std::make_unique<GrepKernelOptions>();
    options->setIndexing(u8index);
    options->setRE(rule);
    options->addAlphabet(mpx, Classes);
    options->setResults(WordBoundaries);
    options->addExternal("UTF8_index", u8index);
    P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
    
    SHOW_STREAM(WordBoundaries);
    return WordBoundaries;
}

// Struct to hold tokenizer configuration
struct TokenizerConfig {
    re::RE_TokenizerKind kind;   // Enum value from boundaries.h
    std::string prefix;  // Prefix for multiplexed alphabet (e.g., "PC", "WS")
};

// Single map combining both pieces of information
const static std::map<PreTokenizerMode, TokenizerConfig> TokenizerConfigs = {
    {whitespace, {re::WhitespaceBoundary, "WS"}},
    {whitespacesplit, {re::WhitespaceSplitBoundary, "WSS"}},
    {punctuation, {re::PunctuationBoundary, "PC"}},
    {digits, {re::DigitBoundary, "DG"}},
    {bytelevel, {re::ByteLevelBoundary, "BL"}},
    {bert, {re::BertPreTokenizer, "BERT"}}
};

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
    
    // Create UTF-8 character boundary index (UTF-8 Index Creation)
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);
    SHOW_STREAM(u8index);
    
    // Unicode Word Boundary Rules
    StreamSet * WordBoundaries = nullptr;

    // Special case: simpleWordBoundaries uses different kernel pipeline
    if(PreTokenizer == simpleWordBoundaries){
        // Use simple word boundaries based on Unicode "word" property
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * wordProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "word");
        wordProp = UCD::linkAndResolve(wordProp);
        re::Name * word = re::makeName("word");
        word->setDefinition(wordProp);
        StreamSet * WordStream = P.CreateStreamSet(1);
        P.CreateKernelFamilyCall<UnicodePropertyKernelBuilder>(word, BasisBits, WordStream);
        P.CreateKernelCall<BoundaryKernel>(WordStream, u8index, WordBoundaries);
    }
    // whitespace uses separate whiteSpaceLogic function
    else if (PreTokenizer == whitespace){
        WordBoundaries = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, BasisBits, u8index, WordBoundaries);
        SHOW_STREAM(WordBoundaries);
    }
    // composite tokenizer (OR of two RE rules)
    else if (PreTokenizer == sequence_whitespace_punctuation) {
        // Composite tokenizer: whitespace + punctuation combined with OR
        StreamSet * preTokenStrm1 = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, BasisBits, u8index, preTokenStrm1);
       
        StreamSet * preTokenStrm2 = buildREBasedTokenizer(P, "PC", 
            re::generateRE_TokenizerRule(re::PunctuationBoundary), BasisBits, u8index);

        // Combine boundaries (OR operation)
        WordBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<OrKernel>(preTokenStrm1, preTokenStrm2, WordBoundaries);
        SHOW_STREAM(WordBoundaries);
    }
    else {
        // Standard RE-based tokenizers - use lookup table
        auto it = TokenizerConfigs.find(PreTokenizer);
        
        if (it != TokenizerConfigs.end()) {
            // Found in map: call the generator function and build pipeline
            re::RE* rule = generateRE_TokenizerRule(it->second.kind);  // Call enum depatcher to get the appropriate RE rule
            WordBoundaries = buildREBasedTokenizer(P, it->second.prefix, 
                                                   rule, BasisBits, u8index);
        } else {
            // Default fallback: UAX#29 word boundaries
            re::RE* rule = re::generateWordBoundaryRule();
            WordBoundaries = buildREBasedTokenizer(P, "WB", rule, BasisBits, u8index);
        }
    }
    // UTF-8 - ensure boundaries only at UTF-8 character starts
    StreamSet * TokenBoundaries = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<AndKernel>(WordBoundaries, u8index, TokenBoundaries);
    SHOW_STREAM(TokenBoundaries);
   
    // UTF-8 to U21 Conversion Pipeline
   // Creating U21 codepoint stream from UTF-8 basis bits (U21 Codepoint Generation)
    StreamSet * U21_u8indexed = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);
    // filter by mask with UTF-8 index stream ?
    SHOW_BIXNUM(U21_u8indexed);

    StreamSet * U21codepoints = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21codepoints);
    SHOW_BIXNUM(U21codepoints); 

    StreamSet * U21_tokenBoundaries = P.CreateStreamSet(1);
    FilterByMask(P, u8index, TokenBoundaries, U21_tokenBoundaries);
    SHOW_STREAM(U21_tokenBoundaries);
    
    // Detect alphanumeric positions using Unicode properties (Letter, Mark, Number)
    // L* categories: Letter (uppercase, lowercase, titlecase, modifier, other)
    // M* categories: Mark (nonspacing, spacing, enclosing)
    // N* categories: Number (decimal, letter number, other)
    // Unicode Properties Created from U21 codepoint stream
    // Create Letter property stream using Unicode general category 'Letter' 
    // Detects all Unicode letter characters (Lu, Ll, Lt, Lm, Lo)
    re::RE * letterProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Letter");
    letterProp = UCD::linkAndResolve(letterProp);
    re::Name * letterName = re::makeName("Letter");
    letterName->setDefinition(letterProp);
    StreamSet * LetterStream = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(letterName, U21codepoints, LetterStream);
    SHOW_STREAM(LetterStream);

    // Create Number property stream using Unicode general category 'Number'
    // Detects all Unicode number characters (Nd, Nl, No)
    // Nd: Decimal digit numbers, Nl: Letter numbers, No: Other numbers
    re::RE * numberProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Number");
    numberProp = UCD::linkAndResolve(numberProp);
    re::Name * numberName = re::makeName("Number");
    numberName->setDefinition(numberProp);
    StreamSet * NumberStream = P.CreateStreamSet(1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(numberName, U21codepoints, NumberStream);
    SHOW_STREAM(NumberStream);

    // Create Punctuation property stream using Unicode general category 'Punctuation'
    // Detects all Unicode punctuation characters (Pc, Pd, Ps, Pe, Pi, Pf, Po)
    // Pc: Connector punctuation (_, ‿), Pd: Dash punctuation (-, –, —)
    // Ps: Open punctuation ( (, {, [ ), Pe: Close punctuation ( ), }, ] )
    // Pi: Initial quote punctuation (‘, “), Pf: Final quote punctuation (’, ”)
    // Po: Other punctuation (!, ?, ., , , ;, :, etc.)
    re::RE * punctuationProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "Punctuation");
    punctuationProp = UCD::linkAndResolve(punctuationProp);
    re::Name * punctuationName = re::makeName("Punctuation");
    punctuationName->setDefinition(punctuationProp);
    StreamSet * PunctuationStream = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodePropertyKernelBuilder>(punctuationName, U21codepoints, PunctuationStream);
    SHOW_STREAM(PunctuationStream);

    // Combine Letter and Number properties to detect alphanumeric characters
    StreamSet * AlphanumericMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UnicodeAlphanumericDetector>(U21codepoints, AlphanumericMask, LetterStream, NumberStream);
    SHOW_STREAM(AlphanumericMask);

    // Detect whitespace/delimiter positions BEFORE spreading/inserting
    // This ensures alignment with U21_tokenBoundaries
    StreamSet * WhitespaceMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<WhitespaceDetector>(U21codepoints, WhitespaceMask);
    SHOW_STREAM(WhitespaceMask);

    // Apply behavior transformation using appropriate kernel
    StreamSet * TransformedBoundaries = P.CreateStreamSet(1, 1);

    // moved here: needs to know effectiveBehavior BEFORE computing lineInsertMask
    // Compute effective behavior mode based on PreTokenizer choice
    SplitBehaviorMode effectiveBehavior = SplitBehavior;
    if (PreTokenizer == bert || PreTokenizer == sequence_whitespace_punctuation || PreTokenizer == whitespacesplit || PreTokenizer == whitespace) {
        effectiveBehavior = removed;
    }

    // For removed behavior: U21_tokenBoundaries has boundaries on BOTH sides of whitespace
    // (UAX29 places a boundary before AND after each whitespace run). Inserting LF before
    // the whitespace-position boundary + removing whitespace = two consecutive LFs = blank line.
    // so to Fix it: we remove boundaries that fall ON whitespace positions before computing lineInsertMask.
    // Compute corrected LF-insertion boundaries at U21 level (BEFORE spreading/insertion).
    // Each behavior kernel removes or adds boundaries here so only the RIGHT LFs get inserted.

    StreamSet * insertionBoundaries = U21_tokenBoundaries;
    if (effectiveBehavior == removed) {
        // Remove boundaries ON whitespace positions so no LF is inserted before a space.
        // FilterByMask later deletes the space characters themselves.
        StreamSet * notWhitespaceMask = P.CreateStreamSet(1);
        P.CreateKernelCall<NotKernel>(WhitespaceMask, notWhitespaceMask);
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<AndKernel>(U21_tokenBoundaries, notWhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == isolated) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<IsolatedBehavior>(U21_tokenBoundaries, WhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == mergedwithprevious) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<MergedWithPreviousBehavior>(U21_tokenBoundaries, WhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == mergedwithnext) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<MergedWithNextBehavior>(U21_tokenBoundaries, WhitespaceMask, insertionBoundaries);
    } else if (effectiveBehavior == contiguous) {
        insertionBoundaries = P.CreateStreamSet(1);
        P.CreateKernelCall<ContiguousBehavior>(U21_tokenBoundaries, WhitespaceMask, AlphanumericMask, PunctuationStream, insertionBoundaries);
    }

    // Create insertion mask and spread original data
    // This is the first place the length of the stream changes.
    StreamSet * lineInsertMask = UnitInsertionSpreadMask(P, insertionBoundaries, kernel::InsertPosition::Before);
    SHOW_STREAM(lineInsertMask);
   
    StreamSet * spreadBasis = P.CreateStreamSet(21);
    SpreadByMask(P, lineInsertMask, U21codepoints, spreadBasis);
    SHOW_BIXNUM(spreadBasis);

    // Insert Token Separators
    StreamSet * tokenBasis = P.CreateStreamSet(21);
    P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    SHOW_BIXNUM(tokenBasis);

    // spreading to match the streams
    // Spread boundaries to match tokenBasis length (L+I)
    StreamSet * spreadTokenBoundaries = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, U21_tokenBoundaries, spreadTokenBoundaries);

    // Spread whitespace mask to match tokenBasis length
    StreamSet * spreadWhitespaceMask = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, WhitespaceMask, spreadWhitespaceMask);
    
    // Spread alphanumeric mask to match tokenBasis length (needed for contiguous behavior)
    StreamSet * spreadAlphanumericMask = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, AlphanumericMask, spreadAlphanumericMask);
    
    // Spread punctuation stream to match tokenBasis length (needed for contiguous behavior)
    StreamSet * spreadPunctuationStream = P.CreateStreamSet(1);
    SpreadByMask(P, lineInsertMask, PunctuationStream, spreadPunctuationStream);
    
    StreamSet * tokenMask = P.CreateStreamSet(1);
    P.CreateKernelCall<NotKernel>(spreadWhitespaceMask, tokenMask);

    if (effectiveBehavior == removed) {
        StreamSet * newBasis = P.CreateStreamSet(21);
        FilterByMask(P, tokenMask, tokenBasis, newBasis);
        tokenBasis = newBasis;
        StreamSet * newBoundaries = P.CreateStreamSet(1);
        FilterByMask(P, tokenMask, spreadTokenBoundaries, newBoundaries);
        spreadTokenBoundaries = newBoundaries;
    }
    
    // After spreading and inserting, we have new codepoints at the inserted positions.
    StreamSet * finalU21codepoints = tokenBasis;
    applySplitBehaviorTransformation(P, effectiveBehavior, tokenBasis, spreadWhitespaceMask, 
                                  spreadTokenBoundaries, spreadAlphanumericMask, spreadPunctuationStream,
                                  finalU21codepoints, TransformedBoundaries);
    SHOW_STREAM(TransformedBoundaries);
    
    StreamSet * TransformedBoundaries1 = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<RemoveFirstMarkKernel>(TransformedBoundaries, TransformedBoundaries1);
    SHOW_STREAM(TransformedBoundaries1);

    // Convert U21 codepoints back to UTF-8 basis bits
    StreamSet * output_basis = P.CreateStreamSet(8);
    U21_to_UTF8(P, finalU21codepoints, output_basis);
    SHOW_BIXNUM(output_basis);

    // Convert parallel bit streams back to serial bytes
    StreamSet * tokenizedOutput = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(output_basis, tokenizedOutput);
    SHOW_BYTES(tokenizedOutput);

    // Write to standard output
    P.CreateKernelCall<StdOutKernel>(tokenizedOutput);
    
    return reinterpret_cast<WordBreakerFunctionType>(P.compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&wordBreakerFlags, pablo::pablo_toolchain_flags(), codegen::codegen_flags()});  //command line options 
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
