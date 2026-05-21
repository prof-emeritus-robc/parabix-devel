/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — vocabulary-trie longest-match scan (byte mode).
 *
 *  Algorithm
 *  ─────────
 *  Vocab words of byte length >= 2 are bucketed by their first two bytes
 *  (b0, b1). One PabloKernel is emitted per (b0, b1) bucket. Inside the
 *  kernel a nested-scope trie walks the bucket's suffix bytes (byte index
 *  2 onward). Each trie level uses
 *
 *      childMark = Advance(parentMark, 1) & EQ(symBN, child_byte)
 *
 *  to extend the match one byte to the right. Pablo `createIf` on the
 *  child marker runtime-skips dead subtries.
 *
 *  Per-kernel longest match wins: deeper trie nodes overwrite the per-Var
 *  vocab-ID slots via createSel(parentMark, …, prevVal). Because each match
 *  fires at the right-most byte of its word, distinct word byte-lengths
 *  land at distinct end positions — overwrite competition is only between
 *  vocab words ending at the same position via this bucket's trie.
 *
 *  Why bytes, not codepoints
 *  ─────────────────────────
 *  GPT-2 vocab.json stores tokens as UTF-8 byte strings. Matching directly
 *  on bytes:
 *    * skips the UTF-8 → U21 decode stage (no applyNormalizationU21);
 *    * skips per-token decodeUTF8 in vocab loading;
 *    * cuts EQ cost from ~log2(21) AND-tree levels to ~log2(8);
 *    * matches HF tokenizers' internal representation exactly.
 *
 *  Phase-1 limitations
 *  ───────────────────
 *   - Single-byte vocab tokens are not in the trie; a fallback layer
 *     covering positions where no >=2-byte match fires is not wired.
 *   - Cross-bucket conflicts at the same end position are OR-merged. A
 *     subsequent resolution kernel (longest-across-buckets) is not yet
 *     wired here.
 */

#include "bpe.h"
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

// Rolling hash of a trie node — used to disambiguate JIT cache entries when
// two buckets share (b0, b1) across different vocab files.
uint64_t hashTrieNode(const TrieNode & n, uint64_t seed = 0) {
    seed ^= 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(n.vocabID + 1);
    seed = (seed << 13) | (seed >> 51);
    for (const auto & kv : n.children) {
        seed ^= static_cast<uint64_t>(kv.first) + 0x9E3779B97F4A7C15ull;
        seed = hashTrieNode(kv.second, seed);
    }
    return seed;
}

}

