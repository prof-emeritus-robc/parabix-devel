/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kernel {
    class PipelineBuilder;
    class StreamSet;
}

// Vocabulary trie node — one entry per byte along a vocab word's spelling.
// vocabID >= 0 marks a complete vocab word ending at this trie depth.
struct TrieNode {
    std::map<uint8_t, TrieNode> children;
    int vocabID = -1;
};

// One bucket per distinct (b0, b1) two-byte prefix found in vocab.
// prefixVocabID holds the vocab ID when (b0, b1) IS itself a two-byte
// vocab word; root.children carry suffixes for vocab words of length >= 3
// bytes.
struct VocabBucket {
    uint8_t b0 = 0;
    uint8_t b1 = 0;
    int prefixVocabID = -1;
    TrieNode root;
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // (byte_value, vocab_ID) for every single-byte vocab token. Drives
    // BPESingleByteKernel (treated as a (single-byte) bucket alongside the
    // (b0, b1) trie buckets).
    std::vector<std::pair<unsigned,unsigned>> buildInitialVocabMap() const;

    // Bucketed trie covering vocab words of byte length >= 2.
    std::vector<VocabBucket> buildVocabBuckets() const;

    // Largest byte length of any token in the loaded vocab. Drives the
    // LookAhead window in BPEAssembleKernel — positions further away than
    // (max-1) bytes cannot be inside any vocab match's span.
    unsigned maxTokenByteLen() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// runBPETrie
//   Builds a uniform pipeline of "bucket" kernels:
//     - BPESingleByteKernel — emits (matchEnd, vocabID) for every 1-byte
//       vocab token.
//     - BPETrieKernel per (b0, b1) — emits (matchEnd, vocabID) for every
//       byte-length-≥2 vocab word in that bucket.
//   All bucket outputs are OR-folded pairwise via BPETriePairMergeKernel.
//
//   Phase-1 limitation: cross-bucket conflicts at the same end position OR
//   into a corrupted ID (no length tracking yet).
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
