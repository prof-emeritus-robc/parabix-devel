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
 *      BPETokenDetect emits matchEnd_L (1 at right-most byte of any L-byte
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
 *  BPETokenDetect name encodes (length L, hashTokenSet(tokens of length L)).
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


// ─── BPETokenDetect ─────────────────────────────────────────────────────────
//
// # 04
// Detects every L-byte vocab token in the input, end-anchored. NO live mask —
// detection is unconditional. All tokens in `mTokens` have byte length
// `mLength`. One BPETokenDetect kernel is instantiated per distinct vocab
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
//   matchLen (8×1)  — constant L at every matchEnd position, 0 elsewhere.
//                     Carried downstream so BPEAssembleKernel can drop a short
//                     match contained in a longer one via a single LookAhead
//                     sweep (no all-pairs length compare).
//
// for each position p:
//     check backwards:
//         does basis[p - k] == token_char[L - 1 - k]?
//     if all L bytes match:
//         mark matchEnd[p] = 1
//         emit vocabID[p], matchLen[p] = L
//
// Overlap resolution is handled downstream — this kernel does NOT suppress
// conflicts. BPELengthFirstWinsMerge keeps the longest match ending at each
// position (descending fold); BPEAssembleKernel then drops matches contained
// in a longer match ending later.
class BPETokenDetect : public PabloKernel {
public:
    BPETokenDetect(LLVMTypeSystemInterface & ts,
                    StreamSet * basis,
                    StreamSet * matchEnd,
                    StreamSet * vocabID,
                    StreamSet * matchLen,
                    unsigned length,
                    std::vector<std::pair<std::string,unsigned>> tokens,
                    uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPETokenDetect_L" + std::to_string(length)
                       + "_h" + std::to_string(shapeHash),
                  {Binding{"basis", basis}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID},
                   Binding{"matchLen", matchLen}}),
      mLength(length), mTokens(std::move(tokens)) {}

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
        // trie 
        // Loop over every token in this length group. Insert it into the trie, reversed (last byte at depth 1, first byte at depth L).
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

        // DFS emit, re_compiler-style sparse createIf gating.
        // Only the FIRST level of the trie (depth == 0, one createIf per
        // distinct last-byte) wraps its subtree in createIf — gives SIMD
        // block-level skip on the cheap, high-selectivity first byte
        // (256 possible values, most absent from any given SIMD block).
        // All deeper levels emit FLAT in the same nested scope. No
        // createIf nesting beyond depth 0 → LLVM optimizer sees a shallow
        // function (one outer scope + N small flat sub-scopes) instead of
        // a recursive control-flow tree, so JIT compile stays linear in
        // trie node count instead of super-linear in nesting depth.
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
                // Emit vocab ID bits. Each bit i of the token's ID is set at every match position.
                for (unsigned i = 0; i < 16; i++) {
                    unsigned bit = (vid >> i) & 1u;
                    pb_cur.createAssign(idBits[i],
                        pb_cur.createSel(stream, bit ? ones : zeroes, idBits[i],
                            "idsel_tok" + std::to_string(vid) + "_bit" + std::to_string(i)));
                }
            }

            // Recurse on children. Each child's stream = parentStream AND factor(byte, depth).
            for (const auto & [b, childIdx] : node.children) {
                PabloAST* factor      = getAdvFactor(b, depth);
                // trie extension 
                PabloAST* childStream = pb_cur.createAnd(stream, factor,
                    "cs_d" + std::to_string(depth) + "_b" + std::to_string((unsigned)b));
                if (depth == 0) {
                    // Top-level gate: one createIf per distinct first byte
                    // (last byte of token, since trie is reversed).
                    auto childScope = pb_cur.createScope();
                    pb_cur.createIf(childStream, childScope);
                    emitNode(childScope, childStream, childIdx, depth + 1);
                } else {
                    // Deeper levels: flat in current scope, no createIf.
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

        // matchLen = constant mLength at matchEnd positions, 0 elsewhere. All
        // tokens in this kernel share length mLength, so bit i is matchEndV
        // wherever ((mLength>>i)&1), zeroes otherwise.
        Var * lenOut = getOutputStreamVar("matchLen");
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * v = ((mLength >> i) & 1u)
                               ? static_cast<PabloAST*>(matchEndV)
                               : zeroes;
            pb.createAssign(pb.createExtract(lenOut, pb.getInteger(i)), v);
        }
    }

