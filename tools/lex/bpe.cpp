/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — pass design (conflict-ordered passes + consumed mask).
 *
 *  Algorithm
 *  ─────────
 *  Preprocessing (buildVocabPasses, all before any kernel):
 *      Partition the vocab into priority PASSES. Priority = LOWER vocab id wins.
 *      Two words "can overlap" if their match spans could intersect in some
 *      input (containment, or a suffix of one == a prefix of the other — a
 *      static, input-free relation). A word is PLACED in the current pass iff no
 *      lower-id word it can overlap remains; else DEFERRED to the next pass.
 *      Repeat on the leftovers until none remain. Single bytes (length 1) are
 *      not partitioned (the byte-level fallback).
 *
 *  Runtime pipeline (one chain per pass p, length i = max..2):
 *      BPETokenDetect — detect every V[p][i] match end-anchored (vocabID).
 *      BPEMaskGate    — drop any match whose span [e-L+1..e] touches a byte
 *                       already consumed by an earlier (higher-priority) kernel.
 *      BPEOccupy      — mark the surviving matches' spans consumed.
 *      The consumed mask is chained kernel→kernel; overlaps resolve purely by
 *      pass/length run order + the mask, so the kernels carry NO overlap logic.
 *      BPEFinalOr ORs every pass's survivors into the final (matchEnd, vocabID).
 *
 *  Cache keys
 *  ──────────
 *  BPETokenDetect name encodes (length L, hashTokenSet(tokens of length L)).
 *  Two vocab files with the same length distribution but different tokens
 *  still produce distinct cache entries.
 *
 *  NOTE: buildVocabPasses is O(n^2) — fine for dev/test vocabs, needs indexing
 *  before the full 50k vocab. it is hang? 
 */

#include "bpe.h"
#include <algorithm>
#include <cstdlib>
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
#include <kernel/bitwise/bixlogic.h>
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
//
// for each position p:
//     check backwards:
//         does basis[p - k] == token_char[L - 1 - k]?
//     if all L bytes match:
//         mark matchEnd[p] = 1
//         emit vocabID[p]
//
// This kernel does NOT suppress conflicts. In the pass design, BPEMaskGate
// drops matches whose span hits an already-consumed byte and BPEOccupy marks
// the survivors' spans — overlap resolution is pass/length order + the mask.
//
// produce vocabID bixnum
//
class BPETokenDetect : public PabloKernel {
public:
    BPETokenDetect(LLVMTypeSystemInterface & ts,
                    StreamSet * basis,
                    StreamSet * matchEnd,
                    StreamSet * vocabID,
                    unsigned length,
                    std::vector<std::pair<std::string,unsigned>> tokens,
                    uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPETokenDetect_L" + std::to_string(length)
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
        // trie 
        // Loop over every token in this length group. Insert it into the trie, 
        // reversed (last byte at depth 1, first byte at depth L).
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
    }

private:
    std::vector<std::pair<std::string,unsigned>> mTokens;
};


