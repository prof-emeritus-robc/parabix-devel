/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — vocab longest-match scan over raw bytes, per-length kernels.
 *
 *  Algorithm
 *  ─────────
 *  Vocabulary tokens are grouped by byte length. One PabloKernel is emitted
 *  per non-empty length group. Inside a length-L kernel, every length-L token
 *  is matched independently as
 *
 *      m = AND over i in 0..L-1 of Advance(EQ(symBN, token[i]), L-1-i)
 *
 *  Common EQ results across tokens at the same length are memoized so each
 *  distinct byte value is compared against the input once per length kernel.
 *
 *  The pipeline folds length groups in DESCENDING length order using
 *  BPELengthFirstWinsMerge — a position that already has a (longer) match
 *  passes through unchanged, otherwise the new (shorter) match's tuple takes
 *  over. No UGE/UGT compare is needed because the fold order alone enforces
 *  longest-wins at any given end position.
 *
 *  After the fold, BPEAssembleKernel uses LookAhead over the carried matchLen
 *  stream to drop matches that fall inside the span of a later, longer match
 *  (e.g. the single-byte `l` at position 2 inside `Ġlove` ending at position
 *  5 with length 6).
 *
 *  Why per-length kernels?
 *  ─────────────────────
 *  GPT-2 max token byte length is ~30, so the pipeline JITs ≤ 30 length
 *  kernels + ≤ 30 merges + 1 assembly — far fewer than the previous
 *  per-(b0,b1)-bucket structure which spawned thousands of small kernels.
 *  Cache keys are derived from a rolling hash of the length group's token
 *  list so different vocab files do not collide.
 */

#include "bpe.h"
#include <fstream>
#include <iostream>
#include <cstdint>
#include <map>
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

// Rolling hash over a length group — used to disambiguate JIT cache entries
// when different vocab files share a length but different token contents.
uint64_t hashLengthGroup(unsigned L,
                         const std::vector<std::pair<std::string,unsigned>> & tokens) {
    uint64_t h = 0xCBF29CE484222325ull;
    h ^= L;
    h *= 1099511628211ull;
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

// ─── BPELengthKernel ───────────────────────────────────────────────────────
//
// One PabloKernel per token byte length L. Inputs: 8×1 basis bit streams of
// the byte-encoded text. Outputs:
//   matchEnd (1×1) — fires at the right-most byte of every length-L vocab
//                   word that ended there.
//   vocabID  (16×1 BixNum) — the matched word's ID at matchEnd positions.
//   matchLen (8×1  BixNum) — constant L at matchEnd positions, 0 elsewhere.
class BPELengthKernel : public PabloKernel {
public:
    BPELengthKernel(LLVMTypeSystemInterface & ts,
                    StreamSet * basis,  // 8×1 byte streams of the input text
                    StreamSet * matchEnd,  // 1×1 stream to fire at end positions of matched tokens
                    StreamSet * vocabID,  // 16×1 BixNum stream to hold the matched token's ID at matchEnd positions
                    StreamSet * matchLen, // 8×1 BixNum stream to hold constant L at matchEnd positions, 0 elsewhere
                    unsigned L,
                    std::vector<std::pair<std::string,unsigned>> tokens,
                    uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPELength_L" + std::to_string(L)
                       + "_h" + std::to_string(shapeHash),
                  {Binding{"basis", basis}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID},
                   Binding{"matchLen", matchLen}}),
      mLength(L), mTokens(std::move(tokens)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        // symBN = Pack basis bits into a BixNum for byte-wise comparison.
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum symBN(basisBits.begin(), basisBits.end());

        // Pre-create common constants.
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        // Memoize EQ(symBN, b) per distinct byte appearing in any token at
        // this length — each byte value compared against input only once.
        std::map<uint8_t, PabloAST*> byteEQ; 
        auto getEQ = [&](uint8_t b) {
            auto it = byteEQ.find(b);
            if (it != byteEQ.end()) return it->second;
            PabloAST * eq = bnc.EQ(symBN, b);
            byteEQ.emplace(b, eq);
            return eq;
        };

        // matchEndV = OR over tokens of m, where m = AND over i of Advance(EQ(symBN, tok[i]), L-1-i).
        Var * matchEndV = pb.createVar("matchEnd", zeroes);
        std::vector<Var*> idBits;
        idBits.reserve(16);
        for (unsigned i = 0; i < 16; i++) {
            idBits.push_back(pb.createVar(
                "id_" + std::to_string(i), zeroes));
        }

        for (const auto & [tok, vid] : mTokens) {
            // Build m = AND over i of Advance(EQ(symBN, tok[i]), L-1-i).
            // At end-position p, Advance(eq, k) reads eq at p-k, so each
            // factor checks the byte at position p-(L-1-i) = p-L+1+i — that
            // is the i-th byte of the token aligned with the right-most
            // byte landing at p.
            PabloAST * m = ones;
            for (unsigned i = 0; i < mLength; i++) {
                uint8_t b = static_cast<uint8_t>(tok[i]);
                PabloAST * eq = getEQ(b);
                unsigned k = mLength - 1 - i;
                PabloAST * factor = (k == 0)
                    ? eq
                    : pb.createAdvance(eq, k,
                          "adv_byte" + std::to_string(static_cast<unsigned>(b))
                              + "_k" + std::to_string(k));
                m = pb.createAnd(m, factor,
                          "m_tok" + std::to_string(vid)
                              + "_factor" + std::to_string(i));
            }
            pb.createAssign(matchEndV,
                pb.createOr(matchEndV, m,
                    "matchEndV_or_tok" + std::to_string(vid)));
            for (unsigned i = 0; i < 16; i++) {
                unsigned bit = (vid >> i) & 1u;
                pb.createAssign(
                    idBits[i],
                    pb.createSel(m, bit ? ones : zeroes, idBits[i],
                        "idsel_tok" + std::to_string(vid)
                            + "_bit" + std::to_string(i)));
            }
        }

        // collect all matches into the output streams:
        // Where words end
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEnd"),
                             pb.getInteger(0)),
            matchEndV);
        // vocabID = the matched token's ID at matchEnd positions, 0 elsewhere.
        // Which word matched
        Var * idOut = getOutputStreamVar("vocabID");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                idBits[i]);
        }
        // matchLen = constant L at matchEnd positions, 0 elsewhere.
        // Length of word
        Var * lenOut = getOutputStreamVar("matchLen");
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * v = ((mLength >> i) & 1u)
                               ? static_cast<PabloAST*>(matchEndV)
                               : zeroes;
            // Advance v by i to align with the correct bit position in the output BixNum stream, then assign to the output.    
            pb.createAssign(
                pb.createExtract(lenOut, pb.getInteger(i)),
                v);
        }
    }

