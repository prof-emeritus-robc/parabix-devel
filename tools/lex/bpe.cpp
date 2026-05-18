/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — vocabulary-trie longest-match scan.
 *
 *  Algorithm
 *  ─────────
 *  Vocab words of length >= 2 are bucketed by their first two codepoints
 *  (cp0, cp1). One PabloKernel is emitted per (cp0, cp1) bucket. Inside the
 *  kernel a nested-scope trie walks the bucket's suffix codepoints
 *  (codepoint index 2 onward). Each trie level uses
 *
 *      childMark = Advance(parentMark, 1) & EQ(symBN, child_cp)
 *
 *  to extend the match one codepoint to the right. Pablo `createIf` on the
 *  child marker runtime-skips dead subtries.
 *
 *  Per-kernel longest match wins: deeper trie nodes overwrite the per-Var
 *  vocab-ID slots via createSel(parentMark, …, prevVal). Because each match
 *  fires at the right-most codepoint of its word, distinct word lengths land
 *  at distinct end positions — overwrite competition is only between vocab
 *  words ending at the same position via this bucket's trie.
 *
 *  Phase-1 limitations
 *  ───────────────────
 *   - Single-codepoint vocab tokens are not in the trie; a fallback layer
 *     covering positions where no >=2-codepoint match fires is not wired.
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

// UTF-8 decode for vocab strings. Returns a flat vector of Unicode codepoints.
std::vector<uint32_t> decodeUTF8(const std::string & s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char lead = static_cast<unsigned char>(s[i]);
        size_t len = (lead < 0x80) ? 1u
                   : (lead < 0xE0) ? 2u
                   : (lead < 0xF0) ? 3u : 4u;
        if (i + len > s.size()) break;
        uint32_t cp = 0;
        if (len == 1) {
            cp = lead;
        } else if (len == 2) {
            cp = ((lead & 0x1Fu) << 6)
               | (static_cast<unsigned char>(s[i+1]) & 0x3Fu);
        } else if (len == 3) {
            cp = ((lead & 0x0Fu) << 12)
               | ((static_cast<unsigned char>(s[i+1]) & 0x3Fu) << 6)
               | (static_cast<unsigned char>(s[i+2]) & 0x3Fu);
        } else {
            cp = ((lead & 0x07u) << 18)
               | ((static_cast<unsigned char>(s[i+1]) & 0x3Fu) << 12)
               | ((static_cast<unsigned char>(s[i+2]) & 0x3Fu) << 6)
               | (static_cast<unsigned char>(s[i+3]) & 0x3Fu);
        }
        cps.push_back(cp);
        i += len;
    }
    return cps;
}
//
// This function creates a unique fingerprint for a trie node by mixing its vocabID, 
// rotating bits, and recursively mixing in all its children's codepoints and structures, 
// so that two different tries never get the same fingerprint.
//
// Rolling hash of a trie node — used to disambiguate JIT cache entries when
// two buckets share (cp0, cp1) across different vocab files.
uint64_t hashTrieNode(const TrieNode & n, uint64_t seed = 0) {
    seed ^= 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(n.vocabID + 1);
    seed = (seed << 13) | (seed >> 51);
    for (const auto & kv : n.children) {
        seed ^= static_cast<uint64_t>(kv.first) + 0x9E3779B97F4A7C15ull;
        seed = hashTrieNode(kv.second, seed);
    }
    return seed;
}

} // anonymous namespace


