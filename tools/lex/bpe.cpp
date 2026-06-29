/*
 *  Part of the Parabix Project, under the Open Software License 3.0.
 *  SPDX-License-Identifier: OSL-3.0
 *
 *  BPE tokenizer — merge-kernel design (real BPE merge on the id stream).
 *
 *  Algorithm
 *  ─────────
 *  Preprocessing (buildMergeRuleRanges, before any kernel):
 *      Read merges.txt; each line "A B" is a merge A+B→AB with rank = line index
 *      and id = 256 + rank (lower rank wins). Resolve parts → MergeRule
 *      {idA, idB, lenB, idAB} via the base alphabet + earlier merge outputs (no
 *      vocab.json needed). Group rules by idAB-RANGE (256-wide over [0,1024),
 *      1000-wide above → 53 groups), sorted by idAB ASC = rank ASC.
 *
 *  Runtime pipeline (one BPEMergeKernel per id-range, lowest range first):
 *      BPERangeSeed   — source = base id of each RAW byte, active = 1s, end = 0s.
 *      BPEMergeKernel — per range, per rule (rank order): Aend = EQ(source,idA);
 *                       merge = Advance(Aend,lenB) AND EQ(source,idB); stamp idAB
 *                       at the merge end via Sel. `source` threads kernel→kernel,
 *                       so a higher-rank merge sees ids stamped by lower-rank ones
 *                       (the Ġthe→Ġthey cascade). All ops forward — no LookAhead.
 *
 *  Emission (Stage-B debug): matchEnd = active (all ones) → the id at EVERY byte
 *  is printed, so stale interior ids of swallowed parts still show. Correct
 *  emission (consume swallowed ends) is Stage C — not done.
 *
 *  Cache keys
 *  ──────────
 *  BPEMergeKernel name encodes hashRuleSet(the group's rule list), so two merges
 *  files producing the same range shape but different rules still get distinct
 *  cache entries.
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

// Hash a rule group's (idA,idB,idAB,lenB) list → unique cache name per kernel,
// since the Pablo body is data-dependent (same shape, different rules).
uint64_t hashRuleSet(const std::vector<MergeRule> & rules) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t x){ h = (h ^ x) * 0x100000001B3ull; };
    for (const auto & r : rules) { mix(r.idA); mix(r.idB); mix(r.idAB); mix(r.lenB); }
    return h;
}

// BPEMergeKernel — one id-range, REAL BPE merge on the id stream (Stage B).
// `source` carries a token id at each token's END byte (seeded = base id per
// raw byte). For each rule (A,B → AB), in idAB-ASC = rank-ASC order:
//     Aend  = EQ(source, idA)                 // where token A currently ends
//     merge = Advance(Aend, lenB) AND EQ(source, idB)   // B ends lenB bytes later
//     source ← Sel(merge, idAB, source)       // stamp merged id at AB's end (B-end)
// The accumulator (idAcc) threads through the rules, so a merge sees ids stamped
// by earlier (lower-rank) merges in the SAME kernel (e.g. Ġthe before Ġthey).
// All ops are forward (Advance on a derived stream — legal); no LookAhead needed.
// NOTE (Stage C, not done): the swallowed A-end is NOT cleared here, so emission
// double-counts until a follow-up consume kernel removes interior ends.
class BPEMergeKernel : public PabloKernel {
public:
    BPEMergeKernel(LLVMTypeSystemInterface & ts,
                   StreamSet * sourceIn, StreamSet * sourceOut,
                   std::vector<MergeRule> rules, uint64_t shapeHash)
    : PabloKernel(ts, "BPEMerge_h" + std::to_string(shapeHash),
                  {Binding{"sourceIn", sourceIn}},
                  {Binding{"sourceOut", sourceOut}}),
      mRules(std::move(rules)) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> srcBits = getInputStreamSet("sourceIn");
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        std::vector<PabloAST*> idAcc(16);                 // threaded id stream
        for (unsigned i = 0; i < 16; i++)
            idAcc[i] = (i < srcBits.size()) ? srcBits[i] : zeroes;

        for (const auto & r : mRules) {
            BixNum cur(idAcc.begin(), idAcc.end());        // current (mutated) id stream
            PabloAST * Aend  = bnc.EQ(cur, r.idA);         // A ends here
            PabloAST * Bend  = bnc.EQ(cur, r.idB);         // B ends here
            PabloAST * merge = pb.createAnd(pb.createAdvance(Aend, r.lenB), Bend); // AB ends here
            for (unsigned i = 0; i < 16; i++)              // stamp idAB at merge (T2-style Sel)
                idAcc[i] = pb.createSel(merge, ((r.idAB >> i) & 1u) ? ones : zeroes, idAcc[i]);
        }

        Var * sOut = getOutputStreamVar("sourceOut");
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), idAcc[i]);
    }
private:
    std::vector<MergeRule> mRules;
};

// ─── Pipeline (merge-kernel design) ──────────────────────────────────────────
// buildBPEPassPipeline — real BPE merge on the id stream.
//   1. buildMergeRuleRanges() groups merges into 53 id-ranges (rank order).
//   2. BPERangeSeed seeds source = base id of each RAW byte (active=1s, end=0s).
//   3. One BPEMergeKernel per range (ascending = rank order) glues adjacent ids:
//      for each rule (A,B→AB) it detects A's end, checks B at +lenB, stamps idAB.
//      `source` threads kernel→kernel; lower-rank merges feed higher-rank ones.
//   Returns (matchEnd, vocabID=source). NOTE: emission is Stage-B debug —
//   matchEnd = active (all ones) prints the id at EVERY byte, so stale interior
//   ids of swallowed parts still show. Correct emission (consume) is Stage C.
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe) {

    auto ruleRanges = bpe.buildMergeRuleRanges();

    // debug: dump the merge-range groups to stderr
    std::cerr << "[BPE] " << ruleRanges.size() << " merge-range kernels\n";
    for (const auto & g : ruleRanges)
        std::cerr << "[" << g.lo << "," << g.hi << ") x" << g.rules.size()
                  << " maxLen=" << g.maxLen << "\n";

    // BPE_RULES=1: dump the resolved merge rules. BPE_RULES_N caps per group (4).
    if (std::getenv("BPE_RULES")) {
        unsigned cap = 4;
        if (const char * n = std::getenv("BPE_RULES_N")) cap = std::atoi(n);
        for (const auto & g : ruleRanges)
            for (unsigned i = 0; i < g.rules.size() && i < cap; i++) {
                const auto & r = g.rules[i];
                std::cerr << "    idA=" << r.idA << " idB=" << r.idB
                          << " lenB=" << r.lenB << " -> idAB=" << r.idAB << "\n";
            }
    }

    // Seed: source = base id of each raw byte, active = ones, end = zeroes.
    StreamSet * source = P.CreateStreamSet(16, 1);
    StreamSet * active = P.CreateStreamSet(1, 1);
    StreamSet * end    = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<BPERangeSeed>(basis, source, active, end, bpe.singleByteIds());
    if (std::getenv("BPE_DBG") && std::string(std::getenv("BPE_DBG")) == "seed")
        return {active, source};   // dump seed source at every byte

    // One BPEMergeKernel per id-range, ascending = rank order. Fresh buffers
    // thread `source` kernel→kernel so a higher-rank merge sees ids stamped by
    // lower-rank merges (the Ġthe→Ġthey cascade).
    for (auto & g : ruleRanges) {
        if (g.rules.empty()) continue;
        StreamSet * sOut = P.CreateStreamSet(16, 1);
        P.CreateKernelCall<BPEMergeKernel>(source, sOut, g.rules, hashRuleSet(g.rules));
        source = sOut;
    }

    // matchEnd = active (all ones) → emit every byte (Stage-B debug). vocabID = source.
    return {active, source};
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

// buildBaseAlphabet generates the GPT-2 byte-level base alphabet
// (bytes_to_unicode): 256 tokens, each the UTF-8 encoding of a code point, with
// id == its position in the alphabet. Reproduces vocab.json ids 0..255 exactly,
// so merges.txt + this table need no vocab.json. Also fills baseByteId_[v] = id
// of the single-byte token for raw byte value v (the byte-level fallback).
void BPETokenizer::buildBaseAlphabet() {
    if (!baseByteId_.empty()) return;                // build once
    bool printable[256] = {false};                   // bytes kept as their own code point
    for (int b = 33;  b <= 126; ++b) printable[b] = true;
    for (int b = 161; b <= 172; ++b) printable[b] = true;
    for (int b = 174; b <= 255; ++b) printable[b] = true;
    std::vector<int> cps, bytes;                      // code point + raw byte per position
    for (int b = 0; b < 256; ++b) if (printable[b]) { cps.push_back(b); bytes.push_back(b); }
    int n = 0;                                        // non-printables map to 256+n
    for (int b = 0; b < 256; ++b) if (!printable[b]) { cps.push_back(256 + n++); bytes.push_back(b); }

    auto utf8 = [](int cp) {                          // all cps < 0x800 here → ≤ 2 bytes
        std::string s;
        if (cp < 0x80) s.push_back(static_cast<char>(cp));
        else { s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
               s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        return s;
    };
    baseByteId_.assign(256, -1);
    for (size_t i = 0; i < cps.size(); ++i) {         // i == base id
        std::string tok = utf8(cps[i]);
        vocab_[tok] = static_cast<int>(i);
        if (i >= idToToken_.size()) idToToken_.resize(i + 1);
        idToToken_[i] = tok;
        baseByteId_[bytes[i]] = static_cast<int>(i);
    }
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
    buildBaseAlphabet();                         // seed the 256 base tokens (self-sufficient)
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
        std::string a = line.substr(0, sp);
        std::string b = line.substr(sp + 1);
        std::string tok = a + b;
        unsigned id = BYTE_BASE + rank;
        merges_.push_back({a, b, id});                   // keep parts for the merge loop
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
    // Merges/base-alphabet path: every raw byte 0..255 has a base id (covers the
    // 2-byte base tokens like Ġ that a tok.size()==1 scan would miss).
    if (!baseByteId_.empty()) return baseByteId_;
    // Legacy --vocab-only path: only ASCII printables are size-1 tokens.
    std::vector<int> t(256, -1);
    for (const auto & [tok, id] : vocab_)
        if (tok.size() == 1) t[static_cast<uint8_t>(tok[0])] = id;
    return t;
}


// ─── Range partitioning (merge-kernel design) ────────────────────────────────
// buildMergeRuleRanges — resolve each raw merge (parts A,B + idAB) into a
// MergeRule {idA,idB,idAB,lenA,lenB} via the base alphabet + earlier merge
// outputs, then group by idAB-range (256-wide over [0,1024), 1000-wide above),
// sorted by idAB ASC = rank ASC. This is the data the merge-loop kernel consumes.
std::vector<MergeRuleGroup> BPETokenizer::buildMergeRuleRanges() const {
    constexpr unsigned RANGE_WIDTH = 1000;
    auto rangeLo = [](unsigned id) -> unsigned {
        if (id < 1024) return (id / 256) * 256;
        return 1024 + ((id - 1024) / RANGE_WIDTH) * RANGE_WIDTH;
    };
    auto rangeHi = [&](unsigned lo) -> unsigned {
        return lo < 1024 ? lo + 256 : lo + RANGE_WIDTH;
    };

    std::map<unsigned, MergeRuleGroup> byRange;
    unsigned skipped = 0;                            // parts not found in the vocab
    for (const auto & m : merges_) {
        auto ia = vocab_.find(m.a);
        auto ib = vocab_.find(m.b);
        if (ia == vocab_.end() || ib == vocab_.end()) { ++skipped; continue; }
        MergeRule r;
        r.idA  = static_cast<unsigned>(ia->second);
        r.idB  = static_cast<unsigned>(ib->second);
        r.idAB = m.idAB;
        r.lenA = static_cast<unsigned>(m.a.size());
        r.lenB = static_cast<unsigned>(m.b.size());
        unsigned lo = rangeLo(r.idAB);
        MergeRuleGroup & g = byRange[lo];
        g.lo = lo;
        g.hi = rangeHi(lo);
        unsigned mergedLen = r.lenA + r.lenB;
        if (mergedLen > g.maxLen) g.maxLen = mergedLen;
        g.rules.push_back(r);
    }
    if (skipped)
        std::cerr << "BPE: buildMergeRuleRanges skipped " << skipped
                  << " merges whose parts are not in the vocab\n";

    std::vector<MergeRuleGroup> ranges;
    for (auto & [lo, g] : byRange) {                 // sort by idAB ASC = rank ASC
        std::sort(g.rules.begin(), g.rules.end(),
                  [](const MergeRule & a, const MergeRule & b) { return a.idAB < b.idAB; });
        ranges.push_back(std::move(g));
    }
    return ranges;
}