// ─── BPETrieKernel ──────────────────────────────────────────────────────────
//
// One PabloKernel per (b0, b1) byte bucket. Input: 8×1 basis bit streams of
// the byte-encoded text. Outputs: matchEnd (1×1, marks the right-most byte
// of every vocab word in this bucket that ended there) and vocabID (16×1
// BixNum, the matched word's ID at matchEnd positions; 0 elsewhere within
// this bucket's output).
class BPETrieKernel : public PabloKernel {
public:
    BPETrieKernel(LLVMTypeSystemInterface & ts,
                  StreamSet * basis,
                  StreamSet * matchEnd,
                  StreamSet * vocabID,
                  StreamSet * matchLen,
                  VocabBucket bucket,
                  unsigned tag,
                  uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPETrie_t" + std::to_string(tag)
                       + "_b" + std::to_string(bucket.b0)
                       + "_" + std::to_string(bucket.b1)
                       + "_h" + std::to_string(shapeHash),
                  {Binding{"basis", basis}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID},
                   Binding{"matchLen", matchLen}}),
      mBucket(std::move(bucket)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        // symBN is the 8-bit BixNum of the input byte stream; used for
        // matching against b0 and b1 in the trie walk.
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum symBN(basisBits.begin(), basisBits.end());

        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        Var * matchEndV = pb.createVar("matchEnd", zeroes);
        std::vector<Var*> idBits;
        idBits.reserve(16);
        for (unsigned i = 0; i < 16; i++) {
            idBits.push_back(pb.createVar(
                "id_" + std::to_string(i), zeroes));
        }
        // matchLen: 8-bit BixNum, length in bytes of the matched word at
        // each end position. 0 elsewhere. Longest-wins semantics enforced
        // by overwriting deeper (longer) matches over shallower ones via
        // Sel(mark, …, current) — same pattern as idBits.
        std::vector<Var*> lenBits;
        lenBits.reserve(8);
        for (unsigned i = 0; i < 8; i++) {
            lenBits.push_back(pb.createVar(
                "len_" + std::to_string(i), zeroes));
        }

        // First match stage: check the bucket's (b0, b1) against the input
        // byte stream. Only positions where both match can possibly match
        // any token in this bucket.
        PabloAST * c0 = bnc.EQ(symBN, mBucket.b0);
        PabloAST * c1 = bnc.EQ(symBN, mBucket.b1);
        //  pairMark[p] = 1  iff  symBN[p-1]==b0 AND symBN[p]==b1
        //  → marks the b1 (right-hand) byte of every (b0,b1) pair.
        PabloAST * pairMark = pb.createAnd(
            pb.createAdvance(c0, 1), c1);

        auto pairScope = pb.createScope();
        pb.createIf(pairMark, pairScope);

        // 2-byte vocab word case: bucket's prefix IS itself a vocab token.
        if (mBucket.prefixVocabID >= 0) {
            recordMatch(pairScope, pairMark, mBucket.prefixVocabID, /*len=*/2,
                        matchEndV, idBits, lenBits, ones, zeroes);
        }
        // Walk the suffix trie for vocab words of byte length >= 3.
        // currentLen is the length of a word ending at parentMark = 2 here
        // (b0 + b1). emitTrie increments per recursion level.
        emitTrie(pairScope, pairMark, mBucket.root, /*currentLen=*/2, basisBits,
                 matchEndV, idBits, lenBits, ones, zeroes);

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEnd"),
                             pb.getInteger(0)),
            matchEndV);
        Var * idOut = getOutputStreamVar("vocabID");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                idBits[i]);
        }
        Var * lenOut = getOutputStreamVar("matchLen");
        for (unsigned i = 0; i < 8; i++) {
            pb.createAssign(
                pb.createExtract(lenOut, pb.getInteger(i)),
                lenBits[i]);
        }
    }

private:
    VocabBucket mBucket;

    static void recordMatch(PabloBuilder & pb, PabloAST * mark,
                            int vocabID, unsigned len,
                            Var * matchEndV,
                            std::vector<Var*> & idBits,
                            std::vector<Var*> & lenBits,
                            PabloAST * ones, PabloAST * zeroes) {
        pb.createAssign(matchEndV, pb.createOr(matchEndV, mark));
        for (unsigned i = 0; i < 16; i++) {
            unsigned bit = (static_cast<unsigned>(vocabID) >> i) & 1u;
            pb.createAssign(
                idBits[i],
                pb.createSel(mark, bit ? ones : zeroes, idBits[i]));
        }
        for (unsigned i = 0; i < 8; i++) {
            unsigned bit = (len >> i) & 1u;
            pb.createAssign(
                lenBits[i],
                pb.createSel(mark, bit ? ones : zeroes, lenBits[i]));
        }
    }

    // Recursive trie emitter — extends parentMark one byte to the right per
    // level and creates a guarded scope for each child branch.
    // currentLen: length of a word ending at parentMark. Each child match
    // has length currentLen + 1.
    static void emitTrie(PabloBuilder & pb, PabloAST * parentMark,
                         const TrieNode & node, unsigned currentLen,
                         const std::vector<PabloAST*> & basisBits,
                         Var * matchEndV,
                         std::vector<Var*> & idBits,
                         std::vector<Var*> & lenBits,
                         PabloAST * ones, PabloAST * zeroes) {
        if (node.children.empty()) return;
        BixNumCompiler bnc(pb);
        BixNum symBN(basisBits.begin(), basisBits.end());
        unsigned childLen = currentLen + 1;
        for (const auto & kv : node.children) {
            uint8_t b = kv.first;
            const TrieNode & child = kv.second;
            PabloAST * byteMark  = bnc.EQ(symBN, b);
            PabloAST * childMark = pb.createAnd(
                pb.createAdvance(parentMark, 1), byteMark);
            auto childScope = pb.createScope();
            pb.createIf(childMark, childScope);
            if (child.vocabID >= 0) {
                recordMatch(childScope, childMark, child.vocabID, childLen,
                            matchEndV, idBits, lenBits, ones, zeroes);
            }
            emitTrie(childScope, childMark, child, childLen, basisBits,
                     matchEndV, idBits, lenBits, ones, zeroes);
        }
    }
};


