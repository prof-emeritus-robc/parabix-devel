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
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <llvm/Support/CommandLine.h>
#include <pablo/pablo.h>
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

// FIXED SIZE: force exactly this many rules per grouped-if gate, regardless of how many
// rules a kernel has (gate COUNT then varies = rules/size). Overrides --if-group-count
// when set (!= 1). Default 1 = defer to --if-group-count.
static cl::opt<unsigned> IfGroupSize(
    "if-group-size",
    cl::desc("Rules per grouped-if gate, fixed regardless of kernel size. Overrides "
             "--if-group-count when != 1. Default 1 = use --if-group-count."),
    cl::init(1));

// Effective grouped-if chunk size for a kernel with n rules. Fixed --if-group-size wins
// when set (!= 1); otherwise derive from --if-group-count (n/count). Used by BOTH the
// cache-name tag and the Pablo body so they never disagree (stale-cache hazard).
static unsigned effGroupSize(size_t n) {
    if (IfGroupSize.getValue() != 1) return IfGroupSize.getValue();
    return std::max<unsigned>(1u, (unsigned)(n / std::max(1u, IfGroupCount.getValue())));
}

// move the B-detection LookAhead INSIDE each rule's createIf gate instead of
// hoisting one shared shift per distinct lenA outside all gates. Inside = skippable on cold
// blocks but DUPLICATED per rule (loses the per-lenA dedup); outside (default) = shared but
// runs every block. 
// --level-partition: schedule merge rules into the MINIMUM number of kernels instead
// of cutting the rank-sorted rule list into contiguous id ranges.
//
// The clean-range walk (buildMergeRuleRanges, step 3) closes a group at the FIRST rule
// that violates either constraint, so a group is necessarily a consecutive interval of
// rank. Measured over full GPT-2: 1098 of its 1123 groups close on a seam conflict, and
// at each break ~179 of the next 200 rules would still have fit in the group being
// closed — ~90% of the available packing is discarded purely to keep groups contiguous.
// Group size therefore saturates near 44 (a new rule must clash with NONE of the ~44
// already present) and 50000/44.5 = 1123.
//
// Level scheduling drops contiguity only. Each rule takes its EARLIEST legal kernel:
//     level(r) = 1 + max( level(producer of idA), level(producer of idB),
//                         level of any lower-rank rule seaming with r )
// A violator no longer ends the group — it defers ITSELF while the rest keep filling the
// current level. Kernel count becomes the constraint DAG's longest path (a provable
// minimum, nothing can go earlier than its own dependencies) rather than a function of
// how often violations occur: 1123 -> 283, mean group size 44.5 -> 176.7.
//
// BOTH correctness constraints are preserved exactly (T4 + the clean-range conflict
// test); only the packing changes. Levels are NOT rank intervals — level 1 legitimately
// holds rank 0 alongside rank 47815.
static cl::opt<bool> LevelPartition(
    "level-partition",
    cl::desc("Partition merge rules by ASAP level scheduling (minimum kernel count) "
             "instead of contiguous clean id ranges."),
    cl::init(false));

// Asymmetric seam (--level-partition only): drop the maxRight/idA seam constraint,
// keep maxLeft/idB. Two rules sharing token t where an earlier (lower-rank) rule used
// t as ITS right/B part and a later rule uses t as ITS left/A part (e.g. rank11 "Ġ+o"
// vs rank17 "o+r" on "Ġor") no longer need separate levels: rank11's fire clears t's
// live-start bit in inPlayMask, and eqAstart's gate now reads that mask LIVE (see
// below), so rank17 correctly sees t already consumed even inside the same kernel.
// The other direction (maxLeft/idB — e.g. rank40 "o+m" vs rank49 "r+o" on "from")
// stays a hard separation: rank49's B-detect is a forward LookAhead on the FROZEN
// kernel input (Pablo: LookAhead legal only on a declared input binding, T5), so it
// can never observe rank40's same-kernel restamp of o's position. Needs a real kernel
// boundary. Cuts 283 -> 76 kernels (measured via merge_analysis.py's scheduling model;
// this flag is the first C++ implementation — verify byte-identical vs HF before
// trusting it for anything beyond experimentation).
static cl::opt<bool> AsymmetricSeam(
    "asymmetric-seam",
    cl::desc("With --level-partition, drop the maxRight seam constraint (keep maxLeft) "
             "for a tighter (but less battle-tested) level schedule."),
    cl::init(false));

// Chain partition (--level-partition only): let a rule whose idA was STAMPED by an
// earlier (lower-rank) rule in the SAME level share that level instead of taking the
// next one — a dependency chain A+B->AB, AB+C->ABC, ABC+D->ABCD... collapses into ONE
// kernel instead of one per link. Same mechanism as --asymmetric-seam, one level up:
// there, a same-kernel rule's MASK read goes live (inPlayMask) so it sees an earlier
// rule's consume; here, a same-kernel rule's ID read goes live (idAcc) so it sees an
// earlier rule's STAMP. Both rely on the same Pablo property — createIf auto-joins a
// Var's reassignment as a Sel when the gated scope closes, so a LATER sibling
// createIf reading that Var outward-of-scope already observes it. No nested createIf
// (T6) — chain rules stay SIBLING ifs in rank order, just reading a live Var instead
// of a frozen one.
//
// The other producer (idB, the RIGHT part) stays a hard separation, same as maxLeft
// under --asymmetric-seam: B-detection is a forward LookAhead on the FROZEN kernel
// input (T5), so a same-kernel producer of idB can never be seen — that side always
// needs a real kernel boundary.
//
static cl::opt<bool> ChainPartition(
    "chain-partition",
    cl::desc("With --level-partition, let a rule whose idA was stamped by an earlier "
             "same-level rule share that level (dependency chains collapse into one "
             "kernel) instead of taking the next level."),
    cl::init(false));

// Chain veto (--level-partition): relax the maxLeft/idB seam — "a lower-rank rule
// already claims my idB as ITS idA" — and pay for it with a runtime test instead of a
// kernel boundary.
//
// Rules A+B->AB (rank 1), C+D->CD (rank 2), AB+C->ABC (rank 3). maxLeft[C] == CD's
// level forces ABC one level later, because on "ABCD" the lower-rank CD must consume C
// first and ABC must NOT fire. Instead of the split, ABC checks CD itself: the id one
// slot PAST the merged token (offset lenA+lenB, i.e. right after C) is read from the
// FROZEN kernel input and compared against D.
//     veto     = EQ(LookAhead(src, lenA+lenB), idD)   // ... OR idE, one per competitor
//     fire_ABC = fire_AB AND hasC AND NOT veto
// "ABCD" -> veto=1, ABC suppressed, CD fires in the same kernel. "ABCX" -> veto=0, ABC
// fires. Both match HF.
//
// Competitor set is exact and small. Rules are scheduled in rank order, so every
// lower-rank rule with idA == this rule's idB is already placed when this rule lands:
//   - competitor at a STRICTLY EARLIER level already stamped its merged id over idB's
//     slot, so this rule's own B-detect fails on its own — no veto term needed;
//   - a LATER-level competitor cannot exist, rank order places it first;
//   - a SAME-level competitor is exactly what this flag creates, and exactly what
//     vetoIdB lists.
//
// Veto recursion is handled by REFUSING the relaxation, never by approximating it. A
// competitor can itself be beaten (D+E at a lower rank kills C+D, so ABC SHOULD fire —
// a depth-1 veto would wrongly suppress it). Such a competitor carries a non-empty
// vetoIdB of its own, so if any candidate is itself vetoed, this rule falls back to the
// strict level and takes the kernel split.
//
// Independent of --chain-partition: the veto reads ONLY the frozen kernel input, never
// the producer's fire or a live idAcc, so it is emitted identically for a flat sibling
// rule (emitBody) and a nested chain child (emitChainBody).
static cl::opt<bool> ChainVeto(
    "chain-veto",
    cl::desc("With --level-partition, let a rule share its level with the lower-rank "
             "rules claiming its idB, suppressing the merge at runtime when such a "
             "competitor applies, instead of splitting the kernel."),
    cl::init(false));

