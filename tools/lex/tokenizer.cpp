/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#include <kernel/core/idisa_target.h>
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
#include <kernel/pipeline/driver/cpudriver.h>
#include <toolchain/toolchain.h>
#include <kernel/unicode/utf8_decoder.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/stream_select.h>
#include <kernel/scan/index_generator.h>
#include <kernel/scan/reader.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include "normalize.h"
#include "pretokenizer.h"
#include "bpe.h"

using namespace llvm;
using namespace codegen;
using namespace kernel;
using namespace pablo;

// Global BPE state accessed by the scan callback.
// Both are set in main() before invoking the compiled BPE pipeline function.
static const BPETokenizer * gBPE           = nullptr;
static bool                  gOutputStrings = false;

// bpe_emit_token
// Called once per surviving BPE token by the scan::Reader stage.
// lo_ptr points into the lo-byte stream at the token's compressed position;
// hi is the matching high byte of the 16-bit vocab ID.
// Reconstructs the full token ID and writes it (or its string) to stdout.
extern "C" void bpe_emit_token(const uint8_t * lo_ptr, uint8_t hi) {
    uint16_t id = static_cast<uint16_t>(*lo_ptr)
                | (static_cast<uint16_t>(hi) << 8);
    if (gOutputStrings && gBPE)
        llvm::outs() << gBPE->decodeToken(static_cast<int>(id)) << "\n";
    else
        llvm::outs() << id << "\n";
}

// AllTokenMarkKernel 
// Marks every in-file position with a 1.  In the compressed BPE output stream
// every surviving position is one complete token, so this gives one scan event
// per token to the downstream ScanIndexGenerator.
class AllTokenMarkKernel : public PabloKernel {
public:
    AllTokenMarkKernel(LLVMTypeSystemInterface & ts,
                       StreamSet * input,
                       StreamSet * marks)
    : PabloKernel(ts, "BPE_AllTokenMark",
                  {Binding{"input", input}},
                  {Binding{"marks", marks}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * allMarks = pb.createInFile(pb.createNot(pb.createZeroes()));
        Var * out = getOutputStreamVar("marks");
        pb.createAssign(pb.createExtract(out, pb.getInteger(0)), allMarks);
    }
};

static cl::OptionCategory wordBreakerFlags("Command Flags", "Unicode word breaker options");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required,
                                      cl::cat(wordBreakerFlags));

static cl::list<NormalizationMode> Normalization(
    "normalize",
    cl::desc("Unicode normalization(s) to apply before tokenizing. "
             "Comma-separate multiple modes to chain (Sequence), "
             "e.g. --normalize=nfd,lowercase :"),
    cl::CommaSeparated,
    cl::values(
        clEnumValN(NormNone,         "none",         "No normalization (default)"),
        clEnumValN(NormNFC,          "nfc",          "NFC: canonical decomposition + canonical composition"),
        clEnumValN(NormNFD,          "nfd",          "NFD: canonical decomposition"),
        clEnumValN(NormNFKC,         "nfkc",         "NFKC: compatibility decomposition + canonical composition"),
        clEnumValN(NormNFKD,         "nfkd",         "NFKD: compatibility decomposition"),
        clEnumValN(NormByteLevel,    "bytelevel",    "GPT-2 byte alphabet: map every byte to a unique printable Unicode char"),
        clEnumValN(NormStripAccents, "stripaccents", "Remove Mn (Mark, Nonspacing) codepoints — apply after NFD"),
        clEnumValN(NormStripLeft,    "stripleft",    "Remove leading Unicode whitespace (\\p{White_Space})"),
        clEnumValN(NormStripRight,   "stripright",   "Remove trailing Unicode whitespace (\\p{White_Space})"),
        clEnumValN(NormStrip,        "strip",        "Remove both leading and trailing Unicode whitespace"),
        clEnumValN(NormLowercase,    "lowercase",    "Map all uppercase codepoints to lowercase (SLC)"),
        clEnumValN(NormNmt,          "nmt",          "Google NMT preprocessing (control char cleanup, whitespace → space)"),
        clEnumValN(NormBertCleanText,   "bertcleantext",    "BERT clean_text: drop \\p{C} (except \\t\\n\\r) + U+FFFD; \\p{Zs}+\\t\\n\\r → U+0020"),
        clEnumValN(NormBertChineseChars,"bertchinesechars",  "BERT handle_chinese_chars: surround each CJK character with spaces")),
    cl::cat(wordBreakerFlags));