private:
    unsigned mLength;
    std::vector<std::pair<std::string,unsigned>> mTokens;
};


// ─── BPELengthFirstWinsMerge ───────────────────────────────────────────────
//
// First-wins merge of two (matchEnd, vocabID, matchLen) tuples. 
// Fold length groups in DESCENDING length order — so "first" already
// holds the longer match and the new one only fills in positions the first
// did not cover. No length comparison needed.

// which word survives when multiple detectors shout at the same position.
class BPELengthFirstWinsMerge : public PabloKernel {
public:
    BPELengthFirstWinsMerge(LLVMTypeSystemInterface & ts,
                            StreamSet * me1, StreamSet * id1, StreamSet * len1,
                            StreamSet * me2, StreamSet * id2, StreamSet * len2,
                            StreamSet * meOut, StreamSet * idOut, StreamSet * lenOut)
    : PabloKernel(ts, "BPELengthFirstWinsMerge",
                  {Binding{"me1", me1},   Binding{"id1", id1},  Binding{"len1", len1},
                   Binding{"me2", me2},   Binding{"id2", id2},  Binding{"len2", len2}},
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

        // me2 wins only where me1 hasn't already fired.
        PabloAST * notMe1   = pb.createNot(me1, "not_me1");
        PabloAST * me2Wins  = pb.createAnd(me2, notMe1, "me2Wins");

        // me1 wins if it fired, otherwise me2 wins if it fired, otherwise no match.
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("meOut"),
                             pb.getInteger(0)),
            // me1 OR (NOT me1 AND me2) simplifies to me1 OR me2.
            pb.createOr(me1, me2, "meOut_me1_or_me2"));
        // Where a match ended, which word ID and length to output — me1's if it fired, else me2's if it fired, else 0.
        Var * idOut = getOutputStreamVar("idOut");
        // For each bit of the vocabID and matchLen BixNums, pick me1's bit if me1 won, else me2's bit if me2 won, else 0.
        for (unsigned i = 0; i < 16; i++) {
            PabloAST * idM2Pick = pb.createSel(me2Wins, id2[i],
                                       pb.createZeroes(),
                                       "id_m2pick_bit" + std::to_string(i));
            PabloAST * idPicked = pb.createSel(me1, id1[i], idM2Pick,
                                       "id_pick_bit" + std::to_string(i));
            // Assign the picked bit to the output vocabID stream at position i.
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                idPicked);
        }
        // Same first-wins fold for matchLen bits.
        Var * lenOut = getOutputStreamVar("lenOut");
        for (unsigned i = 0; i < 8; i++) {
            PabloAST * lenM2Pick = pb.createSel(me2Wins, len2[i],
                                       pb.createZeroes(),
                                       "len_m2pick_bit" + std::to_string(i));
            PabloAST * lenPicked = pb.createSel(me1, len1[i], lenM2Pick,
                                       "len_pick_bit" + std::to_string(i));
            pb.createAssign(
                pb.createExtract(lenOut, pb.getInteger(i)),
                lenPicked);
        }
    }
};


