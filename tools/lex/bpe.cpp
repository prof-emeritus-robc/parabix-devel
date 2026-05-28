/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — pass-layered longest-match scan over raw bytes.
 *
 *  Algorithm
 *  ─────────
 *  Priority is by byte length: a longer vocab word outranks a shorter one
 *  whose match span overlaps it. Rather than detect every word and then
 *  suppress the losers with LookAhead, the vocab is partitioned at load time
 *  into ordered PASSES (buildVocabPasses):
 *
 *      pass(w0) = 1 + max( pass(w1) : len(w1) > len(w0) and overlaps(w0,w1) )
 *               =  0  if no such w1 exists
 *
 *  so pass 0 holds the words nothing longer can override, pass 1 the next
 *  layer, and so on. Two words can only share a pass if neither is strictly
 *  longer-and-overlapping with the other (i.e. equal length, or disjoint).
 *
 *  One BPEPassKernel runs per pass. Each kernel carries a 1-bit `live` byte
 *  mask (all-ones into pass 0). A word matches at end-position p only if every
 *  byte it covers is still live; after detection the kernel clears all matched
 *  spans from `live` and hands the reduced mask to the next pass. Because a
 *  longer overlapping word always runs in an earlier pass and consumes its
 *  bytes, no later pass can re-fire inside it — the per-position OR of every
 *  pass's matchEnd/vocabID is exactly the greedy longest-match segmentation.
 *
 *  Why aligned mask, not stream compression?
 *  ─────────────────────────────────────────
 *  The basis stream keeps its original length and coordinates across every
 *  pass. Compressing out consumed bytes (FilterByMask) would make the bytes
 *  flanking a consumed span adjacent and let a later pass match a word across
 *  the seam (e.g. `abcd` with `bc` consumed → spurious `ad`). Masking in place
 *  keeps every adjacency check in original coordinates, so no seam can form and
 *  no SpreadByMask remap is needed to reassemble the final stream.
 *
 *  Why bytes, not codepoints? GPT-2 vocab.json stores tokens as UTF-8 byte
 *  strings, so matching on the post-norm byte basis skips UTF-8 decode in both
 *  vocab loading and the scan, and mirrors HF tokenizers' representation.
 *
 *  Known limitation: two SAME-LENGTH overlapping words ending at different
 *  positions both emit (priority is pure length, no ID tie-break). Rare with
 *  the GPT-2 vocab. Cache keys for each pass kernel are derived from a rolling
 *  hash of the pass's (token, id) list so different vocab files never collide.
 */

#include "bpe.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>

using namespace pablo;
using namespace kernel;

namespace {

// Rolling hash over a pass's token list — disambiguates JIT cache entries when
// different vocab files produce a pass whose Pablo body differs but whose shape
// (pass index, token count) would otherwise collide.
uint64_t hashTokenSet(const std::vector<std::pair<std::string,unsigned>> & tokens) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const auto & [tok, id] : tokens) {
        for (unsigned char c : tok) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        h ^= static_cast<uint64_t>(id);
        h *= 1099511628211ull;
    }
    return h;
}

// True iff the byte spans of `a` and `b` can intersect in some input with the
// overlapped bytes agreeing — i.e. the two words can co-occur at overlapping
// positions. Covers containment (a inside b) and edge straddles (a non-empty
// suffix of one equals a prefix of the other). This is the relation that lets
// a longer `b` consume bytes a shorter `a` would otherwise match.
bool overlaps(const std::string & a, const std::string & b) {
    const int La = static_cast<int>(a.size());
    const int Lb = static_cast<int>(b.size());
    // Place b at offset d relative to a. Spans intersect for d in
    // [-(Lb-1) .. La-1]; the intersection is [max(0,d) .. min(La-1, d+Lb-1)].
    for (int d = -(Lb - 1); d <= La - 1; ++d) {
        const int lo = std::max(0, d);
        const int hi = std::min(La - 1, d + Lb - 1);
        bool agree = true;
        for (int i = lo; i <= hi; ++i) {
            if (a[i] != b[i - d]) { agree = false; break; }
        }
        if (agree) return true;   // intersection non-empty over [lo..hi] and consistent
    }
    return false;
}

}


