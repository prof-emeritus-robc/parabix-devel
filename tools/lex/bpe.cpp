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
 *      {idA, idB, idAB, lenA, lenB} via the base alphabet + earlier merge outputs
 *      (no vocab.json needed; lenA/lenB = RAW-byte length = codepoint count, see
 *      rawByteLen — NOT display-byte size). Partition into CLEAN ranges (idAB/rank
 *      ASC): a range [lo,hi) holds only rules that are BOTH dependency-independent
 *      (idA,idB < lo) AND token-adjacency-overlap-free (no two rules share a middle
 *      token: right id of one == left id of the other). Full GPT-2 → 1123 ranges.
 *      Mirrors merge_analysis.py::clean_range_analysis.
 *
 *  Runtime pipeline (one BPEMergeKernel per clean range, lowest range first):
 *      BPERangeSeed   — source = base id of each RAW byte (8-bit), active = 1s,
 *                       end = 0s (end is legacy/unused).
 *      BPEMergeKernel — START-anchored, per rule (rank order):
 *                         Astart    = EQ(source, idA)                  // A starts here
 *                         BstartAtA = EQ(LookAhead(source, lenA), idB) // B lenA ahead
 *                         merge     = inPlayMask AND Astart AND BstartAtA
 *                       stamp idAB at A's START via Sel; consume B's start via
 *                       inPlayMask &= NOT(Advance(merge, lenA)). B is read from the
 *                       INPUT via LookAhead (forward reads are input-only; sourceIn
 *                       declares LookAhead(maxLen)). `source` threads kernel→kernel
 *                       (width grows to ceil_log2(hi+1) bits per range), so a
 *                       higher-rank merge sees ids stamped by lower-rank ones (the
 *                       Ġthe→Ġthey cascade).
 *
 *  Emission (Stage-B): matchEnd = the final inPlayMask (surviving token STARTS) —
 *  each fired merge clears the consumed B-start, so the id at each surviving start
 *  prints. Full HF-equivalence not yet validated (Stage C).
 *
 *  Cache keys
 *  ──────────
 *  BPEMergeKernel name encodes hashRuleSet(the group's rule list), so two merges
 *  files producing the same range shape but different rules still get distinct
 *  cache entries. BPERangeSeed is shape-only (constant name).
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
#include <llvm/Support/CommandLine.h>
#include <pablo/pablo_kernel.h>
#include <pablo/builder.hpp>
#include <pablo/pe_zeroes.h>
#include <pablo/bixnum/bixnum.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/re_cc.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/stream_shift.h>   // IndexedShiftBack (BPE_INDEXED_SHIFT)
#include <stdexcept>
#include <boost/intrusive/detail/math.hpp>
using boost::intrusive::detail::ceil_log2;

using namespace llvm;
// Every optional BPE optimization defaults OFF — a bare run is the plain, unoptimized
// pipeline, and each optimization is opted into on the command line. 
static cl::opt<unsigned> CompactionBase(
    "compact-base",
    cl::desc("Base kernel at which filter-by-mask compaction is first applied "
             "(0 = off, the default)."),
    cl::init(0));

static cl::opt<bool> GeometricCompaction(
    "geometric-compaction",
    cl::desc("Use a geometric rather than an arithmetic compaction schecule."),
    cl::init(false));

// Grouped-if: rules sharing a first id (idA) collapse under ONE createIf gate
// (shared Astart EQ) instead of one per rule. Single-level (no nested if → T6).
// Applied only to kernels with index >= IfGroupLowerLimit — the early kernels
// fire on almost every block (grouping there saves gates but never skips), so
// grouping is aimed at the later kernels. -1 = off (per-rule everywhere).
static cl::opt<int> IfGroupLowerLimit(
    "if-group-lower-limit",
    cl::desc("Group merge rules by first id under one createIf, for kernels at/after "
             "this index (-1 = off)."),
    cl::init(-1));


// FIXED COUNT: give every kernel exactly K grouped-if gates. Gate SIZE then VARIES per
// kernel = rules/K (a 900-rule kernel -> 900/K rules per gate, a 10-rule kernel -> 10/K).
// Same gate structure everywhere, scales with kernel size. Only active with
// --if-group-lower-limit >= 0. Default 1 = one gate covering all the kernel's rules.
static cl::opt<unsigned> IfGroupCount(
    "if-group-count",
    cl::desc("Grouped-if gates per kernel (only with --if-group-lower-limit >= 0). "
             "Gate size = rules/count; count scales with kernel. Default 1."),
    cl::init(1));

// Effective grouped-if chunk size for a kernel with n rules: n/IfGroupCount rules per gate.
// Used by BOTH the cache-name tag and the Pablo body so they never disagree (stale-cache hazard).
static unsigned effGroupSize(size_t n) {
    return std::max<unsigned>(1u, (unsigned)(n / std::max(1u, IfGroupCount.getValue())));
}

// move the B-detection LookAhead INSIDE each rule's createIf gate instead of
// hoisting one shared shift per distinct lenA outside all gates. Inside = skippable on cold
// blocks but DUPLICATED per rule (loses the per-lenA dedup); outside (default) = shared but
// runs every block. 
static cl::opt<bool> LookaheadInGate(
    "lookahead-in-gate",
    cl::desc("Build the B-detection LookAhead inside each rule's if-gate (per-rule, "
             "skippable) instead of one shared hoisted shift per lenA (default)."),
    cl::init(false));


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
        std::vector<PabloAST*> bits = getInputStreamSet("basis");
        // newline (0x0A) is a byte-value test on the input basis → character class.
        cc::Parabix_CC_Compiler_Builder ccc(bits);
        PabloAST * isNL = ccc.compileCC(re::makeByte(0x0A), pb);
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
                 StreamSet * source, StreamSet * active, StreamSet * end)
    : PabloKernel(ts, "BPERangeSeed",
                  {Binding{"basis", basis}},
                  {Binding{"source", source}, Binding{"active", active}, Binding{"end", end}}) {}
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
        // Each range is a byte-value character class over the input basis, so compileCC
        // replaces the BixNum ULE/UGE/EQ comparisons 1:1. The id arithmetic below stays
        // BixNum (CC yields only a 1-bit mask, not the multi-bit id).
        cc::Parabix_CC_Compiler_Builder ccc(basisBits);
        PabloAST * m_0_32    = ccc.compileCC(re::makeByte(0,   32),  pb);       // control block 1
        PabloAST * m_33_126  = ccc.compileCC(re::makeByte(33,  126), pb);       // printable ASCII
        PabloAST * m_127_160 = ccc.compileCC(re::makeByte(127, 160), pb);       // control block 2
        PabloAST * m_161_172 = ccc.compileCC(re::makeByte(161, 172), pb);       // printable Latin-1 lo
        PabloAST * m_173     = ccc.compileCC(re::makeByte(173),      pb);       // the lone non-printable

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

        // Write the id into the 16-bit `source` stream (ids ≤255 → high bits 0),
        // active = all 1s, end = all 0s.
        Var * sOut = getOutputStreamVar("source");
        for (unsigned i = 0; i < 8; i++) {
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), id[i]);
        }
        pb.createAssign(pb.createExtract(getOutputStreamVar("active"), pb.getInteger(0)), ones);
        pb.createAssign(pb.createExtract(getOutputStreamVar("end"),    pb.getInteger(0)), zeroes);
    }
};

