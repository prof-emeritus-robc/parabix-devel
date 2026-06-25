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
#include <chrono>
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
// id_ptr points into the 16-bit vocab-ID stream at the match-end byte
// position. The reader passes a single source pointer indexed by the
// match-end byte offset, so we just deref to recover the full ID.
extern "C" void bpe_emit_token(const uint16_t * id_ptr) {
    uint16_t id = *id_ptr;
    if (gOutputStrings && gBPE)
        llvm::outs() << gBPE->decodeToken(static_cast<int>(id)) << "\n";
    else
        llvm::outs() << id << "\n";
}

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
// Full parallel BPE pipeline. Takes a file descriptor, runs normalization +
// pre-tokenization + a vocab-trie longest-match scan entirely in SIMD stream
// kernels, and writes raw 16-bit token IDs (little-endian) to stdout.
//
// Stages:
//   Stage 0  I/O + S2P
//   Stage 1  Normalization     → U21codepoints
//   Stage 2  Pre-tokenization  → codepoint stream feeding BPE
//   Stage 3  buildBPEPassPipeline → (matchEnd, vocabID)  bucket fold of single-byte + 2+byte tries
//   Stage 4  Emit token IDs at every matchEnd position
//
// Phase-1: only trie matches (length >= 2) are emitted. Positions where no
// vocab word ended are silently skipped; single-codepoint fallback will be
// added in a later phase.
//
using BPEPipelineFunctionType = void (*)(uint32_t fd);

static BPEPipelineFunctionType buildBPEPipeline(
        CPUDriver & driver,
        const BPETokenizer & bpe) {

    auto __tBuild0 = std::chrono::steady_clock::now();
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

    // Stage 2: pre-tokenization — produces the byte stream that feeds the
    // byte-mode BPE trie. Two paths:
    //   (a) --pretokenizer flag was given  → buildPreTokenizerBoundaries
    //       (U21-domain regex split) → U21_to_UTF8 → byte basis.
    //   (b) no --pretokenizer flag in BPE mode → inline newline mode
    //       (input is one-pretoken-per-line bytelevel text from
    //        compare_bpe.py step 1; 0x0A bytes mark separators).
    StreamSet * bpeBasis;
    if (PreTokenizer.getNumOccurrences() == 0) {
        bpeBasis = buildLinePretokens(P, normalizedBasis);
    } else {
        PreTokenizerResult ptResult = buildPreTokenizerBoundaries(
            P, normalizedBasis, U21codepoints,
            PreTokenizer, SplitBehavior, DelimiterString);
        bpeBasis = P.CreateStreamSet(8, 1);
        U21_to_UTF8(P, ptResult.U21codepoints, bpeBasis);
    }

    // Stage 3: bucket-fold pipeline — single-byte vocab kernel + per
    // (b0, b1) trie kernels, all OR-merged into a single (matchEnd, vocabID)
    // pair via BPETriePairMergeKernel.
    BPEPassResult tr = buildBPEPassPipeline(P, bpeBasis, bpe);
    StreamSet * matchEnd = tr.matchEnd;
    StreamSet * vocabID  = tr.vocabID;

    // Stage 4: emit a token ID at every matchEnd position.
    //   1. Pack the full 16-bit vocabID BixNum into a single 16-bit-per-
    //      position stream via P2S16Kernel.
    //   2. matchEnd drives ScanIndexGenerator — one index per matched
    //      byte position.
    //   3. scan::Reader passes that byte position as the source pointer
    //      so bpe_emit_token sees the full ID in one deref. (The earlier
    //      design split lo/hi into two byte streams and read hi via the
    //      additionalStreams channel — that channel indexes by scan-
    //      iteration counter, not match-end byte position, which silently
    //      zeroed the high byte.)
    StreamSet * idBytes16 = P.CreateStreamSet(1, 16);
    P.CreateKernelCall<P2S16Kernel>(vocabID, idBytes16);

    StreamSet * scanIndices = P.CreateStreamSet(1, 64);
    P.CreateKernelCall<ScanIndexGenerator>(matchEnd, scanIndices);

    scan::Reader(P, driver, SCAN_CALLBACK(bpe_emit_token),
                 idBytes16, scanIndices);

    auto __tBuild1 = std::chrono::steady_clock::now();
    auto fn = reinterpret_cast<BPEPipelineFunctionType>(P.compile());
    auto __tCompile1 = std::chrono::steady_clock::now();

    std::cerr << "[BPE] pipeline graph build: "
              << std::chrono::duration<double, std::milli>(__tBuild1 - __tBuild0).count()
              << " ms (includes buildVocabRanges above)\n";
    std::cerr << "[BPE] P.compile() JIT+LLVM: "
              << std::chrono::duration<double, std::milli>(__tCompile1 - __tBuild1).count()
              << " ms\n";
    return fn;
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

    // BPE pipeline mode — enabled when --vocab is supplied. --merges is
    // accepted for CLI compatibility but ignored in the trie pipeline.
    if (!VocabFile.empty()) {
        BPETokenizer bpe;
        if (!bpe.loadVocab(VocabFile))
            return 1;

        if (!MergesFile.empty()) {
            std::cerr << "BPE: --merges is ignored in trie pipeline\n";
        }

        gBPE           = &bpe;
        gOutputStrings = OutputStrings;

        CPUDriver driver("bpe_tokenizer");
        BPEPipelineFunctionType bpeFn = buildBPEPipeline(driver, bpe);

        const int fd = open(inputFile.c_str(), O_RDONLY);
        if (LLVM_UNLIKELY(fd == -1)) {
            llvm::errs() << "Error: cannot open " << inputFile << " for processing.\n";
            return 1;
        }
        auto __tRun0 = std::chrono::steady_clock::now();
        bpeFn(fd);
        auto __tRun1 = std::chrono::steady_clock::now();
        std::cerr << "[BPE] run (execute pipeline): "
                  << std::chrono::duration<double, std::milli>(__tRun1 - __tRun0).count()
                  << " ms\n";
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
