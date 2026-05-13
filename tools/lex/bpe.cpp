/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — depth-stratified parallel encoding.
 *
 *  Parallelism key insight
 *  ───────────────────────
 *  BPE merge rules form a DAG.  The merged token AB can only exist AFTER
 *  both A and B exist, so depth(AB) = 1 + max(depth(A), depth(B)).  Rules
 *  at the same depth are mutually independent and can be applied in a single
 *  SIMD pass (BPEMergePassKernel).
 *
 *  Pipeline structure per depth level d
 *  ─────────────────────────────────────
 *    BPEMergePassKernel(symID_d, ptBound_d)  →  newSymID_d + deleteMask_d
 *    InvertStreamKernel(deleteMask_d)          →  keepMask_d
 *    FilterByMask(keepMask_d, newSymID_d)      →  symID_{d+1}     (compressed)
 *    FilterByMask(keepMask_d, ptBound_d)       →  ptBound_{d+1}   (aligned)
 *
 *  Correctness invariants
 *  ──────────────────────
 *  I1. ptBound compressed alongside symID at every depth — positions stay
 *      aligned.  (Omitting this causes wrong boundary data from depth 1 on.)
 *  I2. Conflict resolution (left-priority) runs AFTER all rule scopes to
 *      see the union of all firing positions before suppression.
 *  I3. First-matching rule wins: alreadyMatched accumulator masks later
 *      rules exactly as the sequential `break` does.
 *  I4. BPEMergePassKernel name encodes depth — different rule sets per depth
 *      must not share a compiled-kernel cache entry.
 *
 *  if/scope tree (lib/kernel/util/linebreak_kernel.cpp pattern)
 *  ─────────────────────────────────────────────────────────────
 *  Outer guard if(canMerge) skips all rule work where merging is impossible
 *  (next position is a pre-token boundary).
 *  Per-rule inner guard if(leftMatch) skips right-match + mux when the left
 *  symbol doesn't match, cutting per-rule cost roughly in half for sparse
 *  inputs.  Mirrors UnicodeLinesKernelBuilder "if(u8pfx,it)" / "if(u8pfx2,it2)".
 */

#include "bpe.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>

using namespace pablo;
using namespace kernel;