// ─── BPEOnesKernel ──────────────────────────────────────────────────────────
// Emit an all-ones 1-bit stream at the rate of `anchor` (consumed for its RATE
// only, never read). Used right after a FilterByMask compaction: every position
// that survived the filter was, by construction, a LIVE token start, so the fresh
// inPlayMask at the new (shorter) rate is all ones.
//
// createInFile, not a bare Not(Zeroes): the compacted stream's item count is a
// popcount and rarely lands on a block boundary, so a bare all-ones bleeds into the
// final block's PADDING. Those padded ones survive to matchEnd and the scan emits
// them as junk tokens past end-of-input (observed: exactly 128 rows of `id=0`).
// InFile(x) compiles to x AND NOT EOFmask, so the padding reads 0. Same primitive as
// the EOF trailing-whitespace fix in InFileNonSpaceKernel (pretokenizer.cpp).
class BPEOnesKernel : public PabloKernel {
public:
    BPEOnesKernel(LLVMTypeSystemInterface & ts, StreamSet * anchor, StreamSet * ones)
    // Name says InFile: the body is data-independent so the cache key is the name
    // alone, and the pre-InFile version would otherwise be served from objcache.
    : PabloKernel(ts, "BPE_OnesInFile",
                  {Binding{"anchor", anchor}},
                  {Binding{"ones",   ones}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        pb.createAssign(pb.createExtract(getOutputStreamVar("ones"), pb.getInteger(0)),
                        pb.createInFile(pb.createNot(pb.createZeroes())));
    }
};

// Hash a rule group's (idA,idB,idAB,lenA,lenB) list → unique cache name per kernel,
// since the Pablo body is data-dependent (same shape, different rules). lenA MUST be
// in the hash: it is the LookAhead/Advance distance, and applyCompactionSchedule
// rewrites it from byte distance to slot distance, so two schedules give identical
// (idA,idB,idAB) with different bodies. Conversely, two schedules that yield the same
// lenA set for a group produce the same body and correctly share a cache entry.
uint64_t hashRuleSet(const std::vector<MergeRule> & rules) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t x){ h = (h ^ x) * 0x100000001B3ull; };
    for (const auto & r : rules) { mix(r.idA); mix(r.idB); mix(r.idAB); mix(r.lenA); mix(r.lenB); }
    return h;
}

