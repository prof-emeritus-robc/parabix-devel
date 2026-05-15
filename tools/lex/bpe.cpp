/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — depth-stratified parallel encoding with
 *  rank-priority conflict resolution (HuggingFace-equivalent semantics).
 *
 *  Parallelism key insight
 *  ───────────────────────
 *  BPE merge rules form a DAG.  The merged token AB can only exist AFTER
 *  both A and B exist, so depth(AB) = 1 + max(depth(A), depth(B)).  Rules
 *  at the same depth are mutually independent and can be applied in a
 *  single SIMD pass — BUT when two adjacent merges compete for the same
 *  position, the lower-rank rule wins (HuggingFace BPE semantics).
 *
 *  Pipeline structure per depth level d
 *  ─────────────────────────────────────
 *    BPEDetectFiresKernel(symID_d, ptBound_d)  →  ruleRank_d, desiredID_d, fireFlag_d
 *    BPEResolveFiresKernel(symID_d, ruleRank_d, fireFlag_d, desiredID_d)
 *                                              →  newSymID_d + deleteMask_d
 *    InvertStreamKernel(deleteMask_d)          →  keepMask_d
 *    FilterByMask(keepMask_d, newSymID_d)      →  symID_{d+1}     (compressed)
 *    FilterByMask(keepMask_d, ptBound_d)       →  ptBound_{d+1}   (aligned)
 *
 *  Why two kernels?
 *  ────────────────
 *  Rank-priority resolution at position p needs to compare the rank of the
 *  merge firing at p against the ranks at p-1 (left neighbor) and p+1
 *  (right neighbor).  Pablo `LookAhead` operations require the source to be
 *  a kernel input binding with `LookAhead(N)` declared — internal Vars
 *  cannot be looked ahead on.  Therefore the rank must cross a kernel
 *  boundary as an output→input stream so the resolution kernel can do
 *  `createLookahead(rank_bit, 1)`.
 *
 *  Correctness invariants
 *  ──────────────────────
 *  I1. ptBound compressed alongside symID at every depth — positions stay
 *      aligned.  (Omitting this causes wrong boundary data from depth 1 on.)
 *  I2. Within a depth, rules iterate in rank order so the "beats current
 *      best" update is monotonically decreasing in rank.
 *  I3. Rank-priority survival: a fire at position p survives iff
 *          (rank[p] < rank[p-1] or no fire at p-1)  AND
 *          (rank[p] < rank[p+1] or no fire at p+1)
 *      This matches HuggingFace's "argmin rank over all adjacent pairs"
 *      sequential behavior at the local conflict level.
 *  I4. Detect kernel name encodes depth — different rule sets per depth
 *      must not share a compiled-kernel cache entry.  Resolve kernel is
 *      rule-set-independent but still depth-tagged for cache keying.
 *
 *  if/scope tree (lib/kernel/util/linebreak_kernel.cpp pattern)
 *  ─────────────────────────────────────────────────────────────
 *  Outer guard if(canMerge) skips all rule work where merging is impossible
 *  (next position is a pre-token boundary).
 *  Per-rule inner guard if(leftMatch) skips right-match + rank-update when
 *  the left symbol doesn't match.  Mirrors UnicodeLinesKernelBuilder
 *  "if(u8pfx,it)" / "if(u8pfx2,it2)".
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


//  BPEDetectFiresKernel
//
//  Stage A of the rank-priority depth pass.  Tests every rule at this
//  depth and records, per position p, the LOWEST-rank rule that fires
//  (i.e. whose left symbol matches symID[p] and whose right symbol matches
//  symID[p+1], with p+1 not a pretoken start).
//
//  Inputs
//  symID      16×1 BixNum — current symbol IDs.
//  ptBound    1×1 — 1 at every pre-token boundary.  LookAhead(1) so we can
//             read ptBound[p+1] from position p.
//
//  Outputs
//  ruleRank   RANK_BITS×1 BixNum — rank of the lowest-rank firing rule
//             at each position; ∞ (= all-ones) if no rule fires.
//  desiredID  16×1 BixNum — the mergedID of that lowest-rank rule.
//  fireFlag   1×1 — 1 iff some rule fires at this position (= ruleRank ≠ ∞).
//
//  if/scope tree (linebreak pattern)
//  ──────────────────────────────────
//  Outer scope  if(canMerge)  — skip all work where merging is impossible.
//  Inner scope  if(leftMatch) — skip right-match + rank-update when left misses.

