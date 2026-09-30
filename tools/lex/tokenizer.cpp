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
#include <pablo/pablo.h>
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
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <cstring>
#include <vector>
#include <algorithm>
#include <mutex>
#include <sys/stat.h>
#include <sys/mman.h>
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
// --bench-loop: when true the emit callback drops its output so timing loops
// measure pure tokenization, not millions of stdout writes.
static bool                  gBenchQuiet    = false;

// ─── --offsets: per-token source spans (HuggingFace `Encoding.offsets`) ───────
//
// No extra pipeline stage is needed. GPT-2 byte-level tokens TILE the input
// exactly — every input byte belongs to exactly one token, no gaps, no overlaps
// (verified: sum of token byte-lengths == input length, multibyte included). So a
// token's start offset is just the running sum of the byte-lengths of the tokens
// before it, and the scan callback already fires once per token in input order.
//
// A token's byte length is the CODEPOINT COUNT of its display string, not that
// string's size(): byte-level remapping is a byte↔codepoint bijection, so "Ġ" is
// 2 display bytes (0xC4 0xA0) but 1 source byte.
//
// byte mode reports raw byte spans. char mode reproduces HuggingFace exactly,
// which means two extra rules:
//   * offsets are CHARACTER indices, not byte indices;
//   * a token that covers only part of a character reports that whole
//     character's span. So the 3 bytes of "日" split across two tokens make BOTH
//     tokens report (0,1) — offsets are NOT a partition in char space and may
//     overlap or nest.
enum OffsetMode { OffNone, OffByte, OffChar };
static OffsetMode            gOffsetMode    = OffNone;
static const char *          gBuf           = nullptr;   // the mmap'd input
static size_t                gBufLen        = 0;
static size_t                gPos           = 0;   // running byte offset (next token's start)
static size_t                gLeads         = 0;   // # lead bytes in gBuf[0, gPos)
static bool                  gOffsetWarned  = false;

static inline bool isContByte(unsigned char c) { return (c & 0xC0) == 0x80; }

// Walk the cursor forward to byte p, maintaining gLeads = lead bytes in [0, p).
// Monotonic: tokens arrive in input order, so this is O(total bytes) overall.
static inline void offsetAdvanceTo(size_t p) {
    while (gPos < p && gPos < gBufLen) {
        if (!isContByte((unsigned char) gBuf[gPos])) ++gLeads;
        ++gPos;
    }
}

// Index of the character CONTAINING byte p. gLeads counts lead bytes strictly
// before p, so a lead byte at p starts character gLeads, while a continuation
// byte at p belongs to the character that started earlier — gLeads - 1.
static inline size_t charContaining(size_t p) {
    offsetAdvanceTo(p);
    if (p < gBufLen && isContByte((unsigned char) gBuf[p]))
        return gLeads > 0 ? gLeads - 1 : 0;
    return gLeads;
}

static void resetOffsetState() { gPos = 0; gLeads = 0; }

// bpe_emit_token (below) is registered as a scan::Reader callback, invoked from
// the compiled pipeline's own segment-processing threads -- potentially several
// of them concurrently, one per segment in flight. emitToken mutates plain
// global state (gPos, gLeads, gOffsetWarned) and writes to the shared
// llvm::outs()/errs() streams with no synchronization of its own, so every call
// (including the special-token splice in runBPEWithSpecialTokens, which runs on
// the main thread) must serialize through this mutex.
static std::mutex gEmitMutex;

// Shared print/offset logic for one token (id + its decoded display string).
// Used both by the scan callback (bpe_emit_token, below) and by the special-
// token splice in runBPEWithSpecialTokens (<|endoftext|> is spliced directly
// into the output, never seen by the compiled pipeline).
static void emitToken(uint16_t id, const std::string & tokStr) {
    if (gBenchQuiet) return;   // timing loop: skip output, measure tokenization only

    std::lock_guard<std::mutex> lock(gEmitMutex);

    if (gOffsetMode != OffNone) {
        // Source byte length = codepoint count of the display string.
        size_t len = 0;
        for (unsigned char c : tokStr) if (!isContByte(c)) ++len;
        if (len == 0 && !gOffsetWarned) {
            gOffsetWarned = true;
            llvm::errs() << "Warning: --offsets saw id " << id
                         << " with no vocab entry; spans after this point will drift.\n";
        }
        const size_t sByte = gPos;                 // this token starts where the last ended
        const size_t eByte = sByte + len;
        size_t s, e;
        if (gOffsetMode == OffByte) {
            s = sByte; e = eByte;
            offsetAdvanceTo(eByte);                // keep the cursor in step
        } else {
            // Expand outward to whole characters, matching HuggingFace.
            s = charContaining(sByte);
            e = (eByte > sByte) ? charContaining(eByte - 1) + 1 : s;
            offsetAdvanceTo(eByte);
        }
        if (gOutputStrings)
            llvm::outs() << id << '\t' << s << '\t' << e << '\t' << tokStr << "\n";
        else
            llvm::outs() << id << '\t' << s << '\t' << e << "\n";
        return;
    }

    if (gOutputStrings && gBPE)
        llvm::outs() << id << '\t' << tokStr << "\n";
    else
        llvm::outs() << id << "\n";
}

