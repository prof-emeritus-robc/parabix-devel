/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — range-kernel design (id-range accumulator).
 *
 *  Algorithm
 *  ─────────
 *  Preprocessing (buildVocabRanges, before any kernel):
 *      Group length>=2 tokens by id-RANGE (256-wide over [0,1024) → the
 *      256_511 / 512_767 / 768_1023 kernels, then 1000-wide above). Single bytes
 *      (length 1) are not grouped — they seed the byte-level fallback. Tokens
 *      within a range are sorted by id ASC. Priority = LOWER vocab id wins.
 *
 *  Runtime pipeline (one BPERangeKernel per id-range, lowest range first):
 *      BPERangeSeed   — source = single-byte id per byte, active = 1s, end = 0s.
 *      BPERangeKernel — per range: detect its tokens START-anchored, drop a match
 *                       whose span touches a byte an EARLIER range consumed (the
 *                       `active` mask) or one a lower-id token in the SAME range
 *                       already took (in-kernel `fill`), write the id at the match
 *                       END into `source`, shrink `active`, grow `end`.
 *                       (source, active, end) thread kernel→kernel (fresh buffers).
 *      BPEEmitTrigger — matchEnd = active OR end. `source` already holds the
 *                       single-byte id at every unconsumed byte → free fallback.
 *
 *  Cache keys
 *  ──────────
 *  BPERangeKernel name encodes hashTokenSet(the range's (token,id) list), so two
 *  vocab files producing the same range shape but different tokens still get
 *  distinct cache entries.
 */

#include "bpe.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
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

// Hash the byte->single-byte-id table so BPERangeSeed's cache name is unique per
// vocab. Without this the seed kernel (name has no token data) collides across
// vocabs in ~/.parabix/objcache/ — the first vocab's seed poisons the rest.
uint64_t hashByteIds(const std::vector<int> & ids) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (int v : ids) {
        h ^= static_cast<uint64_t>(static_cast<int64_t>(v));
        h *= 1099511628211ull;
    }
    return h;
}

}

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

// BPERangeSeed — seed the accumulator. source = single-byte id of each byte,
// active = all ones, end = all zeroes.
class BPERangeSeed : public PabloKernel {
public:
    BPERangeSeed(LLVMTypeSystemInterface & ts, StreamSet * basis,
                 StreamSet * source, StreamSet * active, StreamSet * end,
                 std::vector<int> byteIds)
    : PabloKernel(ts, "BPERangeSeed_h" + std::to_string(hashByteIds(byteIds)),
                  {Binding{"basis", basis}},
                  {Binding{"source", source}, Binding{"active", active}, Binding{"end", end}}),
      mByteIds(std::move(byteIds)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum symBN(basisBits.begin(), basisBits.end());
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        // source bit i = OR over bytes b whose single-byte id has bit i set.
        std::vector<PabloAST*> idBits(16, zeroes);
        for (unsigned b = 0; b < 256; b++) {
            int v = mByteIds[b];
            if (v < 0) continue;
            PabloAST * eq = bnc.EQ(symBN, b);
            for (unsigned i = 0; i < 16; i++)
                if ((v >> i) & 1) idBits[i] = pb.createOr(idBits[i], eq);
        }
        Var * sOut = getOutputStreamVar("source");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), idBits[i]);
        pb.createAssign(pb.createExtract(getOutputStreamVar("active"), pb.getInteger(0)), ones);
        pb.createAssign(pb.createExtract(getOutputStreamVar("end"),    pb.getInteger(0)), zeroes);
    }
private:
    std::vector<int> mByteIds;
};