static cl::opt<PreTokenizerMode> PreTokenizer(
    "pretokenizer",
    cl::desc("Pre-tokenizer mode:"),
    cl::init(uax29),
    cl::values(
        clEnumValN(uax29,               "uax29",               "Unicode UAX#29 word boundaries (default)"),
        clEnumValN(whitespace,          "whitespace",          "Split on whitespace characters"),
        clEnumValN(whitespacesplit,     "whitespacesplit",     "Split on whitespace, output delimiters as separate tokens"),
        clEnumValN(digits,              "digits",              "Split on digit sequences"),
        clEnumValN(punctuation,         "punctuation",         "Split on punctuation characters"),
        clEnumValN(simpleWordBoundaries,"simplewordboundaries","Boundaries between \\w and \\W characters"),
        clEnumValN(bytelevel,           "bytelevel",           "ByteLevel tokenization: split on whitespace with byte remapping"),
        clEnumValN(chardelimiter,       "chardelimiter",       "Split on a specific character delimiter"),
        clEnumValN(bert,                "bert",                "BERT pre-tokenizer: separates punctuation and words"),
        clEnumValN(sequence_whitespace_punctuation, "sequence_whitespace_punctuation",
                   "Sequence pre-tokenizer: Whitespace then Punctuation")),
    cl::cat(wordBreakerFlags));

static cl::opt<SplitBehaviorMode> SplitBehavior(
    "behavior",
    cl::desc("Split delimiter behavior:"),
    cl::init(isolated),
    cl::values(
        clEnumValN(removed,          "removed",          "Only keep word/punctuation boundaries, exclude whitespace"),
        clEnumValN(isolated,         "isolated",         "Keep token boundaries AND add space boundaries"),
        clEnumValN(mergedwithprevious,"mergedwithprevious","Attach whitespace to previous word"),
        clEnumValN(mergedwithnext,   "mergedwithnext",   "Attach whitespace to next word"),
        clEnumValN(contiguous,       "contiguous",       "Keep punctuation with words, separate spaces")),
    cl::cat(wordBreakerFlags));

static cl::opt<std::string> DelimiterString(
    "delimiter",
    cl::desc("Delimiter character for --pretokenizer chardelimiter (default: ',')"),
    cl::init(","),
    cl::cat(wordBreakerFlags));

//  BPE options. 
// When both --vocab and --merges are provided, the tokenizer switches into BPE mode.
// merge loop and vocab lookup to emit token IDs.

static cl::opt<std::string> VocabFile(
    "vocab",
    cl::desc("Path to HuggingFace vocab.json  (enables BPE mode)"),
    cl::init(""),
    cl::cat(wordBreakerFlags));

static cl::opt<std::string> MergesFile(
    "merges",
    cl::desc("Path to HuggingFace merges.txt  (enables BPE mode)"),
    cl::init(""),
    cl::cat(wordBreakerFlags));

static cl::opt<bool> OutputStrings(
    "strings",
    cl::desc("BPE mode: print token strings instead of integer IDs"),
    cl::init(false),
    cl::cat(wordBreakerFlags));

using WordBreakerFunctionType = void (*)(uint32_t fd);

// writeToStdout — convert 8x1 parallel basis bits to serial bytes and write
// to stdout.  Extracted to avoid repeating the P2SKernel + StdOutKernel pair
// in the two output paths (normalization-only and tokenization).
static void writeToStdout(PipelineBuilder & P, StreamSet * basis) {
    StreamSet * output = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(basis, output);
    P.CreateKernelCall<StdOutKernel>(output);
}

//  bpePipeline 
//
// Full parallel BPE pipeline.  Takes a file descriptor, runs normalization +
// pre-tokenization + depth-stratified BPE merge passes entirely in SIMD
// stream kernels, and writes raw 16-bit token IDs (little-endian) to stdout.
//
// Building the pipeline:
//   Stage 0  I/O + S2P
//   Stage 1  Normalization    → U21codepoints  (one slot per input character)
//   Stage 2  Pre-tokenization → ptResult       (including U21tokenBoundaries)
//   Stage 3  buildInitialSymID                 → initialSymID (16×1 BixNum)
//   Stage 4  runBPEPipeline                    → finalSymID   (compressed)
//
// The bpe object must have both vocab and merges loaded before this is called.
// mergesByDepth is filled by BPETokenizer::loadMergesWithDepth.
// 
using BPEPipelineFunctionType = void (*)(uint32_t fd);