// bpe_emit_token
// Called once per surviving BPE token by the scan::Reader stage.
// id_ptr points into the 16-bit vocab-ID stream at the match-end byte
// position. The reader passes a single source pointer indexed by the
// match-end byte offset, so we just deref to recover the full ID.
extern "C" void bpe_emit_token(const uint16_t * id_ptr) {
    if (gBenchQuiet) return;   // timing loop: skip output, measure tokenization only
    uint16_t id = *id_ptr;
    if (getenv("BPE_EMIT_DBG")) {
        const uint8_t * bp = reinterpret_cast<const uint8_t *>(id_ptr);
        std::fprintf(stderr, "[emit] id=%u  lo=%u hi=%u  ptr=%p\n",
                     (unsigned)id, (unsigned)bp[0], (unsigned)bp[1], (const void*)id_ptr);
    }
    const std::string tokStr = gBPE ? gBPE->decodeToken(static_cast<int>(id)) : std::string();
    emitToken(id, tokStr);
}

// ─── Special-token bypass: "<|endoftext|>" ─────────────────────────────────
// GPT-2's tokenizer.json has exactly one `added_tokens` entry: "<|endoftext|>",
// id 50256. HuggingFace's AddedVocabulary matches it as a literal string BEFORE
// byte-level BPE runs, splitting it out of the text so ordinary merges never
// see it. Reproduced here at the host level — no pipeline/kernel changes —
// since it's a one-off literal match, not worth a dedicated Pablo kernel:
// find each occurrence, run the compiled BPE pipeline independently on the
// plain-text segments around it, and splice the fixed id in between.
static constexpr char     kSpecialTokenText[] = "<|endoftext|>";
static constexpr size_t   kSpecialTokenLen    = sizeof(kSpecialTokenText) - 1;  // 13
static constexpr uint16_t kSpecialTokenId     = 50256;

static void emitSpecialToken() {
    static const std::string tokStr(kSpecialTokenText);
    emitToken(kSpecialTokenId, tokStr);
}

// MemorySourceKernel now copies its input into its own safely-padded, aligned
// buffer during initialization (see source_kernel.cpp), so a mid-file segment's
// raw pointer (buf + offset) -- of arbitrary alignment, with no overflow past
// len -- can simply be passed straight through.
using BPEFnPtr = void (*)(const char *, size_t);
static void runBPESegment(BPEFnPtr bpeFn, const char * data, size_t len) {
    if (len == 0) return;
    bpeFn(data, len);
}