// decides which mergr should fire at each position, if any.  
// Ranks are stored as a BixNum across multiple Vars so we can do bitwise comparisons and updates.
    // 1. Should a merge happen here?
    // 2. Which rule won here?
    // 3. What should the merged token become?
class BPEDetectFiresKernel : public PabloKernel {
public:
    static constexpr unsigned RANK_BITS = 16;   // ceil(log2(GPT-2 merges)) ≤ 16

    BPEDetectFiresKernel(LLVMTypeSystemInterface & ts,
                         StreamSet * symID,
                         StreamSet * ptBound,
                         StreamSet * ruleRank,
                         StreamSet * desiredID,
                         StreamSet * fireFlag,
                         const std::vector<MergeRule> & rules,
                         unsigned depth)
    : PabloKernel(ts,
                  "BPEDetectFires_D" + std::to_string(depth),
                  {Binding{"symID",   symID,   FixedRate(), LookAhead(1)},
                   Binding{"ptBound", ptBound, FixedRate(), LookAhead(1)}},
                  {Binding{"ruleRank",  ruleRank},
                   Binding{"desiredID", desiredID},
                   Binding{"fireFlag",  fireFlag}}),
      mRules(rules) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());

        // Input streams.
        std::vector<PabloAST*> symBits = getInputStreamSet("symID");
        PabloAST * const ptBoundIn   = getInputStreamSet("ptBound")[0];

        // Right symbol bits: rightBits[i] = symID[i+1] = LookAhead(symID[i], 1).
        std::vector<PabloAST*> rightBits;
        rightBits.reserve(symBits.size());
        for (auto * bit : symBits)
            rightBits.push_back(pb.createLookahead(bit, 1));

        // Can merge if right symbol matches and next position not a pretoken start.
        PabloAST * canMerge = pb.createNot(pb.createLookahead(ptBoundIn, 1));
        PabloAST * const ones   = pb.createNot(pb.createZeroes());
        PabloAST * const zeroes = pb.createZeroes();

        // ruleRank Vars — init to ∞ (all ones).
        // Rank is stored as a BixNum across RANK_BITS Vars, so we can do bitwise
        std::vector<Var*> rankBits;
        rankBits.reserve(RANK_BITS);
        for (unsigned i = 0; i < RANK_BITS; i++)
            rankBits.push_back(pb.createVar(
                "rank_" + std::to_string(i), ones));

        // desiredID Vars (merges)— init to 0 (will mux later: survive ? desired : sym).
        // Same bitwise storage as rank for easy per-bit updates.
        std::vector<Var*> desiredBits;
        desiredBits.reserve(symBits.size());
        for (size_t i = 0; i < symBits.size(); i++)
            desiredBits.push_back(pb.createVar(
                "des_" + std::to_string(i), zeroes));

        // Fire-any flag: set to 1 when any rule fires at this position, else 0.
        // Did ANY rule match here
        Var * fireAny = pb.createVar("fireAny", zeroes);

        // Outer guard: if(canMerge) — skip everything where merge impossible.
        auto mergeScope = pb.createScope();
        pb.createIf(canMerge, mergeScope);
        BixNumCompiler bncOuter(mergeScope);

        // Construct BixNums for left and right symbols so we can test rules.
        BixNum symBN(symBits.begin(), symBits.end());
        BixNum rightBN(rightBits.begin(), rightBits.end());

        // Iterate over rules in rank order (mRules is pre-sorted by rank) and update
        for (const auto & rule : mRules) {

            // Left match: left symbol matches rule's leftID.
            PabloAST * leftMatch = bncOuter.EQ(symBN, rule.leftID);

            // Inner guard: if(leftMatch) — skip right-match + rank-update when left misses.
            auto ruleScope = mergeScope.createScope();
            // Only do merge checking where merging is allowed
            // Only continue where left side matched.
            mergeScope.createIf(leftMatch, ruleScope);
            BixNumCompiler bncR(ruleScope);

            // Right match: right symbol matches rule's rightID.
            PabloAST * rightMatch = bncR.EQ(rightBN, rule.rightID);

            // Construct current rank BixNum from Vars (Var* → PabloAST*).
            // reconstructs the currently winning rank 
            BixNum curRank;
            curRank.reserve(RANK_BITS);
            for (auto * v : rankBits) curRank.push_back(v);

            // "Beats current best": rule.rank < curRank, equivalent to
            // curRank > rule.rank.  rule.rank is a constant unsigned.
            PabloAST * lower = bncR.UGT(curRank,
                static_cast<unsigned>(rule.rank));

            // Beats current best and matches: rightMatch AND lower.
            PabloAST * beats = ruleScope.createAnd(rightMatch, lower);

            // Update rankBits where beats: rankBits[i] ← rule.rank bit i.
            for (unsigned i = 0; i < RANK_BITS; i++) {
                unsigned bit = (static_cast<unsigned>(rule.rank) >> i) & 1u;
                PabloAST * newVal = ruleScope.createSel(beats,
                    bit ? ones : zeroes, rankBits[i]);
                ruleScope.createAssign(rankBits[i], newVal);
            }
            // Update desiredBits where beats: desiredBits[i] ← rule.mergedID bit i.
            for (size_t i = 0; i < symBits.size(); i++) {
                unsigned bit = (rule.mergedID >> i) & 1u;
                PabloAST * newVal = ruleScope.createSel(beats,
                    bit ? ones : zeroes, desiredBits[i]);
                ruleScope.createAssign(desiredBits[i], newVal);
            }
            // Update fireAny: fireAny ← fireAny OR beats/ A merge should happen here.
            ruleScope.createAssign(fireAny,
                ruleScope.createOr(fireAny, beats));
        }

        // Write outputs: rankBits, desiredBits, fireAny.
        Var * rankOut = getOutputStreamVar("ruleRank");
        for (unsigned i = 0; i < RANK_BITS; i++)
            pb.createAssign(
                pb.createExtract(rankOut, pb.getInteger(i)), rankBits[i]);

        // desiredID output is muxed at the end of the resolve kernel for better reuse of this detect kernel across different rule sets.  
        // But we still need to write the desiredID here so the resolve kernel can read it as an input.
        Var * desOut = getOutputStreamVar("desiredID");
        for (size_t i = 0; i < symBits.size(); i++)
            pb.createAssign(
                pb.createExtract(desOut, pb.getInteger(i)), desiredBits[i]);

        // fireFlag output: 1 if any rule fires at this position, else 0.
        Var * flagOut = getOutputStreamVar("fireFlag");
        pb.createAssign(
            pb.createExtract(flagOut, pb.getInteger(0)), fireAny);
    }

