/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — length-grouped detection + longest-wins overlap resolution.
 *
 *  Algorithm
 *  ─────────
 *  Stage 1: Detection (parallel, one kernel per distinct token length L).
 *      buildLengthGroups() buckets the vocab by length. For each L:
 *      BPELengthDetect emits matchEnd_L (1 at right-most byte of any L-byte
 *      vocab match) and vocabID_L (the token's ID at that position). No
 *      `live` mask, no inter-kernel dependency — every detect kernel reads
 *      only `basis`, runs in parallel.
 *
 *  Stage 2: Resolution (one kernel, sequential after detection).
 *      BPELengthResolve takes every (matchEnd_L, vocabID_L) stream. For each
 *      length L_a candidate match at end position p:
 *        - span_a = [p-L_a+1 .. p]
 *        - For every STRICTLY LONGER length L_b > L_a and offset d in
 *          [-(L_a-1), L_b-1]: position q = p+d is the candidate end of an
 *          overlapping L_b match. Read matchEnd_b[q] via Advance (d<0) or
 *          LookAhead (d>0).
 *        - If any such q has matchEnd_b[q]=1, kill matchEnd_a[p] — a strictly
 *          longer overlapping match always wins.
 *      Final: OR of all cleaned (matchEnd, vocabID) streams. Cleaned streams
 *      are mutually exclusive at every position (the longer of any two
 *      overlapping survivors killed the shorter), so OR is safe — no priority
 *      compare in the merge.
 *
 *  Why longest priority, not ID priority?
 *  ──────────────────────────────────────
 *  ID-priority (lower vocab ID wins all overlaps) over-suppresses: single-byte
 *  tokens hold IDs 0-255 and would kill every multi-byte match they touch,
 *  collapsing output to byte-level. Longest-wins emits longer merges where
 *  they exist (e.g. `inst`+`ruction`), matching HF on most non-straddle
 *  inputs. An earlier ID-priority resolver (BPEIDResolve) was removed; see
 *  git history.
 *
 *  Why bytes, not codepoints? GPT-2 vocab.json stores tokens as UTF-8 byte
 *  strings; matching on the post-norm byte basis skips UTF-8 decode and
 *  mirrors HF tokenizers' internal representation.
 *
 *  Cache keys
 *  ──────────
 *  BPELengthDetect name encodes (length L, hashTokenSet(tokens of length L)).
 *  Two vocab files with the same length distribution but different tokens
 *  still produce distinct cache entries.
 */

#include "bpe.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <stdexcept>

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

}