static void runBPEWithSpecialTokens(BPEFnPtr bpeFn, const char * buf, size_t nbytes) {
    std::string_view text(buf, nbytes);
    size_t pos = 0;
    for (;;) {
        size_t hit = text.find(kSpecialTokenText, pos);
        if (hit == std::string_view::npos) {
            runBPESegment(bpeFn, buf + pos, nbytes - pos);
            return;
        }
        if (hit > pos) runBPESegment(bpeFn, buf + pos, hit - pos);
        emitSpecialToken();
        pos = hit + kSpecialTokenLen;
    }
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

static cl::opt<unsigned> MergesLimit(
    "merges-limit",
    cl::desc("BPE mode: use only the first N merges by rank (0 = all). "
             "Fewer merges = smaller kernels = faster JIT, for size sweeps."),
    cl::init(0),
    cl::cat(wordBreakerFlags));

static cl::opt<bool> StripNewlines(
    "strip-newlines",
    cl::desc("BPE mode, no --pretokenizer: drop '\\n' bytes as pretoken separators "
             "(legacy line-delimited input). Default OFF: '\\n' is real content and "
             "seeds to id 198 (GPT-2 'Ċ'), matching HuggingFace byte-level BPE."),
    cl::init(false),
    cl::cat(wordBreakerFlags));

static cl::opt<OffsetMode> OffsetsFlag(
    "offsets",
    cl::desc("BPE mode: also print each token's span in the source:"),
    cl::values(
        clEnumValN(OffNone, "none", "No spans (default)"),
        clEnumValN(OffByte, "byte", "Raw BYTE spans [start,end)"),
        clEnumValN(OffChar, "char", "CHARACTER spans, matching HuggingFace Encoding.offsets "
                                    "(a token splitting a character reports that whole character)")),
    cl::init(OffNone),
    cl::cat(wordBreakerFlags));

static cl::opt<unsigned> BenchLoop(
    "bench-loop",
    cl::desc("BPE mode: tokenize the input N times inside one process (excludes OS "
             "process spawn + merges load + pipeline build/JIT), suppress token "
             "output, and print machine-readable timing to stderr. 0 = off (normal run)."),
    cl::init(0),
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
// The pipeline consumes an in-memory buffer (pointer + byte count) rather than a
// file descriptor, so the caller can load the file ONCE and invoke the compiled
// function repeatedly over the resident bytes with zero per-call I/O — matching
// HuggingFace's benchmark, which tokenizes an already-loaded in-memory string.
using BPEPipelineFunctionType = void (*)(const char * buffer, size_t length);

static BPEPipelineFunctionType buildBPEPipeline(
        CPUDriver & driver,
        const BPETokenizer & bpe) {

    auto __tBuild0 = std::chrono::steady_clock::now();
    auto P = CreatePipeline(driver,
                            Input<const char*>{"buffer"}, Input<size_t>{"length"});
    Scalar * const buffer = P.getInputScalar("buffer");
    Scalar * const length = P.getInputScalar("length");

    // Stage 0: in-memory source — no fd, no per-call read(). The caller loads the
    // file once (see main) and passes the resident pointer + length each call.
    StreamSet * ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<MemorySourceKernel>(buffer, length, ByteStream);
    StreamSet * BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);

    // Stage 1: normalization → U21
    // Skip the UTF8<->U21 round-trip entirely when there's no active
    // normalization and no --pretokenizer flag (the only two consumers of
    // U21codepoints below) — otherwise it's a pure-overhead identity
    // transform (UTF8_Decoder + UTF8_index + U21_to_UTF8, all for nothing).
    std::vector<NormalizationMode> normModes(Normalization.begin(), Normalization.end());
    bool hasActiveNorm = false;
    for (auto m : normModes) if (m != NormNone) { hasActiveNorm = true; break; }
    StreamSet * U21codepoints = nullptr;
    StreamSet * normalizedBasis = BasisBits;
    if (hasActiveNorm || PreTokenizer.getNumOccurrences() != 0) {
        U21codepoints = applyNormalizationU21(P, BasisBits, normModes);
        normalizedBasis = P.CreateStreamSet(8, 1);
        U21_to_UTF8(P, U21codepoints, normalizedBasis);
    }

    // Stage 2: pre-tokenization — produces the byte stream that feeds the
    // byte-mode BPE trie. Two paths:
    //   (a) --pretokenizer flag was given  → buildPreTokenizerBoundaries
    //       (U21-domain regex split) → U21_to_UTF8 → byte basis.
    //   (b) no --pretokenizer flag in BPE mode → inline newline mode
    //       (input is one-pretoken-per-line bytelevel text from
    //        compare_bpe.py step 1; 0x0A bytes mark separators).
    StreamSet * bpeBasis;
    StreamSet * bpeBoundary = nullptr;   // pretoken-start mask fed to BPE (gates merges)
    if (PreTokenizer.getNumOccurrences() == 0) {
        // No pretokenizer. By default treat '\n' as real content — it seeds to
        // id 198 (GPT-2 'Ċ') and flows through the merges, matching HuggingFace
        // byte-level BPE. --strip-newlines restores the legacy behavior where
        // '\n' is only a pretoken separator and is filtered out.
        bpeBasis = StripNewlines ? buildLinePretokens(P, normalizedBasis)
                                 : normalizedBasis;
    } else if (PreTokenizer == bytelevel) {
        // Boundary-gated BPE: the GPT-2 bytelevel pretokenizer's job here is only to
        // mark pretoken STARTS. Feed BPE the RAW bytes (BPERangeSeed does bytes_to_unicode
        // ONCE — using ptResult's Ġ-remapped bytes would double-encode) plus the
        // byte-domain boundary mask (ptResult.U21tokenBoundaries, already 1-per-byte for
        // bytelevel) so merges can't cross a pretoken boundary .
        PreTokenizerResult ptResult = buildPreTokenizerBoundaries(
            P, normalizedBasis, U21codepoints,
            PreTokenizer, SplitBehavior, DelimiterString);
        bpeBasis    = normalizedBasis;
        bpeBoundary = ptResult.U21tokenBoundaries;    // clean class-transition pretoken
                                                       // starts (GPT2PretokenBoundaryKernel),
                                                       // byte-domain, aligned to normalizedBasis
    } else {
        PreTokenizerResult ptResult = buildPreTokenizerBoundaries(
            P, normalizedBasis, U21codepoints,
            PreTokenizer, SplitBehavior, DelimiterString);
        // Finalize like the non-BPE path (wordBreakerPipeline): the raw
        // ptResult.U21codepoints is an intermediate — applyTokenSeparatorInsertion
        // spreads/filters it into the usable stream. Feeding the raw stream to
        // U21_to_UTF8 produced an empty basis (0 BPE tokens).
        StreamSet * finalU21 = applyTokenSeparatorInsertion(P, ptResult);
        bpeBasis = P.CreateStreamSet(8, 1);
        U21_to_UTF8(P, finalU21, bpeBasis);
    }

    // Stage 3: bucket-fold pipeline — single-byte vocab kernel + per
    // (b0, b1) trie kernels, all OR-merged into a single (matchEnd, vocabID)
    // pair via BPETriePairMergeKernel.
    BPEPassResult tr = buildBPEPassPipeline(P, bpeBasis, bpe, bpeBoundary);
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
    P.CreateKernelCall<ScanIndexGenerator>(matchEnd, scanIndices);  //  picks positions to print

    scan::Reader(P, driver, SCAN_CALLBACK(bpe_emit_token),  
                 idBytes16, scanIndices);   // emit token IDs at every matchEnd position 

    auto __tBuild1 = std::chrono::steady_clock::now();
    auto fn = reinterpret_cast<BPEPipelineFunctionType>(P.compile());
    auto __tCompile1 = std::chrono::steady_clock::now();

    std::cerr << "[BPE] pipeline graph build: "
              << std::chrono::duration<double, std::milli>(__tBuild1 - __tBuild0).count()
              << " ms (includes buildMergeRuleRanges above)\n";
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

    // BPE pipeline mode — enabled when --vocab OR --merges is supplied. merges.txt
    // is self-sufficient (it generates the 256 base tokens internally), so
    // --merges alone works without vocab.json.
    if (!VocabFile.empty() || !MergesFile.empty()) {
        BPETokenizer bpe;
        // Optional explicit vocab.json (legacy / id cross-check).
        if (!VocabFile.empty() && !bpe.loadVocab(VocabFile, MergesLimit))
            return 1;
        // --merges: build the priority ranges from merge rank (lower rank wins).
        // Seeds the base alphabet for the single-byte fallback + --strings decode.
        if (!MergesFile.empty() && !bpe.loadMerges(MergesFile, MergesLimit))
            return 1;
        if (!bpe.isLoaded()) {
            std::cerr << "BPE: no tokens loaded from --vocab/--merges\n";
            return 1;
        }

        gBPE           = &bpe;
        gOutputStrings = OutputStrings;
        gOffsetMode    = OffsetsFlag;

        // --offsets reports spans into the byte stream the BPE stage consumed, so
        // anything that ADDS or REMOVES bytes relative to the mmap'd file breaks the
        // tiling the running sum depends on.
        if (gOffsetMode != OffNone && StripNewlines) {
            llvm::errs() << "Error: --offsets cannot be combined with --strip-newlines "
                            "(dropping '\\n' bytes breaks the token/byte tiling spans rely on).\n";
            return 1;
        }
        if (gOffsetMode != OffNone) {
            bool activeNorm = false;
            for (auto m : Normalization) if (m != NormNone) { activeNorm = true; break; }
            if (activeNorm)
                llvm::errs() << "Warning: --offsets with --normalize reports spans in the "
                                "NORMALIZED text, which may not align with the input file.\n";
        }

        CPUDriver driver("bpe_tokenizer");
        BPEPipelineFunctionType bpeFn = buildBPEPipeline(driver, bpe);

        const int fd = open(inputFile.c_str(), O_RDONLY);
        if (LLVM_UNLIKELY(fd == -1)) {
            llvm::errs() << "Error: cannot open " << inputFile << " for processing.\n";
            return 1;
        }

        // Load the whole file ONCE into memory (before any timing). mmap gives a
        // page-aligned pointer, which satisfies MemorySourceKernel's alignment
        // requirement and is zero-copy. Every pipeline call reads these resident
        // bytes with no fd access — so --bench-loop times pure tokenization, and
        // file I/O is excluded from the timed region exactly as HuggingFace does.
        struct stat st;
        const size_t nbytes = (fstat(fd, &st) == 0) ? (size_t)st.st_size : 0;
        void * mapping = MAP_FAILED;
        const char * buf = nullptr;
        if (nbytes > 0) {
            mapping = mmap(nullptr, nbytes, PROT_READ, MAP_PRIVATE, fd, 0);
            if (mapping == MAP_FAILED) {
                llvm::errs() << "Error: cannot mmap " << inputFile << ".\n";
                close(fd);
                return 1;
            }
            buf = static_cast<const char *>(mapping);
        }
        // Empty-file / degenerate case: MemorySourceKernel asserts the buffer is
        // 64-byte aligned even when length == 0, so hand it an aligned dummy.
        alignas(64) static const char emptyBuf[64] = {0};
        if (buf == nullptr) buf = emptyBuf;

        // --offsets: point the span cursor at the same resident bytes the pipeline
        // reads, so char mode can tell lead bytes from continuation bytes.
        gBuf    = buf;
        gBufLen = nbytes;

        // --bench-loop=N: run the (already built + JIT-compiled) pipeline N times
        // in this one process over the resident buffer. Pipeline build AND file
        // I/O are both excluded, so we measure pure tokenization throughput — the
        // fair counterpart to HuggingFace's in-process criterion loop. Token
        // emission is suppressed via gBenchQuiet.
        if (BenchLoop > 0) {
            gBenchQuiet = true;
            const unsigned N = BenchLoop;

            // One discarded warm-up run (first pass pays page-fault / cache-fill cost).
            resetOffsetState();
            bpeFn(buf, nbytes);

            std::vector<double> ms;   // per-iteration wall time, milliseconds
            ms.reserve(N);
            for (unsigned i = 0; i < N; ++i) {
                resetOffsetState();   // spans are cumulative — rewind per iteration
                auto t0 = std::chrono::steady_clock::now();
                bpeFn(buf, nbytes);
                auto t1 = std::chrono::steady_clock::now();
                ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            if (mapping != MAP_FAILED) munmap(mapping, nbytes);
            close(fd);

            std::sort(ms.begin(), ms.end());
            const double minv    = ms.front();
            const double median  = ms[N / 2];
            double sum = 0.0; for (double v : ms) sum += v;
            const double mean    = sum / N;
            // Throughput on the best (min) run — the steady-state peak, like criterion.
            const double mbps    = (minv > 0.0) ? (nbytes / (minv / 1e3)) / 1e6 : 0.0;

            std::cerr << "[BENCH] iters=" << N << " bytes=" << nbytes
                      << " min=" << minv << "ms median=" << median << "ms mean=" << mean
                      << "ms  peak=" << mbps << " MB/s\n";
            // Single machine-readable line for bench_bpe.py to parse (grep BENCH_RESULT).
            std::fprintf(stderr,
                         "BENCH_RESULT bytes=%zu iters=%u min_ms=%.6f median_ms=%.6f "
                         "mean_ms=%.6f mbps=%.4f\n",
                         nbytes, N, minv, median, mean, mbps);
            return 0;
        }

        auto __tRun0 = std::chrono::steady_clock::now();
        resetOffsetState();
        runBPEWithSpecialTokens(bpeFn, buf, nbytes);
        auto __tRun1 = std::chrono::steady_clock::now();
        std::cerr << "[BPE] run (execute pipeline): "
                  << std::chrono::duration<double, std::milli>(__tRun1 - __tRun0).count()
                  << " ms\n";
        if (mapping != MAP_FAILED) munmap(mapping, nbytes);
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