static cl::opt<bool> LookaheadInGate(
    "lookahead-in-gate",
    cl::desc("Build the B-detection LookAhead inside each rule's if-gate (per-rule, "
             "skippable) instead of one shared hoisted shift per lenA (default)."),
    cl::init(false));

// Grouped-only: build the B-detection LookAhead inside each GROUP's createIf, cached
// (deduped) per distinct lenA within that chunk so the chunk's rules SHARE it. Combines
// dedup (unlike --lookahead-in-gate's per-rule build) with block-skip (unlike the hoisted
// shared build that runs every block): a cold chunk skips building its peeks entirely.
// Only takes effect on grouped kernels (--if-group-lower-limit >= 0). Cache tag lg1_/lg0_.
static cl::opt<bool> LookaheadInGroup(
    "lookahead-in-group",
    cl::desc("Grouped kernels: build the B-detection LookAhead inside each group's if-gate, "
             "cached per lenA within the chunk (shared by the chunk's rules, skippable)."),
    cl::init(false));

// Replace the first N kernels' multi-bit source LookAhead with an IndexedShiftBack
// (next-live id) + IndexedAdvance consume. N=0 (default) = pure LookAhead. Adds one
// IndexedShiftBack kernel per converted merge kernel (doubles their count). Measured
// slower (~2.75x) and SIGSEGVs at full merges — kept as an A/B knob. Env var
// BPE_INDEXED_SHIFT still overrides this flag when set.
static cl::opt<unsigned> IndexedShift(
    "indexed-shift",
    cl::desc("Convert the first N merge kernels to IndexedShiftBack (next-live id) + "
             "IndexedAdvance instead of LookAhead. 0 = off (default, pure LookAhead)."),
    cl::init(0));