// ─── BPELengthDetect ─────────────────────────────────────────────────────────
//
// Detects every L-byte vocab token in the input, end-anchored. NO live mask —
// detection is unconditional. All tokens in `mTokens` have byte length
// `mLength`. One BPELengthDetect kernel is instantiated per distinct vocab
// length; the kernels are mutually independent.
//
// Inputs:
//   basis (8×1) — byte streams of the input text.
// Outputs:
//   matchEnd (1×1)  — 1 at the right-most byte p of every L-byte match. Note
//                     two L-byte tokens cannot match at the same end position
//                     (their byte values would have to be identical), so
//                     within one length kernel matchEnd has at most one
//                     winning token per position.
//   vocabID  (16×1) — the token's vocab ID at every matchEnd position.
//
// for each position p:
//     check backwards:
//         does basis[p - k] == token_char[L - 1 - k]?
//     if all L bytes match:
//         mark matchEnd[p] = 1
//         emit vocabID[p]
//
// Overlap resolution is handled downstream by BPELengthResolve — this kernel
// does NOT suppress conflicts. Two overlapping matches from different lengths
// (or two same-length matches at adjacent end positions with overlapping
// spans) will both fire here; the resolver keeps the longest.
class BPELengthDetect : public PabloKernel {
public:
    BPELengthDetect(LLVMTypeSystemInterface & ts,
                    StreamSet * basis,
                    StreamSet * matchEnd,
                    StreamSet * vocabID,
                    unsigned length,
                    std::vector<std::pair<std::string,unsigned>> tokens,
                    uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPELengthDetect_L" + std::to_string(length)
                       + "_h" + std::to_string(shapeHash),
                  {Binding{"basis", basis}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID}}),
      mTokens(std::move(tokens)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum symBN(basisBits.begin(), basisBits.end());

        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        // byteEQ[b] = (symBN == b), memoized in root scope so any nested
        // if-scope can reference it safely.
        std::unordered_map<uint8_t, PabloAST*> byteEQ;
        auto getEQ = [&](uint8_t b) -> PabloAST* {
            auto it = byteEQ.find(b);
            if (it != byteEQ.end()) return it->second;
            PabloAST* eq = bnc.EQ(symBN, b);
            byteEQ[b] = eq;
            return eq;
        };

        // advCache[(b,d)] = Advance(byteEQ[b], d). At end position p, this
        // fires iff input[p-d] == b. Reused across the trie wherever multiple
        // tokens need the same (byte, depth) factor.
        std::unordered_map<uint64_t, PabloAST*> advCache;
        auto getAdvFactor = [&](uint8_t b, unsigned d) -> PabloAST* {
            if (d == 0) return getEQ(b);
            uint64_t key = (static_cast<uint64_t>(b) << 32) | d;
            auto it = advCache.find(key);
            if (it != advCache.end()) return it->second;
            PabloAST* result = pb.createAdvance(
                getEQ(b), d,
                "adv_b" + std::to_string((unsigned)b) + "_d" + std::to_string(d));
            advCache[key] = result;
            return result;
        };

        Var * matchEndV = pb.createVar("matchEnd", zeroes);
        std::vector<Var*> idBits;
        idBits.reserve(16);
        for (unsigned i = 0; i < 16; i++)
            idBits.push_back(pb.createVar("id_" + std::to_string(i), zeroes));

        // Reversed trie over mTokens (all of length mLength). Last byte of
        // each token is at depth 1 (advance=0); first byte at depth L
        // (advance=L-1) — end-anchored backward detection.
        struct TrieNode {
            std::unordered_map<uint8_t, unsigned> children;
            int tokenID = -1;
        };
        std::vector<TrieNode> trie(1);
        for (const auto & [tok, vid] : mTokens) {
            unsigned cur = 0;
            for (int i = static_cast<int>(tok.size()) - 1; i >= 0; i--) {
                uint8_t b = static_cast<uint8_t>(tok[i]);
                auto cit = trie[cur].children.find(b);
                if (cit == trie[cur].children.end()) {
                    unsigned next = static_cast<unsigned>(trie.size());
                    trie[cur].children[b] = next;
                    trie.emplace_back();
                    cur = next;
                } else {
                    cur = cit->second;
                }
            }
            trie[cur].tokenID = static_cast<int>(vid);
        }

        // Pre-compute every (b, d) factor in root scope before any createIf,
        // so factor nodes are visible inside nested if-scopes without scope
        // ordering violation.
        {
            std::vector<std::pair<unsigned,unsigned>> preStack;
            preStack.push_back({0u, 0u});
            while (!preStack.empty()) {
                auto [ni, d] = preStack.back(); preStack.pop_back();
                for (const auto & [b, ci] : trie[ni].children) {
                    getAdvFactor(b, d);
                    preStack.push_back({ci, d + 1});
                }
            }
        }

        // DFS emit. For shallow depths we wrap each child in createIf so SIMD
        // blocks where childStream=0 skip the subtree at runtime. Beyond
        // IF_DEPTH_CAP we emit the child inline in the current scope —
        // long-token kernels (e.g. L=256) would otherwise nest 256 createIf
        // scopes deep, and the resulting LLVM IR triggers super-linear work
        // in the optimizer (multi-minute JIT). The cap keeps SIMD skip on
        // the high-fanout top of the trie where it matters most.
        const unsigned IF_DEPTH_CAP = 8;
        std::function<void(PabloBuilder&, PabloAST*, unsigned, unsigned)> emitNode;
        emitNode = [&](PabloBuilder & pb_cur, PabloAST* stream,
                       unsigned nodeIdx, unsigned depth) {
            const TrieNode & node = trie[nodeIdx];

            // Terminal: emit match (T2: Sel third arg = current Var).
            if (node.tokenID >= 0) {
                unsigned vid = static_cast<unsigned>(node.tokenID);
                pb_cur.createAssign(matchEndV,
                    pb_cur.createOr(matchEndV, stream,
                        "matchEndV_tok" + std::to_string(vid)));
                for (unsigned i = 0; i < 16; i++) {
                    unsigned bit = (vid >> i) & 1u;
                    pb_cur.createAssign(idBits[i],
                        pb_cur.createSel(stream, bit ? ones : zeroes, idBits[i],
                            "idsel_tok" + std::to_string(vid) + "_bit" + std::to_string(i)));
                }
            }

            for (const auto & [b, childIdx] : node.children) {
                PabloAST* factor      = getAdvFactor(b, depth);
                PabloAST* childStream = pb_cur.createAnd(stream, factor,
                    "cs_d" + std::to_string(depth) + "_b" + std::to_string((unsigned)b));
                if (depth < IF_DEPTH_CAP) {
                    auto childScope = pb_cur.createScope();
                    pb_cur.createIf(childStream, childScope);
                    emitNode(childScope, childStream, childIdx, depth + 1);
                } else {
                    // Flat: same scope, no createIf.
                    emitNode(pb_cur, childStream, childIdx, depth + 1);
                }
            }
        };

        emitNode(pb, ones, 0, 0);

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEnd"), pb.getInteger(0)),
            matchEndV);
        Var * idOut = getOutputStreamVar("vocabID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(idOut, pb.getInteger(i)), idBits[i]);
    }