// ─── Pass-design runtime kernels   ────────────────────────────────
// ZeroMaskKernel — produces an all-zero 1×1 consumed-mask to seed the chain.
class ZeroMaskKernel : public PabloKernel {
public:
    ZeroMaskKernel(LLVMTypeSystemInterface & ts, StreamSet * basis, StreamSet * zeroMask)
    : PabloKernel(ts, "BPE_ZeroMask",
                  {Binding{"basis", basis}},          // for rate only; unused
                  {Binding{"zeroMask", zeroMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        pb.createAssign(pb.createExtract(getOutputStreamVar("zeroMask"), pb.getInteger(0)),
                        pb.createZeroes());
    }
};
// ─── Allowed Matches ─────────────────────────────────────────────────────
// filters detected token matches using already-consumed bytes
// BPEMaskGate — drop any L-byte match whose span [e-L+1 .. e] touches an
// already-consumed byte. consumedIn read BACKWARD via Advance (legal on input).
class BPEMaskGate : public PabloKernel {
public:
    BPEMaskGate(LLVMTypeSystemInterface & ts,
                StreamSet * rawEnd, StreamSet * vocabID, StreamSet * consumedIn,
                StreamSet * validEnd, StreamSet * validID, unsigned length)
    : PabloKernel(ts, "BPEMaskGate_L" + std::to_string(length),
                  {Binding{"rawEnd", rawEnd}, Binding{"vocabID", vocabID},
                   Binding{"consumedIn", consumedIn}},
                  {Binding{"validEnd", validEnd}, Binding{"validID", validID}}),
      mLength(length) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * rawEnd     = getInputStreamSet("rawEnd")[0];
        std::vector<PabloAST*> vid = getInputStreamSet("vocabID");
        PabloAST * consumedIn = getInputStreamSet("consumedIn")[0];

        PabloAST * hit = pb.createZeroes();
        for (unsigned k = 0; k < mLength; k++)
            hit = pb.createOr(hit, k == 0 ? consumedIn : pb.createAdvance(consumedIn, k));
        PabloAST * validEnd = pb.createAnd(rawEnd, pb.createNot(hit));

        pb.createAssign(pb.createExtract(getOutputStreamVar("validEnd"), pb.getInteger(0)), validEnd);
        Var * vOut = getOutputStreamVar("validID");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(vOut, pb.getInteger(i)), pb.createAnd(vid[i], validEnd));
    }
private:
    unsigned mLength;
};
// ─── Mask the consumed bytes ─────────────────────────────────────────────────────
// marks the byte ranges of accepted tokens as consumed in the global mask.
// BPEOccupy — mark the span of every surviving L-byte match as consumed.
// validEnd is an INPUT, so the forward peek is a legal LookAhead.
class BPEOccupy : public PabloKernel {
public:
    BPEOccupy(LLVMTypeSystemInterface & ts,
              StreamSet * validEnd, StreamSet * consumedIn, StreamSet * consumedOut,
              unsigned length)
    : PabloKernel(ts, "BPEOccupy_L" + std::to_string(length),
                  {Binding{"validEnd", validEnd, FixedRate(), LookAhead(length - 1)},
                   Binding{"consumedIn", consumedIn}},
                  {Binding{"consumedOut", consumedOut}}),
      mLength(length) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * validEnd   = getInputStreamSet("validEnd")[0];
        PabloAST * consumedIn = getInputStreamSet("consumedIn")[0];

        PabloAST * validStart = pb.createLookahead(validEnd, mLength-1);

        PabloAST * fill = pb.createIntrinsicCall(pablo::Intrinsic::InclusiveSpan, {validStart, validEnd});

        pb.createAssign(pb.createExtract(getOutputStreamVar("consumedOut"), pb.getInteger(0)),
                        pb.createOr(consumedIn, fill));
    }