// ============================================================================
//  BPEMergePassKernel
//
//  One parallel BPE merge pass at a single depth level.
//
//  Inputs
//  symID      16×1 BixNum — current symbol IDs, one slot per surviving token.
//  ptBound    1×1 — 1 at every pre-token boundary.  Merges cannot cross these.
//
//  Outputs
//  newSymID   16×1 BixNum — IDs after merges.  Left-side positions carry the
//             merged ID; all other positions are unchanged.
//  deleteMask 1×1 — 1 at every right-side position consumed by a merge.
//             Downstream FilterByMask removes these positions.
//
//  if/scope tree (linebreak pattern)
//  ──────────────────────────────────
//  Outer scope  if(canMerge)  — skip all work where merging is impossible.
//  Inner scope  if(leftMatch) — skip right-match + mux when left misses.
// ============================================================================
class BPEMergePassKernel : public PabloKernel {
public:
    BPEMergePassKernel(LLVMTypeSystemInterface & ts,
                       StreamSet * symID,
                       StreamSet * ptBound,
                       StreamSet * newSymID,
                       StreamSet * deleteMask,
                       const std::vector<MergeRule> & rules,
                       unsigned depth)
    : PabloKernel(ts,
                  // I4: encode depth in name to prevent cross-depth cache hits.
                  "BPEMergePass_D" + std::to_string(depth),
                  // LookAhead(1): kernel calls createLookahead(bit, 1) to read
                  // the right-neighbour symbol ID and next boundary flag.
                  {Binding{"symID",   symID,   FixedRate(), LookAhead(1)},
                   Binding{"ptBound", ptBound, FixedRate(), LookAhead(1)}},
                  {Binding{"newSymID",   newSymID},
                   Binding{"deleteMask", deleteMask}}),
      mRules(rules) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());

        std::vector<PabloAST*> symBits = getInputStreamSet("symID");
        PabloAST * const ptBound = getInputStreamSet("ptBound")[0];

        // rightBits[i][p] == symBits[i][p+1] — right-neighbour symbol bits.
        std::vector<PabloAST*> rightBits;
        rightBits.reserve(symBits.size());
        for (auto * bit : symBits)
            rightBits.push_back(pb.createLookahead(bit, 1));

        // canMerge[p] = 1 iff p+1 is NOT a pre-token boundary.
        PabloAST * canMerge = pb.createNot(pb.createLookahead(ptBound, 1));

        // Accumulators in outer scope — conflict resolution reads them after
        // all rule scopes have executed (I2).
        Var * mergeAt        = pb.createVar("mergeAt",        pb.createZeroes());
        Var * alreadyMatched = pb.createVar("alreadyMatched", pb.createZeroes());

        // desiredBits[i]: what bit i should be if this position fires.
        // Initialised to symBits[i] so non-fired positions are trivially correct
        // after the final mux.
        std::vector<Var*> desiredBits;
        desiredBits.reserve(symBits.size());
        for (size_t i = 0; i < symBits.size(); i++)
            desiredBits.push_back(pb.createVar(
                "desired_" + std::to_string(i), symBits[i]));

        // ── Outer guard: if(canMerge) ─────────────────────────────────────
        // Mirrors UnicodeLinesKernelBuilder "if (u8pfx, it)" outer scope.
        // Entire rule loop skipped at positions where merging is impossible.
        auto mergeScope = pb.createScope();
        pb.createIf(canMerge, mergeScope);

        for (const auto & rule : mRules) {

            // Left-symbol equality — runs in mergeScope (outer guard active).
            PabloAST * leftMatch = mergeScope.createNot(mergeScope.createZeroes());
            for (size_t i = 0; i < symBits.size(); i++) {
                unsigned wantBit = (rule.leftID >> i) & 1u;
                PabloAST * bitOk = wantBit ? symBits[i]
                                           : mergeScope.createNot(symBits[i]);
                leftMatch = mergeScope.createAnd(leftMatch, bitOk);
            }

            // ── Inner guard: if(leftMatch) ────────────────────────────────
            // Mirrors "if (u8pfx2, it2)" / "if (u8pfx3, it3)" inner scopes.
            // Right-match + mux skipped entirely when left symbol doesn't match.
            auto ruleScope = mergeScope.createScope();
            mergeScope.createIf(leftMatch, ruleScope);

            // Right-symbol equality — inside inner guard.
            PabloAST * rightMatch = ruleScope.createNot(ruleScope.createZeroes());
            for (size_t i = 0; i < rightBits.size(); i++) {
                unsigned wantBit = (rule.rightID >> i) & 1u;
                PabloAST * bitOk = wantBit ? rightBits[i]
                                           : ruleScope.createNot(rightBits[i]);
                rightMatch = ruleScope.createAnd(rightMatch, bitOk);
            }

            // fires[p] = rightMatch & ~alreadyMatched.
            // leftMatch and canMerge are guaranteed true by the enclosing
            // if-scope guards — no need to AND them explicitly.
            PabloAST * fires = ruleScope.createAnd(
                rightMatch, ruleScope.createNot(alreadyMatched));

            // First-match priority (I3): mark position so later rules skip it.
            ruleScope.createAssign(alreadyMatched,
                ruleScope.createOr(alreadyMatched, fires));
            ruleScope.createAssign(mergeAt,
                ruleScope.createOr(mergeAt, fires));

            // Record desired merged-ID bits at firing positions.
            // bit==1: OR fires in  /  bit==0: AND ~fires out
            for (size_t i = 0; i < symBits.size(); i++) {
                unsigned mergedBit = (rule.mergedID >> i) & 1u;
                PabloAST * updated = mergedBit
                    ? ruleScope.createOr (desiredBits[i], fires)
                    : ruleScope.createAnd(desiredBits[i],
                                          ruleScope.createNot(fires));
                ruleScope.createAssign(desiredBits[i], updated);
            }
        }

        // ── Conflict resolution (I2): left-priority ───────────────────────
        // If p-1 fired, p is the consumed right side → suppress p.
        //   finalFires = mergeAt & ~Advance(mergeAt, 1)
        PabloAST * finalFires = pb.createAnd(
            mergeAt, pb.createNot(pb.createAdvance(mergeAt, 1)));

        // ── Output symbol IDs ─────────────────────────────────────────────
        // Mux: use desiredBits where finalFires, else keep original symBits.
        // desiredBits was initialised to symBits, so this is correct even at
        // conflict-suppressed positions (desiredBits was modified but
        // finalFires==0 there → mux selects original symBits).
        PabloAST * notFinalFires = pb.createNot(finalFires);
        Var * symOut = getOutputStreamVar("newSymID");
        for (size_t i = 0; i < symBits.size(); i++) {
            pb.createAssign(
                pb.createExtract(symOut, pb.getInteger(i)),
                pb.createOr(pb.createAnd(desiredBits[i], finalFires),
                            pb.createAnd(symBits[i],     notFinalFires)));
        }

        // ── Delete mask ───────────────────────────────────────────────────
        // Position p+1 consumed when p fired: deleteMask = Advance(finalFires,1).
        Var * delOut = getOutputStreamVar("deleteMask");
        pb.createAssign(pb.createExtract(delOut, pb.getInteger(0)),
                        pb.createAdvance(finalFires, 1));
    }

