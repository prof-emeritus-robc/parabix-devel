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
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/core/attributes.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <stdexcept>

using namespace pablo;
using namespace kernel;

namespace {

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

// BPERangeSeed — the FIRST BPE kernel. It turns each raw input byte into its
// base-alphabet token id: the starting single-byte token, before any merge.
// Outputs:
//   source (16-bit) — base id of each byte   (the id stream the merges build on)
//   active (1-bit)  — all 1s                  (reused downstream as emit-all)
//   end    (1-bit)  — all 0s                  (legacy; unused)
//
// GPT-2's byte-level alphabet (encoder.py bytes_to_unicode) maps
// each raw byte to a printable char, then encoder.json assigns each char an id =
// its POSITION in the alphabet. The alphabet lists the 188 "printable" bytes
// first (ids 0..187), then the 68 remaining bytes (ids 188..255). So a byte's id
// is NOT its value and NOT its unicode code point — it is its position.
//
// Composing byte→char→id gives a closed-form piecewise map: id = byte + offset,
// one offset per contiguous byte range. We compute it by ARITHMETIC (a few range
// masks + Sel) instead of a 256-way EQ lookup table — far fewer Pablo ops. The
// alphabet is a fixed spec, so this kernel is data-independent (constant cache
// name). 
//
//   byte range   offset   id            byte range   offset   id
//   0..32        +188     188..220      161..172     −67      94..105
//   33..126      −33      0..93         173          →255     255
//   127..160     +94      221..254      174..255     −68      106..187
// (e.g. space 32 → 220 = 'Ġ', 'y' 121 → 88, 't' 116 → 83.)
class BPERangeSeed : public PabloKernel {
public:
    BPERangeSeed(LLVMTypeSystemInterface & ts, StreamSet * basis,
                 StreamSet * source, StreamSet * active, StreamSet * end,
                 unsigned outW)
    : PabloKernel(ts, "BPERangeSeed",
                  {Binding{"basis", basis}},
                  {Binding{"source", source}, Binding{"active", active}, Binding{"end", end}}),
      mOutW(outW) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        // `b` = the input byte value at each position, as an 8-bit BixNum built
        // from the 8 basis bit-streams.
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        BixNum b(basisBits.begin(), basisBits.end());
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        // One 1-bit mask per byte range — 1 wherever the byte falls in that range.
        // The six ranges are mutually exclusive and cover all of 0..255; 174..255
        // is the default (else) arm of the Select below, so it needs no mask.
        PabloAST * m_0_32    = bnc.ULE(b, 32);                                  // control block 1
        PabloAST * m_33_126  = pb.createAnd(bnc.UGE(b, 33),  bnc.ULE(b, 126));  // printable ASCII
        PabloAST * m_127_160 = pb.createAnd(bnc.UGE(b, 127), bnc.ULE(b, 160));  // control block 2
        PabloAST * m_161_172 = pb.createAnd(bnc.UGE(b, 161), bnc.ULE(b, 172));  // printable Latin-1 lo
        PabloAST * m_173     = bnc.EQ(b, 173);                                  // the lone non-printable

        // id = byte + per-range offset (173 is a fixed 255). AddModular/SubModular
        // are modular on 8 bits; every SELECTED range lands in 0..255 with no
        // wrap, so the values are exact (unselected arms may wrap but are discarded
        // by the Select). Nested Select picks the arm for each position's range.
        BixNum id = bnc.Select(m_0_32,    bnc.AddModular(b, 188),   // 0..32   → +188
                    bnc.Select(m_33_126,  bnc.SubModular(b, 33),    // 33..126 → −33
                    bnc.Select(m_127_160, bnc.AddModular(b, 94),    // 127..160→ +94
                    bnc.Select(m_161_172, bnc.SubModular(b, 67),    // 161..172→ −67
                    bnc.Select(m_173,     bnc.Create(255),          // 173     → 255
                                          bnc.SubModular(b, 68))))));// 174..255→ −68 (default)