// ─── BPEPassDetect ───────────────────────────────────────────────────────────
//
// Detects every word in one pass, end-anchored, gated by the carried `live`
// byte mask. A word fires only if every byte it covers is still live — the
// gate reads liveIn at earlier byte positions via Advance (a backward shift,
// which Advance does natively; no LookAhead binding needed).
//
// Inputs:
//   basis  (8×1) — byte streams of the input text (full length, all passes).
//   liveIn (1×1) — 1 where a byte is still available. OPTIONAL: pass 0 passes
//                  nullptr, treated as all-ones.
// Outputs:
//   matchEnd (1×1)  — fires at the right-most byte of every live match.
//   vocabID  (16×1) — the matched word's ID at matchEnd positions.
//   matchLen (8×1)  — the matched word's byte length at matchEnd positions.

// for each position p:
//     check backwards:
//         does basis[p - k] == token_char[k]?
//     AND check:
//         is that position still live?
//     if all true:
//         mark matchEnd[p] = 1
//         emit vocabID[p]
//         emit length[p]
//
// The span actually consumed is computed downstream by BPEPassMask, because
// marking bytes BEFORE the end position requires a forward LookAhead over
// matchEnd/matchLen, which is only legal on kernel-input bindings.
class BPEPassDetect : public PabloKernel {
public:
    BPEPassDetect(LLVMTypeSystemInterface & ts,
                  StreamSet * basis,
                  StreamSet * liveIn,   // may be nullptr for the first pass, mask of valid positions
                  StreamSet * matchEnd,
                  StreamSet * vocabID,
                  StreamSet * matchLen,
                  unsigned passNo,
                  std::vector<std::pair<std::string,unsigned>> tokens,
                  uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPEPassDetect_" + std::to_string(passNo)
                       + "_h" + std::to_string(shapeHash)
                       + (liveIn ? "_L" : "_L0"),
                  {Binding{"basis", basis}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID},
                   Binding{"matchLen", matchLen}}),
      mHasLive(liveIn != nullptr), mTokens(std::move(tokens)) {
        if (mHasLive) mInputStreamSets.push_back(Binding{"liveIn", liveIn});
    }

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        // Load the basis bits into a BixNum for convenient per-byte access.
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum symBN(basisBits.begin(), basisBits.end());

        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);
        // liveIn is optional for pass 0; treat nullptr as all-ones. 
        // Later passes get the live mask from the previous pass's output.
        PabloAST * liveIn = mHasLive ? getInputStreamSet("liveIn")[0] : ones;

        // Memoize EQ(symBN, b): each distinct byte compared against input once.
        std::unordered_map<uint8_t, PabloAST*> byteEQ;
        auto getEQ = [&](uint8_t b) {
            auto it = byteEQ.find(b);
            if (it != byteEQ.end()) return it->second;
            PabloAST * eq = bnc.EQ(symBN, b);
            byteEQ.emplace(b, eq);
            return eq;
        };
        // Memoize Advance(liveIn, k): liveIn at the byte k positions back.
        std::unordered_map<unsigned, PabloAST*> liveAdv;
        auto getLive = [&](unsigned k) {
            if (k == 0) return liveIn;
            auto it = liveAdv.find(k);
            if (it != liveAdv.end()) return it->second;
            PabloAST * a = pb.createAdvance(liveIn, k, "liveAdv_k" + std::to_string(k));
            liveAdv.emplace(k, a);
            return a;
        };

        Var * matchEndV = pb.createVar("matchEnd", zeroes);
        std::vector<Var*> idBits, lenBits;
        idBits.reserve(16);
        lenBits.reserve(8);
        // 16-bit vocab ID output
        for (unsigned i = 0; i < 16; i++)
            idBits.push_back(pb.createVar("id_" + std::to_string(i), zeroes));
        // 8-bit match length output.
        for (unsigned i = 0; i < 8; i++)
            lenBits.push_back(pb.createVar("len_" + std::to_string(i), zeroes));

        // 
        for (const auto & [tok, vid] : mTokens) {
            const unsigned L = static_cast<unsigned>(tok.size());
            // m = AND over i of [ Advance(EQ(byte i), L-1-i) AND live there ].
            // At end-position p, factor i checks byte p-(L-1-i) and that the
            // byte is still live.
            PabloAST * m = ones;
            for (unsigned i = 0; i < L; i++) {
                uint8_t b = static_cast<uint8_t>(tok[i]);
                unsigned k = L - 1 - i;  // look back from the end position: 0 for the last byte, L-1 for the first
                PabloAST * eq = getEQ(b);  // EQ(symBN, b) for this byte value
                PabloAST * eqA = (k == 0)  // no Advance for the last byte. 
                    ? eq
                    : pb.createAdvance(eq, k,
                          "adv_byte" + std::to_string(static_cast<unsigned>(b))
                              + "_k" + std::to_string(k));
                PabloAST * factor = pb.createAnd(eqA, getLive(k),
                          "live_tok" + std::to_string(vid)
                              + "_factor" + std::to_string(i));
                m = pb.createAnd(m, factor,
                          "m_tok" + std::to_string(vid)
                              + "_factor" + std::to_string(i));
            }
            pb.createAssign(matchEndV,
                pb.createOr(matchEndV, m, "matchEndV_or_tok" + std::to_string(vid)));
            // T2: third arg is the current Var, not zeroes, so a later token in
            // this pass with bit i = 0 cannot clobber an earlier token's bit.
            for (unsigned i = 0; i < 16; i++) {
                unsigned bit = (vid >> i) & 1u;
                pb.createAssign(idBits[i],
                    pb.createSel(m, bit ? ones : zeroes, idBits[i],
                        "idsel_tok" + std::to_string(vid)
                            + "_bit" + std::to_string(i)));
            }
            // matchLen = this token's length L wherever it matched.
            for (unsigned i = 0; i < 8; i++) {
                unsigned bit = (L >> i) & 1u;
                pb.createAssign(lenBits[i],
                    pb.createSel(m, bit ? ones : zeroes, lenBits[i],
                        "lensel_tok" + std::to_string(vid)
                            + "_bit" + std::to_string(i)));
            }
        }

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEnd"), pb.getInteger(0)),
            matchEndV);
        Var * idOut = getOutputStreamVar("vocabID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(idOut, pb.getInteger(i)), idBits[i]);
        Var * lenOut = getOutputStreamVar("matchLen");
        for (unsigned i = 0; i < 8; i++)
            pb.createAssign(pb.createExtract(lenOut, pb.getInteger(i)), lenBits[i]);
    }

