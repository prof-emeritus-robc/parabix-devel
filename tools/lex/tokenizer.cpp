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

// Enum for pre-tokenizer selection
enum PreTokenizerMode {
  uax29,
  icu,
  gpt2,
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

// pretokenizer selection as named alternative 
static cl::opt<PreTokenizerMode> PreTokenizer(
    "pretokenizer",
    cl::desc("Pre-tokenizer mode:"),
    cl::init(uax29),      // DEFAULT VALUE
    cl::values(
        clEnumValN(uax29, "uax29", "Unicode UAX#29 word boundaries (default)"),
        clEnumValN(icu, "icu", "ICU BreakIterator word boundaries"),
        clEnumValN(gpt2, "gpt2", "GPT-2 style tokenization"),
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
    cl::init(isolated),
    cl::values(
        clEnumValN(removed, "removed", "only keep word/punctuation boundaries, exclude whitespace"),
        clEnumValN(isolated, "isolated", "keep token boundaries AND add space boundaries"),
        clEnumValN(mergedwithprevious, "mergedwithprevious", "attach whitespace to previous word"),
        clEnumValN(mergedwithnext, "mergedwithnext", "attach whitespace to next word"),
        clEnumValN(contiguous, "contiguous", "keep punctuation with words, separate spaces")),
    cl::cat(wordBreakerFlags));

// ICU locale remains as string (accepts arbitrary locale values)
static cl::opt<std::string> Locale("locale",
    cl::desc("ICU locale for word boundaries (e.g., en_US, fr_FR, ja_JP). Empty = default UAX#29"),
    cl::init(""),
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
        //PabloAST * notAtFirst = pb.createAdvance(pb.createZeroes(), 1);
        PabloAST * notAtFirst = pb.createAdvance(pb.createOnes(), 1);
        // PabloAST * notAtFirst = pb.createLookahead(pb.createOnes(), 1);

        
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

// AND kernel for combining streams (intersection)
class CombineAndKernel : public PabloKernel {
public:
    CombineAndKernel(LLVMTypeSystemInterface & ts,
                     StreamSet * input1,
                     StreamSet * input2,
                     StreamSet * output)
    : PabloKernel(ts, "combineAndKernel",
                  {Binding{"input1", input1}, Binding{"input2", input2}},
                  {Binding{"output", output}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * stream1 = getInputStreamSet("input1")[0];
        PabloAST * stream2 = getInputStreamSet("input2")[0];
        PabloAST * result = pb.createAnd(stream1, stream2);
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

// Behavior mode 1: Removed - only keep word/punctuation boundaries, exclude whitespace
class RemovedBehavior : public PabloKernel {
public:
    RemovedBehavior(LLVMTypeSystemInterface & ts,
                    StreamSet * TokenBoundaries,
                    StreamSet * WhitespaceMask,
                    StreamSet * ResultBoundaries)
    : PabloKernel(ts, "removedBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        
        // For removed: keep boundaries at non-whitespace positions only
        // This explicitly filters out any space positions
        PabloAST * notSpace = pb.createNot(whitespace);
        PabloAST * firstSpace = pb.createAnd(whitespace, pb.createNot(pb.createAdvance(whitespace, 1)));
        PabloAST * result = pb.createAnd(boundaries, pb.createOr(notSpace, firstSpace));
        
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 2: Isolated - keep token boundaries AND add space boundaries
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
        
        re::RE * rule1 = re::generateWhitespaceBoundaryRule();
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
// Function to apply split behavior transformation based on pre-tokenizer selection and split behavior mode
void applySplitBehaviorTransformation(
    PipelineBuilder & P,
    PreTokenizerMode PreTokenizer,
    SplitBehaviorMode SplitBehavior,
    StreamSet * U21codepoints,
    StreamSet * WhitespaceMask,
    StreamSet * U21_tokenBoundaries,
    StreamSet * tokenBasis,
    StreamSet * AlphanumericMask,
    StreamSet * PunctuationStream,
    StreamSet *& finalU21codepoints,
    StreamSet *& TransformedBoundaries)
{
    // For some pre-tokenizers, use 'removed' behavior to skip whitespace
    SplitBehaviorMode effectiveBehavior = SplitBehavior;
    if (PreTokenizer == bert || PreTokenizer == sequence_whitespace_punctuation || PreTokenizer == whitespacesplit || PreTokenizer == whitespace) {
        effectiveBehavior = removed;
    }
    finalU21codepoints = U21codepoints; // default to original codepoints if no filtering applied
    
    if (effectiveBehavior == removed) {
        finalU21codepoints = P.CreateStreamSet(21);
        // changed tokenBasis to U21codepoints
        applyRemovedWhitespaceFilter(P, WhitespaceMask, U21codepoints, U21_tokenBoundaries, finalU21codepoints, TransformedBoundaries);
    } else if (effectiveBehavior == isolated) {
        P.CreateKernelCall<IsolatedBehavior>(U21_tokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (effectiveBehavior == mergedwithprevious) {
        P.CreateKernelCall<MergedWithPreviousBehavior>(U21_tokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (effectiveBehavior == mergedwithnext) {
        P.CreateKernelCall<MergedWithNextBehavior>(U21_tokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (effectiveBehavior == contiguous) {
        P.CreateKernelCall<ContiguousBehavior>(U21_tokenBoundaries, WhitespaceMask, AlphanumericMask, PunctuationStream, TransformedBoundaries);
    } else {
        llvm::errs() << "Error: Unknown SplitBehavior mode.\n";
        TransformedBoundaries = U21_tokenBoundaries; // default to no transformation
    }
    SHOW_STREAM(TransformedBoundaries);
}

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
    
    (void)PreTokenizer; // PreTokenizer variable used later; silence unused-warning if any
    (void)Locale;

    if (PreTokenizer == icu || !Locale.empty()) {
        // Use ICU locale-aware word boundaries
        WordBoundaries = buildWordBoundaryMaskFromICU(P, BasisBits, u8index, Locale);
        SHOW_STREAM(WordBoundaries);

    }else if (PreTokenizer == gpt2) {
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
        SHOW_STREAM(WordBoundaries);
    }
    //Input: "Hello there!"
    //Output: "Hello", "there", "!"   
    else if (PreTokenizer == whitespace){
        // seperate function for white space that we call here 
        WordBoundaries = P.CreateStreamSet(1, 1);
        SHOW_STREAM(WordBoundaries);
        whiteSpaceLogic(P, BasisBits, u8index, WordBoundaries);
    }
    //Input: "Hello there!"
    //Output: "Hello", "there!"
    else if(PreTokenizer == whitespacesplit){
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
        SHOW_STREAM(WordBoundaries);
    }
    else if (PreTokenizer == punctuation){
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
        SHOW_STREAM(WordBoundaries);

   }
   else if (PreTokenizer == digits){
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
        SHOW_STREAM(WordBoundaries);

    }
    // simple word boundries 
    else if(PreTokenizer == simpleWordBoundaries){
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
    else if (PreTokenizer == bytelevel) {
        // Use ByteLevel pre-tokenizer
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * byteLevelRule = re::generateByteLevelBoundaryRule();
        const auto BL_Sets = re::collectCCs(byteLevelRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto BL_mpx = cc::makeMultiplexedAlphabet("BL_mpx", BL_Sets);
        byteLevelRule = transformCCs(BL_mpx, byteLevelRule, re::NameTransformationMode::TransformDefinition);
        auto BL_basis = BL_mpx->getMultiplexedCCs();
        StreamSet * const BL_Classes = P.CreateStreamSet(BL_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(BL_basis, BasisBits, BL_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(byteLevelRule);
        options->addAlphabet(BL_mpx, BL_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
        SHOW_STREAM(WordBoundaries);
   }
    else if (PreTokenizer == bert) {
        // Use BERT pre-tokenizer (Whitespace + Punctuation)
        WordBoundaries = P.CreateStreamSet(1, 1);
        re::RE * bertRule = re::generateBertPreTokenizerRule();
        const auto BERT_Sets = re::collectCCs(bertRule, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto BERT_mpx = cc::makeMultiplexedAlphabet("BERT_mpx", BERT_Sets);
        bertRule = transformCCs(BERT_mpx, bertRule, re::NameTransformationMode::TransformDefinition);
        auto BERT_basis = BERT_mpx->getMultiplexedCCs();
        StreamSet * const BERT_Classes = P.CreateStreamSet(BERT_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(BERT_basis, BasisBits, BERT_Classes);
        auto options = std::make_unique<GrepKernelOptions>();
        options->setIndexing(u8index);
        options->setRE(bertRule);
        options->addAlphabet(BERT_mpx, BERT_Classes);
        options->setResults(WordBoundaries);
        options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(options));
        SHOW_STREAM(WordBoundaries);
    }
    else if (PreTokenizer == sequence_whitespace_punctuation) {
        StreamSet * preTokenStrm1 = P.CreateStreamSet(1, 1);
        whiteSpaceLogic(P, BasisBits, u8index, preTokenStrm1);
       
        // Generate Second Pre-tokenizer Boundaries (Punctuation)
        StreamSet * preTokenStrm2 = P.CreateStreamSet(1, 1);
        re::RE * rule2 = re::generatePunctuationBoundaryRule();
        const auto PC_Sets = re::collectCCs(rule2, cc::Unicode, re::NameProcessingMode::ProcessDefinition);
        auto PC_mpx = cc::makeMultiplexedAlphabet("PC_mpx", PC_Sets);
        rule2 = transformCCs(PC_mpx, rule2, re::NameTransformationMode::TransformDefinition);
        auto PC_basis = PC_mpx->getMultiplexedCCs();
        StreamSet * const PC_Classes = P.CreateStreamSet(PC_basis.size());
        P.CreateKernelFamilyCall<CharClassesKernel>(PC_basis, BasisBits, PC_Classes);
        auto pc_options = std::make_unique<GrepKernelOptions>();
        pc_options->setIndexing(u8index);
        pc_options->setRE(rule2);
        pc_options->addAlphabet(PC_mpx, PC_Classes);
        pc_options->setResults(preTokenStrm2);
        pc_options->addExternal("UTF8_index", u8index);
        P.CreateKernelFamilyCall<ICGrepKernel>(std::move(pc_options));
        SHOW_STREAM(preTokenStrm2);

        // Combine Boundaries (OR operation)
        StreamSet * combinedBoundaries = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<OrKernel>(preTokenStrm1, preTokenStrm2, combinedBoundaries);
        SHOW_STREAM(combinedBoundaries);
        
        // Use combined boundaries as WordBoundaries
        WordBoundaries = combinedBoundaries;
    }

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
        SHOW_STREAM(WordBoundaries);
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

    // Whitespace property stream using Unicode property 'space'
    // Detects all Unicode whitespace characters (space, tab, newline, etc.)
    // Includes: SPACE, TAB, LF, VT, FF, CR, NO-BREAK SPACE, etc.
    re::RE * whitespaceProp = re::makePropertyExpression(PropertyExpression::Kind::Codepoint, "space");
    whitespaceProp = UCD::linkAndResolve(whitespaceProp);
    re::Name * whitespaceName = re::makeName("Whitespace");
    whitespaceName->setDefinition(whitespaceProp);

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

    // Create insertion mask and spread original data 
    // This is the first place the length of the stream changes.
    StreamSet * lineInsertMask = UnitInsertionSpreadMask(P, U21_tokenBoundaries, kernel::InsertPosition::Before);
    SHOW_STREAM(lineInsertMask);
   
    StreamSet * spreadBasis = P.CreateStreamSet(21);
    SpreadByMask(P, lineInsertMask, U21codepoints, spreadBasis);  //? same U21_tokenBoundaries used for spreading and later filtering to stay aligned with codepoints
    //SpreadByMask(P, lineInsertMask, U21codepoints, spreadBasis);
    SHOW_BIXNUM(spreadBasis);

    // Insert Token Separators
    StreamSet * tokenBasis = P.CreateStreamSet(21);
    P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    //P.CreateKernelCall<AddUnicodeLineSeparators>(lineInsertMask, spreadBasis, tokenBasis);
    SHOW_BIXNUM(tokenBasis);
    
    // After spreading and inserting, we have new codepoints at the inserted positions.
    StreamSet * finalU21codepoints = U21codepoints;
    applySplitBehaviorTransformation(P, PreTokenizer, SplitBehavior, U21codepoints, WhitespaceMask, 
                                  U21_tokenBoundaries, tokenBasis, AlphanumericMask, PunctuationStream,
                                  finalU21codepoints, TransformedBoundaries);
    SHOW_STREAM(TransformedBoundaries);

     // For BERT and Sequence pre-tokenizers, always use 'removed' behavior to skip whitespace
    // SplitBehaviorMode effectiveBehavior = SplitBehavior;
    // if (PreTokenizer == bert || PreTokenizer == sequence_whitespace_punctuation || PreTokenizer == whitespacesplit || PreTokenizer == whitespace) {
    //     effectiveBehavior = removed;
    // }
    // StreamSet * finalU21codepoints = U21codepoints; // default to original codepoints if no filtering applied
    
    // if (effectiveBehavior == removed) {
    //     finalU21codepoints = P.CreateStreamSet(21);

    //     // calling the helper function to remove whitespace
    //     // removed = applyRemovedWhitespaceFilter(P, WhitespaceMask, tokenBasis, U21_tokenBoundaries);
    //     applyRemovedWhitespaceFilter(P, WhitespaceMask, tokenBasis, U21_tokenBoundaries, finalU21codepoints, TransformedBoundaries);

    // } else if (effectiveBehavior == isolated) {
    //     P.CreateKernelCall<IsolatedBehavior>(U21_tokenBoundaries, WhitespaceMask, TransformedBoundaries);
    // } else if (effectiveBehavior == mergedwithprevious) {
    //     P.CreateKernelCall<MergedWithPreviousBehavior>(U21_tokenBoundaries, WhitespaceMask, TransformedBoundaries);
    // } else if (effectiveBehavior == mergedwithnext) {
    //     P.CreateKernelCall<MergedWithNextBehavior>(U21_tokenBoundaries, WhitespaceMask, TransformedBoundaries);
    // } else if (effectiveBehavior == contiguous) {
    //     P.CreateKernelCall<ContiguousBehavior>(U21_tokenBoundaries, WhitespaceMask, AlphanumericMask, PunctuationStream, TransformedBoundaries);
    // } else {
    //     llvm::errs() << "Error: Unknown SplitBehavior mode.\n";
    //     TransformedBoundaries = U21_tokenBoundaries; // default to no transformation
    // }
    // SHOW_STREAM(TransformedBoundaries);
    
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