private:
    unsigned mLength;
};
// ─── BPEByteFallback — single-byte tokens for uncovered bytes ────────────────
// After all passes, every byte not under a length>=2 match is uncovered. Emit
// that byte's own 1-byte vocab token there. 
//   uncovered = NOT consumed
//   fbEnd     = uncovered AND (byte b has a 1-byte token)
//   fbID      = byteIds[basis]  (masked by uncovered)
// Fires only on uncovered bytes → disjoint from every pass survivor (which ends
// on a consumed byte), so BPEFinalOr's OR stays exact.
class BPEByteFallback : public PabloKernel {
public:
    BPEByteFallback(LLVMTypeSystemInterface & ts,
                    StreamSet * basis, StreamSet * consumed,
                    StreamSet * fbEnd, StreamSet * fbID,
                    std::vector<int> byteIds)
    : PabloKernel(ts, "BPEByteFallback",
                  {Binding{"basis", basis}, Binding{"consumed", consumed}},
                  {Binding{"fbEnd", fbEnd}, Binding{"fbID", fbID}}),
      mByteIds(std::move(byteIds)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum symBN(basisBits.begin(), basisBits.end());
        PabloAST * uncovered = pb.createNot(getInputStreamSet("consumed")[0]);

        PabloAST * zeroes = pb.createZeroes();
        std::vector<PabloAST*> idBits(16, zeroes);
        PabloAST * hasTok = zeroes;
        // Loop over every possible byte value b = 0..255. If b has a 1-byte token, 
        // OR its byte-eq into hasTok and OR its ID bits into idBits. 
        // Mask all outputs by uncovered, so this kernel only fires on bytes with no length>=2 match.
        for (unsigned b = 0; b < 256; b++) {
            int v = mByteIds[b];
            if (v < 0) continue;
            PabloAST * eq = bnc.EQ(symBN, b);
            hasTok = pb.createOr(hasTok, eq);
            for (unsigned bit = 0; bit < 16; bit++)
                if ((v >> bit) & 1) idBits[bit] = pb.createOr(idBits[bit], eq);
        }
        pb.createAssign(pb.createExtract(getOutputStreamVar("fbEnd"), pb.getInteger(0)),
                        pb.createAnd(hasTok, uncovered));
        Var * fbID = getOutputStreamVar("fbID");
        // Each bit of the output ID is set iff the byte has a 1-byte token with that bit set, 
        // AND this byte is uncovered (not consumed by any length>=2 match).
        for (unsigned bit = 0; bit < 16; bit++)
            pb.createAssign(pb.createExtract(fbID, pb.getInteger(bit)),
                            pb.createAnd(idBits[bit], uncovered));
    }
private:
    std::vector<int> mByteIds;
};


// ─── InvertStreamKernel ─────────────────────────────────────────────────────
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
// Inline pretokenizer for compare_bpe.py. Marks each newline (0x0A)
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