        // Write the id into the `source` stream (mOutW bits — base ids ≤255 fit in
        // 8; any extra declared bits are zero). active = all 1s, end = all 0s.
        Var * sOut = getOutputStreamVar("source");
        for (unsigned i = 0; i < mOutW; i++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)),
                            (i < id.size()) ? id[i] : zeroes);
        pb.createAssign(pb.createExtract(getOutputStreamVar("active"), pb.getInteger(0)), ones);
        pb.createAssign(pb.createExtract(getOutputStreamVar("end"),    pb.getInteger(0)), zeroes);
    }
private:
    unsigned mOutW;
};

// Hash a rule group's (idA,idB,idAB,lenB) list → unique cache name per kernel,
// since the Pablo body is data-dependent (same shape, different rules).
uint64_t hashRuleSet(const std::vector<MergeRule> & rules) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t x){ h = (h ^ x) * 0x100000001B3ull; };
    for (const auto & r : rules) { mix(r.idA); mix(r.idB); mix(r.idAB); mix(r.lenB); }
    return h;
}

// Minimum bit-width to represent ids 0..maxId (>=1). Lets each range kernel run a
// narrow id BixNum instead of a fixed 16 bits: fewer per-bit EQ/Sel ops → smaller
// LLVM IR → faster JIT. Width grows down the pipeline as ids get bigger.
//   maxId 255→8, 511→9, 1023→10, 2047→11, 4095→12, 50256→16.
static unsigned bitsFor(unsigned maxId) {
    unsigned b = 1;
    while (b < 16 && (1u << b) <= maxId) ++b;
    return b;
}
// this kernel pads a narrow id stream back up to 16 bits 
// Zero-extend an N-bit id stream to 16 bits. The pipeline runs narrow interior
// streams; emission (P2S16Kernel) needs a 16-bit BixNum, so the final source is
// widened once here. Shape-only (no token data) → constant cache name.
class BPEWidenKernel : public PabloKernel {
public:
    BPEWidenKernel(LLVMTypeSystemInterface & ts, StreamSet * in, StreamSet * out)
    : PabloKernel(ts, "BPEWiden16", {Binding{"in", in}}, {Binding{"out", out}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        // Widen the input stream to 16 bits. The input may be narrower than 16 bits, so
        // we zero-extend it. The output is a 16-bit BixNum. 
        std::vector<PabloAST*> in = getInputStreamSet("in");
        PabloAST * zeroes = pb.createZeroes();
        Var * out = getOutputStreamVar("out");
        // Write the input bits to the output, zero-extending if necessary.
        for (unsigned i = 0; i < 16; i++)
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)),
                            (i < in.size()) ? in[i] : zeroes);
    }
};

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
                   StreamSet * sourceIn, StreamSet * meIn,
                   StreamSet * sourceOut, StreamSet * meOut,
                   std::vector<MergeRule> rules, uint64_t shapeHash,
                   unsigned maxLen, unsigned outW)
    : PabloKernel(ts, "BPEMerge_h" + std::to_string(shapeHash),
                  {Binding{"sourceIn", sourceIn, FixedRate(), LookAhead(maxLen)},
                   Binding{"meIn", meIn}},
                  {Binding{"sourceOut", sourceOut}, Binding{"meOut", meOut}}),
      mRules(std::move(rules)), mOutW(outW) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> srcBits = getInputStreamSet("sourceIn");
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        //
        // WITHIN A KERNEL - Out put Stream from one step goes to the next.
        // cur reads from idAcc and sees the effects of earlier merges in the SAME kernel.
        //
        // idAcc starts as a copy of the input (srcBits)
        // Each rule reads cur from the current idAcc → sees stamps from earlier rules in this same kernel. 
        // That's the intra-kernel carry.
        // Final idAcc written to sourceOut → becomes next kernel's input.
                
        // idAcc threaded as PLAIN values, NOT Pablo Vars: each
        // rule reassigns idAcc[i] = Sel(...) so a later rule reads the just-stamped
        // id.
        const unsigned inW = srcBits.size();               // input id width (prev kernel's outW)
        std::vector<PabloAST*> idAcc(mOutW);               // threaded id stream at THIS range's width
        for (unsigned i = 0; i < mOutW; i++)
            idAcc[i] = (i < inW) ? srcBits[i] : zeroes;     // zero-extend input up to outW

        // matchEnd — 1-bit boundary mask, threaded kernel→kernel (seeded all-ones =
        // every byte ends a base token). Each fired merge A+B→AB clears the swallowed
        // seam (A's last byte) so only outermost token ends survive.
        PabloAST * matchEnd = getInputStreamSet("meIn")[0];   // 1-bit boundary mask, ones from active

        // One looked-ahead copy of sourceIn per DISTINCT lenB. LookAhead(d) reads
        // position p+d — legal only on an INPUT (sourceIn declares LookAhead(maxLen)).
        // Used by the seam clear: is idB present lenB bytes ahead of A's end?
        std::map<unsigned, BixNum> aheadByLen;
        for (const auto & r : mRules) {
            if (aheadByLen.count(r.lenB)) continue;
            std::vector<PabloAST*> bits(inW, zeroes);
            for (unsigned i = 0; i < inW; i++)
                bits[i] = pb.createLookahead(srcBits[i], (int64_t) r.lenB);
            aheadByLen.emplace(r.lenB, BixNum(bits.begin(), bits.end()));
        }

        // For each rule (rank order): detect the merge inside a createIf(Aend) gate
        // (block-skip), carry the fire out via a NON-self-ref Var mergeV, then stamp
        // idAB into the plain-value idAcc OUTSIDE the gate. 
        for (const auto & r : mRules) {   // rules in rank order (lowest id first)
            BixNum cur(idAcc.begin(), idAcc.end());          // current (cascaded) id stream
            PabloAST * Aend = bnc.EQ(cur, r.idA);            // token A ends here

            Var * mergeV = pb.createVar("merge", zeroes);    // temporary variable to indicate where a merge should happen
            auto body = pb.createScope();
            BixNumCompiler bncB(body);
            BixNum curB(idAcc.begin(), idAcc.end());
            PabloAST * Bend = bncB.EQ(curB, r.idB);          // B ends here
            body.createAssign(mergeV,                        // AB ends here: A then B adjacent (B's end)
                body.createAnd(body.createAdvance(Aend, r.lenB), Bend));
            pb.createIf(Aend, body);                         // skip body in blocks with no idA

            for (unsigned i = 0; i < mOutW; i++)             // stamp idAB at the merge
                idAcc[i] = pb.createSel(mergeV, ((r.idAB >> i) & 1u) ? ones : zeroes, idAcc[i]);

            // Mask logic to clear the swallowed A-end from matchEnd. The merge is detected at
            // A's end, but the A-end is swallowed by the merge → clear that bit from matchEnd. 
            // The B-end is where the merged token AB ends, so it stays.
            PabloAST * Bahead     = bnc.EQ(aheadByLen.at(r.lenB), r.idB);  // is idB lenB bytes ahead of A's end?
            PabloAST * mergedAend = pb.createAnd(Aend, Bahead);            // A ends here AND B follows = swallowed seam
            matchEnd = pb.createAnd(matchEnd, pb.createNot(mergedAend), "meClear");   // erase that seam bit
        }

        // Write the final idAcc into the mOutW-bit `sourceOut` stream and matchEnd
        // into the 1-bit `meOut`. The next kernel reads these.
        Var * sOut = getOutputStreamVar("sourceOut");
        for (unsigned i = 0; i < mOutW; i++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), idAcc[i]);  // mOutW-bit token ID stream
        pb.createAssign(pb.createExtract(getOutputStreamVar("meOut"), pb.getInteger(0)), matchEnd);
    }