private:
    unsigned mLength;
    std::vector<std::pair<std::string,unsigned>> mTokens;
};


// ─── BPELengthFirstWinsMerge ───────────────────────────────────────────────
//
// First-wins merge of two (matchEnd, vocabID, matchLen) tuples. Length groups
// are folded in DESCENDING length order, so the accumulated "1" side already
// holds the longer match and the "2" side only fills positions the first did
// not cover. Resolves the SAME-end-position conflict (two tokens ending at p):
// fold order alone keeps the longer — no length compare needed.
//
// Cost: ~1 + 16 + 8 Sel/Or ops, independent of vocab size — O(1) per merge,
// O(#lengths) merges total. (Replaces the old monolithic resolver whose ONE
// kernel held O(#lengths^2 * maxLen) ≈ 47k ops and OOM-killed the JIT.)
class BPELengthFirstWinsMerge : public PabloKernel {
public:
    BPELengthFirstWinsMerge(LLVMTypeSystemInterface & ts,
                            StreamSet * me1, StreamSet * id1, StreamSet * len1,
                            StreamSet * me2, StreamSet * id2, StreamSet * len2,
                            StreamSet * meOut, StreamSet * idOut, StreamSet * lenOut)
    : PabloKernel(ts, "BPELengthFirstWinsMerge",
                  {Binding{"me1", me1}, Binding{"id1", id1}, Binding{"len1", len1},
                   Binding{"me2", me2}, Binding{"id2", id2}, Binding{"len2", len2}},
                  {Binding{"meOut", meOut}, Binding{"idOut", idOut},
                   Binding{"lenOut", lenOut}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());

        PabloAST * me1 = getInputStreamSet("me1")[0];
        PabloAST * me2 = getInputStreamSet("me2")[0];
        std::vector<PabloAST*> id1  = getInputStreamSet("id1");
        std::vector<PabloAST*> id2  = getInputStreamSet("id2");
        std::vector<PabloAST*> len1 = getInputStreamSet("len1");
        std::vector<PabloAST*> len2 = getInputStreamSet("len2");

        // me2 wins only where me1 has not already fired (me1 = longer side).
        PabloAST * me2Wins = pb.createAnd(me2, pb.createNot(me1));

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("meOut"), pb.getInteger(0)),
            pb.createOr(me1, me2));

        // id/len: side-1 bits where me1 fired, else side-2 bits where me2 won,
        // else 0.
        Var * idOut = getOutputStreamVar("idOut");
        for (unsigned i = 0; i < 16; i++) {
            PabloAST * picked = pb.createSel(me1, id1[i],
                                   pb.createSel(me2Wins, id2[i], pb.createZeroes()));
            pb.createAssign(pb.createExtract(idOut, pb.getInteger(i)), picked);
        }
        Var * lenOut = getOutputStreamVar("lenOut");
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * picked = pb.createSel(me1, len1[i],
                                   pb.createSel(me2Wins, len2[i], pb.createZeroes()));
            pb.createAssign(pb.createExtract(lenOut, pb.getInteger(i)), picked);
        }
    }
};


