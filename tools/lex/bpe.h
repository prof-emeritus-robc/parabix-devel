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

// All vocab tokens of one specific byte length L, within one pass. One
// BPETokenDetect kernel is instantiated per (pass, LengthGroup). Overlap
// resolution is NOT here — it is pass/length run order + the consumed mask
// (BPEMaskGate / BPEOccupy).
struct LengthGroup {
    unsigned length;                                       // L in bytes
    std::vector<std::pair<std::string, unsigned>> tokens;  // (token bytes, vocabID), all of size L
};

// One pass of the pass design: the tokens placed in this priority tier, grouped
// by byte length (descending) so within-pass kernels run longest → shortest.
struct VocabPass {
    std::vector<LengthGroup> byLength;
};

class BPETokenizer {
public:
    bool loadVocab(const std::string & path);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    std::string decodeToken(int id) const;

    // byte value -> vocab id of its single-byte token (-1 if none). Drives the
    // BPEByteFallback stage that fills bytes left uncovered by length>=2 tokens.
    std::vector<int> singleByteIds() const;

    // Partition the vocab into conflict-ordered PASSES (the pass design).
    //
    // Priority = LOWER vocab id wins. Two words "can overlap" if their match
    // spans could intersect in SOME input (one is a substring of the other, or
    // a suffix of one equals a prefix of the other — a purely static, input-free
    // relation). A word is placed in the current pass iff NO higher-priority
    // (lower-id) word it can overlap remains; otherwise it is deferred to the
    // next pass. Repeat on the leftovers until none remain.
    //
    // Result: passes[p].byLength holds that pass's tokens grouped by byte length
    // (so kernels can run longest→shortest within a pass). Pass order encodes
    // priority; within a pass no two words can be contested by a higher-priority
    // word, so resolution is handled entirely by run order + a consumed-byte
    // mask — the kernels themselves carry NO overlap logic.
    std::vector<VocabPass> buildVocabPasses() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// buildBPEPassPipeline
//   Builds the pass-design BPE pipeline (direct from the pseudocode):
//     - buildVocabPasses() partitions the vocab into priority passes VP[p][i]
//       (preprocessing; lower vocab id = higher priority).
//     - For each pass p, length i (max..2): BPETokenDetect → BPEMaskGate (drop
//       matches whose span hits an already-consumed byte) → BPEOccupy (mark the
//       surviving span consumed). The consumed mask chains kernel→kernel, so
//       overlaps are resolved purely by pass/length order — kernels carry no
//       overlap logic.
//     - BPEFinalOr ORs every pass's survivors into the final (matchEnd, vocabID).
//   Single bytes (length 1) are not partitioned (byte-fallback is a TODO).
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