// BPERangeKernel — one id-range (e.g. tokens 256..511). Tokens are sorted by id
// ASC: lower id wins, occupying its span first; a higher-id token starting on an
// already-filled byte is dropped (in-range), and any token whose span touches a
// byte an EARLIER range consumed is dropped (cross-range, via `active`).
// detect tokens → validate spans → resolve conflicts → emit updated state
class BPERangeKernel : public PabloKernel {
public:
    BPERangeKernel(LLVMTypeSystemInterface & ts,
                   StreamSet * basis, StreamSet * sourceIn, StreamSet * activeIn, StreamSet * endIn,
                   StreamSet * sourceOut, StreamSet * activeOut, StreamSet * endOut,
                   std::vector<std::pair<std::string,unsigned>> tokens,
                   unsigned maxLen, uint64_t shapeHash)
    : PabloKernel(ts, "BPERange_h" + std::to_string(shapeHash),
                  {Binding{"basis",    basis,    FixedRate(), LookAhead(maxLen > 1 ? maxLen - 1 : 1)},
                   Binding{"sourceIn", sourceIn},
                   Binding{"activeIn", activeIn, FixedRate(), LookAhead(maxLen > 1 ? maxLen - 1 : 1)},
                   Binding{"endIn",    endIn}},
                  {Binding{"sourceOut", sourceOut},
                   Binding{"activeOut", activeOut},
                   Binding{"endOut",    endOut}}),
      mTokens(std::move(tokens)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        std::vector<PabloAST*> srcBits   = getInputStreamSet("sourceIn");
        PabloAST * activeIn = getInputStreamSet("activeIn")[0];
        PabloAST * endIn    = getInputStreamSet("endIn")[0];

        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);
        BixNum sym0(basisBits.begin(), basisBits.end());

        // laByte(i) = byte at offset +i from the start, as a BixNum built from
        // LookAhead of the INPUT basis bits (legal). Memoized per offset.
        std::unordered_map<unsigned, BixNum> laByteCache;
        auto laByte = [&](unsigned i) -> const BixNum & {
            auto it = laByteCache.find(i);
            if (it != laByteCache.end()) return it->second;
            BixNum b;
            if (i == 0) b = sym0;
            else for (PabloAST * bit : basisBits) b.push_back(pb.createLookahead(bit, i));
            return laByteCache.emplace(i, std::move(b)).first->second;
        };
        // factor(b,i) = (byte[start+i] == b). Memoized per (b,i).
        // Give me a bitstream that is 1 wherever the byte at offset i equals value b
        std::unordered_map<uint64_t, PabloAST*> factorCache;
        auto factor = [&](uint8_t b, unsigned i) -> PabloAST* {
            uint64_t key = (static_cast<uint64_t>(b) << 32) | i;
            auto it = factorCache.find(key);
            if (it != factorCache.end()) return it->second;
            PabloAST * f = bnc.EQ(laByte(i), b);
            factorCache[key] = f;
            return f;
        };
        // spanActive(L) = full span [s..s+L-1] all active (no byte consumed by an
        // earlier range). LookAhead on the input activeIn — legal. Memoized per L.
        // checks the entire span for availibility 
        std::unordered_map<unsigned, PabloAST*> spanCache;
        auto spanActive = [&](unsigned L) -> PabloAST* {
            auto it = spanCache.find(L);
            if (it != spanCache.end()) return it->second;
            PabloAST * acc = activeIn;
            for (unsigned k = 1; k < L; k++) acc = pb.createAnd(acc, pb.createLookahead(activeIn, k));
            spanCache[L] = acc;
            return acc;
        };

        // Plain dataflow accumulators (expression DAG, assigned to outputs once).
        PabloAST * fill   = zeroes;                      // Mask of bytes this range consumes
        PabloAST * endAcc = endIn;                       // token-end positions (threaded)
        std::vector<PabloAST*> idAcc(16);                // source bixnum (threaded)
        for (unsigned i = 0; i < 16; i++)
            idAcc[i] = (i < srcBits.size()) ? srcBits[i] : zeroes;

        // mTokens is sorted by id ASC → lower id wins: it occupies its span first,
        // and a higher-id token whose start lands on an already-filled byte is
        // dropped (start-bit `fill` gate). Cross-range conflicts are dropped by
        // the `active` span gate (a span touching an earlier range's byte fails).
        for (const auto & [tok, vid] : mTokens) {
            unsigned L = static_cast<unsigned>(tok.size());
            PabloAST * startM = ones;                       // start-anchored detect
            for (unsigned i = 0; i < L; i++)
                startM = pb.createAnd(startM, factor(static_cast<uint8_t>(tok[i]), i));
            PabloAST * validStart = pb.createAnd(pb.createAnd(startM, spanActive(L)), pb.createNot(fill));
            PabloAST * span = validStart;                   // occupy [s..s+L-1]
            for (unsigned k = 1; k < L; k++) span = pb.createOr(span, pb.createAdvance(validStart, k));
            fill = pb.createOr(fill, span);
            PabloAST * endM = (L <= 1) ? validStart : pb.createAdvance(validStart, L - 1);
            endAcc = pb.createOr(endAcc, endM);
            for (unsigned i = 0; i < 16; i++)               // write id at end (T2-style Sel)
                idAcc[i] = pb.createSel(endM, ((vid >> i) & 1u) ? ones : zeroes, idAcc[i]);
        }