// ─── BPEAssembleKernel ──────────────────────────────────────────────────────
//
// Suppress matches that fall inside the byte span of a longer match ending
// later. A match ending at e with length L covers positions [e-L+1 .. e]; so
// position p is covered iff there exists k in [1 .. maxLen-1] with
// matchEnd(p+k)=1 AND matchLen(p+k) > k. Bindings declare LookAhead(maxLen-1)
// on matchEnd and matchLen so Pablo can peek forward.
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

        // A position p is covered by a later, longer match if there exists k in [1 .. maxLen-1] with matchEnd(p+k)=1 AND matchLen(p+k) > k.
        PabloAST * me0 = getInputStreamSet("matchEndIn")[0];
        // vocabIDIn and matchLenIn are LookAhead streams so we can access future positions without Advance.
        std::vector<PabloAST*> idIn  = getInputStreamSet("vocabIDIn");
        std::vector<PabloAST*> lenIn = getInputStreamSet("matchLenIn");

        // coverMask = OR over k in [1 .. maxLen-1] of
        //   (matchEnd(p+k) AND UGT(matchLen(p+k), k)).
        // Position p is covered if some future match's span reaches back to p.
        PabloAST * coverMask = pb.createZeroes();

        // For each k in [1 .. maxLen-1],
        //compute a mask of positions covered by a match ending at p+k with length > k,
        //then OR them together to get the final coverMask.
        for (unsigned k = 1; k < mMaxLen; k++) {
            PabloAST * futureEnd = pb.createLookahead(me0, k,
                                       "futureEnd_k" + std::to_string(k));
            std::vector<PabloAST*> futureLenBits;
            futureLenBits.reserve(8);
            for (unsigned i = 0; i < 8; i++) {
                futureLenBits.push_back(
                    pb.createLookahead(lenIn[i], k,
                        "futureLen_k" + std::to_string(k)
                            + "_bit" + std::to_string(i)));
            }
            // futureLenBN = BixNum of the lookahead bits for matchLen at position p+k.
            BixNum futureLenBN(futureLenBits.begin(), futureLenBits.end());
            // covered_k = matchEnd(p+k) AND UGT(matchLen(p+k), k)
            // fires at positions p where a match ending at p+k would cover p.
            PabloAST * covered_k = pb.createAnd(
                futureEnd, bnc.UGT(futureLenBN, k),
                "covered_k" + std::to_string(k));
            // OR covered_k into the cumulative coverMask.
            coverMask = pb.createOr(coverMask, covered_k,
                            "coverMask_thru_k" + std::to_string(k));
        }

        // keep = matchEnd(p) AND NOT coverMask
        // fires at positions where a match ends that is not covered by a later, longer match.
        PabloAST * notCover = pb.createNot(coverMask, "not_coverMask");
        PabloAST * keep     = pb.createAnd(me0, notCover,
                                  "keep_m" + std::to_string(mMaxLen));

        // Assign the kept matches to the output streams. 
        // Where a match end is kept, copy the vocabID from the input to the output; otherwise output 0.
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEndOut"),
                             pb.getInteger(0)),
            keep);
        // For each bit of the vocabID BixNum, copy it to the output if keep=1, else output 0.
        Var * idOut = getOutputStreamVar("vocabIDOut");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                pb.createAnd(idIn[i], keep,
                    "idOut_bit" + std::to_string(i) + "_kept"));
        }
    }