private:
    std::vector<MergeRule> mRules;
};


//  BPEResolveFiresKernel
//
//  Stage B of the rank-priority depth pass.  Reads per-position rule rank
//  and applies HuggingFace-equivalent conflict resolution:
//      A fire at p survives iff
//          ( no fire at p-1  OR  rank[p] < rank[p-1] )  AND
//          ( no fire at p+1  OR  rank[p] < rank[p+1] )
//
//  This requires LookAhead(1) on ruleRank and fireFlag inputs — hence the
//  kernel boundary between detect and resolve.
//
//  Inputs
//  symID      16×1 — original symbol IDs (unchanged at suppressed positions).
//  ruleRank   16×1 BixNum, LookAhead(1).
//  fireFlag   1×1, LookAhead(1).
//  desiredID  16×1 — merged ID emitted at surviving fire positions.
//
//  Outputs
//  newSymID   16×1 — survive ? desiredID : symID.
//  deleteMask 1×1 — Advance(survive, 1): right-side consumed positions.
class BPEResolveFiresKernel : public PabloKernel {
public:
    static constexpr unsigned RANK_BITS = BPEDetectFiresKernel::RANK_BITS;

    BPEResolveFiresKernel(LLVMTypeSystemInterface & ts,
                          StreamSet * symID,
                          StreamSet * ruleRank,
                          StreamSet * fireFlag,
                          StreamSet * desiredID,
                          StreamSet * newSymID,
                          StreamSet * deleteMask,
                          unsigned depth)
    : PabloKernel(ts,
                  "BPEResolveFires_D" + std::to_string(depth),
                  {Binding{"symID",     symID},
                   Binding{"ruleRank",  ruleRank,  FixedRate(), LookAhead(1)},
                   Binding{"fireFlag",  fireFlag,  FixedRate(), LookAhead(1)},
                   Binding{"desiredID", desiredID}},
                  {Binding{"newSymID",   newSymID},
                   Binding{"deleteMask", deleteMask}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        std::vector<PabloAST*> symBits  = getInputStreamSet("symID");
        std::vector<PabloAST*> rankBits = getInputStreamSet("ruleRank");
        std::vector<PabloAST*> desBits  = getInputStreamSet("desiredID");
        PabloAST * flag = getInputStreamSet("fireFlag")[0];

        BixNum rank(rankBits.begin(), rankBits.end());

        // Left neighbor's rank (Advance by 1) and fire flag.
        BixNum rankL;
        rankL.reserve(rankBits.size());
        for (auto * b : rankBits) rankL.push_back(pb.createAdvance(b, 1));
        PabloAST * flagL = pb.createAdvance(flag, 1);

        // Right neighbor's rank (LookAhead by 1) and fire flag.
        BixNum rankR;
        rankR.reserve(rankBits.size());
        for (auto * b : rankBits) rankR.push_back(pb.createLookahead(b, 1));
        PabloAST * flagR = pb.createLookahead(flag, 1);

        // "I beat left" = left has no fire OR my rank < left's rank.
        PabloAST * leftLoses  = pb.createOr(
            pb.createNot(flagL), bnc.ULT(rank, rankL));
        PabloAST * rightLoses = pb.createOr(
            pb.createNot(flagR), bnc.ULT(rank, rankR));
        PabloAST * survive = pb.createAnd(flag,
            pb.createAnd(leftLoses, rightLoses));
        PabloAST * notSurvive = pb.createNot(survive);

        Var * symOut = getOutputStreamVar("newSymID");
        for (size_t i = 0; i < symBits.size(); i++) {
            pb.createAssign(
                pb.createExtract(symOut, pb.getInteger(i)),
                pb.createOr(pb.createAnd(desBits[i], survive),
                            pb.createAnd(symBits[i], notSurvive)));
        }

        Var * delOut = getOutputStreamVar("deleteMask");
        pb.createAssign(pb.createExtract(delOut, pb.getInteger(0)),
                        pb.createAdvance(survive, 1));
    }
};

//
//  InvertStreamKernel
//
//  Flips every bit in a 1-bit stream.
//  Converts deleteMask (1=remove) → keepMask (1=keep) for FilterByMask.
//
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


//
//  LinePtBoundKernel
//
//  Inline pretokenizer for compare_bpe.py step 2.  Input is U21 codepoint
//  stream where '\n' (U+000A) marks pretoken separators.
//
//  Outputs:
//    newlineMask   1×1 — 1 at every '\n' codepoint position.
//    ptBoundPre    1×1 — Advance(newlineMask, 1): 1 at first position of
//                  every pretoken (= immediately after a '\n').
//
//  Downstream: FilterByMask(NOT newlineMask) compresses both u21 and
//  ptBoundPre to remove newline positions.  After compression, ptBound[p]=1
//  at position p marks "p is start of a new pretoken in the compressed
//  stream", which is exactly what BPEDetectFiresKernel needs.
//
class LinePtBoundKernel : public PabloKernel {
public:
    LinePtBoundKernel(LLVMTypeSystemInterface & ts,
                      StreamSet * u21,
                      StreamSet * newlineMask,  // Marks positions containing newline.
                      StreamSet * ptBoundPre)   // Marks positions AFTER newline.
    : PabloKernel(ts, "BPE_LinePtBound",
                  {Binding{"u21", u21}},
                  {Binding{"newlineMask", newlineMask},
                   Binding{"ptBoundPre",  ptBoundPre}}) {}  
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> bits = getInputStreamSet("u21");
        BixNum bn(bits.begin(), bits.end());
        PabloAST * isNL = bnc.EQ(bn, 0x0A);
        PabloAST * ptB  = pb.createAdvance(isNL, 1);
        pb.createAssign(
            // newlineMask: 1 at every '\n' codepoint position.
            pb.createExtract(getOutputStreamVar("newlineMask"), 0), isNL);
        pb.createAssign(
            // ptBoundPre: 1 at first position of every pretoken (= immediately after a '\n').
            pb.createExtract(getOutputStreamVar("ptBoundPre"), 0), ptB);
    }
};