private:
    std::vector<MergeRule> mRules;
};

// ============================================================================
//  InvertStreamKernel
//
//  Flips every bit in a 1-bit stream.
//  Converts deleteMask (1=remove) → keepMask (1=keep) for FilterByMask.
// ============================================================================
class InvertStreamKernel : public PabloKernel {
public:
    InvertStreamKernel(LLVMTypeSystemInterface & ts,
                       StreamSet * input,
                       StreamSet * output)
    : PabloKernel(ts, "BPE_InvertStream",
                  {Binding{"input",  input}},
                  {Binding{"output", output}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * in = getInputStreamSet("input")[0];
        writeOutputStreamSet("output", std::vector<PabloAST*>{pb.createNot(in)});
    }
};

// ============================================================================
//  InitialSymIDKernel
//
//  Maps each U21 codepoint slot to its BPE vocab ID (16-bit BixNum output).
//  Uses BixNumCompiler.EQ + createSel per single-character vocab entry.
//  Since codepoints are unique per slot, conditions are mutually exclusive.
//  Pattern from ztf-logic.cpp.
// ============================================================================
class InitialSymIDKernel : public PabloKernel {
public:
    InitialSymIDKernel(LLVMTypeSystemInterface & ts,
                       StreamSet * u21,
                       StreamSet * symID,
                       std::vector<std::pair<unsigned,unsigned>> cpToVocab)
    : PabloKernel(ts, "InitialSymID",
                  {Binding{"u21",   u21}},
                  {Binding{"symID", symID}}),
      mCpToVocab(std::move(cpToVocab)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        std::vector<PabloAST*> u21bits = getInputStreamSet("u21");
        BixNum codepoint(u21bits.begin(), u21bits.end());

        PabloAST * ones   = pb.createNot(pb.createZeroes());
        PabloAST * zeroes = pb.createZeroes();

        BixNum vocabID(16, zeroes);
        for (const auto & [cp, vid] : mCpToVocab) {
            PabloAST * match = bnc.EQ(codepoint, cp);
            for (unsigned i = 0; i < 16; i++) {
                unsigned bit = (vid >> i) & 1u;
                vocabID[i] = pb.createSel(match, bit ? ones : zeroes, vocabID[i]);
            }
        }

        Var * outVar = getOutputStreamVar("symID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(outVar, pb.getInteger(i)), vocabID[i]);
    }

private:
    std::vector<std::pair<unsigned,unsigned>> mCpToVocab;
};

// ============================================================================
//  buildInitialSymID
//  Wraps InitialSymIDKernel; returns 16×1 BixNum of initial vocab IDs.
// ============================================================================
kernel::StreamSet * buildInitialSymID(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * u21,
        const BPETokenizer & bpe) {
    auto cpToVocab = bpe.buildInitialVocabMap();
    StreamSet * symID = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<InitialSymIDKernel>(u21, symID, std::move(cpToVocab));
    return symID;
}

// ============================================================================
//  runBPEPipeline
//
//  Wires D BPEMergePassKernel calls into the pipeline, one per depth level.
//  Between passes, FilterByMask compresses out consumed right-side positions.
//  BOTH symID and ptBound are compressed so they stay position-aligned (I1).
//
//  Returns final compressed 16×1 BixNum — one slot per output token.
// ============================================================================
kernel::StreamSet * runBPEPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * symID,
        kernel::StreamSet * ptBound,
        const std::vector<std::vector<MergeRule>> & mergesByDepth) {

    unsigned depth = 0;
    for (const auto & rulesAtDepth : mergesByDepth) {
        if (rulesAtDepth.empty()) { depth++; continue; }

        StreamSet * newSymID   = P.CreateStreamSet(16, 1);
        StreamSet * deleteMask = P.CreateStreamSet(1,  1);
        P.CreateKernelCall<BPEMergePassKernel>(
            symID, ptBound, newSymID, deleteMask, rulesAtDepth, depth);

        StreamSet * keepMask = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<InvertStreamKernel>(deleteMask, keepMask);

        // Compress both symID and ptBound with the same mask (I1).
        // Omitting ptBound compression is a silent correctness bug: positions
        // in compressed symID would not align with original ptBound after depth 0.
        StreamSet * compressedSym   = P.CreateStreamSet(16, 1);
        StreamSet * compressedBound = P.CreateStreamSet(1,  1);
        FilterByMask(P, keepMask, newSymID,  compressedSym);
        FilterByMask(P, keepMask, ptBound,   compressedBound);

        symID   = compressedSym;
        ptBound = compressedBound;
        depth++;
    }

    return symID;
}

// ============================================================================
//  BPETokenizer — file I/O and sequential reference
// ============================================================================

bool BPETokenizer::loadVocab(const std::string & path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open vocab file: " << path << "\n";
        return false;
    }
    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception & e) {
        std::cerr << "BPE: failed to parse vocab file: " << e.what() << "\n";
        return false;
    }
    for (auto & [key, val] : j.items()) {
        int id = val.get<int>();
        vocab_[key] = id;
        if (id >= 0) {
            if (static_cast<size_t>(id) >= idToToken_.size())
                idToToken_.resize(static_cast<size_t>(id) + 1);
            idToToken_[static_cast<size_t>(id)] = key;
        }
    }
    std::cerr << "BPE: loaded vocab with " << vocab_.size() << " tokens\n";
    return !vocab_.empty();
}