// ─── applyCompactionSchedule (BPE_COMPACT_EVERY=K) ──────────────────────────
// Decide WHERE to inject a FilterByMask compaction into the merge-kernel chain, and
// rewrite every rule's merge distance to match the resulting coordinate space. This
// is pure preprocessing.
//
// This is a preprocessing step that prepares the merge pipeline before any kernels are created.
// Returns compactAfter[i] = inject a compaction after merge kernel i. K = 0 returns
// all-false and leaves every lenA untouched, so the byte-space path stays bit-exact.
static std::vector<bool> applyCompactionSchedule(
        std::vector<MergeRuleGroup> & ruleRanges, unsigned K) {
    unsigned nextCompaction = K;

    //  Compact after every K kernels. The first block is always byte space (F=256),
    //  so the first K kernels see the original lenA = byte distance. 
    std::vector<bool> compactAfter(ruleRanges.size(), false);
    if (nextCompaction == 0) return compactAfter;

    // parts[idAB] = (idA, idB) for every merged token. Base ids (< 256) are absent —
    // they are atomic and terminate the recursion. lookup table
    std::map<unsigned, std::pair<unsigned,unsigned>> parts;
    for (const auto & g : ruleRanges)
        for (const auto & r : g.rules)
            parts.emplace(r.idAB, std::make_pair(r.idA, r.idB));

    unsigned frontier = 256;                 // before any compaction: byte space
    std::map<unsigned, unsigned> memo;        // valid for the CURRENT frontier only
    // slotSpan(id) = the number of tokens that id spans. 
    std::function<unsigned(unsigned)> slotSpan = [&](unsigned id) -> unsigned {
        // 
        if (id < frontier) return 1;
        auto m = memo.find(id);
        if (m != memo.end()) return m->second;
        auto p = parts.find(id);
        if (p == parts.end()) return 1;       // unresolved part → treat as atomic
        unsigned v = slotSpan(p->second.first) + slotSpan(p->second.second);
        memo.emplace(id, v);
        return v;
    };

    // Walk the rule ranges in order, counting kernels since the last compaction. When
    // we hit K, mark this range for compaction and reset the frontier to hi_i. 
    //Recompute every rule's lenA in the new frontier space. 
    unsigned sinceCompact = 0, nCompact = 0, firstBlockDisagree = 0;
    for (size_t i = 0; i < ruleRanges.size(); i++) {
        auto & g = ruleRanges[i];
        if (g.rules.empty()) continue;
        unsigned maxDist = 0;
        for (auto & r : g.rules) {
            unsigned d = slotSpan(r.idA);
            if (nCompact == 0 && d != r.lenA) firstBlockDisagree++;
            r.lenA = d;
            if (d > maxDist) maxDist = d;
        }
        // Update the maximum length in the current rule range.
        g.maxLen = maxDist;                   // LookAhead binding must cover every lenA
        // Decide whether to compact after this range. If we do, the next frontier is
        // the hi of this range, and we reset the memoization table. Otherwise, the
        // frontier stays the same and we keep memoizing.   
        if (++sinceCompact < nextCompaction) continue;
        compactAfter[i] = true;
        frontier = g.hi;                      // every live id after kernel i is < hi_i
        memo.clear();
        sinceCompact = 0;
        if (GeometricCompaction) {
            nextCompaction *=2;
        }
        nCompact++;
    }
    std::cerr << "[BPE] compaction: BPE_COMPACT_EVERY=" << K << " -> "
              << nCompact << " FilterByMask points\n";
    if (firstBlockDisagree)
        std::cerr << "[BPE] WARNING: " << firstBlockDisagree
                  << " first-block slot/byte distance disagreements (merge table bug?)\n";
    return compactAfter;
}

// selfMergeFireStarts — non-overlapping pairing for a SELF-merge X+X→XX (idA==idB).
// X is ANY repeated token, not just dots: a single byte (l+l→ll, 0+0→00, .+.→..,
// ' '+' '→two-space), or an already-merged multi-byte token (--+--→----, ..+..→....,
// ==+==→====). `isX` = the mask of X-token START positions; L = X's raw-byte length,
// so consecutive X starts sit L bytes apart. Returns the subset that should FIRE the
// merge: the 1st, 3rd, 5th, … X in EACH maximal run of consecutive X tokens (per-run
// parity, reset at every run start).
//
// Why: a self-merge fired at EVERY X-start over-consumes — position i glues (i,i+L)
// while position i+L glues (i+L,i+2L), so the shared token is pulled two ways and
// the whole run collapses to one surviving start (the repeated-run bug: any run of a
// repeated token — "lllll", "00000", "     ", "....." — implodes to one token). True
// BPE pairs left-to-right, using each token once. Firing only at per-run-odd positions
// reproduces that: (0,1)(2,3)(4,5)…
//
// Idiom ported from lib/kernel/unicode/normalization.cpp::SelfComposableLogic:
// EveryNth gives GLOBAL parity of the X starts; MatchStar spreads each run's
// starting parity across the run so the odd/even choice RESETS per run (otherwise a
// preceding odd-length run would flip the next run's pairing).
static PabloAST * selfMergeFireStarts(PabloBuilder & pb, PabloAST * isX, unsigned L) {
    // X-token body: the L bytes each X covers (start .. start+L-1). For L==1 this is
    // just isX; the body connects consecutive X tokens so MatchStar spans the gaps.
    PabloAST * body = isX;
    for (unsigned k = 1; k < L; k++)
        body = pb.createOr(body, pb.createAdvance(isX, (int64_t) k));
    // A run starts at an X with no X-token ending immediately L bytes before it.
    PabloAST * runStart = pb.createAnd(isX, pb.createNot(pb.createAdvance(isX, (int64_t) L)));
    PabloAST * A1 = pb.createEveryNth(isX, pb.getInteger(2));  // global 1st,3rd,5th…
    PabloAST * A2 = pb.createXor(isX, A1);                     // global 2nd,4th,6th…
    PabloAST * A1_start = pb.createAnd(runStart, A1);
    PabloAST * A2_start = pb.createAnd(runStart, A2);
    PabloAST * A1_runs = pb.createMatchStar(A1_start, body);   // runs whose 1st X is global-odd
    PabloAST * A2_runs = pb.createMatchStar(A2_start, body);   // runs whose 1st X is global-even
    // Per-run 1st,3rd,5th X = the fire (A-start) positions.
    return pb.createOr(pb.createAnd(A1_runs, A1), pb.createAnd(A2_runs, A2));
}