// ─── BPETriePairMergeKernel ─────────────────────────────────────────────────
//
// Longest-wins merge of two (matchEnd, vocabID, matchLen) bucket outputs.
// At positions where exactly one input fires, that input's tuple passes
// through. At positions where both fire, the input with the larger matchLen
// wins (ties go to input #1 — the caller folds buckets in deterministic
// order, so this gives stable cross-bucket disambiguation). Cache-keyed by
// name alone; semantics are bucket-independent.
class BPETriePairMergeKernel : public PabloKernel {
public:
    BPETriePairMergeKernel(LLVMTypeSystemInterface & ts,
                           StreamSet * me1, StreamSet * id1, StreamSet * len1,
                           StreamSet * me2, StreamSet * id2, StreamSet * len2,
                           StreamSet * meOut, StreamSet * idOut, StreamSet * lenOut)
    : PabloKernel(ts, "BPETriePairMerge",
                  {Binding{"me1", me1},   Binding{"id1", id1},  Binding{"len1", len1},
                   Binding{"me2", me2},   Binding{"id2", id2},  Binding{"len2", len2}},
                  {Binding{"meOut", meOut}, Binding{"idOut", idOut},
                   Binding{"lenOut", lenOut}}) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        PabloAST * me1 = getInputStreamSet("me1")[0];
        PabloAST * me2 = getInputStreamSet("me2")[0];
        std::vector<PabloAST*> id1  = getInputStreamSet("id1");
        std::vector<PabloAST*> id2  = getInputStreamSet("id2");
        std::vector<PabloAST*> len1 = getInputStreamSet("len1");
        std::vector<PabloAST*> len2 = getInputStreamSet("len2");
        BixNum lenBN1(len1.begin(), len1.end());
        BixNum lenBN2(len2.begin(), len2.end());

        // Tie-break to input #1: m1Wins on >= , m2Wins on strict >.
        PabloAST * ge = bnc.UGE(lenBN1, lenBN2);
        PabloAST * gt = bnc.UGT(lenBN2, lenBN1);
        PabloAST * m1Wins = pb.createAnd(me1, pb.createOr(pb.createNot(me2), ge));
        PabloAST * m2Wins = pb.createAnd(me2, pb.createOr(pb.createNot(me1), gt));

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("meOut"),
                             pb.getInteger(0)),
            pb.createOr(me1, me2));
        Var * idOut = getOutputStreamVar("idOut");
        for (unsigned i = 0; i < 16; i++) {
            // mux: m1Wins ? id1[i] : (m2Wins ? id2[i] : 0)
            PabloAST * picked = pb.createSel(m1Wins, id1[i],
                                   pb.createSel(m2Wins, id2[i],
                                                pb.createZeroes()));
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                picked);
        }
        Var * lenOut = getOutputStreamVar("lenOut");
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * picked = pb.createSel(m1Wins, len1[i],
                                   pb.createSel(m2Wins, len2[i],
                                                pb.createZeroes()));
            pb.createAssign(
                pb.createExtract(lenOut, pb.getInteger(i)),
                picked);
        }
    }
};