private:
    std::vector<std::pair<std::string,unsigned>> mTokens;
};


// ─── BPELengthResolve ────────────────────────────────────────────────────────
//
// Longest-wins overlap resolution.
//
// For each candidate match of length L_a ending at p with id_a:
//
//     killed_a[p] = OR over (L_b > L_a, d in [-(L_a-1) .. L_b-1]) of
//                       shift(matchEnd_b, d)[p]
//
// i.e. any STRICTLY LONGER match whose span overlaps a's span kills a.
// No ID compare — strictly-longer always wins (the rule we had under the
// old pass-layered design). Same-length overlapping matches at different
// end positions both emit — documented limitation.
//
// Why this approximates HF better than ID-priority across spans:
//   - HF emits longer merges where they exist (e.g. `inst`+`ruction`).
//   - Longest-wins picks `inst` over its sub-pieces, matching HF on most
//     non-straddle inputs.
//   - Edge-straddles (where HF would split differently across a boundary)
//     still diverge; those need a separate scalar pass downstream.
//
// Cost in Pablo ops is also much smaller than ID-priority:
//   - No 16-bit ULT compare per (a, b, d).
//   - No shiftN over 16 ID bits per (a, b, d) — only shift1 on the
//     matchEnd_b stream is needed.
//   - LookAhead requirement on inputs unchanged (LookAhead(maxL-1) on the
//     matchEnd bindings; id bindings declared but not LookAhead'd here).
class BPELengthResolve : public PabloKernel {
public:
    BPELengthResolve(LLVMTypeSystemInterface & ts,
                     const std::vector<unsigned> & lengths,
                     const std::vector<StreamSet*> & matchEnds,
                     const std::vector<StreamSet*> & vocabIDs,
                     StreamSet * matchEndOut,
                     StreamSet * vocabIDOut)
    : PabloKernel(ts,
                  makeName(lengths),
                  makeInputBindings(lengths, matchEnds, vocabIDs),
                  {Binding{"matchEndOut", matchEndOut},
                   Binding{"vocabIDOut",  vocabIDOut}}),
      mLengths(lengths) {}

private:
    static std::string makeName(const std::vector<unsigned> & lengths) {
        std::string n = "BPELengthResolve";
        for (unsigned L : lengths) n += "_" + std::to_string(L);
        return n;
    }
    static std::vector<Binding> makeInputBindings(
            const std::vector<unsigned> & lengths,
            const std::vector<StreamSet*> & mes,
            const std::vector<StreamSet*> & ids) {
        std::vector<Binding> b;
        unsigned maxL = 0;
        // LookAhead requirement: every input binding declares LookAhead(maxL - 1).
        for (unsigned L : lengths) if (L > maxL) maxL = L;
        unsigned LA = (maxL > 1) ? (maxL - 1) : 0;
        // Inputs are (matchEnd_L, vocabID_L) for every length L. 
        // Every input stream declares LookAhead(maxL - 1) to cover the full offset range of any potential overlap conflict.
        for (size_t i = 0; i < lengths.size(); i++) {
            std::string suf = "_L" + std::to_string(lengths[i]);
            if (LA > 0) {
                // matchEnd is the only input we LookAhead on. id is read
                // identity-only (at the candidate's own end position).
                b.push_back(Binding{"me" + suf, mes[i], FixedRate(), LookAhead(LA)});
                b.push_back(Binding{"id" + suf, ids[i]});
            } else {
                b.push_back(Binding{"me" + suf, mes[i]});
                b.push_back(Binding{"id" + suf, ids[i]});
            }
        }
        return b;
    }

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());

        PabloAST * zeroes = pb.createZeroes();

        auto shift1 = [&](PabloAST* s, int d, const std::string & tag) -> PabloAST* {
            if (d == 0) return s;
            if (d > 0)  return pb.createLookahead(s, d, tag + "_la");
            return pb.createAdvance(s, -d, tag + "_adv");
        };

        const unsigned N = static_cast<unsigned>(mLengths.size());

        std::vector<PabloAST*> meIn(N);
        std::vector<std::vector<PabloAST*>> idIn(N);
        // Pull all (matchEnd_L, vocabID_L) inputs in. meIn[a] = matchEnd_a; idIn[a] = vocabID_a for every length a.
        for (unsigned a = 0; a < N; a++) {
            std::string suf = "_L" + std::to_string(mLengths[a]);
            meIn[a] = getInputStreamSet("me" + suf)[0];
            idIn[a] = getInputStreamSet("id" + suf);
        }

        // Per-length cleaned streams.
        std::vector<PabloAST*> cleanedMe(N);
        std::vector<std::vector<PabloAST*>> cleanedId(N);

        // For each length L_a and position p with matchEnd_a[p]=1, check every other length L_b > L_a and 
        //offset d for an overlapping match at q=p+d. If any found, kill matchEnd_a[p].
        for (unsigned a = 0; a < N; a++) {
            const unsigned La = mLengths[a];

            PabloAST * killed = zeroes;
            // For every other length b > a and offset d where an L_b match would overlap an L_a match at p, check if there's a matchEnd_b[q]. If so, kill the L_a match at p.
            for (unsigned b = 0; b < N; b++) {
                const unsigned Lb = mLengths[b];
                if (Lb <= La) continue;   // only strictly-longer kills

                const int dMin = -static_cast<int>(La - 1);
                const int dMax =  static_cast<int>(Lb - 1);
                for (int d = dMin; d <= dMax; d++) {
                    const std::string tag = "a" + std::to_string(La)
                                          + "_b" + std::to_string(Lb)
                                          + "_d" + std::to_string(d < 0 ? -d : d)
                                          + (d < 0 ? "n" : "");
                    PabloAST* meB = shift1(meIn[b], d, "me_" + tag);
                    killed = pb.createOr(killed, meB, "killed_" + tag);
                }
            }
            cleanedMe[a] = pb.createAnd(meIn[a],
                                        pb.createNot(killed, "nk_a" + std::to_string(La)),
                                        "cleanMe_L" + std::to_string(La));
            cleanedId[a].resize(idIn[a].size());
            for (size_t i = 0; i < idIn[a].size(); i++) {
                cleanedId[a][i] = pb.createAnd(idIn[a][i], cleanedMe[a],
                    "cleanId_L" + std::to_string(La) + "_b" + std::to_string(i));
            }
        }

        // Final OR-fold. Cleaned streams are disjoint at each position by
        // longest-wins reasoning: if two distinct lengths both survived at p,
        // the longer would have killed the shorter. (Same-length overlapping
        // matches at different end positions can both fire — known limitation.)
        PabloAST * finalMe = zeroes;
        std::vector<PabloAST*> finalId(16, zeroes);
        for (unsigned a = 0; a < N; a++) {
            finalMe = pb.createOr(finalMe, cleanedMe[a],
                                  "finalMe_L" + std::to_string(mLengths[a]));
            for (size_t i = 0; i < 16; i++) {
                finalId[i] = pb.createOr(finalId[i], cleanedId[a][i],
                    "finalId_L" + std::to_string(mLengths[a]) + "_b" + std::to_string(i));
            }
        }

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEndOut"), pb.getInteger(0)),
            finalMe);
        Var * idOut = getOutputStreamVar("vocabIDOut");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(idOut, pb.getInteger(i)), finalId[i]);
    }