// ─── BPETrieKernel ──────────────────────────────────────────────────────────
//
//BPETrieKernel is a worker that takes text and a trie of words,
// walks through the trie recursively to find longest matches,
// and outputs a stream marking WHERE matches ended and WHICH word IDs matched
//
// One PabloKernel per (cp0, cp1) bucket. Inputs: u21 codepoints. Outputs:
// matchEnd (1×1, marks the right-most codepoint of every vocab word in this
// bucket that ended there) and vocabID (16×1 BixNum, the matched word's ID at
// matchEnd positions; 0 elsewhere within this bucket's output).
class BPETrieKernel : public PabloKernel {
public:
    BPETrieKernel(LLVMTypeSystemInterface & ts,
                  StreamSet * u21,
                  StreamSet * matchEnd,
                  StreamSet * vocabID,
                  VocabBucket bucket,
                  unsigned tag,
                  uint64_t shapeHash)
    : PabloKernel(ts,
                  "BPETrie_t" + std::to_string(tag)
                       + "_p" + std::to_string(bucket.cp0)
                       + "_" + std::to_string(bucket.cp1)
                       + "_h" + std::to_string(shapeHash),
                  {Binding{"u21", u21}},
                  {Binding{"matchEnd", matchEnd},
                   Binding{"vocabID",  vocabID}}),
      mBucket(std::move(bucket)) {}

protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);

        // symBN is the BixNum of the input u21 codepoint stream; 
        // used for matching against cp0 and cp1 in the trie walk
        std::vector<PabloAST*> u21bits = getInputStreamSet("u21");
        BixNum symBN(u21bits.begin(), u21bits.end());

        // Prepare output variables and constants.
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);
        
        // matchEndV is the per-position marker variable we write to whenever we hit a trie node with a vocabID; 
        // idBits are the per-bit variables we write the vocabID to in the same cases. 
        // After the trie walk, we assign these to the output streams.
        // accumulates match bits
        Var * matchEndV = pb.createVar("matchEnd", zeroes);
        std::vector<Var*> idBits;
        // We have 16 bits to represent the vocabID, so we need 16 per-position variables to accumulate them bitwise via createSel on each match.
        // idBits[i] accumulates the i-th bit of the vocabID across all matches at this position; 
        // after the trie walk, we assign each idBits[i] to the i-th bit of the output vocabID BixNum stream.
        idBits.reserve(16);
        // Accumulates the 16th bit of vocabID across all matches 
        for (unsigned i = 0; i < 16; i++) {
            idBits.push_back(pb.createVar(
                "id_" + std::to_string(i), zeroes));
        }

        // First match stage: check the bucket's (cp0, cp1) against the symBN codepoint stream. 
        // Only positions where both match can possibly match any token in this bucket, 
        // so this is a quick filter before we enter the more expensive trie walk.
        PabloAST * c0 = bnc.EQ(symBN, mBucket.cp0);
        PabloAST * c1 = bnc.EQ(symBN, mBucket.cp1);
        //  pairMark[p] = 1  iff  symBN[p-1]==cp0 AND symBN[p]==cp1
        //  → marks the cp1 (right-hand) position of every (cp0,cp1) pair.
        PabloAST * pairMark = pb.createAnd(
            pb.createAdvance(c0, 1), c1);

        // look at positions where I found an HE pair, skip everywhere else
        auto pairScope = pb.createScope();
        pb.createIf(pairMark, pairScope);

        // Handle the special case of a vocab word of length 2 that ends at the same position as the bucket's (cp0, cp1) pair match:
        // If my bucket has a 2-letter word, record it at this position!"
        if (mBucket.prefixVocabID >= 0) {
            recordMatch(pairScope, pairMark, mBucket.prefixVocabID,
                        matchEndV, idBits, ones, zeroes);
        }
        // Then emit the trie walk for longer vocab words:
        emitTrie(pairScope, pairMark, mBucket.root, u21bits,
                 matchEndV, idBits, ones, zeroes);

        // Finally, assign the per-position matchEndV and idBits to the output streams.
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("matchEnd"),
                             pb.getInteger(0)),
            matchEndV);
        // idOut is a 16-bit BixNum stream; assign each bit from the idBits vector to its corresponding position in the output BixNum.
        Var * idOut = getOutputStreamVar("vocabID");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                idBits[i]);
        }
    }