private:
    bool mHasLive;
    std::vector<std::pair<std::string,unsigned>> mTokens;
};


// ─── BPEPassMask ───────────────────────────────────────────────────────────────
// This kernel marks all bytes that are inside any matched token span and removes them from the live stream so future BPE passes cannot reuse them
// 
// BPEPassDetect → finds matches
// BPEPassMask → erases matched regions
// Next pass → only sees remaining “live” bytes
//
// generated BPEPassMask per pass and one BPEOrMerge between each consecutive pair of passes.
// Clears the bytes consumed by this pass's matches from the live mask. A byte
// at position q is consumed iff some match ends at q+k (k in [0 .. maxLen-1])
// with byte length >= k+1 (its span reaches back to q). matchEnd and matchLen
// are kernel inputs, so the forward look is a legal LookAhead.
//
//   consumed(q) = OR over k of [ LookAhead(matchEnd, k) AND
//                                UGE(LookAhead(matchLen, k), k+1) ]
//   liveOut     = liveIn AND NOT consumed
//
// This kernel computes a new live mask by marking every byte that lies inside
// any matched token span, using lookahead over matchEnd and matchLen 
// to reconstruct backward coverage intervals. 
// updates the live byte mask after a BPE pass by removing all bytes that are part of any matched token.

class BPEPassMask : public PabloKernel {
public:
    BPEPassMask(LLVMTypeSystemInterface & ts,
                StreamSet * liveIn,   // may be nullptr for the first pass
                StreamSet * matchEnd,
                StreamSet * matchLen,
                StreamSet * liveOut,
                unsigned passNo,
                unsigned maxLen)
    : PabloKernel(ts,
                  "BPEPassMask_" + std::to_string(passNo)
                       + "_m" + std::to_string(maxLen)
                       + (liveIn ? "_L" : "_L0"),
                  maxLen > 1. // Only need matchLen if maxLen > 1, if len is 1, no need for future access
                    ? std::vector<Binding>{
                          Binding{"matchEnd", matchEnd, FixedRate(), LookAhead(maxLen - 1)},
                          Binding{"matchLen", matchLen, FixedRate(), LookAhead(maxLen - 1)}}
                    : std::vector<Binding>{
                          Binding{"matchEnd", matchEnd},
                          Binding{"matchLen", matchLen}},
                  {Binding{"liveOut", liveOut}}),
      mHasLive(liveIn != nullptr), mMaxLen(maxLen) {
        if (mHasLive) mInputStreamSets.push_back(Binding{"liveIn", liveIn});
    }

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        PabloAST * me0 = getInputStreamSet("matchEnd")[0];
        std::vector<PabloAST*> lenIn = getInputStreamSet("matchLen");
        PabloAST * ones   = pb.createNot(pb.createZeroes());
        PabloAST * liveIn = mHasLive ? getInputStreamSet("liveIn")[0] : ones;