// max token byte length in the vocab, used for LookAhead in the assembly kernel.
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
        // symBN = Pack basis bits into a BixNum for byte-wise comparison.
        std::vector<PabloAST*> bits = getInputStreamSet("basis");
        // symBN is unused here since we only care about one byte value, so just pack the bits into a BixNum for convenient EQ against 0x0A.
        BixNum bn(bits.begin(), bits.end());
        // isNL = EQ(symBN, 0x0A) — fires at newline byte positions.
        PabloAST * isNL = bnc.EQ(bn, 0x0A);
        // Assign isNL to the output newlineMask stream at position 0.
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("newlineMask"),
                             pb.getInteger(0)),
            isNL);
    }
};


// ─── Pipeline ─────────────────────────────────────────────────────────
// runBPETrie
//
// Builds a per-length pipeline:
//   1. One BPELengthKernel per non-empty token length, emitting
//      (matchEnd, vocabID, matchLen=L) for every length-L vocab word.
//   2. Pair-merge fold in DESCENDING length order using
//      BPELengthFirstWinsMerge — longest match at each end position
//      survives without any length compare.
//   3. BPEAssembleKernel suppresses matches lying inside a later, longer
//      match's byte span by LookAheading over matchLen.
//
// Set BPE_NO_ASSEMBLE in the environment to skip the final assembly stage
// (returns the raw pair-merged streams) — useful for isolating the source
// of unexpected token output during debugging.
BPETrieResult runBPETrie(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {
    auto byLength = bpe.buildVocabByLength();   // descending by length

    // curMe/curId/curLen hold the folded streams for all length groups processed so far, 
    // to merge with the next group. Initially null, then set to the first group's output, 
    // then updated with each merge.
    StreamSet * curMe  = nullptr;
    StreamSet * curId  = nullptr;
    StreamSet * curLen = nullptr;

    // For each length group, emit a BPELengthKernel to detect matches of that length, 
    // then merge with the cumulative streams from previous groups using BPELengthFirstWinsMerge.
    unsigned groupCount = 0;
    for (auto & group : byLength) {
        if (group.tokens.empty()) continue;
        uint64_t shape = hashLengthGroup(group.length, group.tokens);

        // Emit a BPELengthKernel for this length group, producing (me, id, len) streams for the matches of this length.
        StreamSet * me  = P.CreateStreamSet(1, 1);
        StreamSet * id  = P.CreateStreamSet(16, 1);
        StreamSet * len = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BPELengthKernel>(
            basis, me, id, len, group.length, std::move(group.tokens), shape);
        groupCount++;
        if (curMe == nullptr) {
            curMe  = me;
            curId  = id;
            curLen = len;
            continue;
        }
        // Merge the new length group's streams with the cumulative streams from previous groups using BPELengthFirstWinsMerge, 
        //producing updated cumulative streams in outMe/outId/outLen.
        StreamSet * outMe  = P.CreateStreamSet(1, 1);
        StreamSet * outId  = P.CreateStreamSet(16, 1);
        StreamSet * outLen = P.CreateStreamSet(8, 1);
        P.CreateKernelCall<BPELengthFirstWinsMerge>(
            curMe, curId, curLen, me, id, len, outMe, outId, outLen);
        curMe  = outMe;
        curId  = outId;
        curLen = outLen;
    }
    // After processing all length groups, curMe/curId/curLen hold the merged matchEnd/vocabID/matchLen streams for all tokens.
    std::cerr << "BPE: per-length pipeline built with " << groupCount
              << " length kernels\n";

    // If the vocab is empty, no length kernels were built and curMe is still null. 
    // In that case, create dummy output streams that will never fire, 
    //to avoid null pointers in the downstream assembly kernel.
    if (curMe == nullptr) {
        std::cerr << "BPE: vocabulary empty; emitting zero streams\n";
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        return {me, id};
    }

    // Finally, run the BPEAssembleKernel to suppress matches that fall inside the byte span of a later, longer match.
    unsigned maxLen = bpe.maxTokenByteLen();
    if (maxLen < 2) {
        return {curMe, curId};
    }
    // If the longest token is only 1 byte, no assembly needed since single-byte matches cannot be covered by a longer match. 
    // Skip the assembly kernel to save JIT time.
    if (getenv("BPE_NO_ASSEMBLE")) {
        std::cerr << "BPE: skipping assembly stage (BPE_NO_ASSEMBLE set)\n";
        return {curMe, curId};
    }
    // Run the assembly kernel with LookAhead(maxLen-1) on the input streams so it can peek forward to check for covering matches.
    StreamSet * outMe = P.CreateStreamSet(1, 1);
    StreamSet * outId = P.CreateStreamSet(16, 1);
    P.CreateKernelCall<BPEAssembleKernel>(
        curMe, curId, curLen, outMe, outId, maxLen);
    return {outMe, outId};
}
// buildLinePretokens
kernel::StreamSet * buildLinePretokens(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis) {

    // basisBits is the number of bits in the basis StreamSet, which should be 8 for byte streams.
    const unsigned basisBits = basis->getNumElements();

    // Create a mask for newline characters.
    StreamSet * newlineMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<LinePtBoundKernel>(basis, newlineMask);

    // Invert the newline mask to get a keep mask for FilterByMask, 
    // which will keep positions that are not newlines.
    StreamSet * keepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertStreamKernel>(newlineMask, keepMask);

    // Use FilterByMask to filter the basis stream, 
    // keeping only positions that are not newlines, and output the compressed basis stream.
    StreamSet * compressedBasis = P.CreateStreamSet(basisBits, 1);
    FilterByMask(P, keepMask, basis, compressedBasis);

    // Return the compressed basis stream, 
    // which has newlines filtered out and can be used as input to the BPE trie.
    return compressedBasis;
}


// ─── BPETokenizer — file I/O + vocab partitioning ──────────────────────────

// BPETokenizer loads a vocab file mapping tokens to IDs, decodes token IDs back to strings,
// and partitions the vocab by token byte length for the per-length kernels.
bool BPETokenizer::loadVocab(const std::string & path) {
    std::ifstream file(path);
    // If the file cannot be opened, print an error message and return false.
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open vocab file: " << path << "\n";
        return false;
    }
    // Parse the JSON vocab file into a nlohmann::json object. 
    //If parsing fails, print an error message and return false.
    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception & e) {
        std::cerr << "BPE: failed to parse vocab file: " << e.what() << "\n";
        return false;
    }
    // Iterate over the items in the JSON object, where each item is a key-value pair of token and ID.
    for (auto & [key, val] : j.items()) {
        int id = val.get<int>();
        vocab_[key] = id;
        // If the ID is non-negative, ensure that idToToken_ has enough space to hold the token at index id, 
        // and then store the token string at that index.
        if (id >= 0) {
            if (static_cast<size_t>(id) >= idToToken_.size())
                idToToken_.resize(static_cast<size_t>(id) + 1);
            idToToken_[static_cast<size_t>(id)] = key;
        }
    }
    // After loading the vocab, print the number of tokens loaded and return true if the vocab is not empty.
    std::cerr << "BPE: loaded vocab with " << vocab_.size() << " tokens\n";
    return !vocab_.empty();
}