private:
    VocabBucket mBucket;

    static void recordMatch(PabloBuilder & pb, PabloAST * mark, int vocabID,
                            Var * matchEndV, std::vector<Var*> & idBits,
                            PabloAST * ones, PabloAST * zeroes) {
        pb.createAssign(matchEndV, pb.createOr(matchEndV, mark));
        for (unsigned i = 0; i < 16; i++) {
            unsigned bit = (static_cast<unsigned>(vocabID) >> i) & 1u;
            pb.createAssign(
                idBits[i],
                pb.createSel(mark, bit ? ones : zeroes, idBits[i]));
        }
    }
    
    // Recursive trie emitter. Emits a scope for each trie node, 
    // with an If on the childMark that extends the parentMark by one codepoint and EQ-matches it against the node's cp.
    // Whenever we hit a node with a vocabID, call recordMatch to write to matchEndV and idBits.
    static void emitTrie(PabloBuilder & pb, PabloAST * parentMark,
                         const TrieNode & node,
                         const std::vector<PabloAST*> & u21bits,
                         Var * matchEndV, std::vector<Var*> & idBits,
                         PabloAST * ones, PabloAST * zeroes) {
        
        // Base case: if this node has no children, we're done. 
        // The recursive calls below only happen if there is at least one child, so we don't need to emit an empty scope for a leaf node.
        if (node.children.empty()) return;
        BixNumCompiler bnc(pb);
        BixNum symBN(u21bits.begin(), u21bits.end());
        for (const auto & kv : node.children) {
            uint32_t cp = kv.first;
            const TrieNode & child = kv.second;
            PabloAST * cpMark = bnc.EQ(symBN, cp);
            PabloAST * childMark = pb.createAnd(
                pb.createAdvance(parentMark, 1), cpMark);
            auto childScope = pb.createScope();
            pb.createIf(childMark, childScope);
            // If this child node has a vocabID, record the match at this position. 
            // Then recurse to emit the child's children.
            if (child.vocabID >= 0) {
                recordMatch(childScope, childMark, child.vocabID,
                            matchEndV, idBits, ones, zeroes);
            }
            // Recurse to emit the child's children:
            emitTrie(childScope, childMark, child, u21bits,
                     matchEndV, idBits, ones, zeroes);
        }
    }
};


// ─── BPETriePairMergeKernel ─────────────────────────────────────────────────
//
// Bitwise-OR merges two (matchEnd, vocabID) streams. Used to fold per-bucket
// outputs into one combined stream. Cache-keyed by name alone — semantics
// are bucket-independent.
class BPETriePairMergeKernel : public PabloKernel {
public:
    BPETriePairMergeKernel(LLVMTypeSystemInterface & ts,
                           StreamSet * me1, StreamSet * id1,
                           StreamSet * me2, StreamSet * id2,
                           StreamSet * meOut, StreamSet * idOut)
    : PabloKernel(ts, "BPETriePairMerge",
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
            pb.createExtract(getOutputStreamVar("meOut"),
                             pb.getInteger(0)),
            pb.createOr(me1, me2));
        Var * idOut = getOutputStreamVar("idOut");
        for (unsigned i = 0; i < 16; i++) {
            pb.createAssign(
                pb.createExtract(idOut, pb.getInteger(i)),
                pb.createOr(id1[i], id2[i]));
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
                             std::vector<PabloAST*>{pb.createNot(in)});
    }
};


