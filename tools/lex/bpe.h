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

// One id-range for the range-kernel design (BPERangeKernel). Tokens are sorted
// by id ASC; maxLen drives the kernel's LookAhead binding.
struct RangeGroup {
    unsigned lo = 0, hi = 0;                                // id range [lo, hi)
    unsigned maxLen = 0;                                    // longest token byte length
    std::vector<std::pair<std::string, unsigned>> tokens;   // len>=2, sorted by id ASC
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // byte value -> vocab id of its single-byte token (-1 if none). Seeds the
    // range-kernel `source` so every unconsumed byte carries its single-byte id
    // (the byte-level fallback).
    std::vector<int> singleByteIds() const;

    // Partition the vocab into id-RANGES for the range-kernel design. One
    // BPERangeKernel handles one RangeGroup, kernels run lowest-id range first,
    // so lower id wins. Ranges are 256-wide over [256,1024) (the 256_511,
    // 512_767, 768_1023 kernels) then RANGE_WIDTH-wide above that. Only length>=2
    // tokens are partitioned (single bytes are seeded as the byte-level fallback).
    // tokens within a group are sorted by id ASC.
    std::vector<RangeGroup> buildVocabRanges() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// buildBPEPassPipeline
//   Builds the range-kernel BPE pipeline:
//     - buildVocabRanges() groups length>=2 tokens by id-range (lower id wins).
//     - BPERangeSeed seeds (source, active, end); one BPERangeKernel per range
//       (lowest first) detects, gates, and accumulates ids into `source`.
//     - BPEEmitTrigger derives matchEnd = active OR end.
//   Returns (matchEnd, vocabID=source). Single bytes are seeded as the fallback.
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