// BPEMergeKernel — one id-range, REAL BPE merge on the id stream (START-anchored).
// `source` carries a token id at each token's START byte (a base byte is a 1-byte
// token → start == end; seeded = base id per raw byte). For each rule (A,B → AB),
// in idAB-ASC = rank-ASC order:
//     Astart    = EQ(source, idA)                    // token A starts here
//     BstartAtA = EQ(LookAhead(source, lenA), idB)   // B starts lenA bytes later
//     merge     = inPlayMask AND Astart AND BstartAtA // A then B adjacent, not consumed
//     source ← Sel(merge, idAB, source)              // stamp merged id at A's start
// B is read from the INPUT via LookAhead (forward reads are input-only); safe because
// independent_range_analysis puts both parts (idA,idB < lo) in a lower kernel's output
// = this kernel's input. `inPlayMask` (1-bit, threaded meIn→meOut) starts all-ones and
// each merge clears B's start, so after all merges it marks the surviving (outermost)
// token STARTS — the stream emission scans (id read at each start byte).
class BPEMergeKernel : public PabloKernel {
public:
    // boundaryIn (optional, may be null): 1-bit per-byte mask of pretoken STARTS.
    // When present, a merge is blocked if B starts a new pretoken (boundary at
    // A_start+lenA), so merges never cross a pretoken boundary (the 't/quote bug).
    // nextIdIn (optional, may be null): the id of the NEXT LIVE token brought back to
    // each live position, precomputed OUTSIDE the kernel by IndexedShiftBack(inPlayMask,
    // source). When present (BPE_INDEXED_SHIFT), B-detection reads EQ(nextId, idB) at A's
    // position instead of EQ(LookAhead(source, lenA), idB) — one shifted stream replaces
    // every per-lenA multi-bit LookAhead — and B is consumed via the inline forward
    // createIndexedAdvance(mergeV, inPlayMask, 1) instead of Advance(mergeV, lenA).
    BPEMergeKernel(LLVMTypeSystemInterface & ts,
                   StreamSet * sourceIn, StreamSet * meIn, StreamSet * boundaryIn,
                   StreamSet * nextIdIn,
                   StreamSet * sourceOut, StreamSet * meOut,
                   MergeRuleGroup group, uint64_t shapeHash, unsigned maxLen,
                   bool grouped = false)
    // name must encode every body-shaping param hashRuleSet omits
    // — LookAhead distance (L=maxLen, changes with compaction), input width (w) and accumulator width (o, dev 12-bit vs full 16-bit)
    // — else objcache serves a mismatched compiled body. x1_ = indexed-shift body, g1_ = grouped-if body.

    : PabloKernel(ts, std::string("BPEMerge_") + (nextIdIn ? "x1_" : "") + (boundaryIn ? "b1_" : "")
                        + (grouped ? "g" + std::to_string(effGroupSize(group.rules.size())) + "_" : "")
                        + (LookaheadInGate ? "la1_" : "la0_")
                        + "w" + std::to_string(sourceIn->getNumElements())
                        + "o" + std::to_string(ceil_log2(group.hi + 1))
                        + "L" + std::to_string(maxLen) + "_h" + std::to_string(shapeHash),
                  mergeInputs(sourceIn, meIn, boundaryIn, nextIdIn, maxLen),
                  {Binding{"sourceOut", sourceOut}, Binding{"meOut", meOut}}),
      mRuleGroup(group), mHasBoundary(boundaryIn != nullptr), mUseNextId(nextIdIn != nullptr),
      mGrouped(grouped) {}
protected:
    static std::vector<kernel::Binding> mergeInputs(
            StreamSet * sourceIn, StreamSet * meIn, StreamSet * boundaryIn,
            StreamSet * nextIdIn, unsigned maxLen) {
        std::vector<kernel::Binding> in {
            Binding{"sourceIn", sourceIn, FixedRate(), LookAhead(maxLen)},
            Binding{"meIn", meIn} };
        if (boundaryIn)
            in.push_back(Binding{"boundaryIn", boundaryIn, FixedRate(), LookAhead(maxLen)});
        if (nextIdIn)   // Deferred producer (IndexedShiftBack), 2. Kernel receives both as input bindings
            in.push_back(Binding{"nextIdIn", nextIdIn});
        return in;
    }
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        BixNumCompiler bnc(pb);
        std::vector<PabloAST*> srcBits = getInputStreamSet("sourceIn");
        const unsigned W = srcBits.size();
        const unsigned W_out = ceil_log2(mRuleGroup.hi + 1);
        PabloAST * zeroes = pb.createZeroes();

        // idAcc — the id stream we mutate; starts as a copy of the input ids.
        std::vector<Var *> idAcc(W_out);
        for (unsigned i = 0; i < W_out; i++) {
            if (i < W) {
                idAcc[i] = pb.createVar("idAcc_" + std::to_string(i), srcBits[i]);
            } else {
                idAcc[i] = pb.createVar("idAcc_" + std::to_string(i), zeroes);
            }
        }

