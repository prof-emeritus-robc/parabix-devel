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

// Raw merge rule as read from merges.txt: the two already-formed parts A,B and
// the merged token id (== 256 + rank). Part ids/lengths are resolved later
// against the full vocab (base-byte parts like "y" live only in vocab.json).
struct MergeRaw {
    std::string a, b;        // the two merged parts (byte strings)
    unsigned    idAB = 0;    // id of A+B  (= 256 + rank)
};

// One resolved merge for the merge-loop kernel: detect idA-end, check idB at
// +lenB, stamp idAB at the merge end. lenB = byte length of B = Advance distance.
struct MergeRule {
    unsigned idA = 0, idB = 0, idAB = 0;
    unsigned lenA = 0, lenB = 0;        // byte lengths of the two parts
};

// One id-range group of merge rules, sorted by idAB ASC (= rank ASC). maxLen =
// longest merged-token byte length in the group.
struct MergeRuleGroup {
    unsigned lo = 0, hi = 0;
    unsigned maxLen = 0;
    std::vector<MergeRule> rules;
};

class BPETokenizer {
public:
    // `limit` (0 = all) caps the number of merges read to the first N by rank —
    // fewer rules = smaller kernels = faster JIT, for size sweeps / smoke tests.
    bool loadVocab(const std::string & path, unsigned limit = 0);

    // loadMerges reads a HuggingFace merges.txt. Each non-header line "A B"
    // defines a merge producing token AB (concat of the two already-formed
    // parts). The line index (0-based, after the #version header) is the merge
    // RANK = priority (lower rank wins). In GPT-2 the merged token's vocab id
    // == 256 + rank exactly, so we store id = 256 + rank and the rank-ordered
    // partition matches the vocab-id partition for length>=2 tokens.
    // `limit` (0 = all) keeps only the first N merges by rank.
    bool loadMerges(const std::string & path, unsigned limit = 0);

    bool   isLoaded()  const { return !vocab_.empty(); }
    bool   hasMerges() const { return !merges_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // Resolve each raw merge (parts A,B + idAB) into a MergeRule (idA, idB,
    // lenA, lenB, idAB) via the base alphabet + earlier merge outputs, then group
    // by idAB-range, sorted by idAB ASC = rank ASC. This is the data the
    // merge-loop kernel consumes: per rule it detects idA's end, checks idB at
    // +lenB, and stamps idAB. Rules whose parts are unresolvable are skipped
    // (with a count on stderr).
    std::vector<MergeRuleGroup> buildMergeRuleRanges() const;

private:
    // Generate the 256-entry GPT-2 byte-level base alphabet (bytes_to_unicode)
    // into vocab_/idToToken_. Base id == position in the alphabet (matches
    // vocab.json ids 0..255 exactly). Lets merges.txt be self-sufficient: every
    // merge part is a base byte or an earlier merge output → no vocab.json
    // required. (The seed computes base ids arithmetically, so no byte→id table.)
    void buildBaseAlphabet();

    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
    // Raw merges (parts A,B + idAB = 256 + rank), in rank order. Set by loadMerges.
    std::vector<MergeRaw>                merges_;
    bool                                 baseBuilt_ = false;  // buildBaseAlphabet run once
};

// buildBPEPassPipeline
//   Builds the merge-kernel BPE pipeline:
//     - buildMergeRuleRanges() groups merges into id-ranges (rank order).
//     - BPERangeSeed seeds source = base id of each raw byte (active=1s, end=0s).
//     - One BPEMergeKernel per range (lowest first) glues adjacent ids into
//       `source`, threaded kernel→kernel (the Ġthe→Ġthey cascade).
//   Returns (matchEnd=active, vocabID=source). Emission is Stage-B debug
//   (emit-all); correct emission (consume swallowed ends) is Stage C.
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