// decodeToken takes an integer token ID and returns the corresponding token string from the idToToken_ vector.
std::string BPETokenizer::decodeToken(int id) const {
    if (id < 0 || static_cast<size_t>(id) >= idToToken_.size()) return "";
    return idToToken_[static_cast<size_t>(id)];
}

// maxTokenByteLen iterates over all tokens in the vocab and returns the maximum byte length of any token, 
// which is used to determine the LookAhead window size in the assembly kernel.
unsigned BPETokenizer::maxTokenByteLen() const {
    unsigned m = 0;
    for (const auto & [token, id] : vocab_) {
        (void)id;
        if (token.size() > m) m = static_cast<unsigned>(token.size());
    }
    return m;
}

// Partition the vocab by token byte length. Tokens are placed into the
// group whose `length` equals `token.size()`. Output is sorted in DESCENDING
// length order so the pipeline folds the longest group first.
std::vector<VocabLengthGroup> BPETokenizer::buildVocabByLength() const {
    std::map<unsigned, std::vector<std::pair<std::string,unsigned>>> byLen;
    for (const auto & [token, id] : vocab_) {
        if (token.empty()) continue;
        byLen[static_cast<unsigned>(token.size())].push_back(
            {token, static_cast<unsigned>(id)});
    }
    std::vector<VocabLengthGroup> result;
    result.reserve(byLen.size());
    for (auto it = byLen.rbegin(); it != byLen.rend(); ++it) {
        result.push_back({it->first, std::move(it->second)});
    }
    return result;
}