        // inPlayMask — 1-bit mask: which byte positions are still live token starts.
        // Threaded kernel→kernel (meIn/meOut), seeded all-ones. Each fired merge clears
        // B's start (interior seam); A's start survives as AB's start. Final mask marks
        // the surviving (outermost) token STARTS — the stream emission scans.
        Var * inPlayMask = pb.createVar("inPlayMask", getInputStreamSet("meIn")[0]);

        // 3. Read the copy inside the body
        // nextIdBN — indexed mode: the NEXT LIVE token's id at each position (precomputed
        // by IndexedShiftBack outside). B-detection reads EQ(nextIdBN, idB) — one shifted
        // stream for every rule, no per-lenA LookAhead.
        BixNum nextIdBN;
        if (mUseNextId) {
            auto nb = getInputStreamSet("nextIdIn");
            nextIdBN = BixNum(nb.begin(), nb.end());
        }

        // aheadByLenA[len] = the source id stream shifted so position p reads the id
        // `len` bytes ahead. One copy per distinct lenA; a rule uses aheadByLenA[lenA]
        // to check what token starts right after A. LookAhead is legal only on an INPUT
        // (sourceIn declares LookAhead(maxLen), and lenA < mergedLen ≤ maxLen).
        // Skipped in indexed mode — nextIdBN replaces every source LookAhead.
        // 4. Because of the copy → NO LookAhead built into the body, so the kernel's body is identical for every rule (no per-lenA LookAhead).
        // Skipped when --lookahead-in-gate: emitBody builds the shift per rule instead.
        std::map<unsigned, BixNum> aheadByLenA;
        if (!mUseNextId && !LookaheadInGate)
        for (const auto & r : mRuleGroup.rules) {
            if (aheadByLenA.count(r.lenA)) continue;
            std::vector<PabloAST*> bits(W);
            for (unsigned i = 0; i < W; i++)
                bits[i] = pb.createLookahead(srcBits[i], (int64_t) r.lenA);
            aheadByLenA.emplace(r.lenA, BixNum(bits.begin(), bits.end()));
        }

        // boundaryAheadByLen[lenA] = is a pretoken START lenA bytes ahead (= at B's
        // start)? If so the merge would cross a pretoken boundary → block it. Read via
        // LookAhead on the boundaryIn input (declares LookAhead(maxLen)). Null when no
        // boundary stream was supplied → no gating (mHasBoundary == false).
        PabloAST * boundaryBit = mHasBoundary ? getInputStreamSet("boundaryIn")[0] : nullptr;
        std::map<unsigned, PabloAST*> boundaryAheadByLen;
        if (mHasBoundary && !LookaheadInGate) {   // in-gate mode builds it per rule in emitBody
            for (const auto & r : mRuleGroup.rules) {
                if (boundaryAheadByLen.count(r.lenA)) continue;
                boundaryAheadByLen[r.lenA] = pb.createLookahead(boundaryBit, (int64_t) r.lenA);
            }
        }

        // Frozen kernel input, shared by the Astart compare and the per-rule stamps:
        // srcFrozen = entry ids, meInFrozen = entry live-start mask. Reading the frozen
        // input (not the mutating idAcc/inPlayMask) is safe by T4 independence +
        // clean-range conflict-freedom.
        std::vector<PabloAST*> frozenBits(W_out);       // entry idAcc value, immutable
        for (unsigned i = 0; i < W_out; i++) frozenBits[i] = (i < W) ? srcBits[i] : zeroes;
        BixNum      srcFrozen(frozenBits.begin(), frozenBits.end());
        PabloAST *  meInFrozen = getInputStreamSet("meIn")[0];

        // ── Astart decode ────────────────────────────────────────────────────────
        // Astart(idA) = EQ(srcFrozen, idA) AND meInFrozen — a W_out-deep AND/OR chain.
        // Every rule pays it OUTSIDE its createIf gate (the gate condition IS Astart),
        // so it is the dominant never-skipped per-byte cost. eqAstart already folds in
        // meInFrozen, so the returned value IS the gate value — callers hand it straight
        // to emitBody, whose fire is a 2-input And.
        auto eqAstart = [&](auto & bld, unsigned id) -> PabloAST * {
            BixNumCompiler bncL(bld);
            return bld.createAnd(bncL.EQ(srcFrozen, id), meInFrozen);
        };