static BPEPipelineFunctionType buildBPEPipeline(
        CPUDriver & driver,
        const BPETokenizer & bpe,
        const std::vector<std::vector<MergeRule>> & mergesByDepth) {

    auto P = CreatePipeline(driver, Input<uint32_t>{"fileDescriptor"});
    Scalar * const fileDescriptor = P.getInputScalar("fileDescriptor");

    // Stage 0: I/O
    StreamSet * ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);
    StreamSet * BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);

    // Stage 1: normalization → U21
    std::vector<NormalizationMode> normModes(Normalization.begin(), Normalization.end());
    StreamSet * U21codepoints = applyNormalizationU21(P, BasisBits, normModes);
    StreamSet * normalizedBasis = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, U21codepoints, normalizedBasis);

    // Stage 2: pre-tokenization — produces ptBound + the codepoint stream
    // that feeds BPE.  Two paths:
    //   (a) --pretokenizer flag was given  → buildPreTokenizerBoundaries
    //       (full HF-equivalent pretokenizer pipeline from pretokenizer.cpp).
    //   (b) no --pretokenizer flag in BPE mode → inline newline mode
    //       (input is one-pretoken-per-line bytelevel text from
    //        compare_bpe.py step 1; '\n' marks separators).
    StreamSet * ptBound;
    StreamSet * bpeU21;
    if (PreTokenizer.getNumOccurrences() == 0) {
        // Path (b): inline newline-based pretokenizer.
        LinePretokensResult lr = buildLinePretokens(P, U21codepoints);
        bpeU21  = lr.u21Compressed;
        ptBound = lr.ptBoundCompressed;
    } else {
        // Path (a): existing pretokenizer pipeline.
        // U21tokenBoundaries[p] = 1 at the start of each new pre-token, so a
        // BPE merge at position p is blocked when U21tokenBoundaries[p+1] = 1.
        PreTokenizerResult ptResult = buildPreTokenizerBoundaries(
            P, normalizedBasis, U21codepoints,
            PreTokenizer, SplitBehavior, DelimiterString);
        ptBound = ptResult.U21tokenBoundaries;
        bpeU21  = ptResult.U21codepoints;
    }

    // Stage 3: map each U21 codepoint to its initial BPE vocab ID.
    // buildInitialSymID wraps InitialSymIDKernel which uses the (codepoint →
    // vocab_ID) table built by BPETokenizer::buildInitialVocabMap().
    StreamSet * initialSymID = buildInitialSymID(P, bpeU21, bpe);

    // Stage 4: D depth-level merge passes (Detect+Resolve per depth).
    // Each pass detects the lowest-rank firing rule per position, resolves
    // adjacent-merge conflicts by rank, then FilterByMask compresses out
    // consumed right-side positions.  After all passes, finalSymID holds
    // one slot per output token.
    StreamSet * finalSymID = runBPEPipeline(P, initialSymID, ptBound, mergesByDepth);

    // Stage 5: emit token IDs.
    //
    // finalSymID is a 16×1 BixNum (16 parallel 1-bit streams encoding a
    // 16-bit integer per position).  We need one scan callback per surviving
    // token.  The scan::Reader pattern requires:
    //   (a) a byte-width source stream (field width != 1)
    //   (b) a 64-bit index stream (from ScanIndexGenerator)
    //   (c) optional per-token data streams
    //
    // Strategy:
    //   1. Split the BixNum into bits 0-7 (lo) and bits 8-15 (hi).
    //   2. Pack each half to a byte stream via P2SKernel.
    //   3. Mark every compressed position as a token (AllTokenMarkKernel).
    //   4. ScanIndexGenerator → one index per token.
    //   5. scan::Reader calls bpe_emit_token(lo_ptr, hi_byte) per token.
    namespace su = kernel::streamutils;
    StreamSet * lo8basis = su::Select(P, finalSymID,
                               std::vector<uint32_t>{0,1,2,3,4,5,6,7});
    StreamSet * hi8basis = su::Select(P, finalSymID,
                               std::vector<uint32_t>{8,9,10,11,12,13,14,15});

    StreamSet * loBytes = P.CreateStreamSet(1, 8);
    StreamSet * hiBytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(lo8basis, loBytes);
    P.CreateKernelCall<P2SKernel>(hi8basis, hiBytes);

    StreamSet * tokenMarks = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<AllTokenMarkKernel>(loBytes, tokenMarks);

    StreamSet * scanIndices = P.CreateStreamSet(1, 64);
    P.CreateKernelCall<ScanIndexGenerator>(tokenMarks, scanIndices);

    scan::Reader(P, driver, SCAN_CALLBACK(bpe_emit_token),
                 loBytes, scanIndices, {hiBytes});

    return reinterpret_cast<BPEPipelineFunctionType>(P.compile());
}