// ─── LinePtBoundKernel ──────────────────────────────────────────────────────
//
// Inline pretokenizer for compare_bpe.py step 2. Marks each newline codepoint
// so callers can FilterByMask it out of the downstream stream.
class LinePtBoundKernel : public PabloKernel {
public:
    LinePtBoundKernel(LLVMTypeSystemInterface & ts,
                      StreamSet * u21,
                      StreamSet * newlineMask)
    : PabloKernel(ts, "BPE_LinePtBound",
                  {Binding{"u21", u21}},
                  {Binding{"newlineMask", newlineMask}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> bits = getInputStreamSet("u21");
        BixNum bn(bits.begin(), bits.end());
        PabloAST * isNL = bnc.EQ(bn, 0x0A);
        pb.createAssign(
            pb.createExtract(getOutputStreamVar("newlineMask"),
                             pb.getInteger(0)),
            isNL);
    }
};


// ─── Pipeline glue ─────────────────────────────────────────────────────────
//
// This function takes all vocabulary words, groups them by their first two letters, 
// creates a worker for each group to find matches in the text, 
// and combines all the results together.
//
// Builds the BPE trie longest-match pipeline. 
//Returns the (matchEnd, vocabID) stream pair from the final merge stage.
BPETrieResult runBPETrie(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * u21,
        const BPETokenizer & bpe) {
    // Build the per-(cp0, cp1) buckets from the vocab - groups by the first two letters
    // Each bucket has its own trie kernel. 
    // Then merge the outputs with BPETriePairMergeKernel.
    auto buckets = bpe.buildVocabBuckets();
    // If there are no tokens of length >= 2, we won't emit any trie kernels;
    //return empty streams to avoid special-casing the caller.
    if (buckets.empty()) {
        std::cerr << "BPE: vocabulary has no length>=2 tokens; "
                     "trie pipeline has nothing to do\n";
        StreamSet * me = P.CreateStreamSet(1, 1);  // matchEnd
        StreamSet * id = P.CreateStreamSet(16, 1);  // vocabID
        return {me, id};
    }

    // storage for the current merged output of the trie pipeline as we build it up;

    StreamSet * curMe = nullptr;  // current matchEnd stream
    StreamSet * curId = nullptr; // current vocabID stream
    unsigned tag = 0;  // for debug naming of the kernels; not semantically significant
    for (auto & bucket : buckets) {
        // a unique ID for this bucket's structure.
        uint64_t shape = hashTrieNode(bucket.root,
            (static_cast<uint64_t>(bucket.cp0) << 32) ^ bucket.cp1
                ^ static_cast<uint64_t>(bucket.prefixVocabID + 1));
        
        // Emit the kernel for this bucket. It takes the u21 codepoint stream as input, and produces a (matchEnd, vocabID) stream pair as output.
        StreamSet * me = P.CreateStreamSet(1, 1);
        StreamSet * id = P.CreateStreamSet(16, 1);
        P.CreateKernelCall<BPETrieKernel>(
            u21,  // input codapoint stream
            me,   // output matchEnd stream
            id,   // output vocabID stream
            std::move(bucket),  // bucket structure (trie and prefixVocabID)
            tag,   // tag for debug naming
            shape);  // shape hash for JIT cache disambiguation
        tag++;
        // Merge this bucket's output with the current merged output using BPETriePairMergeKernel.
        if (curMe == nullptr) {
            curMe = me;
            curId = id;
            continue;
        }
        // Merge curMe/curId with me/id into new streams outMe/outId, 
        // then update curMe/curId to point to the merged result for the next iteration.
        StreamSet * outMe = P.CreateStreamSet(1, 1);
        StreamSet * outId = P.CreateStreamSet(16, 1);
        P.CreateKernelCall<BPETriePairMergeKernel>(
            curMe, curId, me, id, outMe, outId);
        curMe = outMe;
        curId = outId;
    }
    std::cerr << "BPE: trie pipeline built with " << tag
              << " (cp0,cp1) buckets\n";
    // After the loop, curMe and curId are the merged output of all the buckets' trie kernels. 
    // Return them as the final result of this function.
    return {curMe, curId};
}

kernel::StreamSet * buildLinePretokens(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * u21) {

    const unsigned u21Bits = u21->getNumElements();

    StreamSet * newlineMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<LinePtBoundKernel>(u21, newlineMask);

    StreamSet * keepMask = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<InvertStreamKernel>(newlineMask, keepMask);

    StreamSet * compressedU21 = P.CreateStreamSet(u21Bits, 1);
    FilterByMask(P, keepMask, u21, compressedU21);

    return compressedU21;
}


// ─── BPETokenizer — file I/O ────────────────────────────────────────────────

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

//
// sorting vocabulary words into groups
// This function creates organized "buckets" - group of words
//
// This function takes all vocabulary words, groups them by their first two letters,
// stores 2-letter words in a special slot, and stores longer words in a tree structure under each bucket,
// then returns the organized buckets as a list.
//
std::vector<VocabBucket> BPETokenizer::buildVocabBuckets() const {
    std::map<std::pair<uint32_t,uint32_t>, VocabBucket> byPrefix;
    // the same (cp0, cp1) prefix can appear in multiple vocab files with different vocabIDs;
    // the hashTrieNode seed incorporates the prefix and vocabID to disambiguate them in the JIT cache key
    for (const auto & [token, id] : vocab_) {
        auto cps = decodeUTF8(token); // convert all the vocab to codepoints
        if (cps.size() < 2) continue;  // single-codepoint tokens skipped (not in trie)

        // take the first two letters of the word and use them as the bucket label.
        auto key = std::make_pair(cps[0], cps[1]);  // bucket by (cp0, cp1) prefix
        // insert token into the trie of the appropriate bucket
        auto & b = byPrefix[key]; // Find the box labeled key - create if it doesn't exist
        // If this is the first time we've seen this bucket, set its cp0 and cp1 from the key.
        b.cp0 = cps[0];
        b.cp1 = cps[1];
        // tokens of length 2 land in the bucket's prefixVocabID slot; longer tokens go in the trie
        if (cps.size() == 2) {
            b.prefixVocabID = id;
        } 
        // For tokens of length > 2, we need to insert them into the trie structure of the bucket.
        else {
            // insert into trie: walk/create nodes for cps[2..end-1], then set vocabID at the leaf
            TrieNode * cur = &b.root;
            for (size_t i = 2; i < cps.size(); i++) {
                cur = &cur->children[cps[i]];
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
