/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kernel {
    class PipelineBuilder;
    class StreamSet;
}

// One pass of the pass-layered longest-match scheme. Each pass is realised by
// a single BPEPassKernel that detects every word in `tokens` and masks off the
// bytes it consumes. Passes are ordered so a word never lands in the same (or
// earlier) pass as a strictly longer word whose match could overlap it — see
// BPETokenizer::buildVocabPasses.
struct VocabPass {
    std::vector<std::pair<std::string, unsigned>> tokens;  // (token bytes, vocabID)
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // Layer the vocab into ordered passes for the aligned-mask longest-match
    // scan. A word w0 is placed in pass = 1 + max(pass of any strictly longer
    // word whose byte span can overlap w0), or pass 0 if no such word exists.
    // Result[0] holds the highest-priority (never-overridden) words; later
    // entries hold words that a longer overlapping word could consume first.
    std::vector<VocabPass> buildVocabPasses() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// buildBPEPassPipeline
//   Builds a pass-layered longest-match pipeline:
//     - buildVocabPasses() layers the vocab so a word never shares a pass with
//       (or precedes) a strictly longer word whose span could overlap it.
//     - One BPEPassKernel per pass. Each detects all its words against the
//       basis AND a carried `live` byte mask (a word fires only if every byte
//       it covers is still live), emits (matchEnd, vocabID), then clears the
//       consumed spans from `live` for the next pass.
//     - Because longer overlapping words always run in earlier passes and
//       consume their bytes, no later pass can re-fire inside them — the
//       per-position OR of all passes is the longest-match segmentation. No
//       LookAhead suppression stage is needed.
struct BPEPassResult {
    kernel::StreamSet * matchEnd;   //  1×1
    kernel::StreamSet * vocabID;    // 16×1 BixNum
};

BPEPassResult buildBPEPassPipeline(
    kernel::PipelineBuilder & P,
    kernel::StreamSet       * basis,
    const BPETokenizer      & bpe);

// Line-delimited pretokenizer used when --vocab is given without
// --pretokenizer (compare_bpe.py step-2 input format).
// Returns the basis byte stream with '\n' bytes removed via FilterByMask.
kernel::StreamSet * buildLinePretokens(
    kernel::PipelineBuilder & P,
    kernel::StreamSet       * basis);
