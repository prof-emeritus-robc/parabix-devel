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

// Group of vocab tokens that all share a fixed byte length. The BPE pipeline
// uses one BPELengthKernel per non-empty group, processed in descending length
// order so first-write-wins gives the longest match at every end position.
struct VocabLengthGroup {
    unsigned length;
    std::vector<std::pair<std::string, unsigned>> tokens;  // (token bytes, vocabID)
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // Partition the vocab by token byte length. Returned groups are sorted
    // by length DESCENDING — the pipeline folds them in that order so the
    // first-wins merge surfaces the longest match at each end position.
    std::vector<VocabLengthGroup> buildVocabByLength() const;

    // Largest byte length of any token in the loaded vocab. Drives the
    // LookAhead window in BPEAssembleKernel — positions further away than
    // (max-1) bytes cannot be inside any vocab match's span.
    unsigned maxTokenByteLen() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// runBPETrie
//   Builds a flat per-length pipeline:
//     - One BPELengthKernel per distinct token byte length present in vocab.
//       Each emits (matchEnd, vocabID, matchLen=L-constant) for every vocab
//       word of that length that ends in the input.
//     - Length groups are folded by BPELengthFirstWinsMerge in DESCENDING
//       length order — first hit wins, so longer matches naturally outrank
//       shorter ones at the same end position with no UGE/UGT compare.
//     - A final BPEAssembleKernel uses LookAhead over matchLen to suppress
//       shorter matches that fall inside the span of a longer match ending
//       later in the stream.
struct BPETrieResult {
    kernel::StreamSet * matchEnd;   //  1×1
    kernel::StreamSet * vocabID;    // 16×1 BixNum
};

BPETrieResult runBPETrie(
    kernel::PipelineBuilder & P,
    kernel::StreamSet       * basis,
    const BPETokenizer      & bpe);

// Line-delimited pretokenizer used when --vocab is given without
// --pretokenizer (compare_bpe.py step-2 input format).
// Returns the basis byte stream with '\n' bytes removed via FilterByMask.
kernel::StreamSet * buildLinePretokens(
    kernel::PipelineBuilder & P,
    kernel::StreamSet       * basis);