// 
//  InitialSymIDKernel
//
//  Maps each U21 codepoint slot to its BPE vocab ID (16-bit BixNum output).
//  Uses BixNumCompiler.EQ + createSel per single-character vocab entry.
//  Since codepoints are unique per slot, conditions are mutually exclusive.
//  Pattern from ztf-logic.cpp.
// 
class InitialSymIDKernel : public PabloKernel {
public:
    InitialSymIDKernel(LLVMTypeSystemInterface & ts,
                       StreamSet * u21,
                       StreamSet * symID,
                       // cpToVocab is a vector of (codepoint, vocabID) pairs for all single-character vocab entries.
                       // This is pre-computed by buildInitialVocabMap() from the BPE merges
                       // is basically a lookup table.
                       std::vector<std::pair<unsigned,unsigned>> cpToVocab)
    : PabloKernel(ts, "InitialSymID",
                  {Binding{"u21",   u21}},
                  {Binding{"symID", symID}}),
      mCpToVocab(std::move(cpToVocab)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        // Construct BixNum for input codepoint.
        std::vector<PabloAST*> u21bits = getInputStreamSet("u21");
        // bit streams like integers.
        BixNum codepoint(u21bits.begin(), u21bits.end());

        // Constants for EQ + Sel: 1 and 0 as PabloAST*.
        PabloAST * ones   = pb.createNot(pb.createZeroes());
        PabloAST * zeroes = pb.createZeroes();

        // vocabID starts as all zeroes; each codepoint match sets it to that codepoint's vocab ID.
        BixNum vocabID(16, zeroes); // GPT-2 vocab IDs fit in 16 bits.
        // For each single-character vocab entry, if codepoint matches, set vocabID to that entry's vocab ID.
        // Loop Through Lookup Table
        for (const auto & [cp, vid] : mCpToVocab) {
            PabloAST * match = bnc.EQ(codepoint, cp);
            for (unsigned i = 0; i < 16; i++) {
                unsigned bit = (vid >> i) & 1u;
                vocabID[i] = pb.createSel(match, bit ? ones : zeroes, vocabID[i]);
            }
        }

        // Write vocabID to output symID stream.
        Var * outVar = getOutputStreamVar("symID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(outVar, pb.getInteger(i)), vocabID[i]);
    }

private:
    std::vector<std::pair<unsigned,unsigned>> mCpToVocab;
};

// 
//  buildInitialSymID
//  Wraps InitialSymIDKernel; returns 16×1 BixNum of initial vocab IDs.
// 
kernel::StreamSet * buildInitialSymID(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * u21,
        const BPETokenizer & bpe) {
    // Build lookup table for InitialSymIDKernel from single-character vocab entries in the BPE merges.
    auto cpToVocab = bpe.buildInitialVocabMap();
    // Create output stream for InitialSymIDKernel.
    StreamSet * symID = P.CreateStreamSet(16, 1);
    // Create InitialSymIDKernel to map each U21 codepoint to its BPE vocab ID.
    P.CreateKernelCall<InitialSymIDKernel>(u21, symID, std::move(cpToVocab));
    return symID;
}

//
//  runBPEPipeline
//
//  Wires D depth passes into the enclosing pipeline.  Each depth pass is a
//  Detect+Resolve kernel pair (see BPEDetectFiresKernel/BPEResolveFiresKernel
//  for rationale) plus a FilterByMask compression of both symID and ptBound
//  to remove consumed right-side positions (I1).
//
//  Returns final compressed 16×1 BixNum — one slot per output token.
// main BPE engine loop
// 
kernel::StreamSet * runBPEPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * symID,
        kernel::StreamSet * ptBound,
        const std::vector<std::vector<MergeRule>> & mergesByDepth) {

    // how many bits needed to store merge ranks, 16
    constexpr unsigned RANK_BITS = BPEDetectFiresKernel::RANK_BITS;

    // For each depth, create Detect+Resolve kernel pair and FilterByMask compression.
    // Run one BPE pass per depth, with symID and ptBound compressed at each step to keep them aligned and minimize work for subsequent passes.
    unsigned depth = 0;
    for (const auto & rulesAtDepth : mergesByDepth) {
        if (rulesAtDepth.empty()) { depth++; continue; }  // skip empty depths, but still increment depth counter for correct kernel naming and cache keying 

        // Stage A: detect best-rank firing rule per position.
        // ruleRank: which rule won
        // desiredID: what merged token should become
        // fireFlag: whether merge should happen
        StreamSet * ruleRank  = P.CreateStreamSet(RANK_BITS, 1);
        StreamSet * desiredID = P.CreateStreamSet(16, 1);
        StreamSet * fireFlag  = P.CreateStreamSet(1,  1);
        // Create detect kernel for this depth, passing in the rules for this depth.
        P.CreateKernelCall<BPEDetectFiresKernel>(
            symID, ptBound, ruleRank, desiredID, fireFlag,
            rulesAtDepth, depth);

        // Stage B: rank-priority survival + emit newSymID + deleteMask.
        // newSymID: Updated token stream after merges.
        // deleteMask: Marks tokens to REMOVE
        StreamSet * newSymID   = P.CreateStreamSet(16, 1);
        StreamSet * deleteMask = P.CreateStreamSet(1,  1);
        P.CreateKernelCall<BPEResolveFiresKernel>(
            symID, ruleRank, fireFlag, desiredID,
            newSymID, deleteMask, depth);

        // Compress both symID and ptBound with the same keepMask (I1).
        // Invert Delete Mask
        StreamSet * keepMask = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<InvertStreamKernel>(deleteMask, keepMask);

        // Compress symID and ptBound with the same keepMask to keep them aligned (I1).
        // after deletion remove deleted position / compress them 
        StreamSet * compressedSym   = P.CreateStreamSet(16, 1);
        StreamSet * compressedBound = P.CreateStreamSet(1,  1);
        FilterByMask(P, keepMask, newSymID,  compressedSym);
        FilterByMask(P, keepMask, ptBound,   compressedBound);

        //  Next iteration's input is this iteration's compressed output.
        symID   = compressedSym;
        ptBound = compressedBound;
        depth++;
    }

    return symID;
}