// ─── BPEAssembleKernel ──────────────────────────────────────────────────────
//
// Drops matches contained in the byte span of a longer match ending later.
// A match ending at e with length L covers positions [e-L+1 .. e]; so position
// p is covered iff there exists k in [1 .. maxLen-1] with
//     matchEnd(p+k) = 1  AND  matchLen(p+k) > k.
// matchEnd and matchLen are kernel INPUTS declaring LookAhead(maxLen-1), so the
// forward peek is a legal LookAhead.
//
// Single forward sweep: O(maxLen) iterations, each ~1 + 8 LookAhead + one UGT +
// AND/OR. ~5k ops at maxLen=256 — an order of magnitude below the old all-pairs
// resolver, and it does NOT OOM the JIT.
//
// Example: single-byte `l` at position 2 inside `Ġlove` ending at position 5
// with length 6 — at p=2, k=3: matchEnd(5)=1 and matchLen(5)=6 > 3, so `l` is
// covered and suppressed.
class BPEAssembleKernel : public PabloKernel {
public:
    BPEAssembleKernel(LLVMTypeSystemInterface & ts,
                      StreamSet * matchEndIn, StreamSet * vocabIDIn,
                      StreamSet * matchLenIn,
                      StreamSet * matchEndOut, StreamSet * vocabIDOut,
                      unsigned maxLen)
    : PabloKernel(ts,
                  "BPEAssemble_m" + std::to_string(maxLen),
                  {Binding{"matchEndIn", matchEndIn, FixedRate(), LookAhead(maxLen - 1)},
                   Binding{"vocabIDIn",  vocabIDIn},
                   Binding{"matchLenIn", matchLenIn, FixedRate(), LookAhead(maxLen - 1)}},
                  {Binding{"matchEndOut", matchEndOut},
                   Binding{"vocabIDOut",  vocabIDOut}}),
      mMaxLen(maxLen) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        PabloAST * me0 = getInputStreamSet("matchEndIn")[0];
        std::vector<PabloAST*> idIn  = getInputStreamSet("vocabIDIn");
        std::vector<PabloAST*> lenIn = getInputStreamSet("matchLenIn");

        // coverMask = OR over k in [1 .. maxLen-1] of
        //             ( matchEnd(p+k) AND UGT(matchLen(p+k), k) ).
        PabloAST * coverMask = pb.createZeroes();
        for (unsigned k = 1; k < mMaxLen; k++) {
            PabloAST * futureEnd = pb.createLookahead(me0, k);
            std::vector<PabloAST*> futureLenBits;
            futureLenBits.reserve(8);
            for (unsigned i = 0; i < 8; i++)
                futureLenBits.push_back(pb.createLookahead(lenIn[i], k));
            BixNum futureLenBN(futureLenBits.begin(), futureLenBits.end());
            PabloAST * covered_k = pb.createAnd(futureEnd, bnc.UGT(futureLenBN, k));
            coverMask = pb.createOr(coverMask, covered_k);
        }

        // keep = matchEnd(p) AND NOT coverMask.
        PabloAST * keep = pb.createAnd(me0, pb.createNot(coverMask));

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEndOut"), pb.getInteger(0)),
            keep);
        Var * idOut = getOutputStreamVar("vocabIDOut");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(idOut, pb.getInteger(i)),
                            pb.createAnd(idIn[i], keep));
    }

private:
    unsigned mMaxLen;
};