private:
    std::vector<MergeRule> mRules;
    unsigned mOutW;
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

    // Id width grows down the pipeline: the seed holds base ids (≤255 → 8 bits),
    // each range kernel widens to fit its top id (bitsFor(hi-1)), the final source is
    // widened to 16 for emission. Narrow interiors = fewer per-bit EQ/Sel = faster JIT.
    // Base alphabet (bytes_to_unicode) assigns ids 0..255 → 8 bits.
    unsigned seedW = bitsFor(255);

    // Seed: source = base id of each raw byte, active = ones, end = zeroes.
    StreamSet * source = P.CreateStreamSet(seedW, 1);
    StreamSet * active = P.CreateStreamSet(1, 1);
    StreamSet * end    = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<BPERangeSeed>(basis, source, active, end, seedW);

    // Widen an id stream of width w to the 16-bit BixNum emission expects (P2S16).
    auto widen16 = [&](StreamSet * s, unsigned w) -> StreamSet * {
        // If the final width is already 16 (full GPT-2 vocab), it skips the kernel entirely
        if (w >= 16) return s;
        StreamSet * s16 = P.CreateStreamSet(16, 1);
        P.CreateKernelCall<BPEWidenKernel>(s, s16);
        return s16;
    };

    if (std::getenv("BPE_DBG") && std::string(std::getenv("BPE_DBG")) == "seed")
        return {active, widen16(source, seedW)};   // dump seed source at every byte

    //
    // BETWEEN KERNELS, the source is an input and sOut is the output. 
    // kernel reads source (seed = raw byte ids), writes sOut. Then source = sOut.
    // kernel reads that SAME sOut as its input, writes a new one. And so on.
    // The next kernel's input = the previous kernel's output. 
    // So kernel N receives the id stream already stamped by kernels 1..N−1. 
    // It does not re-run their rules — their merges are baked into the ids it reads. 
    // Each kernel only applies its own range's rules once → no duplicated work.
    //
    // One BPEMergeKernel per id-range, ascending = rank order. Fresh buffers
    // thread `source` kernel→kernel so a higher-rank merge sees ids stamped by
    // lower-rank merges (the Ġthe→Ġthey cascade).
    // matchEnd seed = active (all ones = every byte ends a base token); each kernel
    // clears the seams its merges swallow, so the final stream marks only real ends.
    StreamSet * matchEnd = active;
    unsigned curW = seedW;                               // current source id width
    for (auto & g : ruleRanges) {
        if (g.rules.empty()) continue;
        unsigned outW = std::max(curW, bitsFor(g.hi - 1)); // widen to hold this range's ids
        StreamSet * sOut  = P.CreateStreamSet(outW, 1);
        StreamSet * meOut = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<BPEMergeKernel>(source, matchEnd, sOut, meOut,
                                           g.rules, hashRuleSet(g.rules), g.maxLen, outW);
        source   = sOut;
        matchEnd = meOut;
        curW     = outW;
    }

    // matchEnd now marks real (outermost) token ends; vocabID = source (widened to 16).
    return {matchEnd, widen16(source, curW)};
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
// so merges.txt needs no vocab.json. Fills vocab_/idToToken_ (base parts must be
// resolvable by buildMergeRuleRanges + decodeToken). The seed computes base ids
// arithmetically, so no byte→id table is produced here.
void BPETokenizer::buildBaseAlphabet() {
    if (baseBuilt_) return;                          // build once
    baseBuilt_ = true;
    bool printable[256] = {false};                   // bytes kept as their own code point
    for (int b = 33;  b <= 126; ++b) printable[b] = true;   // printable ASCII characters.
    for (int b = 161; b <= 172; ++b) printable[b] = true;   // printable Latin-1 characters.
    for (int b = 174; b <= 255; ++b) printable[b] = true;   // printable Latin-1 characters.
    std::vector<int> cps;                             // code point per alphabet position
    for (int b = 0; b < 256; ++b) if (printable[b]) cps.push_back(b);
    int n = 0;                                        // non-printables map to 256+n
    for (int b = 0; b < 256; ++b) if (!printable[b]) cps.push_back(256 + n++);

    auto utf8 = [](int cp) {                          // all cps < 0x800 here → ≤ 2 bytes
        std::string s;
        if (cp < 0x80) s.push_back(static_cast<char>(cp));
        else { s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
               s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        return s;
    };
    // Each position i is the base-alphabet id of the UTF-8 token utf8(cps[i]).
    for (size_t i = 0; i < cps.size(); ++i) {         // i == base id
        std::string tok = utf8(cps[i]);
        vocab_[tok] = static_cast<int>(i);
        if (i >= idToToken_.size()) idToToken_.resize(i + 1);
        idToToken_[i] = tok;
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

// ─── Range partitioning (merge-kernel design) ────────────────────────────────
// creating range groups
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