        PabloAST * consumed = pb.createZeroes();
        for (unsigned k = 0; k < mMaxLen; k++) {
            PabloAST * futureEnd = (k == 0)
                ? me0
                : pb.createLookahead(me0, k, "futureEnd_k" + std::to_string(k));
            std::vector<PabloAST*> futureLenBits;
            futureLenBits.reserve(8);
            for (unsigned i = 0; i < 8; i++) {
                futureLenBits.push_back((k == 0)
                    ? lenIn[i]
                    : pb.createLookahead(lenIn[i], k,
                          "futureLen_k" + std::to_string(k) + "_bit" + std::to_string(i)));
            }
            BixNum futureLenBN(futureLenBits.begin(), futureLenBits.end());
            // A match ending at q+k covers q iff its length >= k+1.
            PabloAST * cov_k = pb.createAnd(futureEnd,
                                   bnc.UGE(futureLenBN, k + 1),
                                   "cov_k" + std::to_string(k));
            consumed = pb.createOr(consumed, cov_k,
                           "consumed_thru_k" + std::to_string(k));
        }
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("liveOut"), pb.getInteger(0)),
            pb.createAnd(liveIn, pb.createNot(consumed, "not_consumed"), "liveOut"));
    }

private:
    bool mHasLive;
    unsigned mMaxLen;
};

// ─── BPEOrMerge ───────────────────────────────────────────────────────────────
// merges results from two BPE passes by taking a bitwise OR of their outputs
//
// Per-position OR of two (matchEnd, vocabID) tuples. The pass outputs are
// disjoint at end positions (a longer overlapping word always ran earlier and
// consumed the shared end byte, so the shorter word could not fire there), so a
// plain OR — no priority compare — yields the combined result.
class BPEOrMerge : public PabloKernel {
public:
    BPEOrMerge(LLVMTypeSystemInterface & ts,
               StreamSet * me1, StreamSet * id1,
               StreamSet * me2, StreamSet * id2,
               StreamSet * meOut, StreamSet * idOut)
    : PabloKernel(ts, "BPEOrMerge",
                  {Binding{"me1", me1}, Binding{"id1", id1},
                   Binding{"me2", me2}, Binding{"id2", id2}},
                  {Binding{"meOut", meOut}, Binding{"idOut", idOut}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * me1 = getInputStreamSet("me1")[0];
        PabloAST * me2 = getInputStreamSet("me2")[0];
        std::vector<PabloAST*> id1 = getInputStreamSet("id1");
        std::vector<PabloAST*> id2 = getInputStreamSet("id2");
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("meOut"), pb.getInteger(0)),
            pb.createOr(me1, me2, "meOut"));
        Var * idOut = getOutputStreamVar("idOut");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(pb.createExtract(idOut, pb.getInteger(i)),
                pb.createOr(id1[i], id2[i], "idOut_bit" + std::to_string(i)));
        }
    }
};

// ─── InvertStreamKernel ─────────────────────────────────────────────────────
//
// Flips every bit of a 1×1 stream. Used by buildLinePretokens to convert a
// newline mask into a keep mask for FilterByMask.
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
        writeOutputStreamSet("output",
                             std::vector<PabloAST*>{pb.createNot(in, "notNewline")});
    }
};


// ─── LinePtBoundKernel ──────────────────────────────────────────────────────
//
// Inline pretokenizer for compare_bpe.py step 2. Marks each newline (0x0A)
// byte position, to FilterByMask it out of the downstream stream.
class LinePtBoundKernel : public PabloKernel {
public:
    LinePtBoundKernel(LLVMTypeSystemInterface & ts,
                      StreamSet * basis,
                      StreamSet * newlineMask)
    : PabloKernel(ts, "BPE_LinePtBound",
                  {Binding{"basis", basis}},
                  {Binding{"newlineMask", newlineMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> bits = getInputStreamSet("basis");
        BixNum bn(bits.begin(), bits.end());
        PabloAST * isNL = bnc.EQ(bn, 0x0A);
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("newlineMask"), pb.getInteger(0)),
            isNL);
    }
};