// ─── BPEOverlapSplit — the "find the two groups" pass O(overlap), N(independent) ────────────────────────
//
// Splits the longest-per-end-position match stream into two DISJOINT groups:
//   • overlap     — matches whose byte span intersects another match's span.
//   • independent — matches that touch nothing else (loners).
//
// A match A ends at p with length La = matchLen(p), span [p-La+1 .. p].
//   overlapsLater(p)   = some match ends at p+k (k≥1) and reaches back over p:
//                        matchEnd(p+k) AND matchLen(p+k) > k.        (other's length)
//   overlapsEarlier(p) = some match ends strictly inside A's span:
//                        matchEnd(p-k) AND La > k  (i.e. k < La).    (A's own length)
//   overlap(p)      = matchEnd(p) AND (overlapsLater OR overlapsEarlier)
//   independent(p)  = matchEnd(p) AND NOT (overlapsLater OR overlapsEarlier)
//
// Forward peek = LookAhead (inputs declare LookAhead(maxLen-1)); backward = Advance.
// The two output match-end streams partition the input matchEnd exactly.
class BPEOverlapSplit : public PabloKernel {
public:
    BPEOverlapSplit(LLVMTypeSystemInterface & ts,
                    StreamSet * matchEndIn, StreamSet * vocabIDIn, StreamSet * matchLenIn,
                    StreamSet * overlapEnd, StreamSet * overlapID,
                    StreamSet * indepEnd,   StreamSet * indepID,
                    unsigned maxLen)
    : PabloKernel(ts,
                  "BPEOverlapSplit_m" + std::to_string(maxLen),
                  {Binding{"matchEndIn", matchEndIn, FixedRate(), LookAhead(maxLen - 1)},
                   Binding{"vocabIDIn",  vocabIDIn},
                   Binding{"matchLenIn", matchLenIn, FixedRate(), LookAhead(maxLen - 1)}},
                  {Binding{"overlapEnd", overlapEnd}, Binding{"overlapID", overlapID},
                   Binding{"indepEnd",   indepEnd},   Binding{"indepID",   indepID}}),
      mMaxLen(maxLen) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        // For each match ending at position p, look forward to every match ending at p+k (k≥1) and check if it overlaps p. 
        // Look backward to every match ending at p-k (k≥1) and check if it overlaps p. 
        // If either overlaps, p is in the overlap group; else it's independent.

        PabloAST * me0 = getInputStreamSet("matchEndIn")[0];
        std::vector<PabloAST*> idIn      = getInputStreamSet("vocabIDIn");
        std::vector<PabloAST*> lenInBits = getInputStreamSet("matchLenIn");
        const unsigned lenBits = static_cast<unsigned>(lenInBits.size());
        BixNum lenIn(lenInBits.begin(), lenInBits.end());

        // A match ending later (p+k) overlaps p iff its own length > k.
        PabloAST * later = pb.createZeroes();
        // LookAhead is legal since inputs declare LookAhead(maxLen-1).
        for (unsigned k = 1; k < mMaxLen; k++) {
            PabloAST * futEnd = pb.createLookahead(me0, k);
            std::vector<PabloAST*> fBits;
            fBits.reserve(lenBits);
            // LookAhead every bit of matchLen at p+k, compare the whole thing to k via UGT. If matchLen(p+k) > k, 
            // then the future match overlaps p and p is not independent.
            for (unsigned i = 0; i < lenBits; i++)
                fBits.push_back(pb.createLookahead(lenInBits[i], k));
            BixNum futLen(fBits.begin(), fBits.end());
            later = pb.createOr(later, pb.createAnd(futEnd, bnc.UGT(futLen, k)));
        }

        // A match ending earlier (p-k) overlaps p iff THIS match's length > k.
        PabloAST * earlier = pb.createZeroes();
        for (unsigned k = 1; k < mMaxLen; k++) {
            PabloAST * pastEnd = pb.createAdvance(me0, k);
            earlier = pb.createOr(earlier, pb.createAnd(pastEnd, bnc.UGT(lenIn, k)));
        }

        // overlap = me0 AND (later OR earlier); independent = me0 AND NOT (later OR earlier).
        PabloAST * touches = pb.createOr(later, earlier);
        PabloAST * overlap = pb.createAnd(me0, touches);
        PabloAST * indep   = pb.createAnd(me0, pb.createNot(touches));

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("overlapEnd"), pb.getInteger(0)), overlap);
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("indepEnd"), pb.getInteger(0)), indep);

        Var * ovID = getOutputStreamVar("overlapID");
        Var * inID = getOutputStreamVar("indepID");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(pb.createExtract(ovID, pb.getInteger(i)),
                            pb.createAnd(idIn[i], overlap));
            pb.createAssign(pb.createExtract(inID, pb.getInteger(i)),
                            pb.createAnd(idIn[i], indep));
        }
    }