private:
    std::vector<unsigned> mLengths;
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


// ─── Pipeline ────────────────────────────────────────────────────────────────
// buildBPEPassPipeline assembles the length-grouped, ID-priority BPE pipeline:
//   1. One BPELengthDetect kernel per distinct token length L. All kernels
//      read `basis` only and run independently.
//   2. One BPELengthResolve kernel that takes every (matchEnd_L, vocabID_L)
//      stream and produces the final (matchEnd, vocabID) with longest-wins
//      overlap arbitration: a strictly-longer match's span kills any shorter
//      match that overlaps. Same-length overlapping matches at different end
//      positions both emit (documented limitation). The disabled
//      BPEIDResolve below kept the alternative ID-priority rule for
//      reference — see comments above that class.
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {
    auto groups = bpe.buildLengthGroups();

    if (groups.empty()) {
        std::cerr << "BPE: vocabulary empty; emitting zero streams\n";
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        return {me, id};
    }

    // Per-length detect kernels (parallel).
    std::vector<unsigned> lengths;
    std::vector<StreamSet*> mes;
    std::vector<StreamSet*> ids;
    lengths.reserve(groups.size());
    mes.reserve(groups.size());
    ids.reserve(groups.size());

    // Each BPELengthDetect emits (matchEnd_L, vocabID_L) for every token of length L
    for (auto & g : groups) {
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        uint64_t shape = hashTokenSet(g.tokens);
        P.CreateKernelCall<BPELengthDetect>(
            basis, me, id, g.length, std::move(g.tokens), shape);
        lengths.push_back(g.length);
        mes.push_back(me);
        ids.push_back(id);
    }

    // Debug info: print the length distribution of the vocab, which determines the number of detect kernels and the lookahead requirements.
    std::cerr << "BPE: length-grouped detection — " << groups.size()
              << " length kernels (";
    for (size_t i = 0; i < lengths.size(); i++) {
        if (i) std::cerr << ",";
        std::cerr << lengths[i];
    }
    std::cerr << ")\n";

    // Longest-wins overlap resolution (single kernel).
    StreamSet * finalMe = P.CreateStreamSet(1, 1);
    StreamSet * finalId = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPELengthResolve>(lengths, mes, ids, finalMe, finalId);
    return {finalMe, finalId};
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
// Length grouping for the BPE vocab.
//
// Bucket every vocab token by its byte length. Result is sorted by length
// ascending. Each group's `tokens` is sorted by byte content so the JIT cache
// key (hashTokenSet) is stable across runs and across machines. Empty tokens
// are skipped.
//
// O(N log N) on vocab size (dominated by per-group sort).
std::vector<LengthGroup> BPETokenizer::buildLengthGroups() const {
    std::unordered_map<unsigned, std::vector<std::pair<std::string,unsigned>>> byLen;
    // Group tokens by length. 
    // Skip empty tokens since they don't need a detect kernel and would just waste space in the length groups.
    for (const auto & [tok, id] : vocab_) {
        if (tok.empty()) continue;
        byLen[static_cast<unsigned>(tok.size())]
            .push_back({tok, static_cast<unsigned>(id)});
    }

    // Sort each length group by byte content for stable JIT cache keys, then sort groups by length ascending.
    std::vector<LengthGroup> groups;
    groups.reserve(byLen.size());
    // Each BPELengthDetect emits (matchEnd_L, vocabID_L) for every token of length L
    for (auto & [L, toks] : byLen) {
        // Deterministic order for stable cache keys.
        std::sort(toks.begin(), toks.end(),
                  [](const auto & a, const auto & b) { return a.first < b.first; });
        groups.push_back(LengthGroup{L, std::move(toks)});
    }
    // Sort groups by length ascending.
    std::sort(groups.begin(), groups.end(),
              [](const LengthGroup & a, const LengthGroup & b) {
                  return a.length < b.length;
              });
    return groups;
}