// Batch the write-back: instead of one Advance (carry op) + a full W_out-bit stamp
// inside EVERY rule's createIf gate, each gate only ORs its `fire` into shared
// entry-scope accumulators (anyFire, setBit[i], fireByLen[lenA]); the kernel then
// applies ONE Advance per DISTINCT lenA and ONE Sel per id bit at the end.
//
// Carry ops per kernel: one per RULE (~177 under --level-partition) → one per
// distinct lenA (≤ maxLen; 8 under --compact-base=2 --geometric-compaction).
// Gate body drops from ~20 ops + 1 carry + W_out+1 mutated Vars to
// popcount(idAB)+2 Ors, 0 carries — so createIf also drops its carry save/restore.
//
// Exact, three legs:
//  (a) Advance is linear over Or — Advance(a|b,L) == Advance(a,L)|Advance(b,L) — and
//      NOT(a|b) == NOT a AND NOT b; AND is commutative + idempotent, so deferring the
//      inPlayMask clears reorders nothing.
//  (b) At most ONE rule fires per position in a kernel: the id stream is single-valued
//      so at most one rule's idA matches, and rules sharing an idA share its lenA and
//      therefore probe the SAME B slot, where only one idB can sit. So setBit[i] and
//      the anyFire clear can never disagree (T3 preserved).
//  (c) No rule reads the mutating idAcc/inPlayMask — gates and compares read the FROZEN
//      kernel input (srcFrozen/meInFrozen) — so the deferred writes have no in-kernel
//      readers.
// Not applied on the --indexed-shift path (END-anchored via createIndexedAdvance).
// Cache tag bw1_/bw0_.
static cl::opt<bool> BatchWriteback(
    "batch-writeback",
    cl::desc("Accumulate merge fires into shared Vars and apply ONE Advance per "
             "distinct lenA + ONE Sel per id bit at the end of each merge kernel, "
             "instead of one Advance + a full stamp per rule."),
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
        std::vector<PabloAST*> basisBits = getInputStreamSet("basis");
        PabloAST * zeroes = pb.createZeroes();
        PabloAST * ones   = pb.createNot(zeroes);

        // id(byte) is a fixed byte->id bijection: piecewise byte + offset (T1). Expressed
        // as CC instead of BixNum arithmetic: for each id output bit k, the SET of bytes
        // whose id has bit k set is a byte character class, so compileCC over the byte
        // basis yields that id-bit stream directly — no Select/AddModular/SubModular.
        // Data-independent (constant) → cache name stays constant. Ranges (must stay a
        // bijection over 0..255): 0..32→+188  33..126→-33  127..160→+94  161..172→-67
        // 173→255  174..255→-68.
        cc::Parabix_CC_Compiler_Builder ccc(basisBits);
        auto idOf = [](unsigned by) -> unsigned {
            if (by <= 32)  return by + 188;
            if (by <= 126) return by - 33;
            if (by <= 160) return by + 94;
            if (by <= 172) return by - 67;
            if (by == 173) return 255;
            return by - 68;                        // 174..255
        };
        const unsigned IDBITS = 8;                 // base ids are 0..255
        std::vector<PabloAST*> idBits(IDBITS);
        for (unsigned k = 0; k < IDBITS; k++) {
            re::CC * cc = nullptr;                  // bytes whose id has bit k set
            for (unsigned by = 0; by < 256; ) {
                if ((idOf(by) >> k) & 1u) {
                    unsigned lo = by;
                    while (by < 256 && ((idOf(by) >> k) & 1u)) by++;
                    re::CC * part = re::makeByte(lo, by - 1);
                    cc = cc ? re::makeCC(cc, part) : part;
                } else ++by;
            }
            idBits[k] = cc ? ccc.compileCC(cc, pb) : zeroes;
        }
        // Write the id into the 16-bit `source` stream (ids ≤255 → high bits 0),
        // active = all 1s, end = all 0s. idBits[i] = the i-th id bit (a compiled CC).
        Var * sOut = getOutputStreamVar("source");
        for (unsigned i = 0; i < 8; i++) {
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(i)), idBits[i]);
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
    // vetoOff/vetoIdB shape the Pablo body (an extra peek + one EQ per competitor)
    // without touching any field above, so they must be mixed in too — T7.
    for (const auto & r : rules) {
        mix(r.idA); mix(r.idB); mix(r.idAB); mix(r.lenA); mix(r.lenB);
        mix(r.vetoOff);
        for (unsigned v : r.vetoIdB) mix(v | 0x8000000000000000ull);
    }
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
    // kernelOf[idAB] = index of the kernel that STAMPS that id, i.e. WHEN it is built.
    std::map<unsigned, std::pair<unsigned,unsigned>> parts;
    std::map<unsigned, size_t> kernelOf;
    for (size_t gi = 0; gi < ruleRanges.size(); gi++)
        for (const auto & r : ruleRanges[gi].rules) {
            parts.emplace(r.idAB, std::make_pair(r.idA, r.idB));
            kernelOf.emplace(r.idAB, gi);
        }

    long lastCompactKernel = -1;              // -1 = nothing compacted yet
    std::map<unsigned, unsigned> memo;        // valid for the CURRENT compaction point only
    // slotSpan(id) = how many SLOTS this token occupies in the current compacted frame.
    // A token owns one slot iff it already existed when the last FilterByMask ran (the
    // filter kept exactly the live token starts, one position each); anything built after
    // sits on top of several, so its span is the sum of its parts'. Kernels run in order
    // and a compaction happens after a specific kernel, so kernelOf IS the build time.
    // Testing `id < hi` instead is equivalent only when groups tile the id axis in rank
    // order, which --level-partition does not.
    std::function<unsigned(unsigned)> slotSpan = [&](unsigned id) -> unsigned {
        if (id < 256) return 1;               // base byte — the seed supplies it
        auto k = kernelOf.find(id);
        if (k != kernelOf.end() && (long) k->second <= lastCompactKernel)
            return 1;                         // built at or before the last compaction
        auto m = memo.find(id);
        if (m != memo.end()) return m->second;
        auto p = parts.find(id);
        if (p == parts.end()) return 1;       // unresolved part → treat as atomic
        unsigned v = slotSpan(p->second.first) + slotSpan(p->second.second);
        memo.emplace(id, v);
        return v;
    };

    // Walk the rule ranges in order, counting kernels since the last compaction. When
    // we hit K, mark this range for compaction and advance lastCompactKernel to i.
    // Recompute every rule's lenA as a SLOT distance in the resulting compacted frame.
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
            // --chain-veto: the probe sits one slot past idB, so its slot-space distance
            // is slotSpan(idA) + slotSpan(idB) — lenB alone is still a BYTE count here.
            // It can exceed every lenA in the group, so it must widen maxDist too, or the
            // LookAhead binding comes up short and the kernel refuses to compile (T5).
            if (!r.vetoIdB.empty()) {
                r.vetoOff = d + slotSpan(r.idB);
                if (r.vetoOff > maxDist) maxDist = r.vetoOff;
            }
        }
        // Update the maximum length in the current rule range.
        g.maxLen = maxDist;                   // LookAhead binding must cover every lenA
        // Decide whether to compact after this range. If we do, every token stamped by
        // kernel <= i is now one slot wide, so the memo (built against the previous
        // compaction point) is stale and must be dropped.
        if (++sinceCompact < nextCompaction) continue;
        compactAfter[i] = true;
        lastCompactKernel = (long) i;
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
                        + (LookaheadInGroup ? "lg1_" : "lg0_")
                        + (BatchWriteback ? "bw1_" : "bw0_")
                        + (ChainPartition ? "cp1_" : "cp0_")
                        + (ChainVeto ? "cv1_" : "cv0_")
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
        // Skipped when --lookahead-in-gate (emitBody builds per rule) or when a grouped
        // kernel uses --lookahead-in-group (each chunk builds its own cached peeks below).
        const bool grpCache = LookaheadInGroup && mGrouped;
        std::map<unsigned, BixNum> aheadByLenA;
        if (!mUseNextId && !LookaheadInGate && !grpCache)
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
        if (mHasBoundary && !LookaheadInGate && !grpCache) {   // in-gate/in-group build it below
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

        // ── Batched write-back accumulators (--batch-writeback) ──────────────────
        // Each rule's gate only ORs its fire in here; the id-stamp write is applied
        // once at the end of the kernel. setBit[i] = positions where id bit i must
        // become 1, anyFire = positions where SOME rule fired (drives the bit clear).
        //
        // --asymmetric-seam can put two rules in the SAME kernel where the second
        // one's idA is the first one's idB (MergeRule::needsFlush, set by
        // tagFlushPoints) — the second rule's eqAstart MUST see the first rule's
        // consume, or it wrongly fires on a position that's already been taken.
        //
        // We tried making that work by keeping BOTH mask and stamp batched and
        // inserting extra "flush now" points mid-kernel wherever a dependency like
        // this shows up. It was correct, but expensive: every flush point is its own
        // unshared block of IR, and a kernel can have dozens of them.
        //
        // What we do instead is simpler: split the mask and the stamp apart, and only
        // ever defer the mask when it's safe to.
        //   - MASK (inPlayMask): if this kernel has NO such dependency, defer it —
        //     batch every rule's consume and apply one Advance per length at the end,
        //     same as before. If the kernel DOES have a dependency, don't defer it at
        //     all — update it immediately, rule by rule, exactly like the pre-batching
        //     (non-batch) design always did. That makes the mask always up to date,
        //     so the live-read asymmetric-seam needs is satisfied for free — no
        //     mid-kernel flush needed anywhere.
        //   - STAMP (idAcc): always deferred, in every kernel, no exception. Unlike
        //     the mask, one rule's stamp write can never depend on another rule's —
        //     at most one rule can ever fire at a given position (the id stream only
        //     has one value there, checked against the FROZEN input), so there's no
        //     ordering to get wrong. Deferring it is always safe, including inside
        //     grouped-if kernels.
        const bool groupNeedsLiveMask = std::any_of(mRuleGroup.rules.begin(), mRuleGroup.rules.end(),
                                                     [](const MergeRule & r) { return r.needsFlush; });
        // groupNeedsLiveId (--chain-partition): some rule is nested inside its
        // producer's gate (see emitChain below) — batching's deferred, end-of-kernel
        // stamp write doesn't compose with that structure, so batching is off
        // entirely for the whole group, same fallback groupNeedsLiveMask uses for
        // the mask.
        const bool groupNeedsLiveId = std::any_of(mRuleGroup.rules.begin(), mRuleGroup.rules.end(),
                                                   [](const MergeRule & r) { return r.needsLiveId; });
        const bool batch     = BatchWriteback && !mUseNextId && !groupNeedsLiveId;  // defer the id-stamp
        const bool deferMask = batch && !groupNeedsLiveMask;       // also defer the mask consume
        std::vector<Var *> setBit;
        std::map<unsigned, Var *> fireByLen;
        Var * anyFire = nullptr;
        if (batch) {
            setBit.resize(W_out);
            for (unsigned i = 0; i < W_out; i++)
                setBit[i] = pb.createVar("setBit_" + std::to_string(i), zeroes);
            anyFire = pb.createVar("anyFire", zeroes);
            if (deferMask)
                for (const auto & r : mRuleGroup.rules)
                    if (fireByLen.find(r.lenA) == fireByLen.end())
                        fireByLen.emplace(r.lenA,
                            pb.createVar("fireByLen_" + std::to_string(r.lenA), zeroes));
        }

        // ── Astart decode ────────────────────────────────────────────────────────
        // Astart(idA) = EQ(srcFrozen, idA) AND inPlayMask — a W_out-deep AND/OR chain.
        // Every rule pays it OUTSIDE its createIf gate (the gate condition IS Astart),
        // so it is the dominant never-skipped per-byte cost. eqAstart already folds in
        // the mask, so the returned value IS the gate value — callers hand it straight
        // to emitBody, whose fire is a 2-input And.
        //
        // The id-compare term stays FROZEN (srcFrozen) — a token's id value at a given
        // position never changes except via this kernel's OWN stamp at that SAME
        // position (which only a self-merge or an already-forbidden overlap could hit),
        // so re-reading srcFrozen is always correct. The MASK term reads the LIVE
        // `inPlayMask` Var instead of the frozen kernel-input snapshot: Pablo's
        // createIf auto-joins Var reassignments as a Sel when a gated scope closes, so
        // by the time a later rule in this same kernel calls eqAstart again (back at
        // the outer scope), inPlayMask already reflects every earlier rule's consume in
        // THIS kernel. Under the shipped partition schemes (clean-range default,
        // --level-partition symmetric seam) this is a no-op — conflict-freedom already
        // guarantees no rule's fire touches a position another same-kernel rule reads 
        auto eqAstart = [&](auto & bld, unsigned id) -> PabloAST * {
            cc::Parabix_CC_Compiler_Builder ccS(srcFrozen);
            return bld.createAnd(ccS.compileCC(re::makeCC(id), bld), inPlayMask, "Astart_" + std::to_string(id));
        };

        // --chain-veto: suppress this rule where a lower-rank competitor claims its idB.
        // Reads the id one slot PAST idB (r.vetoOff) from the frozen kernel input; if it
        // is that competitor's right part, the competitor applies here, wins on rank, and
        // this merge must not fire. One peek serves every competitor (all probe the same
        // slot), one EQ each. Returns `fire` untouched when the rule carries no veto, so
        // both emission paths call it unconditionally. Reads nothing but the kernel
        // input, which is why it needs neither nesting nor --chain-partition.
        auto applyVeto = [&](auto & body, const MergeRule & r, PabloAST * fire) -> PabloAST * {
            if (r.vetoIdB.empty()) return fire;
            if (mUseNextId)   // --indexed-shift: no byte-distance LookAhead exists here
                llvm::report_fatal_error("--chain-veto is incompatible with --indexed-shift");
            std::vector<PabloAST*> vbits(W);
            for (unsigned i = 0; i < W; i++)
                vbits[i] = body.createLookahead(srcBits[i], (int64_t) r.vetoOff);
            cc::Parabix_CC_Compiler_Builder ccVeto(BixNum(vbits.begin(), vbits.end()));
            PabloAST * veto = nullptr;
            for (unsigned vid : r.vetoIdB) {
                PabloAST * hit = ccVeto.compileCC("veto_" + std::to_string(r.idB) + "_" + std::to_string(vid),
                                                  re::makeCC(vid), body);
                veto = veto ? body.createOr(veto, hit) : hit;
            }
            // The competitor only wins if it could actually FIRE, and it is subject to
            // the same pretoken-boundary rule this kernel applies to every merge: a
            // merge whose right part begins a new pretoken is blocked. The competitor's
            // right part sits at vetoOff, so a boundary there means the competitor can
            // never run and must not suppress anything.
            //
            // GPT-2's regex splits contractions, which makes this reachable rather than
            // theoretical: in "al-Mu'minin", "'m" is its own pretoken and "inin" the
            // next, so the lower-rank competitor m+in straddles the boundary and is
            // blocked — HF merges '+m. Without this gate the veto killed that merge
            // (first mismatch at token 2687270 of the 49MB val.txt run).
            if (mHasBoundary) {
                PabloAST * bAtVeto = body.createLookahead(boundaryBit, (int64_t) r.vetoOff);
                veto = body.createAnd(veto, body.createNot(bAtVeto), "vetoLive_" + std::to_string(r.idAB));
            }
            return body.createAnd(fire, body.createNot(veto), "vetoed_" + std::to_string(r.idAB));
        };

        // One rule's fire → B-detect + stamp all idAB bits + consume B, all inside `body`
        // (the gated scope, so it's block-skippable). fireStart is supplied by the caller
        // (per-rule Astart) and MUST already be AND-ed with the live mask — eqAstart
        // does that (its result is EQ AND inPlayMask).
        // grpAhead/grpBoundary (non-null only on the --lookahead-in-group path) are the
        // chunk-local cached peeks, deduped per lenA and built inside this gate body.
        auto emitBody = [&](auto & body, const MergeRule & r, PabloAST * fireStart,
                            const std::map<unsigned, BixNum> * grpAhead = nullptr,
                            const std::map<unsigned, PabloAST*> * grpBoundary = nullptr) {
            //indexed mode reads the next-live id, byte mode reads lenA ahead via LookAhead.
            // Peek source precedence: indexed nextId > group-cache > per-rule in-gate > hoisted.
            PabloAST * BstartAtA;
            const std::string bstartName = "Bstart_" + std::to_string(r.idA) + "_" + std::to_string(r.idB);
            if (mUseNextId) {
                cc::Parabix_CC_Compiler_Builder ccNext(nextIdBN);
                BstartAtA = ccNext.compileCC(bstartName, re::makeCC(r.idB), body);
            } else if (grpAhead) {   // chunk-cached shared peek (built once per lenA in the gate)
                cc::Parabix_CC_Compiler_Builder ccAhead(grpAhead->at(r.lenA));
                BstartAtA = ccAhead.compileCC(bstartName, re::makeCC(r.idB), body);
            } else if (LookaheadInGate) {
                std::vector<PabloAST*> bits(W);
                for (unsigned i = 0; i < W; i++)
                    bits[i] = body.createLookahead(srcBits[i], (int64_t) r.lenA);
                cc::Parabix_CC_Compiler_Builder ccAhead(BixNum(bits.begin(), bits.end()));
                BstartAtA = ccAhead.compileCC(bstartName, re::makeCC(r.idB), body);
            } else {
                cc::Parabix_CC_Compiler_Builder ccAhead(aheadByLenA.at(r.lenA));
                BstartAtA = ccAhead.compileCC(bstartName, re::makeCC(r.idB), body);
            }
            PabloAST * fire = body.createAnd(fireStart, BstartAtA, "fire1_" + std::to_string(r.idA) + "_" + std::to_string(r.idB) + "_" + std::to_string(r.idAB));
            if (mHasBoundary) {  // block merges where B begins a new pretoken (cross-boundary)
                PabloAST * bAhead = grpBoundary ? grpBoundary->at(r.lenA)
                    : LookaheadInGate ? body.createLookahead(boundaryBit, (int64_t) r.lenA)
                    : boundaryAheadByLen.at(r.lenA);
                fire = body.createAnd(fire, body.createNot(bAhead));
            }
            fire = applyVeto(body, r, fire);

            // Batched: no stamp in the gate — just OR the fire into the shared
            // accumulators; the id-stamp write is applied once per kernel below (see
            // flushWriteback). The mask consume is ALSO deferred (accumulate into
            // fireByLen) when deferMask allows it; otherwise it's applied right here,
            // eagerly, exactly like the non-batch path below — see groupNeedsLiveMask.
            if (batch) {
                const std::string ruleTag = std::to_string(r.idA) + "_" + std::to_string(r.idB) + "_" + std::to_string(r.idAB);
                body.createAssign(anyFire, body.createOr(anyFire, fire, "anyFire_" + ruleTag));
                for (unsigned i = 0; i < W_out; i++)
                    if ((r.idAB >> i) & 1u)
                        body.createAssign(setBit[i], body.createOr(setBit[i], fire, "setBit" + std::to_string(i) + "_" + ruleTag));
                if (deferMask) {
                    Var * fl = fireByLen.at(r.lenA);
                    body.createAssign(fl, body.createOr(fl, fire, "fireByLen" + std::to_string(r.lenA) + "_" + ruleTag));
                } else {
                    PabloAST * removeAt = body.createAdvance(fire, r.lenA, "removeAt_" + ruleTag);
                    body.createAssign(inPlayMask, body.createAnd(inPlayMask, body.createNot(removeAt)));
                }
                return;
            }

            // Anchor. Indexed path (--indexed-shift) is END-anchored: stamp idAB at B's
            // position (2nd part) and remove A. `fire` is at A; IndexedAdvance moves it one
            // live position forward → B. LookAhead path stays START-anchored (stamp at A,
            // remove B) 
            PabloAST * stampAt;   // where idAB is written
            PabloAST * removeAt;  // which start is cleared from the live mask
            const std::string ruleTag = std::to_string(r.idA) + "_" + std::to_string(r.idB) + "_" + std::to_string(r.idAB);
            if (mUseNextId) {
                PabloAST * fireB = body.createIndexedAdvance(fire, meInFrozen, 1, "fireB_" + ruleTag);
                stampAt  = fireB;   // merged id lands at B (2nd position)
                removeAt = fire;    // A's start removed; B survives as AB
            } else {
                stampAt  = fire;                        // merged id at A's start
                removeAt = body.createAdvance(fire, r.lenA, "removeAt_" + ruleTag);  // B's start removed; A survives
            }
            // Stamp all W_out bits of idAB at stampAt (inside the gate → block-skippable).
            PabloAST * notStamp = body.createNot(stampAt);
            for (unsigned i = 0; i < W_out; i++) {
                if ((r.idAB >> i) & 1u)
                    body.createAssign(idAcc[i], body.createOr(idAcc[i], stampAt));
                else
                    body.createAssign(idAcc[i], body.createAnd(idAcc[i], notStamp));
            }
            body.createAssign(inPlayMask, body.createAnd(inPlayMask, body.createNot(removeAt)));
        };

        // Self-merge X+X→XX (idA==idB): pair per-run on the frozen live starts. No
        // same-kernel rule can have consumed an X-run position — that would overlap the
        // self-merge, which the clean-range partition forbids — so frozen == live.
        unsigned maskGen = 0;

        // Apply the deferred id-stamp (and, when deferMask, the deferred mask consume
        // too) once at the very end of the kernel — no mid-kernel calls, since the
        // mask is either fully eager (groupNeedsLiveMask) or fully deferred for the
        // whole kernel, never a mix that needs an early catch-up point.
        auto flushWriteback = [&]() {
            if (deferMask)
                for (auto & kv : fireByLen)
                    pb.createAssign(inPlayMask,
                        pb.createAnd(inPlayMask,
                            pb.createNot(pb.createAdvance(kv.second, (int64_t) kv.first))));
            PabloAST * notAny = pb.createNot(anyFire);
            for (unsigned i = 0; i < W_out; i++)
                pb.createAssign(idAcc[i],
                    pb.createOr(pb.createAnd(idAcc[i], notAny), setBit[i]));
        };

        auto emitRule = [&](const MergeRule & r) {
            // eqAstart already includes the live mask, and selfMergeFireStarts returns a
            // subset of its input, so fireStart IS the gate value — no extra And.
            PabloAST * fireStart = eqAstart(pb, r.idA);
            if (r.idA == r.idB)
                fireStart = selfMergeFireStarts(pb, fireStart, r.lenA);
            auto body = pb.createScope();
            emitBody(body, r, fireStart);
            pb.createIf(fireStart, body);
            // Rebind inPlayMask to a FRESH Var after every rule. Pablo's PabloBuilder
            // memoizes createAnd(a,b) by OPERAND POINTER IDENTITY (mExprTable), with no
            // awareness that a Var's value changes across an intervening createAssign
            // inside a closed createIf scope. Two rules sharing idA call eqAstart with
            // the SAME idpart AND the SAME inPlayMask pointer — the second call hits the
            // cache and silently gets back the FIRST rule's pre-fire (stale) gate, so it
            // can fire on a position the first rule already consumed. This is particularly
            // problematic for self-merges where the same position can be consumed by multiple
            // rules. For example, rules "0+0","0+1","1+2" sharing a kernel, "0+1" reusing "0+0"'s
            // stale gate, misfiring on "0"'s already-consumed position and stealing "1"
            // out from under "1+2"). A fresh Var* per rule is a new cache key, so the
            // next eqAstart call is forced to rebuild against the truly-current mask.
            Var * freshMask = pb.createVar("inPlayMask_" + std::to_string(maskGen++), zeroes);
            pb.createAssign(freshMask, inPlayMask);
            inPlayMask = freshMask;
        };

        // ── --chain-partition: REAL nested createIf ─────────────────────────────
        // A rule flagged needsLiveId (its idA was stamped by an earlier SAME-level
        // rule — see levelPartition's ChainPartition branch) is emitted physically
        // INSIDE that producer's gate, instead of as a sibling createIf reading a
        // live Var. Isolated from emitBody/emitRule (used by every other path,
        // including plain-sibling non-chain rules in the SAME kernel) so no other
        // flag's behavior changes.
        //
        // Why no idA re-check is needed for a nested child: being inside the
        // producer's gate body, predicated on the producer's `fire`, already proves
        // the producer matched (both its A and B parts) at this exact position —
        // that's precisely when idAB now holds here. So the child only needs its OWN
        // B-detect (is idB `lenA` bytes ahead, where lenA is the FULL chain-so-far
        // length) — no id-compare, no read of idAcc at all, live or frozen. This is
        // simpler than a sibling-if + live-read design would need (no expression-
        // cache staleness risk — see eqAstart's comment on that hazard — since
        // nothing ever RE-READS idAcc through a cached compileCC call).
        //
        // idAcc/inPlayMask mutations still happen via plain createAssign inside each
        // nested scope; Pablo's createIf auto-joins those as a Sel at every scope
        // close, cascading correctly through however many levels are nested — same
        // mechanism the rest of the file already relies on, just applied at more
        // than one depth. T6 (no nested createIf) is intentionally set aside HERE
        // ONLY, behind this one opt-in flag, specifically to test whether the IR-size
        // cost it warns about is bearable for real merge chains.
        //
        // childrenOf[idAB] = the rules in THIS group whose idA == idAB (i.e. every
        // rule that should nest inside idAB's own gate). Empty (all rules go through
        // the untouched emitRule path below) whenever --chain-partition is off,
        // since needsLiveId is never set in that case.
        std::unordered_map<unsigned, std::vector<const MergeRule*>> childrenOf;
        for (const auto & r : mRuleGroup.rules)
            if (r.needsLiveId) childrenOf[r.idA].push_back(&r);

        // One rule's B-detect + stamp + consume, built fresh (not shared with
        // emitBody — deliberately isolated). Returns `fire` (Astart AND B-detect) so
        // a nested child can use it as ITS gate condition directly.
        auto emitChainBody = [&](PabloBuilder & body, const MergeRule & r,
                                  PabloAST * fireStart) -> PabloAST * {
            std::vector<PabloAST*> bits(W);
            for (unsigned i = 0; i < W; i++)
                bits[i] = body.createLookahead(srcBits[i], (int64_t) r.lenA);
            cc::Parabix_CC_Compiler_Builder ccAhead(BixNum(bits.begin(), bits.end()));
            PabloAST * BstartAtA = ccAhead.compileCC("Bstart_" + std::to_string(r.idA) + "_" + std::to_string(r.idB), re::makeCC(r.idB), body);
            PabloAST * fire = body.createAnd(fireStart, BstartAtA, "chainfire_" + std::to_string(r.idA) + "_"  + std::to_string(r.idB) + "_" + std::to_string(r.idAB));
            if (mHasBoundary) {
                PabloAST * bAhead = body.createLookahead(boundaryBit, (int64_t) r.lenA);
                fire = body.createAnd(fire, body.createNot(bAhead));
            }
            fire = applyVeto(body, r, fire);   // same helper the flat path uses
            PabloAST * notStamp = body.createNot(fire);
            for (unsigned i = 0; i < W_out; i++) {
                if ((r.idAB >> i) & 1u)
                    body.createAssign(idAcc[i], body.createOr(idAcc[i], fire));
                else
                    body.createAssign(idAcc[i], body.createAnd(idAcc[i], notStamp));
            }
            PabloAST * removeAt = body.createAdvance(fire, r.lenA, "removeAt_" + std::to_string(r.idA) + "_" + std::to_string(r.idB) + "_" + std::to_string(r.idAB));
            body.createAssign(inPlayMask, body.createAnd(inPlayMask, body.createNot(removeAt)));
            return fire;
        };

        // Recurse: build r's gate, then nest every one of r's chain-children INSIDE
        // it (using r's `fire`, not a fresh Astart) before closing r's createIf.
        std::function<void(PabloBuilder&, const MergeRule&, PabloAST*)> emitChain =
            [&](PabloBuilder & bld, const MergeRule & r, PabloAST * fireStart) {
                auto body = bld.createScope();
                PabloAST * fire = emitChainBody(body, r, fireStart);
                auto it = childrenOf.find(r.idAB);
                if (it != childrenOf.end())
                    for (const MergeRule * child : it->second)
                        emitChain(body, *child, fire);   // physically nested inside `body`
                bld.createIf(fireStart, body);
            };

        if (!mGrouped) {
            for (const auto & r : mRuleGroup.rules) {
                if (r.needsLiveId) continue;      // emitted as a nested child above, not a root
                if (!childrenOf.count(r.idAB)) {
                    emitRule(r);                   // no chain involved — untouched path
                    continue;
                }
                // Root of a chain: same Astart as emitRule, then nest the dependents
                // inside via emitChain instead of a flat sibling loop.
                PabloAST * fireStart = eqAstart(pb, r.idA);
                if (r.idA == r.idB)
                    fireStart = selfMergeFireStarts(pb, fireStart, r.lenA);
                emitChain(pb, r, fireStart);
                // Same rebind emitRule does after every rule — a later sibling's
                // eqAstart must not reuse the pre-chain inPlayMask pointer.
                Var * freshMask = pb.createVar("inPlayMask_" + std::to_string(maskGen++), zeroes);
                pb.createAssign(freshMask, inPlayMask);
                inPlayMask = freshMask;
            }
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
                cc::Parabix_CC_Compiler_Builder ccId(frozenBits);
                PabloAST * inRange = ccId.compileCC(re::makeCC(gLo, gHi), pb);
                auto body = pb.createScope();
                // --lookahead-in-group: build this chunk's B-detection peeks INSIDE the gate,
                // cached (deduped) per distinct lenA so the chunk's rules share them. Cold
                // chunk → the whole gate (peeks included) is block-skipped.
                std::map<unsigned, BixNum> chunkAhead;
                std::map<unsigned, PabloAST*> chunkBoundary;
                if (LookaheadInGroup && !mUseNextId) {
                    for (size_t k = s; k < e; k++) {
                        const unsigned lenA = sorted[k]->lenA;
                        if (!chunkAhead.count(lenA)) {
                            std::vector<PabloAST*> bits(W);
                            for (unsigned i = 0; i < W; i++)
                                bits[i] = body.createLookahead(srcBits[i], (int64_t) lenA);
                            chunkAhead.emplace(lenA, BixNum(bits.begin(), bits.end()));
                        }
                        if (mHasBoundary && !chunkBoundary.count(lenA))
                            chunkBoundary[lenA] = body.createLookahead(boundaryBit, (int64_t) lenA);
                    }
                }
                const std::map<unsigned, BixNum> * grpAhead = chunkAhead.empty() ? nullptr : &chunkAhead;
                const std::map<unsigned, PabloAST*> * grpBoundary = chunkBoundary.empty() ? nullptr : &chunkBoundary;
                // Per-rule Astart EQ stays INSIDE the gate body so the range gate can
                // block-skip it. Single level (T6-safe).
                for (size_t k = s; k < e; k++) {
                    const MergeRule & r = *sorted[k];
                    PabloAST * fireStart = eqAstart(body, r.idA);
                    if (r.idA == r.idB)
                        fireStart = selfMergeFireStarts(body, fireStart, r.lenA);
                    emitBody(body, r, fireStart, grpAhead, grpBoundary);
                }
                pb.createIf(pb.createAnd(inRange, meInFrozen), body);
            }
        }

        // ── Apply whatever's left in the batch accumulators ─────────────────────
        if (batch) flushWriteback();

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
    unsigned indexedShiftN = IndexedShift;
    if (const char * is = std::getenv("BPE_INDEXED_SHIFT")) indexedShiftN = (unsigned) std::atoi(is);
    bool useIndexedShift = indexedShiftN > 0;
    if (useIndexedShift)
        std::cerr << "[BPE] --indexed-shift: first " << indexedShiftN
                  << " kernels via IndexedShiftBack (1 shift/kernel)\n";
    auto compactAfter = applyCompactionSchedule(ruleRanges, compactEvery);

    // debug: dump the merge-range groups to stderr. BPE_GROUPS=1 for the
    // per-group [lo,hi) xN maxLen=M lines too (verbose, 1 line/kernel).
    std::cerr << "[BPE] " << ruleRanges.size() << " merge-range kernels ("
              << (LevelPartition ? "ASAP level schedule" : "contiguous clean ranges") << ")\n";
    if (std::getenv("BPE_GROUPS")) {
        for (const auto & g : ruleRanges)
            std::cerr << "[" << g.lo << "," << g.hi << ") x" << g.rules.size()
                      << " maxLen=" << g.maxLen << "\n";
    }

    // BPE_RULES=1: dump the resolved merge rules, one "-- kernel i --" header per
    // group so you can see exactly which ids/rules share a kernel (BPE_RULES_N
    // raises the per-kernel cap, default 4; BPE_RULES_N=999999 for "all of them").
    if (std::getenv("BPE_RULES")) {
        unsigned cap = 4;
        if (const char * n = std::getenv("BPE_RULES_N")) cap = std::atoi(n);
        for (size_t gi = 0; gi < ruleRanges.size(); gi++) {
            const auto & g = ruleRanges[gi];
            std::cerr << "-- kernel " << gi << ": [" << g.lo << "," << g.hi << ") x"
                      << g.rules.size() << " maxLen=" << g.maxLen << " --\n";
            for (unsigned i = 0; i < g.rules.size() && i < cap; i++) {
                const auto & r = g.rules[i];
                std::cerr << "    idA=" << r.idA << " idB=" << r.idB
                          << " lenB=" << r.lenB << " -> idAB=" << r.idAB
                          << "  (" << bpe.decodeToken(r.idA) << "+" << bpe.decodeToken(r.idB)
                          << "->" << bpe.decodeToken(r.idAB) << ")\n";
            }
            if (g.rules.size() > cap)
                std::cerr << "    ... (" << (g.rules.size() - cap) << " more, raise BPE_RULES_N)\n";
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
        // Excludes any group with a --chain-partition rule: eqAstart's live-idAcc read
        // (and its per-rule idAcc rebind) is only wired into the per-rule (!mGrouped)
        // loop below, not the grouped-if chunk loop.
        bool groupHasChain = std::any_of(g.rules.begin(), g.rules.end(),
                                          [](const MergeRule & r) { return r.needsLiveId; });
        bool grouped = (IfGroupLowerLimit >= 0) && ((long) i >= (long) IfGroupLowerLimit)
                       && !groupHasChain;
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
// levelPartition — ASAP level schedule of the rank-sorted rules (see --level-partition).
// Same two constraints as the clean-range walk, minus the contiguity requirement:
//   dependency — a rule's parts must be stamped by a STRICTLY earlier kernel (T4), so
//                level(r) > level(producer(idA)) and > level(producer(idB));
//   seam       — two rules sharing a token (one's right part == the other's left part)
//                must land in different kernels, lower rank first, so the lower-rank
//                write reaches the later kernel's input and starves the other.
// Walking in rank order, the seam constraint needs only two lookups: maxRight[t] is the
// highest level of an already-scheduled rule using token t as its RIGHT part, maxLeft[t]
// the same for its LEFT part. Rule r must sit after maxRight[r.idA] and maxLeft[r.idB].
//
// Group bookkeeping: `hi` must exceed every id that can appear in the kernel's OUTPUT =
// the ids already present at its input (every idAB from a lower level) plus the idABs it
// stamps itself, so it is a RUNNING maximum over levels. That keeps it monotonically
// non-decreasing, which the threaded `source` width requires (W_out never shrinks).
// NOTE `hi` is a WIDTH BOUND ONLY — under levels it is ~50k from the first kernel on, so
// it is NOT an "everything below is already stamped" watermark; applyCompactionSchedule
// asks kernelOf[] instead. `lo` is the level's lowest idAB — informational (debug dump).
// --chain-veto bookkeeping, reported on the [BPE] stderr dump.
static unsigned gChainVetoRelaxed = 0;   // rules carrying a runtime veto
static unsigned gChainVetoTerms   = 0;   // total competitor EQs those rules emit

static std::vector<MergeRuleGroup> levelPartition(const std::vector<MergeRule> & rules) {
    std::unordered_map<unsigned, unsigned> prodLevel;   // idAB -> level of the kernel that stamps i
    // token -> highest level using it as idA  
    // maxLeft[68] = 1      <- token e(68) was last used as a LEFT part in kernel 1
    std::unordered_map<unsigned, unsigned> maxLeft; 
    // token -> highest level using it as idB
    //  // maxRight[98] = 2      <- token e(98) was last used as a RIGHT part in kernel 1
    std::unordered_map<unsigned, unsigned> maxRight;    
    std::vector<std::vector<MergeRule>> levels;  // what rules are in each level (rank order)

    for (const auto & r : rules) {                      // rank order, Take every rule one at a time.
        unsigned lvl = 1;    // lets assume the rule can sit in level 1 (no deps, no seams). Then check the constraints.
        auto after = [&](const std::unordered_map<unsigned, unsigned> & m, unsigned key) {
            auto it = m.find(key);  // search/find if the token was used as a part in a lower kernel
            // If it was, the rule must sit after that kernel (level = that kernel's level + 1).
            // If not, the rule can sit in level 1 (no constraint).
            if (it != m.end() && it->second + 1u > lvl) lvl = it->second + 1u;
        };
        // sameOrAfter: like `after`, but level ITSELF is acceptable (not level+1). Used
        // only for the --asymmetric-seam maxRight relaxation: the live-mask read makes
        // same-kernel coexistence correct (rank order inside the kernel still runs the
        // consumer before the dependent), but the rule must never land STRICTLY EARLIER
        // than the token's consumer.
        auto sameOrAfter = [&](const std::unordered_map<unsigned, unsigned> & m, unsigned key) {
            auto it = m.find(key);
            if (it != m.end() && it->second > lvl) lvl = it->second;
        };
        // dependency: idA must be stamped by a lower-OR-SAME kernel under
        // --chain-partition (excluding self-merges, see ChainPartition's comment);
        // otherwise (default) strictly a lower kernel, same as idB below.
        if (ChainPartition && r.idA != r.idB) sameOrAfter(prodLevel, r.idA);
        else after(prodLevel, r.idA);
        after(prodLevel, r.idB);      // dependency: idB stamped by a lower kernel
        // seam: an earlier rule consumed this token as its B. Under --asymmetric-seam,
        // softened to same-level-or-after — the runtime gate now reads inPlayMask LIVE
        // (see eqAstart), so a same-kernel earlier consume is visible without a level
        // split, but the rule still can't jump to an EARLIER level than its consumer.
        // EXCEPT self-merges (idA==idB): selfMergeFireStarts' run-parity math assumes a
        // STATIC isX (see its call site comment — "no same-kernel rule can have
        // consumed an X-run position... so frozen==live"). 
        if (AsymmetricSeam && r.idA != r.idB) sameOrAfter(maxRight, r.idA);
        else after(maxRight, r.idA);
        // seam: an earlier rule claimed this token as its A. Without --chain-veto this is
        // a hard split — the forward LookAhead reads frozen input only (T5), so a
        // same-kernel competitor's consume of idB is invisible. With it, the rule tests
        // the competitor itself and stands down at runtime (see ChainVeto).
        std::vector<unsigned> veto;
        bool relaxed = false;
        if (ChainVeto && r.idA != r.idB) {
            unsigned lvlStrict = lvl, lvlRelaxed = lvl;      // lvl = level ignoring maxLeft
            auto ml = maxLeft.find(r.idB);
            if (ml != maxLeft.end()) {
                if (ml->second + 1u > lvlStrict)  lvlStrict  = ml->second + 1u;
                if (ml->second      > lvlRelaxed) lvlRelaxed = ml->second;
            }
            if (lvlRelaxed < lvlStrict && lvlRelaxed >= 1 && lvlRelaxed <= levels.size()) {
                bool competitorItselfVetoed = false;
                for (const auto & c : levels[lvlRelaxed - 1]) {
                    if (c.idA != r.idB) continue;            // not a competitor
                    if (c.idA == c.idB || !c.vetoIdB.empty()) {
                        competitorItselfVetoed = true;       // beatable → depth-1 veto
                        break;                               // would be wrong; take split
                    }
                    veto.push_back(c.idB);
                }
                if (!competitorItselfVetoed && !veto.empty()) {
                    lvl = lvlRelaxed;          // share the competitor's kernel
                    relaxed = true;
                    gChainVetoRelaxed++;
                    gChainVetoTerms += veto.size();
                }
            }
        }
        if (!relaxed) { veto.clear(); after(maxLeft, r.idB); }

        // Base ids (< 256) are absent from prodLevel — the seed supplies them, so they
        // impose no constraint and such a rule can sit in level 1.
        // If the number of levels we currently have is LESS than the level needed for this rule, create more level slots.
        if (levels.size() < lvl) levels.resize(lvl);

        // needsLiveId: true exactly when idA's producer landed at THIS SAME level —
        // only possible via the sameOrAfter(prodLevel, r.idA) branch above (the plain
        // after() branch always forces a STRICTLY later level), so this is a no-op
        // (always false) whenever --chain-partition is off.
        MergeRule rc = r;
        {
            auto it = prodLevel.find(r.idA);
            rc.needsLiveId = (it != prodLevel.end() && it->second == lvl);
        }
        if (relaxed) {
            rc.vetoIdB = std::move(veto);
            rc.vetoOff = rc.lenA + rc.lenB;   // byte space; rewritten under compaction
        }
        levels[lvl - 1].push_back(rc);
        prodLevel[r.idAB] = lvl;
        auto keepMax = [](std::unordered_map<unsigned, unsigned> & m, unsigned k, unsigned v) {
            unsigned & slot = m[k];
            if (v > slot) slot = v;
        };
        // Mask for at what level this rule used idA and idB.
        // so that later rules can respect those dependencies.
        keepMax(maxLeft,  r.idA, lvl);
        keepMax(maxRight, r.idB, lvl);
    }
    // map the level-partitioned rules into MergeRuleGroups, 
    // each level will be a MergeRuleGroup, and compute the lo/hi/maxLen for each group.
    std::vector<MergeRuleGroup> ranges;
    unsigned runningMax = 255;                          // base alphabet occupies 0..255, the largest token ID we've seen so far is 255
    // go through each level's rules. 
    for (auto & lv : levels) {                         
        if (lv.empty()) continue;
        MergeRuleGroup g;
        // Move the level's rules into the group
        g.rules = std::move(lv);
        g.lo = g.rules.front().idAB;                    // pushed in rank order -> lowest, first rule in g.rules
        for (const auto & r : g.rules) {
            unsigned mergedLen = r.lenA + r.lenB;
            if (mergedLen > g.maxLen) g.maxLen = mergedLen;   // maximum merged length among all rules in the group, the longest resulting token
            if (r.idAB > runningMax) runningMax = r.idAB;
        }
        g.hi = runningMax + 1;
        ranges.push_back(std::move(g));
    }
    return ranges;
}

// Mark rules whose gate needs a POST-write live mask: an earlier (lower-rank) rule
// in the SAME group already consumed this rule's idA as its own idB. Under the
// default symmetric seam test (clean-range and --level-partition without
// --asymmetric-seam) this can never occur — the seam test forbids both overlap
// directions within one group — so this pass is a no-op there. Under
// --asymmetric-seam it is exactly the ONE relaxed direction (see AsymmetricSeam's
// `sameOrAfter(maxRight, r.idA)` in levelPartition). --batch-writeback defers the
// inPlayMask clear to the end of the kernel; a flagged rule must force a flush of
// the pending write-back first, or its eqAstart sees the kernel's frozen entry
// mask instead of the producer's consume and misfires.
static void tagFlushPoints(std::vector<MergeRuleGroup> & groups) {
    unsigned totalFlags = 0, totalRules = 0, groupsWithFlags = 0;
    for (auto & g : groups) {
        std::unordered_set<unsigned> consumedAsB;   // idB values used by earlier rules in this group
        unsigned before = totalFlags;
        for (auto & r : g.rules) {
            // Two reasons a rule's gate must read the LIVE mask rather than the kernel's
            // frozen entry mask:
            //  1. --asymmetric-seam: an earlier rule in this group consumed this rule's
            //     idA as ITS idB, so idA is already gone and the gate has to see that.
            //  2. --chain-veto: this rule now shares its kernel with the lower-rank rules
            //     claiming its idB — that sharing is exactly what the veto buys. Those
            //     competitors fire first and clear starts around this rule's B, so the
            //     same staleness applies and the frozen entry mask would hide it.
            if (consumedAsB.count(r.idA) || !r.vetoIdB.empty()) { r.needsFlush = true; totalFlags++; }
            consumedAsB.insert(r.idB);
        }
        totalRules += g.rules.size();
        if (totalFlags > before) groupsWithFlags++;
    }
    if (std::getenv("BPE_FLUSH_STATS"))
        std::cerr << "[BPE] tagFlushPoints: " << totalFlags << "/" << totalRules
                  << " rules flagged needsFlush across " << groupsWithFlags << "/"
                  << groups.size() << " groups\n";
}

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

    // 3. Partition. --level-partition takes the ASAP level schedule (minimum kernel
    //    count, same constraints); the default is the clean_range walk: lo = first
    //    rule's idAB (its parts always precede it, so it always fits); extend while the
    //    rule is dependency-independent (idA<lo && idB<lo) AND token-adjacency-overlap-
    //    free vs every rule already in the group. The first rule violating either
    //    starts the next range.
    if (LevelPartition) {
        auto groups = levelPartition(rules);
        tagFlushPoints(groups);
        if (std::getenv("BPE_CHAIN_STATS")) {
            unsigned n = 0, total = 0;
            for (auto & g : groups) {
                total += g.rules.size();
                for (auto & r : g.rules) if (r.needsLiveId) n++;
            }
            std::cerr << "[BPE] chain-partition: " << n << "/" << total
                       << " rules flagged needsLiveId\n";
        }
        if (ChainVeto)
            std::cerr << "[BPE] chain-veto: " << gChainVetoRelaxed
                      << " rules kept their level via a runtime veto ("
                      << gChainVetoTerms << " competitor EQs)\n";
        return groups;
    }

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
    tagFlushPoints(ranges);
    return ranges;
}