// ============================================================================
//  loadMergesWithDepth — single-pass depth assignment
//
//  BPE training guarantees rank ordering: when merged token AB appears as a
//  component of another merge at rank R, the merge that created AB has rank
//  < R.  Processing rules in rank order (= file order) allows depth[AB] to
//  be computed in a single O(N) pass without fixed-point iteration.
//
//    depth[merged] = 1 + max(depth[left], depth[right])
//    Components absent from symbolDepth are base-vocab tokens at depth 0.
// ============================================================================
bool BPETokenizer::loadMergesWithDepth(
        const std::string & path,
        std::vector<std::vector<MergeRule>> & mergesByDepth) {

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open merges file: " << path << "\n";
        return false;
    }

    struct ParsedRule { std::string left, right, merged; int rank; };
    std::vector<ParsedRule> parsed;

    std::string line;
    int rank = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string left  = line.substr(0, sp);
        std::string right = line.substr(sp + 1);
        if (!right.empty() && right.back() == '\r') right.pop_back();
        parsed.push_back({left, right, left + right, rank++});
    }

    if (parsed.empty()) {
        std::cerr << "BPE: no merge rules found in " << path << "\n";
        return false;
    }

    // Single-pass depth assignment in rank (file) order.
    std::unordered_map<std::string, int> symbolDepth;
    int maxDepth = 0;
    for (const auto & r : parsed) {
        int dLeft  = symbolDepth.count(r.left)  ? symbolDepth.at(r.left)  : 0;
        int dRight = symbolDepth.count(r.right) ? symbolDepth.at(r.right) : 0;
        int d = 1 + std::max(dLeft, dRight);
        symbolDepth[r.merged] = d;
        maxDepth = std::max(maxDepth, d);
    }

    // Bucket rules by depth.
    mergesByDepth.clear();
    mergesByDepth.resize(static_cast<size_t>(maxDepth) + 1);

    for (const auto & r : parsed) {
        auto leftIt   = vocab_.find(r.left);
        auto rightIt  = vocab_.find(r.right);
        auto mergedIt = vocab_.find(r.merged);
        if (leftIt   == vocab_.end() ||
            rightIt  == vocab_.end() ||
            mergedIt == vocab_.end())
            continue;   // skip symbols absent from vocabulary

        int d = symbolDepth.at(r.merged);
        MergeRule rule;
        rule.leftID   = static_cast<unsigned>(leftIt->second);
        rule.rightID  = static_cast<unsigned>(rightIt->second);
        rule.mergedID = static_cast<unsigned>(mergedIt->second);
        rule.rank     = r.rank;
        mergesByDepth[static_cast<size_t>(d)].push_back(rule);
    }

    std::cerr << "BPE: loaded " << parsed.size() << " merge rules in "
              << mergesByDepth.size() << " depth levels\n";
    return true;
}