//
//  buildLinePretokens
//
//  Inline pretokenizer for BPE-mode invocations without --pretokenizer.
//  Builds ptBound + compressed u21 directly from newline positions.
// this is basically the pretokenizer for the BPE pipeline, which is just splitting on newlines and figuring out where the pretoken boundaries are.
LinePretokensResult buildLinePretokens(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * u21) {

    const unsigned u21Bits = u21->getNumElements();

    StreamSet * newlineMask = P.CreateStreamSet(1, 1);  // where newline characters are
    StreamSet * ptBoundPre  = P.CreateStreamSet(1, 1);  // where next pretoken begins
    P.CreateKernelCall<LinePtBoundKernel>(u21, newlineMask, ptBoundPre);

    // keep everything EXCEPT newline
    StreamSet * keepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertStreamKernel>(newlineMask, keepMask);

    // Unicode stream WITHOUT newlines
    StreamSet * compressedU21    = P.CreateStreamSet(u21Bits, 1);
    // compressed boundaries.
    StreamSet * compressedPtBound = P.CreateStreamSet(1, 1);
    // removes newline characters, ? 
    FilterByMask(P, keepMask, u21,        compressedU21);
    // compresses boundary stream
    FilterByMask(P, keepMask, ptBoundPre, compressedPtBound);

    return {compressedU21, compressedPtBound};
}

