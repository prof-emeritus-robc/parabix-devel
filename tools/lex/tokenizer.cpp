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

// Split delimiter behavior: removed, isolated, mergedwithprevious, mergedwithnext, contiguous
static cl::opt<std::string> SplitBehavior("behavior",
    cl::desc("Split delimiter behavior: removed|isolated|mergedwithprevious|mergedwithnext|contiguous"),
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
// Alphanueric detector: marks positions of ASCII alphanumeric characters
class AlphanumericDetector : public PabloKernel {
public:
    AlphanumericDetector(LLVMTypeSystemInterface & ts,
                       StreamSet * BasisBits,
                       StreamSet * AlphanumericMask)
    : PabloKernel(ts, "alphanumericDetector",
                  {Binding{"BasisBits", BasisBits}},
                  {Binding{"AlphanumericMask", AlphanumericMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> basis = getInputStreamSet("BasisBits"); 

        // Letters A-Z and a-z have bit 6 set (bit[6]=1) and bit 7 clear (bit[7]=0)
        // (01xxxxxx range: 0x40-0x7F, but we want 0x41-0x5A and 0x61-0x7A)
        PabloAST * isLetter = pb.createAnd(basis[6], pb.createNot(basis[7]));
        
        // Digits 0-9 are 0x30-0x39 (00111xxx)
        // Bit 7,6 must be clear (00xxxxxx), bits 5,4 must be set (xx11xxxx)
        PabloAST * isDigit = pb.createAnd(
            pb.createNot(basis[7]),
            pb.createAnd(pb.createNot(basis[6]), 
                        pb.createAnd(basis[5], basis[4]))
        );
        
        // Result: letters OR digits
        PabloAST * result = pb.createOr(isLetter, isDigit);
        
        writeOutputStreamSet("AlphanumericMask", std::vector<PabloAST*>{result});
    }
};

// Detect whitespace/delimiter positions (ASCII space 0x20 = 00100000)
class WhitespaceDetector : public PabloKernel {
public:
    WhitespaceDetector(LLVMTypeSystemInterface & ts,
                       StreamSet * BasisBits,
                       StreamSet * WhitespaceMask)
    : PabloKernel(ts, "whitespaceDetector",
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
        PabloAST * result = pb.createAnd(boundaries, notSpace);
        
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
                               StreamSet * whitespaceMask,
                               StreamSet * ResultBoundaries)
    : PabloKernel(ts, "mergedWithPreviousBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"whitespaceMask", whitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("whitespaceMask")[0];
        // Shift right by 1: moves boundaries to merge whitespace with previous token
        //PabloAST * result = pb.createLookahead(boundaries, 1);
        PabloAST * result = pb.createAnd(boundaries, pb.createNot(whitespace));
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{result});
    }
};

// Behavior mode 4: MergedWithNext - attach whitespace to next word
class MergedWithNextBehavior : public PabloKernel {
public:
    MergedWithNextBehavior(LLVMTypeSystemInterface & ts,
                           StreamSet * TokenBoundaries,
                           StreamSet * whitespaceMask,
                           StreamSet * ResultBoundaries)
    : PabloKernel(ts, "mergedWithNextBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"whitespaceMask", whitespaceMask}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("whitespaceMask")[0];
        
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
                       StreamSet * AlphanumericMask,
                       StreamSet * ResultBoundaries)
    : PabloKernel(ts, "contiguousBehavior",
                  {Binding{"TokenBoundaries", TokenBoundaries}, Binding{"WhitespaceMask", WhitespaceMask, FixedRate(), LookAhead(1)}, Binding{"AlphanumericMask", AlphanumericMask, FixedRate(), LookAhead(1)}},
                  {Binding{"ResultBoundaries", ResultBoundaries}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * boundaries = getInputStreamSet("TokenBoundaries")[0];
        PabloAST * whitespace = getInputStreamSet("WhitespaceMask")[0];
        PabloAST * alphanumeric = getInputStreamSet("AlphanumericMask")[0];
        
        // Detect punctuation: non-alphanumeric AND non-whitespace
        PabloAST * notAlphanumeric = pb.createNot(pb.createLookahead(alphanumeric, 1));

        PabloAST * notWhitespace = pb.createNot(pb.createLookahead(whitespace, 1));
        PabloAST * isPunctuation = pb.createAnd(notAlphanumeric, notWhitespace);

        // Detect punctuation that follows alphanumeric - these boundaries should be removed
        PabloAST * punctAfterAlpha = pb.createAnd(isPunctuation, alphanumeric);
        
        // Filter out boundaries at punctuation positions following alphanumeric
        PabloAST * filteredBoundaries = pb.createAnd(boundaries, pb.createNot(punctAfterAlpha));
       
        writeOutputStreamSet("ResultBoundaries", std::vector<PabloAST*>{filteredBoundaries});
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

        SHOW_STREAM(WordBoundaries);

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

        SHOW_STREAM(WordBoundaries);
    }
    //Input: "Hello there!"
    //Output: "Hello", "there", "!"   
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

        SHOW_STREAM(WordBoundaries);
    }
    //Input: "Hello there!"
    //Output: "Hello", "there!"
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

        SHOW_STREAM(WordBoundaries);
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

    SHOW_STREAM(WordBoundaries);

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

    SHOW_STREAM(WordBoundaries);

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

    // Detect whitespace/delimiter positions
    StreamSet * WhitespaceMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<WhitespaceDetector>(BasisBits, WhitespaceMask);
    SHOW_STREAM(WhitespaceMask);

    // Detect alphanumeric positions (for contiguous behavior to keep punctuation with words)
    StreamSet * AlphanumericMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<AlphanumericDetector>(BasisBits, AlphanumericMask);
    SHOW_STREAM(AlphanumericMask);

    // Apply behavior transformation using appropriate kernel
    StreamSet * TransformedBoundaries = P.CreateStreamSet(1, 1);
    
    if (SplitBehavior == "removed") {
        P.CreateKernelCall<RemovedBehavior>(TokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (SplitBehavior == "isolated") {
        P.CreateKernelCall<IsolatedBehavior>(TokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (SplitBehavior == "mergedwithprevious") {
        P.CreateKernelCall<MergedWithPreviousBehavior>(TokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (SplitBehavior == "mergedwithnext") {
        P.CreateKernelCall<MergedWithNextBehavior>(TokenBoundaries, WhitespaceMask, TransformedBoundaries);
    } else if (SplitBehavior == "contiguous") {
        P.CreateKernelCall<ContiguousBehavior>(TokenBoundaries, WhitespaceMask, AlphanumericMask, TransformedBoundaries);
    } else {
        // Default to isolated if behavior not specified
        P.CreateKernelCall<IsolatedBehavior>(TokenBoundaries, WhitespaceMask, TransformedBoundaries);
    }
    
    SHOW_STREAM(TransformedBoundaries);

    // Create insertion mask and spread original data
    StreamSet * lineInsertMask = UnitInsertionSpreadMask(P, TransformedBoundaries, kernel::InsertPosition::Before);
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