// ─── Pipeline ─────────────────────────────────────────────────────────────────
// buildBPEPassPipeline - assembles the pass-layered BPE pipeline.
// Builds a BPEPassDetect kernel for each pass,
// then a BPEPassMask for each non-final pass,
// then BPEOrMerge kernels to combine the pass outputs into the final matchEnd/vocabID streams.
// The live mask chains through the passes, but the matchEnd/vocabID outputs are merged across passes at the end since they are disjoint and can be OR-ed together with no priority compare.
//
// Per pass: BPEPassDetect (end-anchored, live-gated) then BPEPassMask (clears
// consumed spans from the live mask). The live mask chains pass→pass; a
// BPEOrMerge fold accumulates every pass's (matchEnd, vocabID). The last pass
// skips masking since nothing reads the updated mask after it.
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {
    auto passes = bpe.buildVocabPasses(); // partition the vocab into passes based on overlaps

    StreamSet * accMe = nullptr;   // accumulated matchEnd over all passes so far(where token ends)
    StreamSet * accId = nullptr;   // accumulated vocabID (which tokens matched at each position)
    StreamSet * live  = nullptr;   // live mask: which positions are still allowed to be matched

    unsigned passNo = 0;
    // itterate through each pass,
    // building the detect and mask kernels, and merging the results into accMe and accId
    for (size_t pi = 0; pi < passes.size(); pi++) {
        auto & pass = passes[pi];
        if (pass.tokens.empty()) continue;
        // Compute a hash of the pass's token set for JIT cache disambiguation.
        uint64_t shape = hashTokenSet(pass.tokens);
        unsigned passMaxLen = 0;
        for (const auto & [tok, id] : pass.tokens) {
            (void)id;
            if (tok.size() > passMaxLen) passMaxLen = static_cast<unsigned>(tok.size());
        }
        const bool lastPass = (pi + 1 == passes.size());

        // Detect: end-anchored matches gated by the current live mask.
        StreamSet * me  = P.CreateStreamSet(1, 1);  // matchEnd
        StreamSet * id  = P.CreateStreamSet(16, 1); // vocabID
        StreamSet * len = P.CreateStreamSet(8, 1); // matchLen (for masking downstream)
        
        // Build the BPEPassDetect kernel for each pass, 
        // which detects matches of the tokens in this pass and outputs the match end positions, vocab IDs, and match lengths.
        P.CreateKernelCall<BPEPassDetect>(
            basis, live, me, id, len, passNo, std::move(pass.tokens), shape);

        // Mask: clear this pass's consumed spans from live for the next pass.
        // Skipped on the last pass — no later pass reads the updated mask.
        if (!lastPass) {
            StreamSet * liveOut = P.CreateStreamSet(1, 1); // live mask output for the next pass

            // Build the BPEPassMask kernel for this pass, which takes the matchEnd and matchLen outputs from the detect kernel 
            // and updates the live mask to clear the bytes consumed by this pass's matches.
            // mark consumed bytes as dead so later passes can't re-match inside them
            P.CreateKernelCall<BPEPassMask>(live, me, len, liveOut, passNo, passMaxLen);
            live = liveOut;  // updated mask for the next pass
        }
        // increment pass counter for naming and hashing purposes
        passNo++;

        // accumulator
        if (accMe == nullptr) {
            accMe = me;
            accId = id;
        } else {
            StreamSet * outMe = P.CreateStreamSet(1, 1);
            StreamSet * outId = P.CreateStreamSet(16, 1);
            // accumulate results
            P.CreateKernelCall<BPEOrMerge>(accMe, accId, me, id, outMe, outId);
            accMe = outMe;
            accId = outId;
        }
    }
    std::cerr << "BPE: pass-layered pipeline built with " << passNo << " passes\n";

    if (accMe == nullptr) {
        std::cerr << "BPE: vocabulary empty; emitting zero streams\n";
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        return {me, id};
    }
    return {accMe, accId};
}

// buildLinePretokens
// \n bytes are just separators between pretokens, not real content. 
kernel::StreamSet * buildLinePretokens(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis) {

    const unsigned basisBits = basis->getNumElements();

    StreamSet * newlineMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<LinePtBoundKernel>(basis, newlineMask);

    StreamSet * keepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertStreamKernel>(newlineMask, keepMask);

    StreamSet * compressedBasis = P.CreateStreamSet(basisBits, 1);
    FilterByMask(P, keepMask, basis, compressedBasis);

    return compressedBasis;
}