private:
    unsigned mMaxLen;
};


// ─── BPEResolveOverlaps — LONGEST-WINS inside the overlap group ──────────────
//
// Efficient revival of the old (OOM-killed) BPELengthResolve: same rule — kill A
// if a STRICTLY LONGER match overlaps A's span — but O(maxLen·lenBits) instead of
// O(#lengths²·maxLen). Runs only on the overlap stream. matchLen is the FULL
// length stream; every read is gated on overlapEnd, so independent matches
// (overlapEnd=0) never contribute — sound because an independent match shares no
// byte with any overlap match (else it would be in the overlap group).
//
//   coverLenMax[i] = max over k in [0..maxLen-1] of
//                    (overlapEnd(i+k) AND matchLen(i+k) > k) ? matchLen(i+k) : 0
//   killed[p]      = OR over j in [0..maxLen-1] of
//                    (matchLen(p) > j) AND (coverLenMax(p-j) > matchLen(p))
//   keep           = overlapEnd AND NOT killed
//
// Straddle: ĠMun@6(L5) span{2..6} coverLenMax=5 → survives; uni@7(L3) span{5,6,7}
// coverLenMax[5]=5>3 → killed; iza@9(L3) survives. Longest wins.
class BPEResolveOverlaps : public PabloKernel {
public:
    BPEResolveOverlaps(LLVMTypeSystemInterface & ts,
                       StreamSet * overlapEnd, StreamSet * overlapID, StreamSet * matchLen,
                       StreamSet * resEnd, StreamSet * resID,
                       unsigned maxLen)
    : PabloKernel(ts,
                  "BPEResolveOverlaps_m" + std::to_string(maxLen),
                  {Binding{"overlapEnd", overlapEnd, FixedRate(), LookAhead(maxLen - 1)},
                   Binding{"overlapID",  overlapID},
                   Binding{"matchLen",   matchLen, FixedRate(), LookAhead(maxLen - 1)}},
                  {Binding{"resEnd", resEnd}, Binding{"resID", resID}}),
      mMaxLen(maxLen) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        PabloAST * me0 = getInputStreamSet("overlapEnd")[0];
        std::vector<PabloAST*> idIn      = getInputStreamSet("overlapID");
        std::vector<PabloAST*> lenInBits = getInputStreamSet("matchLen");
        const unsigned lenBits = static_cast<unsigned>(lenInBits.size());
        BixNum lenIn(lenInBits.begin(), lenInBits.end());

        // Stage A: coverLenMax[i] = longest OVERLAP match covering byte i.
        BixNum coverLenMax = bnc.Select(me0, lenIn, 0u);
        for (unsigned k = 1; k < mMaxLen; k++) {
            PabloAST * futEnd = pb.createLookahead(me0, k);
            std::vector<PabloAST*> fBits;
            fBits.reserve(lenBits);
            for (unsigned i = 0; i < lenBits; i++)
                fBits.push_back(pb.createLookahead(lenInBits[i], k));
            BixNum futLen(fBits.begin(), fBits.end());
            PabloAST * covers = pb.createAnd(futEnd, bnc.UGT(futLen, k));
            BixNum cand = bnc.Select(covers, futLen, 0u);
            coverLenMax = bnc.Select(bnc.UGT(cand, coverLenMax), cand, coverLenMax);
        }

        // Stage B: kill A if a strictly longer overlap match covers a byte of its span.
        PabloAST * killed = pb.createZeroes();
        for (unsigned j = 0; j < mMaxLen; j++) {
            std::vector<PabloAST*> shBits;
            shBits.reserve(lenBits);
            for (unsigned i = 0; i < lenBits; i++)
                shBits.push_back(j == 0 ? coverLenMax[i]
                                        : pb.createAdvance(coverLenMax[i], j));
            BixNum covShift(shBits.begin(), shBits.end());
            PabloAST * inSpan       = bnc.UGT(lenIn, j);
            PabloAST * longerCovers = bnc.UGT(covShift, lenIn);
            killed = pb.createOr(killed, pb.createAnd(inSpan, longerCovers));
        }