// ─── Pipeline (pass design) ──────────────────────────────────────────────────
// buildBPEPassPipeline
//   1. buildVocabPasses() partitions the vocab into priority passes VP[p][i]
//      (preprocessing; lower vocab id = higher priority).
//   2. For each pass p (0..P), length i (max..2): BPETokenDetect → BPEMaskGate
//      (drop matches whose span hits an already-consumed byte) → BPEOccupy (mark
//      the surviving span consumed). The consumed mask chains kernel→kernel, so
//      overlaps resolve by pass/length order alone — no overlap logic in kernels.
//   3. BPEFinalOr ORs every pass's survivors into the final (matchEnd, vocabID).
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {

    // Pass design
    // Preprocessing (buildVocabPasses) runs before any kernel; resolution is
    // pass/length order + the consumed mask, so kernels carry no overlap logic.
    auto passes = bpe.buildVocabPasses();

    // Print the pass partition: which lengths/counts land in each pass.
    std::cerr << "[BPE] " << passes.size() << " passes\n";
    for (size_t p = 0; p < passes.size(); ++p) {
        size_t total = 0;
        std::cerr << "  pass " << p << ":";
        for (const auto & g : passes[p].byLength) {
            std::cerr << " L" << g.length << "x" << g.tokens.size();
            total += g.tokens.size();
        }
        std::cerr << "  (" << total << " tokens)\n";
    }

    // Seed an all-zero consumed mask.
    StreamSet * consumed = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<ZeroMaskKernel>(basis, consumed);

    // For pass p = 0..P, length max..2 (byLength already DESC):
    //   detect → gate (skip consumed bytes) → occupy (mark this match's span).
    std::vector<StreamSet*> ends, ids;
    for (auto & vp : passes) {
        for (auto & g : vp.byLength) {
            unsigned L = g.length;
            uint64_t shape = hashTokenSet(g.tokens);

            StreamSet * rawEnd = P.CreateStreamSet(1, 1);
            StreamSet * vid    = P.CreateStreamSet(16, 1);
            P.CreateKernelCall<BPETokenDetect>(
                basis, rawEnd, vid, L, g.tokens, shape);

            StreamSet * validEnd = P.CreateStreamSet(1, 1);
            StreamSet * validID  = P.CreateStreamSet(16, 1);
            P.CreateKernelCall<BPEMaskGate>(rawEnd, vid, consumed, validEnd, validID, L);

            StreamSet * consumedNext = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<BPEOccupy>(validEnd, consumed, consumedNext, L);
            consumed = consumedNext;

            ends.push_back(validEnd);
            ids.push_back(validID);
        }
    }

    // Single-byte fallback: emit a 1-byte token at every byte still uncovered by
    // a length>=2 match. `consumed` holds the final mask after all passes (or the
    // zero seed if there were none → pure byte-level output).
    StreamSet * fbEnd = P.CreateStreamSet(1, 1);
    StreamSet * fbID  = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPEByteFallback>(basis, consumed, fbEnd, fbID, bpe.singleByteIds());
    ends.push_back(fbEnd);
    ids.push_back(fbID);

    if (ends.size() == 1) return {ends[0], ids[0]};

    // N-ary OR via a fold of library OrCombine kernels. Survivors are byte-disjoint
    // (consumed mask + fallback's uncovered gate guarantee it), so plain OR is exact.
    // OrCombine ORs all 16 ID streams in one call (it loops over streams).
    StreamSet * accEnd = ends[0];
    StreamSet * accId  = ids[0];
    for (size_t i = 1; i < ends.size(); ++i) {
        StreamSet * nextEnd = P.CreateStreamSet(1, 1);
        OrCombine(P, accEnd, ends[i], nextEnd);
        accEnd = nextEnd;

        StreamSet * nextId = P.CreateStreamSet(16, 1);
        OrCombine(P, accId, ids[i], nextId);
        accId = nextId;
    }
    return {accEnd, accId};
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

// byte value b -> id of the single-byte token "b" (-1 if the vocab has none).
std::vector<int> BPETokenizer::singleByteIds() const {
    std::vector<int> t(256, -1);
    for (const auto & [tok, id] : vocab_)
        if (tok.size() == 1) t[static_cast<uint8_t>(tok[0])] = id;
    return t;
}

// ─── Pass partitioning ────────────────────────────
// PROPER overlap = the two spans CROSS: a suffix of one equals a prefix of the
// other, and each token sticks out past the shared region on opposite ends.
// The tokens share some bytes but neither contains the other.
static bool canOverlap(const std::string & a, const std::string & b) {
    const int la = static_cast<int>(a.size());
    const int lb = static_cast<int>(b.size());
    for (int d = -(lb - 1); d <= la - 1; d++) {
        const int lo = std::max(0, d);
        const int hi = std::min(la, d + lb);
        if (lo >= hi) continue;                       // windows don't intersect here
        // Skip containment: one window fully inside the other = nesting, not a cross.
        const bool aContainsB = (d >= 0) && (d + lb <= la);
        const bool bContainsA = (d <= 0) && (d + lb >= la);
        if (aContainsB || bContainsA) continue;
        bool agree = true;
        for (int x = lo; x < hi; x++)
            if (a[x] != b[x - d]) { agree = false; break; }
        if (agree) return true;                       // proper crossing with shared bytes
    }
    return false;
}

// ─── Preprocessing ────────────────────────────
// Literal 4-loop partition (length-stratified). Priority = LOWER vocab id wins.
// For each pass: for length i = maxLen..2, for each w in V[i], scan w2 in V[k]
// for k = 2..i; if a lower-id w2 (k <= i) can overlap w, DEFER w (RVP), else
// PLACE w (VP). Repeat on the deferred leftovers (RV) until empty.
std::vector<VocabPass> BPETokenizer::buildVocabPasses() const {
    // W = word + id tuple for partitioning.
    struct W { std::string tok; unsigned id; };

    // V = vocabulary. This design partitions only length >= 2,  single-byte tokens are the universal fallback,
    // remaining -> FULL VOCABULARY
    // make the vocabulary 
    std::vector<W> remaining;
    for (const auto & [tok, id] : vocab_)
        if (tok.size() >= 2) remaining.push_back({tok, static_cast<unsigned>(id)});

    // Result is a sequence of passes, each with a VP (placed tokens) and RVP (deferred tokens). Each pass's VP is grouped by length descending for longest-first emit order.
    // VP = tokens that PASS the overlap rule
    // RVP = tokens that FAIL the overlap rule 
    std::vector<VocabPass> passes;
    while (!remaining.empty()) {                 // Repeat for V = RV
        // V[i] = { w in remaining : length(w) == i }
        std::unordered_map<unsigned, std::vector<W>> V;
        unsigned maxLen = 2;

        // Group remaining tokens by length. Track maxLen for the loop bounds.
        for (const W & w : remaining) {
            unsigned L = static_cast<unsigned>(w.tok.size());
            V[L].push_back(w);
            if (L > maxLen) maxLen = L;
        }
        // Vof(len) is a safe accessor that returns all tokens of length len from V, 
        // or an empty vector if none exist, preventing map lookup errors and simplifying iteration logic.
        auto Vof = [&](unsigned len) -> const std::vector<W> & {
            static const std::vector<W> empty;
            auto it = V.find(len);
            return (it == V.end()) ? empty : it->second;
        };

        std::vector<W> placed, deferred;          // VP[p][*]  and  RVP[p][*]

        // For each length i from maxLen downto 2:
        for (int i = static_cast<int>(maxLen); i >= 2; --i) {  // walk lengths in descending order for longest-first priority
            //   for each w in V[i]: - iterate over all token of length i
            for (const W & w : Vof(static_cast<unsigned>(i))) {
                // checks whether w gets blocked by a higher-priority overlapping token
                bool overlapFound = false;
                //   for each length k <= i (k >= 2):
                for (int k = 2; k <= i && !overlapFound; ++k) {
                    //   for each w2 in V[k]:
                    for (const W & w2 : Vof(static_cast<unsigned>(k))) {
                        //   w2 higher priority (lower id) AND w overlaps w2 → defer w.
                        if (w2.id < w.id && canOverlap(w.tok, w2.tok)) {
                            deferred.push_back(w);          // put w in RVP[p][i] (overlapped)
                            overlapFound = true;
                            break;
                        }
                    }
                }
                if (!overlapFound) placed.push_back(w);     // put w in VP[p][i] (no overlap)
            }
        }

        // Group VP[p] by length, descending (longest-first for the mask order).
        std::unordered_map<unsigned, std::vector<std::pair<std::string,unsigned>>> byLen;
        // Walk the placed tokens and bucket them by length. 
        // Each VP[p][i] is a vector of tokens of length i that passed the overlap test for this pass.
        for (const W & w : placed)
            byLen[static_cast<unsigned>(w.tok.size())].push_back({w.tok, w.id});
        VocabPass vp;
        // Walk the length buckets in byLen and sort each bucket's tokens by byte content for stable JIT cache keys, then push them into vp.byLength.
        for (auto & [L, toks] : byLen) {
            std::sort(toks.begin(), toks.end(),
                      [](const auto & a, const auto & b) { return a.first < b.first; });
            vp.byLength.push_back(LengthGroup{L, std::move(toks)});
        }
        // sort largest length → smallest length
        std::sort(vp.byLength.begin(), vp.byLength.end(),
                  [](const LengthGroup & a, const LengthGroup & b) { return a.length > b.length; });
        passes.push_back(std::move(vp));

        remaining.swap(deferred);                 // RV = leftovers
    }
    return passes;
}


// passes from longest to shortest?
// longest wins priority? not needed 
// no subset overlap ? TODO?
// each kernel one pass and each pass is of a specific length ?
// kernel generation should make one kernel for each pass - of each length group in the pass design. 
// Each kernel gets the tokens for that pass+length, generates the trie, and detects them all together. The kernels are mutually independent, so no overlap logic is needed in them — the pass design guarantees that within one pass, no two tokens can match at the same end position (their byte values would have to be identical), so the first match is the only match. The mask gate and occupy kernels then resolve overlaps across passes by pass/length order + the mask, so no overlap logic is needed in them either.