// ─── BPEAssembleKernel ──────────────────────────────────────────────────────
//
// Suppress matches that fall inside the span of a longer match ending later.
// Inputs: matchEnd, vocabID, matchLen — already longest-wins-merged across
// buckets. Output: keep_matchEnd, keep_vocabID — both zeroed at positions
// covered by a longer match's span.
//
// A match ending at position e with length L covers byte positions
// [e-L+1 .. e]. For each position p, p is covered by some future match iff
// there exists k in [1 .. maxLen-1] such that matchEnd(p+k) AND matchLen(p+k) > k.
// We unroll the LookAhead over k = 1..maxLen-1.
//
// We do NOT suppress matchEnd at its own end position (k=0 case), since by
// construction longest-wins already picked the right token there.
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

        PabloAST * coverMask = pb.createZeroes();

        for (unsigned k = 1; k < mMaxLen; k++) {
            PabloAST * futureEnd = pb.createLookahead(me0, k);
            std::vector<PabloAST*> futureLenBits;
            futureLenBits.reserve(8);
            for (unsigned i = 0; i < 8; i++) {
                futureLenBits.push_back(pb.createLookahead(lenIn[i], k));
            }
            BixNum futureLenBN(futureLenBits.begin(), futureLenBits.end());
            // p covered by match ending at p+k iff len(p+k) > k.
            PabloAST * covered_k = pb.createAnd(
                futureEnd, bnc.UGT(futureLenBN, k));
            coverMask = pb.createOr(coverMask, covered_k);
        }

        PabloAST * keep = pb.createAnd(me0, pb.createNot(coverMask));

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEndOut"),
                             pb.getInteger(0)),
            keep);
        Var * idOut = getOutputStreamVar("vocabIDOut");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                pb.createAnd(idIn[i], keep));
        }
    }

private:
    unsigned mMaxLen;
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
                             std::vector<PabloAST*>{pb.createNot(in)});
    }
};


// ─── LinePtBoundKernel ──────────────────────────────────────────────────────
//
// Inline pretokenizer for compare_bpe.py step 2. Marks each newline (0x0A)
// byte position so callers can FilterByMask it out of the downstream stream.
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
            pb.createExtract(getOutputStreamVar("newlineMask"),
                             pb.getInteger(0)),
            isNL);
    }
};


// ─── BPESingleByteKernel ────────────────────────────────────────────────────
//
// Treats every single-byte vocab token as a length-1 "bucket". For each
// (byte, vocabID) pair, EQ + Sel writes the vocab ID at matching positions
// and ORs into matchEnd. Output shape matches BPETrieKernel so the
// downstream pair-merge fold sees uniform tuples.
class BPESingleByteKernel : public PabloKernel {
public:
    BPESingleByteKernel(LLVMTypeSystemInterface & ts,
                        StreamSet * basis,
                        StreamSet * matchEnd,
                        StreamSet * vocabID,
                        StreamSet * matchLen,
                        std::vector<std::pair<unsigned,unsigned>> byteToVocab,
                        uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPE_SingleByte_h" + std::to_string(shapeHash),
                  {Binding{"basis", basis}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID},
                   Binding{"matchLen", matchLen}}),
      mByteToVocab(std::move(byteToVocab)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum byteBN(basisBits.begin(), basisBits.end());

        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        Var * matchEndV = pb.createVar("matchEnd", zeroes);
        std::vector<Var*> idBits;
        idBits.reserve(16);
        for (unsigned i = 0; i < 16; i++) {
            idBits.push_back(pb.createVar(
                "id_" + std::to_string(i), zeroes));
        }

        for (const auto & [byte, vid] : mByteToVocab) {
            PabloAST * match = bnc.EQ(byteBN, byte);
            pb.createAssign(matchEndV, pb.createOr(matchEndV, match));
            for (unsigned i = 0; i < 16; i++) {
                unsigned bit = (vid >> i) & 1u;
                pb.createAssign(
                    idBits[i],
                    pb.createSel(match, bit ? ones : zeroes, idBits[i]));
            }
        }

        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEnd"),
                             pb.getInteger(0)),
            matchEndV);
        Var * idOut = getOutputStreamVar("vocabID");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                idBits[i]);
        }
        // matchLen = 1 wherever matchEnd fires; 0 elsewhere. (binary 0000_0001)
        Var * lenOut = getOutputStreamVar("matchLen");
        pb.createAssign(
            pb.createExtract(lenOut, pb.getInteger(0)),
            matchEndV);
        for (unsigned i = 1; i < 8; i++) {
            pb.createAssign(
                pb.createExtract(lenOut, pb.getInteger(i)),
                zeroes);
        }
    }