WordBreakerFunctionType wordBreakerPipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>{"fileDescriptor"});

    Scalar * const fileDescriptor = P.getInputScalar("fileDescriptor");

    //  Stage 0: I/O — read file, convert to parallel bit streams 
    StreamSet * ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);

    StreamSet * BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);

    // Normalization
    // Using applyNormalizationU21 to get U21 codepoints directly
    std::vector<NormalizationMode> normModes(Normalization.begin(), Normalization.end());
    StreamSet * U21codepoints = applyNormalizationU21(P, BasisBits, normModes);

    // Re-encode to BasisBits once — needed by the pretokenizer's bytelevel mode
    // and by the normalization-only output path below.
    StreamSet * normalizedBasis = P.CreateStreamSet(8, 1);
    U21_to_UTF8(P, U21codepoints, normalizedBasis);

    // If any active normalization was requested but no --pretokenizer was given,
    // output the raw normalized text and stop
    bool hasActiveNorm = false;
    for (auto m : normModes) if (m != NormNone) { hasActiveNorm = true; break; }
    if (hasActiveNorm && PreTokenizer.getNumOccurrences() == 0) {
        writeToStdout(P, normalizedBasis);
        return reinterpret_cast<WordBreakerFunctionType>(P.compile());
    }

    //Pre-tokenization
    // U21codepoints is already decoded above — no re-decode needed here.
    PreTokenizerResult ptResult = buildPreTokenizerBoundaries(
        P, normalizedBasis, U21codepoints, PreTokenizer, SplitBehavior, DelimiterString);

    StreamSet * finalU21 = applyTokenSeparatorInsertion(P, ptResult);

    // Output — U21 → UTF-8 → serial bytes → stdout
    StreamSet * output_basis = P.CreateStreamSet(8);
    U21_to_UTF8(P, finalU21, output_basis);
    writeToStdout(P, output_basis);

    return reinterpret_cast<WordBreakerFunctionType>(P.compile());
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, 
        {&wordBreakerFlags, &codegen::JIT_InfoOptions, &codegen::InstrumentationOptions});

    // BPE pipeline mode — enabled when both --vocab and --merges are supplied.
    // Runs the full integrated pipeline: I/O → normalize → pre-tokenize →
    // initial symbol assignment → depth-stratified BPE merges → emit IDs.
    if (!VocabFile.empty() && !MergesFile.empty()) {
        BPETokenizer bpe;
        if (!bpe.loadVocab(VocabFile))
            return 1;

        std::vector<std::vector<MergeRule>> mergesByDepth;
        if (!bpe.loadMergesWithDepth(MergesFile, mergesByDepth))
            return 1;

        // Wire globals used by bpe_emit_token.
        gBPE           = &bpe;
        gOutputStrings = OutputStrings;

        CPUDriver driver("bpe_tokenizer");
        BPEPipelineFunctionType bpeFn = buildBPEPipeline(driver, bpe, mergesByDepth);

        const int fd = open(inputFile.c_str(), O_RDONLY);
        if (LLVM_UNLIKELY(fd == -1)) {
            llvm::errs() << "Error: cannot open " << inputFile << " for processing.\n";
            return 1;
        }
        bpeFn(fd);
        close(fd);
        return 0;
    }

    // Pre-tokenizer pipeline mode (existing behaviour) 
    CPUDriver driver("unicode_word_tokenizer");
    WordBreakerFunctionType wordBreakerFn = wordBreakerPipeline(driver);

    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing. Skipped.\n";
        return 1;
    }
    wordBreakerFn(fd);
    close(fd);
    return 0;
}