// ─── BPETokenizer — file I/O + pass partitioning ───────────────────────────
// loadVocab reads a vocab.json file mapping token strings to integer IDs. 
// decodeToken looks up an ID in the reverse map built by loadVocab.  
bool BPETokenizer::loadVocab(const std::string & path) {  
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open vocab file: " << path << "\n";
        return false;
    }
    // the nlohmann lib parses the whole file into memory.
    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception & e) {
        std::cerr << "BPE: failed to parse vocab file: " << e.what() << "\n";
        return false;
    }
    // The vocab file maps token strings to integer IDs. 
    //We build both the forward map (vocab_) and the reverse map (idToToken_) for decoding. 
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

std::string BPETokenizer::decodeToken(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= idToToken_.size()) return "";
    return idToToken_[static_cast<size_t>(id)];
}
//
// multi-pass scheduling system for BPE vocabulary matching
//
// Layer the vocab into passes. Words are sorted by length DESCENDING (ties by
// bytes for determinism), then each word's pass is 1 + the max pass of any
// strictly longer word whose span can overlap it (0 if none). Processing in
// descending order guarantees every strictly-longer word already has its final
// pass assigned when a shorter word is considered.
// passes :
// Pass 0 = strongest/longest words
// Pass 1 = words blocked by pass 0
// Pass 2 = words blocked by pass 1
//
// 1. Take all vocab words
// 2. Sort longest first
// 3. Find which longer words overlap smaller words
// 4. Put smaller overlapping words into later passes
// 5. Return the layered groups
// assignment to disk keyed by a vocab-content hash.
//
// recommendaton?
// NOTE: this is O(N^2 · L) in the vocab size. Instant for the truncated dev
// vocab; slow to build for the full ~50k GPT-2 vocab. TODO: accelerate the
// overlap query with an Aho-Corasick / suffix structure and/or cache the pass

std::vector<VocabPass> BPETokenizer::buildVocabPasses() const {  // A list of passes, each containing a list of (token, vocabID) pairs.
    std::vector<std::pair<std::string,unsigned>> words;  // word storage 
    words.reserve(vocab_.size());  // reserve memory for all vocab entries
    // loop over the vocab map and populate the words vector with (token, vocabID) pairs, skipping empty tokens
    for (const auto & [tok, id] : vocab_) {
        if (tok.empty()) continue;
        words.push_back({tok, static_cast<unsigned>(id)}); // store the token and its corresponding ID as a pair in the words vector
    }
    // Sort words by length DESCENDING, then by bytes for deterministic tie-break. Longer words get higher priority.
    std::sort(words.begin(), words.end(),
        // comparison function
        [](const auto & x, const auto & y) {
            if (x.first.size() != y.first.size())         // longer first
                return x.first.size() > y.first.size();   // longer first
            return x.first < y.first;                     // deterministic tie-break
        });
    // Store number of words in n
    const size_t n = words.size();
    std::vector<int> passOf(n, 0);  // Which pass each word belongs to
    int maxPass = 0;
    // loop through each word in n, They are already sorted longest → shortest
    for (size_t i = 0; i < n; i++) {
        // word length Li
        const size_t Li = words[i].first.size();
        // highest pass of overlapping longer words.
        int best = -1;
        // check all earlier words 
        for (size_t k = 0; k < i; k++) {
            // skip same length words, they can't block each other
            if (words[k].first.size() == Li) continue;
            // check if the words overlap
            if (overlaps(words[i].first, words[k].first)) {
                // if they do, update best to the max pass of the overlapping longer word
                if (passOf[k] > best) best = passOf[k];
            }
        }
        // Current word goes AFTER highest overlapping longer word.
        passOf[i] = best + 1;
        // keep track of highest pass index assigned. 
        if (passOf[i] > maxPass) maxPass = passOf[i];
    }

    // pass containers 
    // Group words by pass into the final output structure. Each pass contains a list of (token, vocabID) pairs.
    std::vector<VocabPass> passes(static_cast<size_t>(maxPass) + 1);
    // loop through each word and add it to the appropriate pass based on its assigned pass index in passOf
    for (size_t i = 0; i < n; i++) {
        passes[static_cast<size_t>(passOf[i])].tokens.push_back(std::move(words[i]));
    }
    return passes;
}