private:
    std::vector<std::pair<unsigned,unsigned>> mByteToVocab;
};


// ─── Pipeline ─────────────────────────────────────────────────────────
// runBPETrie
//
// Treats single-byte vocab + every (b0, b1) trie bucket uniformly. Each
// bucket emits (matchEnd, vocabID, matchLen). Longest-wins pair-merge folds
// them into one combined stream. A final BPEAssembleKernel uses LookAhead
// over matchLen to suppress shorter matches that fall inside the span of a
// longer match ending later — so single-byte tokens emit only at positions
// not covered by a multi-byte vocab word.
BPETrieResult runBPETrie(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {
    auto byteMap = bpe.buildInitialVocabMap();
    auto buckets = bpe.buildVocabBuckets();

    StreamSet * curMe  = nullptr;
    StreamSet * curId  = nullptr;
    StreamSet * curLen = nullptr;

    // First "bucket" — single-byte vocab tokens.
    if (!byteMap.empty()) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto & [b, v] : byteMap) {
            h ^= (static_cast<uint64_t>(b) << 16) ^ static_cast<uint64_t>(v);
            h = (h << 13) | (h >> 51);
        }
        StreamSet * me  = P.CreateStreamSet(1, 1);
        StreamSet * id  = P.CreateStreamSet(16, 1);
        StreamSet * len = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BPESingleByteKernel>(
            basis, me, id, len, std::move(byteMap), h);
        curMe  = me;
        curId  = id;
        curLen = len;
    }

    // 2+ byte trie buckets.
    unsigned tag = 0;
    for (auto & bucket : buckets) {
        uint64_t shape = hashTrieNode(bucket.root,
            (static_cast<uint64_t>(bucket.b0) << 32) ^ bucket.b1
                ^ static_cast<uint64_t>(bucket.prefixVocabID + 1));

        StreamSet * me  = P.CreateStreamSet(1, 1);
        StreamSet * id  = P.CreateStreamSet(16, 1);
        StreamSet * len = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BPETrieKernel>(
            basis, me, id, len, std::move(bucket), tag, shape);
        tag++;
        if (curMe == nullptr) {
            curMe  = me;
            curId  = id;
            curLen = len;
            continue;
        }
        StreamSet * outMe  = P.CreateStreamSet(1, 1);
        StreamSet * outId  = P.CreateStreamSet(16, 1);
        StreamSet * outLen = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BPETriePairMergeKernel>(
            curMe, curId, curLen, me, id, len, outMe, outId, outLen);
        curMe  = outMe;
        curId  = outId;
        curLen = outLen;
    }
    std::cerr << "BPE: trie pipeline built with " << tag
              << " (b0,b1) buckets + single-byte bucket\n";

    if (curMe == nullptr) {
        // Empty vocab — emit zero streams.
        std::cerr << "BPE: vocabulary empty; emitting zero streams\n";
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        return {me, id};
    }

    // Final assembly: suppress positions inside a longer match's span.
    unsigned maxLen = bpe.maxTokenByteLen();
    if (maxLen < 2) {
        // Nothing longer than 1 byte exists — no assembly needed.
        return {curMe, curId};
    }
    if (getenv("BPE_NO_ASSEMBLE")) {
        std::cerr << "BPE: skipping assembly stage (BPE_NO_ASSEMBLE set)\n";
        return {curMe, curId};
    }
    StreamSet * outMe = P.CreateStreamSet(1, 1);
    StreamSet * outId = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPEAssembleKernel>(
        curMe, curId, curLen, outMe, outId, maxLen);
    return {outMe, outId};
}

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
// ─── BPETokenizer — file I/O ────────────────────────────────────────────────
// 1
//
// string → id
// id → string
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

