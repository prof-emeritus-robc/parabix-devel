/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

// Forward declarations — callers using pipeline functions must include the
// full pipeline headers themselves.  bpe.h stays lightweight.
namespace kernel {
    class PipelineBuilder;
    class StreamSet;
}

// One BPE merge rule: (left, right) → merged.
struct MergeRule {
    unsigned leftID;    // vocab ID of left symbol
    unsigned rightID;   // vocab ID of right symbol
    unsigned mergedID;  // vocab ID of merged output
    int      rank;      // line number in merges.txt (tiebreaking)
};

// ── BPETokenizer ─────────────────────────────────────────────────────────────

class BPETokenizer {
public:
    // Load token→ID mapping from HuggingFace vocab.json.
    bool loadVocab(const std::string & path);

    // Load merge rules and stratify by depth for parallel execution.
    // mergesByDepth[d] holds all rules at tree depth d.  Rules at the same
    // depth are mutually independent — one BPEMergePassKernel pass handles all.
    bool loadMergesWithDepth(const std::string & path,
                             std::vector<std::vector<MergeRule>> & mergesByDepth);

    bool   isLoaded()  const { return !vocab_.empty(); }
    size_t vocabSize() const { return vocab_.size(); }

    // Map a single token ID back to its string representation.
    std::string decodeToken(int id) const;

    // Returns (codepoint_value, vocab_ID) for every single-character token.
    // Used by buildInitialSymID to construct the InitialSymIDKernel lookup table.
    std::vector<std::pair<unsigned,unsigned>> buildInitialVocabMap() const;

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::string>             idToToken_;
};

// ── Parabix pipeline integration ─────────────────────────────────────────────

// buildInitialSymID
//   Maps each U21 codepoint slot to its BPE vocab ID (16×1 BixNum output).
//   u21 must be the U21 codepoint stream (one slot per input character).
//   bpe must have vocab loaded before calling.
kernel::StreamSet * buildInitialSymID(
    kernel::PipelineBuilder  & P,
    kernel::StreamSet        * u21,
    const BPETokenizer       & bpe);

// runBPEPipeline
//   Wires D BPEMergePassKernel passes into the enclosing pipeline.
//   symID         16×1 BixNum — initial symbol IDs from buildInitialSymID.
//   ptBound       1×1 — 1 at every pre-token boundary.
//   mergesByDepth from BPETokenizer::loadMergesWithDepth.
//   Returns final compressed 16×1 BixNum of output token IDs.
kernel::StreamSet * runBPEPipeline(
    kernel::PipelineBuilder                       & P,
    kernel::StreamSet                             * symID,
    kernel::StreamSet                             * ptBound,
    const std::vector<std::vector<MergeRule>>     & mergesByDepth);
