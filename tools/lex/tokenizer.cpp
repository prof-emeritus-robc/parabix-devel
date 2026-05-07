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

    //  two-step BPE mode 
    // Input file is already pre-tokenized
    // tokenizer --pretokenizer bytelevel input.txt > pretokens.txt
    // tokenizer --vocab vocab.json --merges merges.txt pretokens.txt
    if (!VocabFile.empty() && !MergesFile.empty()) {
        BPETokenizer bpe;
        if (!bpe.loadVocab(VocabFile) || !bpe.loadMerges(MergesFile))
            return 1;

        std::ifstream inFile(inputFile.c_str());
        if (!inFile.is_open()) {
            llvm::errs() << "Error: cannot open " << inputFile << "\n";
            return 1;
        }
        std::vector<std::string> preTokens;
        std::string line;
        while (std::getline(inFile, line))
            if (!line.empty()) preTokens.push_back(line);

        std::vector<int> ids = bpe.encodePreTokens(preTokens);
        for (int id : ids) {
            if (OutputStrings)
                llvm::outs() << bpe.decodeToken(id) << "\n";
            else
                llvm::outs() << id << "\n";
        }
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