std::string BPETokenizer::decodeToken(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= idToToken_.size()) return "";
    return idToToken_[static_cast<size_t>(id)];
}

// maxTokenByteLen — largest token byte length across the loaded vocab.
// Drives the LookAhead window in BPEAssembleKernel.
unsigned BPETokenizer::maxTokenByteLen() const {
    unsigned m = 0;
    for (const auto & [token, id] : vocab_) {
        (void)id;
        if (token.size() > m) m = static_cast<unsigned>(token.size());
    }
    return m;
}

// buildInitialVocabMap — list (byte, vocabID) for every single-byte vocab
// token. Drives InitialSymIDKernel's per-position fallback.
std::vector<std::pair<unsigned,unsigned>>
BPETokenizer::buildInitialVocabMap() const {
    std::vector<std::pair<unsigned,unsigned>> result;
    result.reserve(256);
    for (const auto & [token, id] : vocab_) {
        if (token.size() != 1) continue;
        result.push_back({static_cast<unsigned>(static_cast<uint8_t>(token[0])),
                          static_cast<unsigned>(id)});
    }
    return result;
}

// buildVocabBuckets — byte mode.
// Iterates each vocab token as raw bytes (vocab.json stores tokens in
// UTF-8 byte form already). Tokens of byte length < 2 are skipped.
// Tokens of byte length 2 land in the bucket's prefixVocabID slot;
// longer tokens insert into the byte-keyed trie under the (b0, b1) bucket.
std::vector<VocabBucket> BPETokenizer::buildVocabBuckets() const {
    std::map<std::pair<uint8_t,uint8_t>, VocabBucket> byPrefix;
    for (const auto & [token, id] : vocab_) {
        if (token.size() < 2) continue;
        uint8_t b0 = static_cast<uint8_t>(token[0]);
        uint8_t b1 = static_cast<uint8_t>(token[1]);
        auto key = std::make_pair(b0, b1);
        auto & b = byPrefix[key];
        b.b0 = b0;
        b.b1 = b1;
        if (token.size() == 2) {
            b.prefixVocabID = id;
        } else {
            TrieNode * cur = &b.root;
            for (size_t i = 2; i < token.size(); i++) {
                cur = &cur->children[static_cast<uint8_t>(token[i])];
            }
            // if the same token appears multiple times with different vocabIDs, the last one wins; 
            // this is not a well-defined scenario but we should at least be deterministic about it
            cur->vocabID = id;
        }
    }
    // convert from map to vector; the order doesn't matter but we want it to be deterministic
    std::vector<VocabBucket> result;
    result.reserve(byPrefix.size());
    for (auto & kv : byPrefix) result.push_back(std::move(kv.second));
    return result;
}

// use bytes, decodeUTF8 not needed 
// work with bytes ?
// single bytes ?
// kernel for first and second byte. kernel that finds all the single bytes - look at every first byte and - look at two bytes and confirms no two bytes - second bytes confirms 
// 

// run with a small vocab?
// Bixnum - 8 bit input - 16 bit output - vocabID is 16 bits, so we need 16 output bits to represent it in the BixNum stream.