        // One rule's fire → B-detect + stamp all idAB bits + consume B, all inside `body`
        // (the gated scope, so it's block-skippable). fireStart is supplied by the caller
        // (per-rule Astart) and MUST already be AND-ed with meInFrozen — eqAstart does
        // that (its result is EQ AND meInFrozen).
        auto emitBody = [&](auto & body, const MergeRule & r, PabloAST * fireStart) {
            BixNumCompiler bncB(body);
            // B-detection: indexed mode reads the next-live id (frozen), byte mode reads
            // lenA ahead via LookAhead. Both frozen → identical semantics.
            // --lookahead-in-gate: build the shift HERE (inside body, per rule, skippable)
            // instead of the hoisted shared aheadByLenA/boundaryAheadByLen maps.
            PabloAST * BstartAtA;
            if (mUseNextId) {
                BstartAtA = bncB.EQ(nextIdBN, r.idB);
            } else if (LookaheadInGate) {
                std::vector<PabloAST*> bits(W);
                for (unsigned i = 0; i < W; i++)
                    bits[i] = body.createLookahead(srcBits[i], (int64_t) r.lenA);
                BstartAtA = bncB.EQ(BixNum(bits.begin(), bits.end()), r.idB);
            } else {
                BstartAtA = bncB.EQ(aheadByLenA.at(r.lenA), r.idB);
            }
            PabloAST * fire = body.createAnd(fireStart, BstartAtA);
            if (mHasBoundary) {  // block merges where B begins a new pretoken (cross-boundary)
                PabloAST * bAhead = LookaheadInGate
                    ? body.createLookahead(boundaryBit, (int64_t) r.lenA)
                    : boundaryAheadByLen.at(r.lenA);
                fire = body.createAnd(fire, body.createNot(bAhead));
            }

            // Stamp all W_out bits of idAB at A's start (inside the gate → block-skippable).
            PabloAST * notFire = body.createNot(fire);
            for (unsigned i = 0; i < W_out; i++) {
                if ((r.idAB >> i) & 1u)
                    body.createAssign(idAcc[i], body.createOr(idAcc[i], fire));
                else
                    body.createAssign(idAcc[i], body.createAnd(idAcc[i], notFire));
            }
            // Consume B's start from the live-start mask (per rule).
            PabloAST * clearB = mUseNextId
                ? body.createIndexedAdvance(fire, meInFrozen, 1)
                : body.createAdvance(fire, r.lenA);
            body.createAssign(inPlayMask, body.createAnd(inPlayMask, body.createNot(clearB)));
        };

        // Self-merge X+X→XX (idA==idB): pair per-run on the frozen live starts. No
        // same-kernel rule can have consumed an X-run position — that would overlap the
        // self-merge, which the clean-range partition forbids — so frozen == live.
        auto emitRule = [&](const MergeRule & r) {
            // eqAstart already includes meInFrozen, and selfMergeFireStarts returns a
            // subset of its input, so fireStart IS the gate value — no extra And.
            PabloAST * fireStart = eqAstart(pb, r.idA);
            if (r.idA == r.idB)
                fireStart = selfMergeFireStarts(pb, fireStart, r.lenA);
            auto body = pb.createScope();
            emitBody(body, r, fireStart);
            pb.createIf(fireStart, body);
        };

        if (!mGrouped) {
            for (const auto & r : mRuleGroup.rules) emitRule(r);
        } else {
            // Grouped-if (numeric-range): sort rules by first id (idA), chop into chunks
            // of GROUP_SIZE, and gate each chunk with ONE createIf on the id RANGE
            // [gLo,gHi] it spans (2 compares) instead of one if per rule. A block with no
            // live id in the chunk's range skips all its per-rule EQs. Sorted → the high
            // (rare) chunks have narrow high ranges → skipped most blocks, while the low
            // (common) chunk stays lit. Per-rule EQ is flat inside the gate → single level
            // (T6-safe). Reorder is safe (T4 independence). GROUP_SIZE=5 → N/5 groups.
            const unsigned GROUP_SIZE = effGroupSize(mRuleGroup.rules.size());
            std::vector<const MergeRule*> sorted;
            sorted.reserve(mRuleGroup.rules.size());
            for (const auto & r : mRuleGroup.rules) sorted.push_back(&r);
            std::sort(sorted.begin(), sorted.end(),
                      [](const MergeRule* a, const MergeRule* b){ return a->idA < b->idA; });
            for (size_t s = 0; s < sorted.size(); s += GROUP_SIZE) {
                size_t e = std::min(sorted.size(), s + GROUP_SIZE);
                unsigned gLo = sorted[s]->idA, gHi = sorted[e-1]->idA;   // sorted → tight range
                PabloAST * inRange = pb.createAnd(bnc.UGE(srcFrozen, gLo),
                                                  bnc.ULE(srcFrozen, gHi));
                auto body = pb.createScope();
                // Per-rule Astart EQ stays INSIDE the gate body so the range gate can
                // block-skip it. Single level (T6-safe).
                for (size_t k = s; k < e; k++) {
                    const MergeRule & r = *sorted[k];
                    PabloAST * fireStart = eqAstart(body, r.idA);
                    if (r.idA == r.idB)
                        fireStart = selfMergeFireStarts(body, fireStart, r.lenA);
                    emitBody(body, r, fireStart);
                }
                pb.createIf(pb.createAnd(inRange, meInFrozen), body);
            }
        }

        Var * sOut = getOutputStreamVar("sourceOut");
        for (unsigned i = 0; i < W_out; i++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), idAcc[i]);  // 16-bit token ID stream
        pb.createAssign(pb.createExtract(getOutputStreamVar("meOut"), pb.getInteger(0)), inPlayMask);
    }
