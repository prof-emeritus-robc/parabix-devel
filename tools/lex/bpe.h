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

// Vocabulary trie node — one entry per codepoint along a vocab word's spelling.
// vocabID >= 0 marks a complete vocab word ending at this trie depth.
struct TrieNode {
    std::map<uint32_t, TrieNode> children;
    int vocabID = -1;
};

// One bucket per distinct (cp0, cp1) two-codepoint prefix found in vocab.
// prefixVocabID holds the vocab ID when (cp0, cp1) IS itself a two-codepoint
// vocab word; root.children carry suffixes for vocab words of length >= 3.
struct VocabBucket {
    uint32_t cp0 = 0;
    uint32_t cp1 = 0;
    int prefixVocabID = -1;
    TrieNode root;
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // Bucketed trie covering vocab words of length >= 2. One bucket per
    // distinct (cp0, cp1) prefix; each bucket carries a suffix trie for words
    // of length >= 3.
    std::vector<VocabBucket> buildVocabBuckets() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// runBPETrie
//   Builds one BPETrieKernel per (cp0, cp1) prefix bucket. Each kernel emits
//   a per-position matchEnd bit (1 at the last codepoint of any vocab word
//   that ended there) and a 16-bit vocabID BixNum (the matched token's ID
//   at matchEnd positions; 0 elsewhere). Within a single bucket, longer
//   matches override shorter ones at their own end position via nested
//   Pablo scopes. Across buckets, outputs are bitwise OR-merged pairwise.
//
//   Phase-1 limitation: cross-bucket conflicts at the same end position are
//   resolved by simple OR, NOT by longest-match. A separate resolution layer
//   will handle that.
struct BPETrieResult {
    kernel::StreamSet * matchEnd;   //  1×1
    kernel::StreamSet * vocabID;    // 16×1 BixNum
};

BPETrieResult runBPETrie(
    kernel::PipelineBuilder & P,
    kernel::StreamSet       * u21,
    const BPETokenizer      & bpe);

// Line-delimited pretokenizer used when --vocab is given without
// --pretokenizer (compare_bpe.py step-2 input format).
// Returns the u21 stream with '\n' codepoints removed via FilterByMask.
kernel::StreamSet * buildLinePretokens(
    kernel::PipelineBuilder & P,
    kernel::StreamSet       * u21);