// 
//  BPETokenizer — file I/O 
// Pure C++ functions to load BPE vocab and merges from disk, 
// build the initial codepoint→vocabID mapping, 
// and organize merges by depth for the pipeline.  
// These are called from the main() function in bpe.cpp 
// to set up the pipeline before running it.
// 

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
    // vocab_: token string -> token ID
    // idToToken_: token ID -> token string
    for (auto & [key, val] : j.items()) {
        int id = val.get<int>();
        // Insert into vocab_ mapping token string to token ID.
        vocab_[key] = id;
        // Ensure idToToken_ is large enough and insert reverse mapping from token ID to token string.
        if (id >= 0) {
            if (static_cast<size_t>(id) >= idToToken_.size())
                idToToken_.resize(static_cast<size_t>(id) + 1);
                // idToToken_[id] = key;  // Insert token string at index of token ID. reverse lookup
            idToToken_[static_cast<size_t>(id)] = key;
        }
    }
    std::cerr << "BPE: loaded vocab with " << vocab_.size() << " tokens\n";
    return !vocab_.empty();
}

// 
//  loadMergesWithDepth — single-pass depth assignment
// Loads all BPE merge rules AND organizes them into dependency depths.

// four tasks 
// 1.  Read merge rules from file
// 2	Assign rank to each rule
// 3	Compute dependency depth
// 4	Convert strings into token IDs
//  BPE training guarantees rank ordering: when merged token AB appears as a
//  component of another merge at rank R, the merge that created AB has rank
//  < R.  Processing rules in rank order (= file order) allows depth[AB] to
//  be computed in a single O(N) pass without fixed-point iteration.
//
//    depth[merged] = 1 + max(depth[left], depth[right])
//    Components absent from symbolDepth are base-vocab tokens at depth 0.
// 
bool BPETokenizer::loadMergesWithDepth(
        const std::string & path,
        std::vector<std::vector<MergeRule>> & mergesByDepth) {

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open merges file: " << path << "\n";
        return false;
    }

    // 1. Read merge rules from file and assign rank.
    struct ParsedRule { std::string left, right, merged; int rank; };
    std::vector<ParsedRule> parsed;

    std::string line;
    int rank = 0;
    // Each non-empty, non-comment line should contain "left right".  Merged token is concatenation of left+right.
    // We assign rank in file order, starting from 0.  HuggingFace's BPE implementation guarantees that merges are listed in rank order in the merges.txt file, so this matches the intended ranks.
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue; // skip empty lines and comments
        size_t sp = line.find(' ');  
        if (sp == std::string::npos) continue; // skip malformed lines
        std::string left  = line.substr(0, sp); // left token
        std::string right = line.substr(sp + 1); // right token
        if (!right.empty() && right.back() == '\r') right.pop_back(); // handle Windows line endings
        parsed.push_back({left, right, left + right, rank++}); // stores merged token is concatenation of left and right
    }

    if (parsed.empty()) {
        std::cerr << "BPE: no merge rules found in " << path << "\n";
        return false;
    }

    // 2. Compute dependency depth with single pass in rank order.
    std::unordered_map<std::string, int> symbolDepth;  // depth of each symbol (base vocab tokens start at depth 0)
    int maxDepth = 0;
    // For each rule in rank order, compute depth of merged token as 1 + max(depth of left, depth of right).
    for (const auto & r : parsed) {
        // Get depth of left and right components; default to 0 if not found (base vocab).
        int dLeft  = symbolDepth.count(r.left)  ? symbolDepth.at(r.left)  : 0;
        // Get depth of right component; default to 0 if not found (base vocab).
        int dRight = symbolDepth.count(r.right) ? symbolDepth.at(r.right) : 0;
        // Depth of merged token is 1 + max(depth of left, depth of right).
        int d = 1 + std::max(dLeft, dRight);
        // Store depth of merged token and track max depth.
        symbolDepth[r.merged] = d;
        // Track maximum depth across all merges for sizing mergesByDepth.
        maxDepth = std::max(maxDepth, d);
    }

    // Bucket rules by depth.
    mergesByDepth.clear();
    // Resize mergesByDepth to have enough buckets for all depths up to maxDepth.
    mergesByDepth.resize(static_cast<size_t>(maxDepth) + 1);

    // 3. Convert strings in rules to token IDs and store in mergesByDepth.
    for (const auto & r : parsed) {
        auto leftIt   = vocab_.find(r.left);
        auto rightIt  = vocab_.find(r.right);
        auto mergedIt = vocab_.find(r.merged);
        if (leftIt   == vocab_.end() ||
            rightIt  == vocab_.end() ||
            mergedIt == vocab_.end())
            continue;   // skip symbols absent from vocabulary

        // Create MergeRule with token IDs and rank, and add to appropriate depth bucket.
        int d = symbolDepth.at(r.merged);
        MergeRule rule;
        // Convert token IDs to unsigned for storage in MergeRule.
        rule.leftID   = static_cast<unsigned>(leftIt->second);
        rule.rightID  = static_cast<unsigned>(rightIt->second);
        rule.mergedID = static_cast<unsigned>(mergedIt->second);
        rule.rank     = r.rank;
        // Store rule in mergesByDepth at index corresponding to its depth.
        mergesByDepth[static_cast<size_t>(d)].push_back(rule);
    }

    // Log summary of loaded rules and depth distribution.
    std::cerr << "BPE: loaded " << parsed.size() << " merge rules in "
              << mergesByDepth.size() << " depth levels\n";
    return true;
}