std::string BPETokenizer::decodeToken(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= idToToken_.size()) return "";
    return idToToken_[static_cast<size_t>(id)];
}

// Returns (codepoint_value, vocab_ID) for every single-character token.
// Used by InitialSymIDKernel to map U21 codepoints to initial vocab IDs.
std::vector<std::pair<unsigned,unsigned>>
BPETokenizer::buildInitialVocabMap() const {
    std::vector<std::pair<unsigned,unsigned>> result;
    result.reserve(512);   // GPT-2 has 256 initial byte-level symbols

    for (const auto & [token, id] : vocab_) {
        if (token.empty()) continue;
        unsigned char lead = static_cast<unsigned char>(token[0]);
        size_t cpLen = (lead < 0x80) ? 1u : (lead < 0xE0) ? 2u : (lead < 0xF0) ? 3u : 4u;
        if (token.size() != cpLen) continue;

        const auto & s = token;
        unsigned cp = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if (lead < 0xE0) {
            cp = ((lead & 0x1F) << 6)
               | (static_cast<unsigned char>(s[1]) & 0x3F);
        } else if (lead < 0xF0) {
            cp = ((lead & 0x0F) << 12)
               | ((static_cast<unsigned char>(s[1]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[2]) & 0x3F);
        } else {
            cp = ((lead & 0x07) << 18)
               | ((static_cast<unsigned char>(s[1]) & 0x3F) << 12)
               | ((static_cast<unsigned char>(s[2]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[3]) & 0x3F);
        }
        result.push_back({cp, static_cast<unsigned>(id)});
    }
    return result;
}