private:
    MergeRuleGroup mRuleGroup;
    bool mHasBoundary;
    bool mUseNextId;
    bool mGrouped;
};
// ─── Pipeline (merge-kernel design) ──────────────────────────────────────────
// buildBPEPassPipeline — real BPE merge on the id stream.
//   1. buildMergeRuleRanges() partitions merges into clean id-ranges (dependency-independent
//      + token-adjacency-overlap-free, rank order).
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
        const BPETokenizer & bpe,
        kernel::StreamSet * boundary) {

    auto ruleRanges = bpe.buildMergeRuleRanges();

    // Inject a FilterByMask compaction after every K merge kernels and rewrite the
    // rules' merge distances into the compacted (slot) frame. Default K = 40, so
    // compactions land after kernel 40, 80, 120, ... (28 points for 1123 kernels).
    unsigned compactEvery = CompactionBase;
    if (const char * ce = std::getenv("BPE_COMPACT_EVERY")) compactEvery = (unsigned) std::atoi(ce);
    // BPE_INDEXED_SHIFT: replace each kernel's multi-bit source LookAheads with ONE
    // IndexedShiftBack(inPlayMask, source) → next-live id, and consume via inline
    // createIndexedAdvance.
    // Value = how many kernels (from the start) to convert to indexed-shift. Lets us
    // isolate the mechanism on 1 kernel before scaling to all 1123 (Deferred-rate chain).
    //
    // INDEPENDENT of BPE_COMPACT_EVERY — the two compose, and the FilterByMask
    // schedule is identical whether a kernel uses LookAhead or IndexedShiftBack:
    //   - B-detection needs no distance at all in indexed mode (reads nextId at p),
    //     so the slot-space lenA that applyCompactionSchedule computes is simply
    //     unused there;
    //   - the 1-bit boundary LookAhead and selfMergeFireStarts DO still use lenA,
    //     and slot-space lenA is the correct distance for them: boundary is filtered
    //     by the same mask, so its positions are slots too;
    //   - createIndexedAdvance(mergeV, inPlayFrozen, 1) is "one live position
    //     forward" in whatever space the stream is in, compacted or not.
    // Right after a compaction the mask is all-ones, so IndexedShiftBack there
    // degenerates to a plain 1-position shift — correct, and cheaper.
    unsigned indexedShiftN = 0;
    if (const char * is = std::getenv("BPE_INDEXED_SHIFT")) indexedShiftN = (unsigned) std::atoi(is);
    bool useIndexedShift = indexedShiftN > 0;
    if (useIndexedShift)
        std::cerr << "[BPE] BPE_INDEXED_SHIFT: first " << indexedShiftN
                  << " kernels via IndexedShiftBack (1 shift/kernel)\n";
    auto compactAfter = applyCompactionSchedule(ruleRanges, compactEvery);

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
    StreamSet * source = P.CreateStreamSet(8, 1);
    StreamSet * active = P.CreateStreamSet(1, 1);
    StreamSet * end    = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<BPERangeSeed>(basis, source, active, end);
    if (std::getenv("BPE_DBG") && std::string(std::getenv("BPE_DBG")) == "seed")
        return {active, source};   // dump seed source at every byte
    // BPE_DBG=bound: dump the pretoken-boundary mask — prints the base id at each
    // boundary byte (matchEnd = boundary). Verifies boundary placement on a tiny input.
    if (boundary && std::getenv("BPE_DBG") && std::string(std::getenv("BPE_DBG")) == "bound")
        return {boundary, source};

    //
    // BETWEEN KERNELS, the source is an input and sOut is the output. 
    // kernel reads source (seed = raw byte ids), writes sOut. Then source = sOut.
    // kernel reads that SAME sOut as its input, writes a new one. And so on.
    // The next kernel's input = the previous kernel's output. 
    // So kernel N receives the id stream already stamped by kernels 1..N−1. 
    // It does not re-run their rules — their merges are baked into the ids it reads. 
    // Each kernel only applies its own range's rules once → no duplicated work.
    //
    // One BPEMergeKernel per id-range, ascending = rank order. `source` threads
    // kernel→kernel; `inPlayMask` (seeded active = all ones) threads too, each kernel
    // clearing the token starts it consumes, so the final mask marks surviving starts.
    StreamSet * inPlayMask = active;
    for (size_t i = 0; i < ruleRanges.size(); i++) {
        auto & g = ruleRanges[i];
        if (g.rules.empty()) continue;
        unsigned output_bits = ceil_log2(g.hi+1);
        StreamSet * sOut  = P.CreateStreamSet(output_bits, 1);
        StreamSet * meOut = P.CreateStreamSet(1, 1);
        // 1. Make the copied/shifted stream
        // Indexed mode: precompute next-live id for this kernel (frozen source+mask).
        // Only the first indexedShiftN kernels convert (isolation); rest stay byte-space.
        StreamSet * nextId = nullptr;
        if (useIndexedShift && i < indexedShiftN) {
            nextId = P.CreateStreamSet(source->getNumElements(), 1);
            P.CreateKernelCall<IndexedShiftBack>(inPlayMask, source, nextId);
        }
        // Grouped-if for kernels at/after the lower limit (later kernels); -1 = off.
        bool grouped = (IfGroupLowerLimit >= 0) && ((long) i >= (long) IfGroupLowerLimit);
        P.CreateKernelCall<BPEMergeKernel>(source, inPlayMask, boundary, nextId, sOut, meOut,
                                           g, hashRuleSet(g.rules), g.maxLen, grouped);
        source     = sOut;
        inPlayMask = meOut;

        // Scheduled compaction (applyCompactionSchedule): drop the positions this
        // block consumed. source and boundary move to the PopcountOf(inPlayMask) rate;
        // `boundary` MUST be filtered by the SAME mask or the gating lookahead reads
        // the wrong token. The fresh mask is all-ones because every position that
        // survived the filter was, by construction, a live token start.
        if (!compactAfter[i]) continue;
        StreamSet * sourceC = P.CreateStreamSet(output_bits, 1);
        FilterByMask(P, inPlayMask, source, sourceC);
        if (boundary) {
            StreamSet * boundaryC = P.CreateStreamSet(1, 1);
            FilterByMask(P, inPlayMask, boundary, boundaryC);
            boundary = boundaryC;
        }
        StreamSet * onesC = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<BPEOnesKernel>(sourceC, onesC);
        source     = sourceC;
        inPlayMask = onesC;
    }

    // inPlayMask marks surviving (outermost) token STARTS; vocabID = source (start-anchored,
    // id stamped at each token's start). Emission scans inPlayMask and reads source there.
    return {inPlayMask, source};
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
bool BPETokenizer::loadVocab(const std::string & path, unsigned limit) {
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
        return loadMerges(path, limit);
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
bool BPETokenizer::loadMerges(const std::string & path, unsigned limit) {
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
        if (limit && rank >= limit) break;        // --merges-limit: keep only first N by rank
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


// Raw-byte length of a merges.txt display token. Byte-level remapping is a
// byte↔codepoint bijection (bytes_to_unicode), so a token's RAW byte length ==
// its codepoint count in the UTF-8 display string — NOT the display byte count
// (e.g. "Ġ" is 2 display bytes 0xC4 0xA0 but 1 raw byte = space). The id stream is
// indexed by raw bytes, so lenA/lenB (LookAhead + Advance distances) must use this.
static unsigned rawByteLen(const std::string & s) {
    unsigned n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;   // count non-continuation bytes = codepoints
    return n;
}

// ─── Range partitioning (merge-kernel design) ────────────────────────────────
// buildMergeRuleRanges — resolve each raw merge (parts A,B + idAB) into a
// MergeRule {idA,idB,idAB,lenA,lenB}, then partition into ranges that are BOTH
//   (1) dependency-independent — a range [lo,hi) holds only rules whose BOTH part
//       ids are < lo, so every part was already stamped by a lower kernel
//       (`source` threading resolves cross-range deps; no intra-kernel cascade), AND
//   (2) overlap-free by TOKEN-ID — no two rules in a range compete for a shared
//       middle token: the RIGHT part id of one == the LEFT part id of the other
//       (cur.idB==p.idA || p.idB==cur.idA). e.g. Ġt=(Ġ,t) and ter=(t,er) both
//       claim the 't' token → split into separate rank-ordered kernels so the
//       lower-rank one's write reaches the later kernel's input and starves the other.
// (1) alone (independent_range_analysis) let competitors share a kernel; (2) is
// the token-grid conflict test (interior sub-token byte overlaps are NOT counted —
// interior bytes are not live token starts). Mirrors merge_analysis.py
// clean_range_analysis (id-based merges_overlap). Rules stay idAB-ASC = rank-ASC.
std::vector<MergeRuleGroup> BPETokenizer::buildMergeRuleRanges() const {
    // 1. Resolve every raw merge → MergeRule.
    std::vector<MergeRule> rules;
    unsigned skipped = 0;                            // parts not found in the vocab
    for (const auto & m : merges_) {
        auto ia = vocab_.find(m.a);
        auto ib = vocab_.find(m.b);
        if (ia == vocab_.end() || ib == vocab_.end()) { ++skipped; continue; }
        MergeRule r;
        r.idA  = static_cast<unsigned>(ia->second);
        r.idB  = static_cast<unsigned>(ib->second);
        r.idAB = m.idAB;
        r.lenA = rawByteLen(m.a);   // RAW byte length (codepoints), not display byte size
        r.lenB = rawByteLen(m.b);
        rules.push_back(r);
    }
    if (skipped)
        std::cerr << "BPE: buildMergeRuleRanges skipped " << skipped
                  << " merges whose parts are not in the vocab\n";

    // 2. Rank order (idAB ASC == 256 + rank).
    std::sort(rules.begin(), rules.end(),
              [](const MergeRule & x, const MergeRule & y) { return x.idAB < y.idAB; });

    // 3. clean_range partition: lo = first rule's idAB (its parts always precede
    //    it, so it always fits); extend while the rule is dependency-independent
    //    (idA<lo && idB<lo) AND token-adjacency-overlap-free vs every rule already
    //    in the group. The first rule violating either starts the next range.
    std::vector<MergeRuleGroup> ranges;
    size_t i = 0, n = rules.size();
    while (i < n) {
        MergeRuleGroup g;
        g.lo = rules[i].idAB;                        // new range starts here
        while (i < n && rules[i].idA < g.lo && rules[i].idB < g.lo) {
            const MergeRule & cur = rules[i];
            bool overlap = false;                    // token-adjacency: right == left
            for (const MergeRule & p : g.rules)
                if (cur.idB == p.idA || p.idB == cur.idA) { overlap = true; break; }
            if (overlap) break;
            unsigned mergedLen = cur.lenA + cur.lenB;
            if (mergedLen > g.maxLen) g.maxLen = mergedLen;
            g.rules.push_back(cur);
            ++i;
        }
        g.hi = (i < n) ? rules[i].idAB               // next range's lo
                       : rules[n - 1].idAB + 1;      // last range: past top id
        ranges.push_back(std::move(g));
    }
    return ranges;
}