// decodeToken
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

    // For each vocab entry, if it's a single-character token, decode its codepoint and add (codepoint, vocabID) to the result.
    for (const auto & [token, id] : vocab_) {
        if (token.empty()) continue;
        // Decode the single-character token to get its Unicode codepoint value.
        unsigned char lead = static_cast<unsigned char>(token[0]);
        // Determine expected UTF-8 length based on lead byte.
        size_t cpLen = (lead < 0x80) ? 1u : (lead < 0xE0) ? 2u : (lead < 0xF0) ? 3u : 4u;
        // Skip tokens that aren't exactly one Unicode codepoint (i.e. not single-character tokens).
        if (token.size() != cpLen) continue;

        // Decode UTF-8 codepoint from token string.
        const auto & s = token; 
        unsigned cp = 0;
        // Decode UTF-8 codepoint based on expected length.
        if (lead < 0x80) {
            cp = lead;
        // For multi-byte UTF-8, decode according to UTF-8 rules.
        } else if (lead < 0xE0) {
            cp = ((lead & 0x1F) << 6)
               | (static_cast<unsigned char>(s[1]) & 0x3F);
        // For 3-byte UTF-8, decode using 3 bytes.
        } else if (lead < 0xF0) {
            cp = ((lead & 0x0F) << 12)
               | ((static_cast<unsigned char>(s[1]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[2]) & 0x3F);
        // For 4-byte UTF-8, decode using 4 bytes.
        } else {
            cp = ((lead & 0x07) << 18)
               | ((static_cast<unsigned char>(s[1]) & 0x3F) << 12)
               | ((static_cast<unsigned char>(s[2]) & 0x3F) << 6)
               | (static_cast<unsigned char>(s[3]) & 0x3F);
        }
        // Add (codepoint, vocabID) pair to result for this single-character token.
        result.push_back({cp, static_cast<unsigned>(id)});
    }
    return result;
}

