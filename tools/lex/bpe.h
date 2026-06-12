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

// All vocab tokens of one specific byte length L. One BPETokenDetect kernel
// is instantiated per LengthGroup. The kernels are INDEPENDENT — each reads
// only `basis`, never another kernel's output. All overlap resolution
// (cross-length AND intra-length) happens in BPELengthResolve, downstream.
struct LengthGroup {
    unsigned length;                                       // L in bytes
    std::vector<std::pair<std::string, unsigned>> tokens;  // (token bytes, vocabID), all of size L
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // Partition the vocab by token length. Result is sorted by length
    // ascending. Empty tokens are skipped. Each entry's `tokens` vector is
    // sorted by bytes for deterministic JIT cache keys.
    std::vector<LengthGroup> buildLengthGroups() const;

    // Largest token byte length in the vocab — sets the LookAhead window for
    // the BPEAssemble containment sweep.
    unsigned maxTokenByteLen() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// buildBPEPassPipeline
//   Builds a length-grouped, longest-wins BPE pipeline:
//     - buildLengthGroups() partitions the vocab by token length.
//     - One BPETokenDetect kernel per distinct length L. Each reads `basis`
//       and emits (matchEnd_L, vocabID_L) for every L-byte vocab word match.
//       Detection kernels are independent and run in parallel.
//     - BPELengthResolve takes every length's (matchEnd_L, vocabID_L) stream
//       and suppresses any match whose span overlaps a STRICTLY LONGER match.
//       Outputs the cleaned (matchEnd, vocabID) where the longest overlapping
//       match always wins.
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