        pb.createAssign(pb.createExtract(getOutputStreamVar("activeOut"), pb.getInteger(0)),
                        pb.createAnd(activeIn, pb.createNot(fill)));
        pb.createAssign(pb.createExtract(getOutputStreamVar("endOut"), pb.getInteger(0)), endAcc);
        Var * sOut = getOutputStreamVar("sourceOut");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), idAcc[i]);
    }
private:
    std::vector<std::pair<std::string,unsigned>> mTokens;
};
// where tokens should be emitted (output).
// BPEEmitTrigger — matchEnd = end OR active. At an end position `source` holds a
// multi-byte token id; at an active (unconsumed) position it holds a single-byte
// id; interior consumed bytes are neither, so they are not emitted.
class BPEEmitTrigger : public PabloKernel {
public:
    BPEEmitTrigger(LLVMTypeSystemInterface & ts, StreamSet * active, StreamSet * end, StreamSet * matchEnd)
    : PabloKernel(ts, "BPEEmitTrigger",
                  {Binding{"active", active}, Binding{"end", end}},
                  {Binding{"matchEnd", matchEnd}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        pb.createAssign(pb.createExtract(getOutputStreamVar("matchEnd"), pb.getInteger(0)),
                        pb.createOr(getInputStreamSet("active")[0], getInputStreamSet("end")[0]));
    }
};

// ─── Pipeline (range-kernel design) ──────────────────────────────────────────
// buildBPEPassPipeline
//   1. buildVocabRanges() groups length>=2 tokens by id-RANGE, ascending
//      (preprocessing; lower vocab id = higher priority). Ranges run lowest first.
//   2. BPERangeSeed seeds (source = single-byte id per byte, active = 1s, end = 0s).
//   3. For each range (ascending): one BPERangeKernel detects its tokens
//      start-anchored, drops a match whose span touches a byte an earlier range
//      consumed (active) or a lower-id token in the same range took (fill), writes
//      the id at the match end into `source`, shrinks `active`, grows `end`.
//      (source, active, end) thread kernel→kernel via fresh buffers.
//   4. BPEEmitTrigger derives matchEnd = active OR end; vocabID = source.
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {

    // Range-kernel design (Kernel256_511, Kernel512_767, …). buildVocabRanges
    // groups tokens by id-range; one BPERangeKernel per range accumulates ids
    // into `source`, shrinks `active`, grows `end`, lowest-id range first. The
    // unconsumed-byte id already lives in `source` (seeded) → free byte-level
    // fallback, no FinalOr.
    // Build the length>=2 priority ranges from merges.txt when --merges is
    // supplied (rank-ordered, lower rank wins); else fall back to the vocab-id
    // partition. Single bytes always seed the fallback (singleByteIds, vocab).
    auto ranges = bpe.hasMerges() ? bpe.buildMergeRanges() : bpe.buildVocabRanges();

    // debug: dump the merge-range groups to stderr (merges path only)
    if (bpe.hasMerges()) {
        std::cerr << "[BPE] " << ranges.size() << " merge-range kernels\n";
        for (const auto & g : ranges)
            std::cerr << "[" << g.lo << "," << g.hi << ") x" << g.tokens.size()
                      << " maxLen=" << g.maxLen << "\n";
    }

    // Seed: source = single-byte id per byte, active = ones, end = zeroes.
    // source - stores token IDs
    // active - bitmask: “this position is still usable”
    // end - marks token endings
    StreamSet * source = P.CreateStreamSet(16, 1);
    StreamSet * active = P.CreateStreamSet(1, 1);
    StreamSet * end    = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<BPERangeSeed>(basis, source, active, end, bpe.singleByteIds());
    if (std::getenv("BPE_DBG") && std::string(std::getenv("BPE_DBG")) == "seed")
        return {active, source};   // dump seed source at every (seed-active) position

    // One kernel per id-range, threaded in order. Fresh buffers (NOT InOut): the
    // multi-byte detection + span mask need LookAhead, which is illegal on an
    // InOut/derived stream and on a deep alias chain; fresh buffers carry the
    // identical source/active/end dataflow the spec describes.
    for (auto & g : ranges) {
        if (g.tokens.empty()) continue;
        uint64_t shape = hashTokenSet(g.tokens);
        StreamSet * sOut = P.CreateStreamSet(16, 1);
        StreamSet * aOut = P.CreateStreamSet(1, 1);
        StreamSet * eOut = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<BPERangeKernel>(basis, source, active, end,
                                           sOut, aOut, eOut, g.tokens, g.maxLen, shape);
        source = sOut; active = aOut; end = eOut;
    }

    // matchEnd = active OR end; vocabID = source.
    // BPE_DBG=active|end routes that stream alone into matchEnd for debugging.
    const char * dbg = std::getenv("BPE_DBG");
    if (dbg && std::string(dbg) == "active") return {active, source};
    if (dbg && std::string(dbg) == "end")    return {end, source};
    StreamSet * matchEnd = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<BPEEmitTrigger>(active, end, matchEnd);
    return {matchEnd, source};
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
    // Sniff the first non-whitespace byte: a JSON vocab starts with '{'.
    // Anything else (e.g. "#version" / "Ġ t") is a merges.txt → delegate so
    // --vocab=merges.txt works directly.
    int c = file.peek();
    while (c == ' ' || c == '\n' || c == '\t' || c == '\r') { file.get(); c = file.peek(); }
    if (c != '{') {
        file.close();
        return loadMerges(path);
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

// loadMerges reads a HuggingFace merges.txt. Each non-header line "A B" defines a
// merge producing token AB (concat). The line index (0-based, after the #version
// header) is the merge RANK = priority — lower rank wins. In GPT-2 the merged
// token's vocab id == 256 + rank exactly (verified against vocab.json), so we
// store id = 256 + rank; the rank-ordered partition then matches the vocab-id
// partition for length>=2 tokens, and the kernels emit the correct vocab id.
bool BPETokenizer::loadMerges(const std::string & path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "BPE: cannot open merges file: " << path << "\n";
        return false;
    }
    constexpr unsigned BYTE_BASE = 256;          // ids 0..255 = byte-level base alphabet
    std::string line;
    unsigned rank = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // CRLF tolerance
        // Skip ONLY the "#version" header — NOT every '#' line: "# #", "## ##",
        // etc. are real merges, and each consumes a rank (id == 256 + rank), so
        // dropping them would shift the ids of every later merge.
        if (line.empty() || line.rfind("#version", 0) == 0) continue;
        // Split on the single separating space. Byte-level tokens never contain a
        // literal space (it is encoded as Ġ), so the first space is the boundary.
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string tok = line.substr(0, sp) + line.substr(sp + 1);
        unsigned id = BYTE_BASE + rank;
        merges_.push_back({tok, id});
        // Also feed the forward/reverse maps so isLoaded()/decodeToken work when
        // merges.txt is the sole source (e.g. --vocab=merges.txt). Idempotent
        // when vocab.json was already loaded: same token → same id (= 256+rank).
        vocab_[tok] = static_cast<int>(id);
        if (id >= idToToken_.size()) idToToken_.resize(id + 1);
        idToToken_[id] = tok;
        ++rank;
    }
    std::cerr << "BPE: loaded " << merges_.size() << " merges\n";
    return !merges_.empty();
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


// ─── Range partitioning (range-kernel design) ────────────────────────────────
// Group length>=2 tokens by id-range. Ranges: 256-wide over [0,1024)
// (→ 256_511, 512_767, 768_1023), then RANGE_WIDTH-wide above. One BPERangeKernel
// per group, run lowest-id-range first (so lower id wins).
std::vector<RangeGroup> BPETokenizer::buildVocabRanges() const {
    constexpr unsigned RANGE_WIDTH = 1000;          // width of ranges above id 1024

    // rangeLo(id): lower bound of id's range.
    auto rangeLo = [](unsigned id) -> unsigned {
        if (id < 1024) return (id / 256) * 256;     // 0, 256, 512, 768
        return 1024 + ((id - 1024) / RANGE_WIDTH) * RANGE_WIDTH;
    };
    // rangeHi(lo): upper bound of id's range (exclusive).
    auto rangeHi = [&](unsigned lo) -> unsigned {
        return lo < 1024 ? lo + 256 : lo + RANGE_WIDTH;
    };

    // create map of ranges 
    std::map<unsigned, RangeGroup> byRange;          // keyed by lo, ascending
    // loop through vocab, group tokens by their id-range, and populate the RangeGroup structure
    for (const auto & [tok, id] : vocab_) {
        if (tok.size() < 2) continue;                // single bytes = seeded fallback
        unsigned lo = rangeLo(static_cast<unsigned>(id));
        RangeGroup & g = byRange[lo];
        g.lo = lo;
        g.hi = rangeHi(lo);
        // fine the maximum token length in this range for LookAhead purposes
        // it sets the kernel's LookAhead binding size later 
        if (tok.size() > g.maxLen) g.maxLen = static_cast<unsigned>(tok.size());
        // add the token and its id to the range's token list
        g.tokens.push_back({tok, static_cast<unsigned>(id)});
    }

    // final result 
    std::vector<RangeGroup> ranges;
    // sort each range's tokens by id ascending (lower id wins in-range)
    for (auto & [lo, g] : byRange) {
        std::sort(g.tokens.begin(), g.tokens.end(),  
                  [](const auto & a, const auto & b) { return a.second < b.second; });
        ranges.push_back(std::move(g));
    }
    return ranges;
}


// buildMergeRanges — same id-range partition as buildVocabRanges, but the
// length>=2 token set + priority come from merges.txt (merges_: token, id =
// 256 + rank) instead of the vocab.json keys. Lower rank (= lower id) wins.
std::vector<RangeGroup> BPETokenizer::buildMergeRanges() const {
    constexpr unsigned RANGE_WIDTH = 1000;          // width of ranges above id 1024

    auto rangeLo = [](unsigned id) -> unsigned {
        if (id < 1024) return (id / 256) * 256;     // 0, 256, 512, 768
        return 1024 + ((id - 1024) / RANGE_WIDTH) * RANGE_WIDTH;
    };
    auto rangeHi = [&](unsigned lo) -> unsigned {
        return lo < 1024 ? lo + 256 : lo + RANGE_WIDTH;
    };

    std::map<unsigned, RangeGroup> byRange;          // keyed by lo, ascending
    for (const auto & [tok, id] : merges_) {
        if (tok.size() < 2) continue;                // merges are always len>=2; guard
        unsigned lo = rangeLo(id);
        RangeGroup & g = byRange[lo];
        g.lo = lo;
        g.hi = rangeHi(lo);
        if (tok.size() > g.maxLen) g.maxLen = static_cast<unsigned>(tok.size());
        g.tokens.push_back({tok, id});
    }

    std::vector<RangeGroup> ranges;
    for (auto & [lo, g] : byRange) {                 // sort by id ASC = rank ASC
        std::sort(g.tokens.begin(), g.tokens.end(),
                  [](const auto & a, const auto & b) { return a.second < b.second; });
        ranges.push_back(std::move(g));
    }
    return ranges;
}


// passes from longest to shortest?
// longest wins priority? not needed 
// no subset overlap ? TODO?
// each kernel one pass and each pass is of a specific length ?
// kernel generation should make one kernel for each pass - of each length group in the pass design. 
// Each kernel gets the tokens for that pass+length, generates the trie, and detects them all together. The kernels are mutually independent, so no overlap logic is needed in them — the pass design guarantees that within one pass, no two tokens can match at the same end position (their byte values would have to be identical), so the first match is the only match. The mask gate and occupy kernels then resolve overlaps across passes by pass/length order + the mask, so no overlap logic is needed in them either.