        PabloAST * keep = pb.createAnd(me0, pb.createNot(killed));
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("resEnd"), pb.getInteger(0)), keep);
        Var * resID = getOutputStreamVar("resID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(resID, pb.getInteger(i)),
                            pb.createAnd(idIn[i], keep));
    }

private:
    unsigned mMaxLen;
};


// ─── BPEMergeFinal — OR the two groups back together ─────────────────────────
//
// independent (loners, untouched) OR resolved-overlap survivors. The two
// match-end streams are disjoint (BPEOverlapSplit partitions matchEnd), so
// plain OR is exact.
class BPEMergeFinal : public PabloKernel {
public:
    BPEMergeFinal(LLVMTypeSystemInterface & ts,
                  StreamSet * indepEnd, StreamSet * indepID,
                  StreamSet * resEnd,   StreamSet * resID,
                  StreamSet * finalEnd, StreamSet * finalID)
    : PabloKernel(ts, "BPEMergeFinal",
                  {Binding{"indepEnd", indepEnd}, Binding{"indepID", indepID},
                   Binding{"resEnd",   resEnd},   Binding{"resID",   resID}},
                  {Binding{"finalEnd", finalEnd}, Binding{"finalID", finalID}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * inEnd = getInputStreamSet("indepEnd")[0];
        PabloAST * rEnd  = getInputStreamSet("resEnd")[0];
        std::vector<PabloAST*> inID = getInputStreamSet("indepID");
        std::vector<PabloAST*> rID  = getInputStreamSet("resID");

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("finalEnd"), pb.getInteger(0)),
            pb.createOr(inEnd, rEnd));
        Var * fID = getOutputStreamVar("finalID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(fID, pb.getInteger(i)),
                            pb.createOr(inID[i], rID[i]));
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

// # 03
// ─── Pipeline ────────────────────────────────────────────────────────────────
// buildBPEPassPipeline assembles the length-grouped, longest-wins BPE pipeline:
//   1. One BPETokenDetect kernel per distinct token length L. All kernels
//      read `basis` only and run independently; each emits
//      (matchEnd_L, vocabID_L, matchLen_L).
//   2. A chain of BPELengthFirstWinsMerge kernels folds the length groups in
//      DESCENDING length order into one (matchEnd, vocabID, matchLen). At any
//      single end position the longest match wins (fold order — no compare).
//   3. One BPEAssembleKernel drops matches contained in a longer match ending
//      later, via a single O(maxLen) LookAhead sweep over matchLen. Produces
//      the final (matchEnd, vocabID).
//
// This replaces the old monolithic BPELengthResolve, whose single kernel held
// O(#lengths^2 * maxLen) ≈ 47k Pablo ops on the full GPT-2 vocab and OOM-killed
// the JIT. The biggest kernel here is BPEAssemble at O(maxLen) ≈ 5k ops.
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {
    auto groups = bpe.buildLengthGroups();   // ascending by length

    if (groups.empty()) {
        std::cerr << "BPE: vocabulary empty; emitting zero streams\n";
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        return {me, id};
    }

    // Per-length detect kernels (parallel), each emitting (me, id, len).
    std::vector<unsigned> lengths;
    std::vector<StreamSet*> mes, ids, lens;
    lengths.reserve(groups.size());
    mes.reserve(groups.size());
    ids.reserve(groups.size());
    lens.reserve(groups.size());

    // Walk groups in ascending length order. 
    for (auto & g : groups) {
        StreamSet * me  = P.CreateStreamSet(1, 1);
        StreamSet * id  = P.CreateStreamSet(16, 1);
        StreamSet * len = P.CreateStreamSet(8, 1);
        uint64_t shape = hashTokenSet(g.tokens);
        // kernel creation encodes both length and token set hash in the name → distinct cache
        P.CreateKernelCall<BPETokenDetect>(
            basis, me, id, len, g.length, std::move(g.tokens), shape);
        lengths.push_back(g.length);
        mes.push_back(me);
        ids.push_back(id);
        lens.push_back(len);
    }

    std::cerr << "BPE: length-grouped detection — " << groups.size()
              << " length kernels (";
    for (size_t i = 0; i < lengths.size(); i++) {
        if (i) std::cerr << ",";
        std::cerr << lengths[i];
    }
    std::cerr << ")\n";

    // FirstWins fold in DESCENDING length order. groups is ascending, so walk
    // it in reverse; the accumulator always holds the longer-length side, so a
    // shorter match only fills end positions the longer one left empty.
    const size_t N = groups.size();
    StreamSet * curMe  = mes [N - 1];
    StreamSet * curId  = ids [N - 1];
    StreamSet * curLen = lens[N - 1];
    for (size_t i = N - 1; i-- > 0; ) {
        StreamSet * outMe  = P.CreateStreamSet(1, 1);
        StreamSet * outId  = P.CreateStreamSet(16, 1);
        StreamSet * outLen = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BPELengthFirstWinsMerge>(
            curMe, curId, curLen,        // side 1 = accumulated (longer)
            mes[i], ids[i], lens[i],     // side 2 = next shorter length
            outMe, outId, outLen);
        curMe  = outMe;
        curId  = outId;
        curLen = outLen;
    }

    // Single-byte vocab → no two matches can overlap → everything independent.
    unsigned maxLen = bpe.maxTokenByteLen();
    if (maxLen < 2)
        return {curMe, curId};

    // Split matches: overlap group vs independent loners.
    StreamSet * overlapEnd = P.CreateStreamSet(1, 1);
    StreamSet * overlapID  = P.CreateStreamSet(16, 1);
    StreamSet * indepEnd   = P.CreateStreamSet(1, 1);
    StreamSet * indepID    = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPEOverlapSplit>(
        curMe, curId, curLen,
        overlapEnd, overlapID, indepEnd, indepID, maxLen);

    // test the two groups of overlaps and the independent vocab
    // TEMP visualization: emit ONE group so we can eyeball the split.
    //   true  → overlap group (tangled matches, unresolved)
    //   false → independent group (loners)
    // const bool SHOW_OVERLAP_GROUP = true;
    // if (SHOW_OVERLAP_GROUP)
    //     return {overlapEnd, overlapID};
    // return {indepEnd, indepID};

    // Resolve the overlap group by LONGEST-WINS (loners need no resolution).
    StreamSet * resEnd = P.CreateStreamSet(1, 1);
    StreamSet * resID  = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPEResolveOverlaps>(
        overlapEnd, overlapID, curLen, resEnd, resID, maxLen);

    // Final tiling = independent loners OR resolved-overlap survivors (disjoint → OR).
    StreamSet * finalMe = P.CreateStreamSet(1, 1);
    StreamSet * finalId = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPEMergeFinal>(
        indepEnd, indepID, resEnd, resID, finalMe, finalId);
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

// # 01
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

// Largest token byte length — sets the BPEAssemble LookAhead window.
unsigned BPETokenizer::maxTokenByteLen() const {
    unsigned m = 0;
    for (const auto & [tok, id] : vocab_) {
        (void)id;
        if (tok.size() > m) m = static_cast<unsigned>(tok.size());
    }
    return m;
}
// # 02
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
    // Each BPETokenDetect emits (matchEnd_L, vocabID_L) for every token of length L
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


// flatenning the if structure 
// Master/lib/re/compile//re_compiler  - 28, 603 making the if structure depending on the Gap
// kernel for the particular vocab
// then the trie logic that goes through the whole vocab + the overlap and mask at tt