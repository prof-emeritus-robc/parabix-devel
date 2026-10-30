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
 *                       (width grows to ceil_log2(hi) bits per range), so a
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
#include <atomic>
#include <memory>
#include <queue>
#include <array>
#include <iomanip>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MathExtras.h>
#include <pablo/pablo.h>
#include <pablo/bixnum/bixnum.h>
#include <re/cc/cc_compiler.h>
#include <re/cc/cc_compiler_target.h>
#include <re/adt/re_cc.h>
#include <ucd/core/unicode_set.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/core/kernel_builder.h>        // BPEStuckKernel (--retire-stuck)
#include <kernel/basis/p2s_kernel.h>
#include <llvm/IR/Module.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>    // SpreadByMask (--retire-words)
#include <kernel/streamutils/stream_shift.h>   // IndexedShiftBack (BPE_INDEXED_SHIFT)
#include <stdexcept>
#include <boost/intrusive/detail/math.hpp>
#include <kernel/io/source_kernel.h>              // MemorySourceBlockCapacity (--memsource-block-capacity)
#include <kernel/scan/base.h>                     // TwoLevelScanKernel (--tail-scan-kernel)
#include <kernel/util/bixhash.h>                  // BixHash (--tail-bixhash)
#include <numeric>
#include <random>
#include <sys/mman.h>                             // --tail-shared-cache-bits
#include <toolchain/toolchain.h>                  // codegen::SegmentThreads (--tail-shared-cache-bits)
#include <chrono>                                 // --tail-vocab-table build time
#include <thread>                                 // --tail-vocab-table builder
using boost::intrusive::detail::ceil_log2;

using namespace llvm;
// Every optional BPE optimization defaults OFF — a bare run is the plain, unoptimized
// pipeline, and each optimization is opted into on the command line. 
// --compaction: where FilterByMask compaction goes between merge kernels.
//   linear         — after every N kernels (N = --compact-base).
//   geometric      — after N kernels, then 2N more, 4N more, ... (N = --compact-base).
//   by-output-bits — between two kernels whose output id widths differ, i.e. after the
//                    last kernel of each width (with --partition=bit-tier: after
//                    each bit tier). --compact-base=N is the minimum kernel number: a width
//                    change after fewer than N kernels is skipped, deferring compaction to
//                    the next width change.
// Not given: linear when --compact-base > 0 (geometric with --geometric-compaction),
// otherwise no compaction. Conflicting settings halt (see compactionMode).
enum class CompactionMode { None, Linear, Geometric, ByOutputBits };
static cl::opt<CompactionMode> Compaction(
    "compaction",
    cl::desc("FilterByMask compaction schedule between merge kernels:"),
    cl::values(clEnumValN(CompactionMode::Linear, "linear",
                          "after every --compact-base kernels"),
               clEnumValN(CompactionMode::Geometric, "geometric",
                          "after --compact-base kernels, then doubling the interval"),
               clEnumValN(CompactionMode::ByOutputBits, "by-output-bits",
                          "between kernels whose output id widths differ, from kernel "
                          "--compact-base on")),
    cl::init(CompactionMode::None));

static cl::opt<unsigned> CompactionBase(
    "compact-base",
    cl::desc("linear/geometric compaction: kernels per compaction interval (0 = off, the "
             "default). by-output-bits: minimum kernel number for a compaction."),
    cl::init(0));

static cl::opt<bool> GeometricCompaction(
    "geometric-compaction",
    cl::desc("Same as --compaction=geometric."),
    cl::init(false));

// Grouped-if: rules sharing a first id (idA) collapse under ONE createIf gate
// (shared Astart EQ) instead of one per rule. Single-level (no nested if → T6).
// Applied only to kernels with index >= IfGroupLowerLimit — the early kernels
// fire on almost every block (grouping there saves gates but never skips), so
// grouping is aimed at the later kernels. -1 = off (per-rule everywhere), unless one of
// --if-group-size/--if-group-count/--if-test-significant-bits is given, which implies 0
// (see effIfGroupLowerLimit).
static cl::opt<int> IfGroupLowerLimit(
    "if-group-lower-limit",
    cl::desc("Group merge rules by first id under one createIf, for kernels at/after "
             "this index (-1 = off; default -1, or 0 when --if-group-size, "
             "--if-group-count or --if-test-significant-bits is given)."),
    cl::init(-1));


// FIXED COUNT: give every kernel exactly K grouped-if gates. Gate SIZE then VARIES per
// kernel = rules/K (a 900-rule kernel -> 900/K rules per gate, a 10-rule kernel -> 10/K).
// Same gate structure everywhere, scales with kernel size. Applies to grouped kernels
// (giving it turns grouping on; see effIfGroupLowerLimit). Default 1 = one gate covering
// all the kernel's rules.
static cl::opt<unsigned> IfGroupCount(
    "if-group-count",
    cl::desc("Grouped-if gates per kernel (implies --if-group-lower-limit=0 if unset). "
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

// HIGH-BIT PREFIX: instead of chunking the idA-sorted rules by size or count, group rules
// whose idA share the same K significant bits, counted down from the highest 1 bit of idA
// (so idA of bit-length L is keyed by bits [L-K, L), and ids with L <= K by their whole
// value). The group's gate tests those K bits together with the zero bits above them;
// inside the gate each rule's Astart ANDs in the test of the remaining L-K low bits
// (shared by rules with the same idA). Giving it turns grouping on (see
// effIfGroupLowerLimit); overrides --if-group-count/--if-group-size when > 0.
static cl::opt<unsigned> IfTestSignificantBits(
    "if-test-significant-bits",
    cl::desc("Grouped kernels: group rules by the K bits of idA starting at its highest 1 bit; "
             "the if-gate tests those K bits (and the zero bits above them) and each rule "
             "completes its idA test inside the gate. 0 = off (default)."),
    cl::init(0));

// EMBEDDED IF (with --if-test-significant-bits): inside a group's gate, each idA-idB rule
// gets its own inner createIf whose condition tests the idA bits the gate left untested
// plus enough of idB's LOW bits to make E bits in all. The rest of the rule (the high
// bits of idB, boundary, veto, stamp) is emitted in that inner body. 0 = off (default).
static cl::opt<unsigned> EmbeddedIfBits(
    "embedded-if-bits",
    cl::desc("With --if-test-significant-bits: nest each rule in an inner if testing the "
             "remaining idA bits plus low idB bits, E bits in all. 0 = off (default)."),
    cl::init(0));

// Effective --if-group-lower-limit: when left at its default (-1 = off) but a grouping
// option is given on the command line, grouping is evidently wanted, so apply it to every
// kernel (0) rather than silently ignoring that option.
static int effIfGroupLowerLimit() {
    if (IfGroupLowerLimit.getNumOccurrences() == 0 && IfGroupLowerLimit < 0
            && (IfGroupSize.getNumOccurrences() > 0 || IfGroupCount.getNumOccurrences() > 0
                || IfTestSignificantBits.getNumOccurrences() > 0))
        return 0;
    return IfGroupLowerLimit;
}

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
// --partition=mode: how merge rules are packed into kernels. Same shape as
// --compaction=mode. clean is the default and most battle-tested; the others trade
// more risk for fewer kernels.
//
//   clean       Cut the rank-sorted rule list into contiguous id ranges, closing a
//               group at the FIRST rule that violates a dependency/seam constraint.
//               Measured over full GPT-2: 1098 of its 1123 groups close on a seam
//               conflict, and at each break ~179 of the next 200 rules would still
//               have fit — ~90% of the available packing discarded purely to keep
//               groups contiguous. Group size saturates near 44, 50000/44.5 = 1123.
//
//   level       Drop contiguity only: each rule takes its EARLIEST legal kernel,
//                 level(r) = 1 + max( level(producer of idA), level(producer of idB),
//                                     level of any lower-rank rule seaming with r )
//               A violator defers ITSELF instead of ending the group. Kernel count
//               becomes the constraint DAG's longest path (a provable minimum):
//               1123 -> 283, mean group size 44.5 -> 176.7. BOTH correctness
//               constraints (T4 + the clean-range conflict test) still hold exactly;
//               only the packing changes. Levels are NOT rank intervals — level 1
//               legitimately holds rank 0 alongside rank 47815.
//
//   asymmetric  level, plus drop the maxRight/idA seam (keep maxLeft/idB). Two rules
//               sharing token t where an earlier rule used t as its B part and a
//               later rule uses t as its A part (e.g. rank11 "Ġ+o" vs rank17 "o+r" on
//               "Ġor") no longer need separate levels: rank11's fire clears t's
//               live-start bit in inPlayMask, and the later rule's gate reads that
//               mask LIVE. The other direction (maxLeft/idB) stays a hard separation —
//               B-detect is a forward LookAhead on the FROZEN kernel input (T5), so it
//               can never see a same-kernel restamp. Cuts 283 -> 76 kernels (measured
//               via merge_analysis.py's scheduling model; first C++ implementation —
//               less battle-tested than level).
//
//   chain       level, plus two more relaxations, always together:
//               - chain-partition: a rule whose idA was STAMPED by an earlier
//                 same-level rule shares that level instead of taking the next one —
//                 a dependency chain A+B->AB, AB+C->ABC... collapses into ONE kernel.
//                 Same mechanism as asymmetric, one level up: a same-kernel rule's ID
//                 read goes live (idAcc) instead of frozen, via createIf's normal
//                 Var-reassignment-as-Sel join (no nested createIf, T6 -- chain rules
//                 stay SIBLING ifs in rank order).
//               - chain-veto: relaxes the OTHER seam (maxLeft/idB) that asymmetric
//                 leaves intact, by paying for it with a runtime test instead of a
//                 kernel boundary. Rules A+B->AB (rank1), C+D->CD (rank2), AB+C->ABC
//                 (rank3): maxLeft[C] normally forces ABC one level later, because on
//                 "ABCD" the lower-rank CD must consume C first. Instead, ABC checks
//                 CD itself -- the id one slot past the merged token is read from the
//                 FROZEN kernel input and compared against D:
//                   veto     = EQ(LookAhead(src, lenA+lenB), idD)  // + one per competitor
//                   fire_ABC = fire_AB AND hasC AND NOT veto
//               "ABCD" -> veto=1, ABC suppressed, CD fires same kernel; "ABCX" ->
//               veto=0, ABC fires. Both match HF. The competitor set is exact and
//               small (rank order guarantees every same-or-earlier-level competitor is
//               already placed); a competitor that could itself be beaten by a
//               lower-rank rule carries its own non-empty vetoIdB, and THAT makes this
//               rule fall back to the strict level instead of approximating.
//               In both idA (chain-partition) and idB (chain-veto), the RIGHT-hand
//               producer side always needs a real kernel boundary -- B-detection is a
//               forward LookAhead on the frozen input (T5) and can never see a
//               same-kernel producer.
// BitTier: confine every kernel to merged ids of ONE bit length. Tier b holds the rules
// with idAB in [2^(b-1), 2^b) — tier 9 = ids 256..511, tier 10 = 512..1023, … Each tier is
// partitioned on its own via ASAP level scheduling (same as Level) and the tiers run in
// order, so the first kernel of tier N+1 reads an N-bit id stream and writes N+1 bits, and
// every later kernel of that tier reads and writes N+1 bits. Both correctness constraints
// carry across a tier boundary for free: every rule of a lower tier has lower rank and
// runs in a strictly earlier kernel, which is exactly what the dependency and seam
// constraints ask of an earlier rule. Only the packing changes. Mutually exclusive with
// Level/Asymmetric/Chain (one PartitionMode at a time).
enum class PartitionMode { Clean, Level, Asymmetric, Chain, BitTier };
static cl::opt<PartitionMode> Partition(
    "partition",
    cl::desc("Merge-rule partitioning strategy:"),
    cl::values(
        clEnumValN(PartitionMode::Clean, "clean",
            "Contiguous clean id ranges (default, most battle-tested)."),
        clEnumValN(PartitionMode::Level, "level",
            "ASAP level scheduling: minimum kernel count (283 vs 1123)."),
        clEnumValN(PartitionMode::Asymmetric, "asymmetric",
            "Level scheduling, relaxed maxRight seam (76 kernels; less battle-tested)."),
        clEnumValN(PartitionMode::Chain, "chain",
            "Level scheduling, same-level dependency chains + veto (fewer kernels; "
            "slower in testing so far)."),
        clEnumValN(PartitionMode::BitTier, "bit-tier",
            "ASAP level scheduling confined to one merged-id bit length per kernel, "
            "tiers run in order, so each kernel grows the id stream by at most one bit.")),
    cl::init(PartitionMode::Clean));
static bool isLevelPartition()  { return Partition != PartitionMode::Clean; }
static bool isAsymmetricSeam()  { return Partition == PartitionMode::Asymmetric; }
static bool isChainPartition()  { return Partition == PartitionMode::Chain; }
static bool isChainVeto()       { return Partition == PartitionMode::Chain; }
static bool isBitTierPartition() { return Partition == PartitionMode::BitTier; }

// Cap the rules per merge kernel: a partition group with more than this many rules is
// split into ceil(n/cap) consecutive kernels of near-equal size, in rank order. Pablo's
// compile time is superlinear in kernel size, and --partition=bit-tier puts most of a
// bit tier into its first level (8023 rules in the 16-bit tier's first kernel). Splitting
// is exact: the rules of one group have
// no dependencies or seams among each other that a later kernel could break —
//   - dependency / seam: a lower-rank rule moves to an EARLIER kernel, which is what
//     both constraints ask of it anyway;
//   - --partition=chain: needsLiveId is recomputed per chunk; a child whose producer
//     landed in an earlier chunk reads the producer's stamp from its frozen input;
//   - --partition=chain: a competitor in an earlier chunk has already stamped its merged id
//     over this rule's idB slot, so B-detect fails exactly where the veto would fire;
//   - --partition=asymmetric: an earlier chunk's consume reaches this kernel through meIn.
// 0 = no cap.
static cl::opt<unsigned> MaxMergesPerKernel(
    "max-merges-per-kernel",
    cl::desc("Split any merge kernel with more than this many rules into consecutive "
             "kernels of near-equal size (0 = no cap). Default 500."),
    cl::init(500));

// --partition=chain emission shape. Output-identical: Pablo ORs pending carries into
// every if test (CarryManager::generateEntrySummaryTest), so fusing same-condition ifs,
// or dropping one whose body is a no-op when its condition is zero, changes nothing.
static cl::opt<bool> ChainFuseSiblings(
    "chain-fuse-siblings",
    cl::desc("With --partition=chain, nest all chain-children of a rule under ONE createIf "
             "on its fire instead of one createIf per child."),
    cl::init(false));

static cl::opt<bool> ChainUngateRoots(
    "chain-ungate-roots",
    cl::desc("With --partition=chain on grouped kernels, drop a chain root's own createIf "
             "when the group gate already tests its full idA."),
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
// Carry ops per kernel: one per RULE (~177 under --partition=level) → one per
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

// Apply the end-of-file mask ONCE per merge kernel instead of once per id compare.
// cc::Parabix_CC_Compiler::compileCC wraps every result in InFile (cc_compiler.cpp:144),
// one extra load+not+and per compare per block — 35,104 of them in the entry scopes alone
// at the best config, ~16% of the always-run ops. With this flag the compares are built
// as the same balanced AND tree (so Pablo CSE shares them exactly as before) but without
// the InFile, and InFile is applied once to the kernel's live-start mask instead. Every
// gate and every fire is ANDed with that mask, so nothing changes inside the file; past
// EOF the threaded mask now reads 0 instead of 1. Cache tag io1_ (only when on).
static cl::opt<bool> InFileOnce(
    "infile-once",
    cl::desc("Apply the end-of-file mask once per merge kernel (on the live-start mask) "
             "instead of once per id compare."),
    cl::init(false));

// Grouped kernels with --if-test-significant-bits: fold the live-start mask into each
// gate's id-compare AND tree instead of ANDing it onto the finished compare. The gate
// condition was And(idHi_compare, meInFrozen) — one private AND per gate, ~35k across the
// pipeline, all in the always-run entry scope. With the mask as one more leaf of the tree
// it lands on a partial product that gates with the same bit length share (Pablo CSE), so
// that AND is paid a handful of times per kernel instead of once per gate. Same value:
// AND is associative and commutative. Cache tag gm1_.
static cl::opt<bool> GateMaskFold(
    "gate-mask-fold",
    cl::desc("Grouped kernels: fold the live-start mask into each gate's shared id-compare "
             "tree instead of one AND per gate."),
    cl::init(false));

// Compile time: leave InstCombine out of the LLVM IR passes for the BPE merge kernels
// only (via the NoInstCombinePass marker in addOptimizationPasses; every other kernel and
// every other tool keeps it). Profiling a cold 283-kernel compile put InstCombine at ~29%
// of compile CPU, while Pablo has already simplified the and/or/not trees it works on.
// Measured: cold compile 44 s -> 34 s, runtime unchanged (<=1%), compare_bpe MATCH.
// Cache tag ic1_ (the IR passes are not part of the cache key otherwise).
static cl::opt<bool> SkipInstCombine(
    "skip-instcombine",
    cl::desc("BPE merge kernels: skip LLVM's InstCombine pass (faster compile)."),
    cl::init(false));

// Merge kernel gate codegen. The skip path of a grouped gate (the code run on every block
// the gate does not enter, ~95% of them) measured ~18 AArch64 instructions on a cached
// best-config kernel: 6 ANDs, 6 for the test, 5 spill loads/stores. The two flags below
// attack the test and the spills through PabloKernel hooks that are off by default.
// Measured on val_4MB (bit-tier config, M3, 3 interleaved rounds, bench min, 2026-10-15):
// --if-reduce-test -7.3%, --if-unlikely-weight=32 alone -5.9%, both -9.2%; compare_bpe MATCH.
//
// --if-reduce-test (PabloKernel::reduceIfTest): test each gate with a max-reduce over its
// 32-bit lanes. AArch64: umaxv, fmov, cbz instead of cmeq, bic (a {1,2} constant kept
// live in a register), addp, fmov, tst, b.eq. Meant for ARM; x86 bitblock_any is already
// short. Cache tag rt1_.
static cl::opt<bool> IfReduceTest(
    "if-reduce-test",
    cl::desc("BPE merge kernels: test each if condition with a max-reduce (shorter on AArch64)."),
    cl::init(false));

// --if-unlikely-weight=N (PabloKernel::ifBodyUnlikelyWeight): give every if of a grouped
// merge kernel branch_weights 1:N. Gates are entered on ~5% of blocks, but without weights
// LLVM treats each body as half the time hot, keeps body-only values in registers and
// spills the skip path instead. Grouped kernels only: the flat early kernels gate on common
// ids. 0 = off. Cache tag uwN_.
static cl::opt<unsigned> IfUnlikelyWeight(
    "if-unlikely-weight",
    cl::desc("Grouped BPE merge kernels: mark if bodies unlikely with branch weights 1:N "
             "(0 = off, the default)."),
    cl::init(0));

// --retire-words: at every FilterByMask point, also drop the finished words from the
// stream. In the compacted frame every position is a live token start, so a token that
// starts a word and is followed by another word start is a whole word. BPE never merges
// across words, so no later kernel can change it. Late kernels then scan about half the
// positions (model, bit-tier config on the 1 MiB sample: 1.59x fewer positions, -39%
// cycle-weighted merge work). The retired ids go back in order before emission:
// SpreadByMask(keep) plus an OR of the retired ids at each point, then
// SpreadByMask(inPlayMask) between points, so the emitter scans the frame of the first
// retiring point. Dropping a word can make X tokens of two different words adjacent, so
// self-merge runs also break at word starts under this flag. Needs --compaction and a
// pretokenizer. Cache tag rw1_.
// Measured (bit-tier + --if-reduce-test --if-unlikely-weight=32, M3, 2026-10-15): bench_bpe
// val_4MB 201 -> 167 ms (-17%, 3 interleaved rounds), -20% at 1 thread; cold compile +3 s
// (19 -> 22 s); compare_bpe MATCH on every tokenizer_files input up to val.txt (50 MB).
static cl::opt<bool> RetireWords(
    "retire-words",
    cl::desc("BPE: drop finished single-token words from the stream at each compaction "
             "point (needs --compaction and a pretokenizer)."),
    cl::init(false));

// --retire-points=i,j,...: with --retire-words, retire only at these FilterByMask points
// (1 = the first). Empty = every point. A point saves (words it retires) x (kernels after
// it); points that retire few words late cost more than they save.
static cl::list<unsigned> RetirePoints(
    "retire-points", cl::CommaSeparated,
    cl::desc("With --retire-words: the compaction points (1-based) that retire words; "
             "default all."));

// --retire-frozen (with --retire-words): also retire frozen tokens, whose id no rule of a
// later kernel uses as its left or right part, so they can never merge again. GPT-2 has no
// rule with NUL (id 188) at all, so every NUL of a tar stream is frozen from the start. The
// token after a dropped frozen token gets a boundary bit (a barrier), so the merge kernels
// never join its neighbours, and one-token segments between barriers retire too. Model,
// bit-tier on the 1 MiB sample: 2.95x fewer merge positions (-68% cycle-weighted) vs 1.59x
// (-38%) for single-token words alone. Cache tag: the frozen set's hash in BPE_Retire.
static cl::opt<bool> RetireFrozen(
    "retire-frozen",
    cl::desc("With --retire-words: also retire tokens no later rule can merge."),
    cl::init(false));

// --retire-stuck=N (with --retire-words): from compaction point N on (1-based), retire every
// segment that can never change again: no adjacent pair (a, b) in it is a rule of a later
// kernel. BPE only ever merges an adjacent pair that is a rule, so such a segment is final.
// Segments run between word starts, barriers and frozen tokens; frozen tokens retire as
// under --retire-frozen. A pair test is too big for bit-parallel code, so it is scalar: at
// each such point BPEStuckKernel hands the Fp blocks to bpe_retire_stuck (C++), which looks
// the pairs up in a bloom filter of the remaining rules and the ids in a frozen-id bitmap. It
// replaces BPEFrozenKernel and BPERetireKernel there. Model (firesim/retire_more.py, LF 1 MiB
// sample): from point 5 on, 18.6% less cycle-weighted merge work than --retire-frozen, for a
// scan of 0.2 tokens per input byte. Measured (M3, 3 interleaved rounds, bench_bpe):
// --retire-stuck=5 val_4MB 117.4 -> 109.9 ms (-6.4%), webtext_cases 241 -> 221 ms (-8.3%);
// earlier points cost more scanning than they save. 0 = off. Cache tag: the tables' hash
// in BPE_Stuck16.
static cl::opt<unsigned> RetireStuck(
    "retire-stuck",
    cl::desc("With --retire-words: from compaction point N on (1-based), also retire segments "
             "in which no adjacent pair is a later rule (0 = off, the default)."),
    cl::init(0));

// --bpe-tail=P (needs a pretokenizer and --compaction): stop the merge kernels at
// compaction point P (1-based) and finish BPE in the emitter. From there each segment
// (between word starts and retirement barriers) gets the standard algorithm: merge its
// lowest-ranked adjacent pair among the rules of the kernels not built, until none is
// left. The bit-parallel kernels apply the early, frequent merges to the dense stream;
// the late tiers rarely fire on a short residue, which a scalar loop handles far more
// cheaply than ~200 kernels that each scan every live position. Same output: the rules of
// the kernels run can no longer apply (their parts are all built by then), so what is left
// is plain rank-order BPE on the remaining rules. The emitter's scan runs in order on one
// thread, so a segment can span pipeline calls. 0 = off. Cache tag: "_t" on the merge-back
// kernels, which carry an 18th stream, the segment starts.
static cl::opt<unsigned> BPETail(
    "bpe-tail",
    cl::desc("Stop the merge kernels at compaction point P (1-based) and finish BPE per "
             "segment in the emitter (0 = off, the default)."),
    cl::init(0));

// --tail-direct-input (with --bpe-tail, when no retirement point comes before the cut): hand
// the tail the last kernel's id bits, live mask and segment starts as they are. Otherwise
// they go through BPEWidenKernel (to 16 id bits), BPERetirePackKernel and
// BPERetireUnpackKernel, which with no merge-back in between copy 18 streams twice for
// nothing; P2S16Kernel zero-pads the missing id bits itself. Cache tag: the kernels differ
// (BPE_TailSeg, and P2S16 named by its input's shape).
static cl::opt<bool> TailDirectInput(
    "tail-direct-input",
    cl::desc("With --bpe-tail and no retirement point before the cut: feed the tail the "
             "kernel streams directly, without the widen/pack/unpack copies (default off)."),
    cl::init(false));

// --tail-from-bytes (needs a pretokenizer): no merge kernels at all, and no partition (the
// partition and compaction flags are inert). The --bpe-tail emitter runs rank-order BPE
// with every rule on each pretoken, straight from the input bytes, and its per-thread word
// cache is keyed by the pretoken's bytes (2..15 of them: two 8-byte loads, no id
// gathering). Cheaper than the hybrid because a cache hit costs about the same at any cut
// and nothing else runs: no BPERangeSeed, no bit-transformation kernels, no P2S16.
// Single-thread harness (val_4MB): the whole BPE in 10.8 ms warm / 14.8 ms cold, vs ~30 ms
// for the 15 kernels + the --bpe-tail=1 tail. Cache tag: BPE_TailResolveBytes.
static cl::opt<bool> TailFromBytes(
    "tail-from-bytes",
    cl::desc("With a pretokenizer: build no merge kernels; the tail applies every rule per "
             "pretoken from the raw bytes (default off)."),
    cl::init(false));

// --tail-cache-pool: keep the tail's per-thread scratch (its 2 MB segment cache) in a pool
// that outlives the pipeline's worker threads. The pipeline starts new workers on every run,
// so without it each worker allocates, zeroes and page-faults a fresh cache and starts with
// it cold. With it a worker takes a cache a finished thread left, still valid (the
// generation check drops it when the rule table changes). Runtime only, no cache tag.
static cl::opt<bool> TailCachePool(
    "tail-cache-pool",
    cl::desc("With --bpe-tail: reuse the tail's per-thread segment caches across pipeline "
             "runs and threads (default off)."),
    cl::init(false));

// --vm-ring-buffers: retired, accepted and ignored. Circular buffers are now built with Mach
// vm_remap on macOS by default (kernel/core make_circular_buffer); --CircularBufferImpl=memfd
// selects the old temp-file mapping.
static cl::opt<bool> VMRingBuffers(
    "vm-ring-buffers",
    cl::desc("Retired: macOS circular buffers use vm_remap by default (see --CircularBufferImpl)."),
    cl::init(false), cl::Hidden);

// --memsource-block-capacity: the in-memory source's buffer capacity is the input length
// rounded up to a whole block (kernel::MemorySourceBlockCapacity). Without it, an input whose
// last segment has 1..127 bytes (length % --segment-size in 1..127, at least 4 segments in)
// gets its final partial block read from block 0: the last tokens are made from the
// file's first bytes (found as bug 6: long.txt at --segment-size=4096; 70 * 16384 + 38 bytes
// fails at the default size). Cache tag "+bc" on the source kernel.
static cl::opt<bool> MemSourceBlockCapacity(
    "memsource-block-capacity",
    cl::desc("Round the in-memory source's buffer capacity up to a whole block (fixes wrong "
             "last tokens when the last segment is under one block; default off)."),
    cl::init(false));

// --bit-xfrm-limit=N: build every merge kernel whose output ids have at most N bits as a
// bit transformation (BPEXfrmKernel) instead of per-rule gates: every output bit is the
// input bit XOR a "change" stream, a character class over the (idA, idB) pair. With
// --partition=bit-tier these are the kernels of merge id bit tiers 9..N. Ids of
// up to kXfrmLowBits bits fit the 20-bit class whole; wider ids are split into subtiers
// (see BPEXfrmKernel). 0 = off.
static cl::opt<unsigned> BitXfrmLimit(
    "bit-xfrm-limit",
    cl::desc("Build merge kernels whose output ids have at most N bits as bit "
             "transformations, ids over 10 bits by subtiers (0 = off, the default)."),
    cl::init(0));

// --subtier-grouping-by-min-ID=N: in the subtiered kernels of merge id bit tiers >= N, key
// each rule's subtier by its SMALLER part. Rules with idA < idB (group A) keep the idA key;
// rules with idA > idB (group B) take the high bits of idB as their key and build their
// character classes over (low bits of idB, all bits of idA). The smaller part is the one
// that tends to be small, so a kernel's rules fall into fewer subtiers (about a third
// fewer per kernel in tiers 13-16). idB is read L slots ahead, so group B's subtiers are
// split by L; in tiers 11-12 that costs more than the grouping saves. 0 = off (default):
// measured at limit 14, grouping only helps while every subtier is gated, and without
// gates it is no faster (default backend level) or slightly slower (aggressive).
static cl::opt<unsigned> SubtierGroupingByMinID(
    "subtier-grouping-by-min-ID",
    cl::desc("Key the subtiers of bit-transformation kernels of merge id bit tiers >= N "
             "by the high bits of the smaller part id (0 = off, the default)."),
    cl::init(0));

static bool subtierGroupingApplies(unsigned tierBits) {
    return SubtierGroupingByMinID > 0 && tierBits >= SubtierGroupingByMinID;
}

// --subtier-gating-bits=K: a subtier gets an if-gate only when its key shows that the
// keyed part (idA, or the smaller part under --subtier-grouping-by-min-ID) has at least K
// bits; other subtiers are computed unconditionally in the kernel's top scope. A gate can
// only skip a block where its key is absent from the whole block, which in practice needs
// large ids (see --bit-frequency-statistics). 0 = gate every subtier; the default 17 gates
// none, which measured fastest (tiers 12-14 10-25% cheaper than with every subtier gated).
static cl::opt<unsigned> SubtierGatingBits(
    "subtier-gating-bits",
    cl::desc("Gate a subtier of bit-transformation kernels with an if only when its keyed "
             "part id has at least K bits (0 = gate every subtier; default 17 = no gates)."),
    cl::init(17));

// --self-merge-block: resolve a kernel's self-merges (X+X -> XX) together in one gated
// block instead of per rule (see emitSelfMergeBlock).
static cl::opt<bool> SelfMergeBlock(
    "self-merge-block",
    cl::desc("BPE merge kernels: resolve all self-merges of a kernel in one block gated "
             "by an any-self-merge test, stamping by bit transformation."),
    cl::init(false));

static cl::opt<bool> KeyCluster(
    "key-cluster",
    cl::desc("With --partition=level/asymmetric/chain and --if-test-significant-bits: move rules (only later, only "
             "inside their compaction segment) so rules sharing a gate key share a kernel."),
    cl::init(false));

static cl::opt<unsigned> KeyClusterCap(
    "key-cluster-cap",
    cl::desc("With --key-cluster: never move a rule into a kernel already holding this many "
             "rules (0 = no cap). Default: the --max-merges-per-kernel value."),
    cl::init(0));

// Key-cluster's move cap: --key-cluster-cap when given, else --max-merges-per-kernel, so
// clustering never builds a kernel that splitOversizedGroups would cut up again.
static unsigned effKeyClusterCap() {
    return KeyClusterCap.getNumOccurrences() > 0 ? (unsigned) KeyClusterCap : (unsigned) MaxMergesPerKernel;
}

// Effective count-based compaction interval: BPE_COMPACT_EVERY overrides --compact-base.
static unsigned effCompactEvery() {
    if (const char * ce = std::getenv("BPE_COMPACT_EVERY")) return (unsigned) std::atoi(ce);
    return CompactionBase;
}

static const char * compactionName(CompactionMode m) {
    switch (m) {
        case CompactionMode::Linear:       return "linear";
        case CompactionMode::Geometric:    return "geometric";
        case CompactionMode::ByOutputBits: return "by-output-bits";
        default:                           return "none";
    }
}

// Effective --compaction mode, after folding in the legacy --geometric-compaction and the
// --compact-base default. Conflicting or incomplete settings halt immediately.
static CompactionMode compactionMode() {
    static const CompactionMode mode = [] {
        auto halt = [](const std::string & msg) {
            std::cerr << "tokenizer: compaction settings conflict: " << msg << "\n";
            std::exit(1);
        };
        const bool given = Compaction.getNumOccurrences() > 0;
        CompactionMode m = Compaction;
        if (GeometricCompaction) {
            if (given && m != CompactionMode::Geometric)
                halt(std::string("--geometric-compaction contradicts --compaction=") + compactionName(m));
            m = CompactionMode::Geometric;
        } else if (!given) {
            m = effCompactEvery() > 0 ? CompactionMode::Linear : CompactionMode::None;
        }
        if ((m == CompactionMode::Linear || m == CompactionMode::Geometric) && effCompactEvery() == 0)
            halt(std::string(GeometricCompaction ? "--geometric-compaction" : "--compaction=")
                 + (GeometricCompaction ? "" : compactionName(m))
                 + " needs --compact-base=N with N > 0 (the compaction interval)");
        return m;
    }();
    return mode;
}

// The count-based schedule (--compaction=linear/geometric), stepped one kernel at a time.
// applyCompactionSchedule and keyCluster both use it, so key-cluster's segments are the
// real compaction segments. step() returns true when this kernel ends a segment; it never
// does under by-output-bits or no compaction.
struct CompactionCounter {
    unsigned next, since = 0;
    CompactionCounter()
    : next(compactionMode() == CompactionMode::Linear || compactionMode() == CompactionMode::Geometric
           ? effCompactEvery() : 0) {}
    bool step() {
        if (next == 0 || ++since < next) return false;
        since = 0;
        if (compactionMode() == CompactionMode::Geometric) next *= 2;
        return true;
    }
};

// Where one levelPartition call sits in the whole pipeline: the global index of its first
// kernel and the compaction count on entry. --partition=bit-tier partitions each
// bit tier separately, so key-cluster needs these to see global kernel indices
// (--if-group-lower-limit) and the real compaction points. The defaults describe a
// partition of the whole rule list.
struct PartitionContext {
    unsigned kernelOffset = 0;
    CompactionCounter counter;
};

// Gate key of idA under --if-test-significant-bits=K: the K bits from idA's highest 1 bit,
// tagged with the count of low bits below them. Rules with equal keys share a grouped-if gate.
static uint64_t gateKeyOf(unsigned idA) {
    const unsigned K = IfTestSignificantBits;
    const unsigned b = idA ? 32u - __builtin_clz(idA) : 0;
    const unsigned lo = (b > K) ? b - K : 0;
    return (uint64_t(lo) << 32) | (idA >> lo);
}



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
// GPT-2's base id of a byte (bytes_to_unicode order, T1): piecewise byte + offset, a
// bijection over 0..255.
static unsigned bpeBaseId(unsigned by) {
    if (by <= 32)  return by + 188;
    if (by <= 126) return by - 33;
    if (by <= 160) return by + 94;
    if (by <= 172) return by - 67;
    if (by == 173) return 255;
    return by - 68;                        // 174..255
}

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
        auto idOf = bpeBaseId;
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

// BPEWidenKernel — copy an N-bit id stream into a wider one, zero-filling the high
// bits. The emitter (P2S16Kernel) reads exactly 16 streams, but the final id stream
// is only ceil_log2(hi) wide — fewer than 16 under --merges-limit — and reading a
// narrower stream set as 16 elements misaligns every block.
class BPEWidenKernel : public PabloKernel {
public:
    BPEWidenKernel(LLVMTypeSystemInterface & ts, StreamSet * idIn, StreamSet * idOut)
    : PabloKernel(ts, "BPE_Widen" + std::to_string(idIn->getNumElements())
                        + "to" + std::to_string(idOut->getNumElements()),
                  {Binding{"idIn", idIn}},
                  {Binding{"idOut", idOut}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST*> in = getInputStreamSet("idIn");
        Var * out = getOutputStreamVar("idOut");
        for (unsigned i = 0; i < out->getType()->getArrayNumElements(); i++)
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)),
                            i < in.size() ? in[i] : pb.createZeroes());
    }
};

// --retire-words kernels. The merge-back carries each frame's tokens as one 17-stream
// set: the 16 id bits plus bit 16, the token-start mask the emitter scans. Under --bpe-tail
// an 18th stream (bit 17) marks the segment starts for the emitter's BPE; every retired
// token is final, so it is one by itself.
static constexpr unsigned kRetireStreams = 17;
static constexpr unsigned kTailStreams = 18;

// A retirement point works in two frames: the one before the point's FilterByMask (Fp:
// live token starts marked by inPlayMask, consumed positions in between) and the compacted
// one (Fc: every position a live token start). Only the 1-bit streams visit Fc. The ids
// are filtered once, from Fp, by the combined mask, and the merge-back reads the retired
// ids straight from Fp.

// --retire-frozen, in Fp: frozen = live token starts whose id no later rule uses as a part,
// so they can never merge again. segStarts = word starts plus the live start right after
// each frozen token (the barrier), found forward through the consumed positions in between.
// testSet / negate: the frozen ids, or (negate) their complement when it has fewer intervals.
class BPEFrozenKernel : public PabloKernel {
public:
    BPEFrozenKernel(LLVMTypeSystemInterface & ts, StreamSet * idIn, StreamSet * liveIn,
                    StreamSet * boundaryIn, const UCD::UnicodeSet & testSet, bool negate,
                    uint64_t setHash, StreamSet * frozen, StreamSet * segStarts)
    : PabloKernel(ts, "BPE_Frozen" + std::to_string(setHash) + (negate ? "n" : "")
                        + "_w" + std::to_string(idIn->getNumElements()),
                  {Binding{"idIn", idIn}, Binding{"liveIn", liveIn}, Binding{"boundaryIn", boundaryIn}},
                  {Binding{"frozen", frozen}, Binding{"segStarts", segStarts}}),
      mTestSet(testSet), mNegate(negate) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        const std::vector<PabloAST *> idBits = getInputStreamSet("idIn");
        PabloAST * const live = getInputStreamSet("liveIn")[0];
        cc::Parabix_CC_Compiler ccc(idBits);
        PabloAST * inSet = ccc.compileCC("retireSet", re::makeCC(UCD::UnicodeSet(mTestSet)), pb);
        PabloAST * fz = pb.createInFile(pb.createAnd(live, mNegate ? pb.createNot(inSet) : inSet), "frozen");
        PabloAST * afterFrozen = pb.createAnd(pb.createMatchStar(pb.createAdvance(fz, (int64_t) 1),
                                                                 pb.createNot(live)), live);
        pb.createAssign(pb.createExtract(getOutputStreamVar("frozen"), pb.getInteger(0)), fz);
        pb.createAssign(pb.createExtract(getOutputStreamVar("segStarts"), pb.getInteger(0)),
                        pb.createOr(getInputStreamSet("boundaryIn")[0], afterFrozen));
    }
private:
    const UCD::UnicodeSet mTestSet;
    const bool mNegate;
};

// In Fc: finished = the token starts a segment and so does the next one, i.e. it is a whole
// segment. A segment is a word, cut further under --retire-frozen: a frozen token is
// finished too, and the token after it starts a segment. (That barrier also goes into the
// boundary stream of the kept frame, from BPEFrozenKernel's segStarts, so the merge kernels
// never join the tokens on both sides of a dropped frozen token.) keep = the rest of the
// file. Past the last token the lookahead reads 0, so the last segment is never retired
// unless frozen.
class BPERetireKernel : public PabloKernel {
public:
    BPERetireKernel(LLVMTypeSystemInterface & ts, StreamSet * boundaryIn, StreamSet * frozenIn,
                    StreamSet * keep)
    : PabloKernel(ts, std::string("BPE_Retire") + (frozenIn ? "_f" : ""),
                  frozenIn ? std::vector<Binding>{Binding{"boundaryIn", boundaryIn, FixedRate(), LookAhead(1)},
                                                  Binding{"frozenIn", frozenIn}}
                           : std::vector<Binding>{Binding{"boundaryIn", boundaryIn, FixedRate(), LookAhead(1)}},
                  {Binding{"keep", keep}}),
      mHasFrozen(frozenIn != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        PabloAST * const wordStart = getInputStreamSet("boundaryIn")[0];
        PabloAST * segStart = wordStart;
        PabloAST * frozen = nullptr;
        if (mHasFrozen) {
            frozen = getInputStreamSet("frozenIn")[0];
            segStart = pb.createOr(wordStart, pb.createAdvance(frozen, (int64_t) 1), "segStart");
        }
        // next token starts a segment = it starts a word, or this token is frozen
        PabloAST * fin = pb.createAnd(segStart, pb.createLookahead(wordStart, (int64_t) 1));
        if (frozen) fin = pb.createOr(fin, frozen);
        PabloAST * keep = pb.createInFile(pb.createNot(fin), "keep");
        pb.createAssign(pb.createExtract(getOutputStreamVar("keep"), pb.getInteger(0)), keep);
    }
private:
    const bool mHasFrozen;
};

// --retire-stuck tables, one per stuck point, built with the pipeline; BPEStuckKernel bakes
// its point's index into its call.
//   frozen: one bit per id, set when no rule of a later kernel uses the id as a part
//   pairs:  a one-hash bloom filter over idA << 16 | idB of every rule of a later kernel.
//           A false positive only keeps a segment; the lookup is one load, no branch
//           (an exact probing hash set made the kernel ~1 cycle/position slower).
static constexpr unsigned kStuckBloomBits = 21;   // 256 KiB; <= 50k pairs -> <= 2.4% false hits
struct StuckTables {
    std::vector<uint64_t> frozen;
    std::vector<uint64_t> pairs;
};
static std::vector<StuckTables> gStuckTables;

static inline uint32_t stuckHash(uint32_t key) { return (key * 0x9E3779B1u) >> (32 - kStuckBloomBits); }

static inline bool stuckPairLive(const StuckTables & t, uint32_t key) {
    const uint32_t h = stuckHash(key);
    return (t.pairs[h >> 6] >> (h & 63)) & 1;
}

// BPEStuckKernel's body, on numItems positions of Fp (whole blocks; the first n are input).
//   ids: one 16-bit id per position (P2S16); live: token starts; seg: segment starts (word
//   starts and barriers). kept: the live starts that stay; segOut: the segment starts of the
//   kept frame (word starts, barriers, and the start after each frozen token).
// Pass 1 walks the tokens: frozen bit F, segment start S (seg, or the previous token was
// frozen), and L = the pair (previous token, this token) may be a later rule. Pass 2 is
// bit-parallel over the words: a token stays if its segment has an L, i.e. an L lies
// before it in the segment (forward MatchStar by long addition through non-barrier
// positions) or at/after it (the same on bit-reversed words, word order reversed).
//   state[0]: bit 0 = the forward fill runs past the call (the open segment has an L),
//   bit 1 = the last token was frozen, bit 2 = a token was seen; state[1] = its id.
// Past the end of a non-final call the backward fill starts with a carry, so the segment
// still open there is kept (it may continue). Its later tokens still retire if it turns out
// stuck: no merge can reach the kept head, as its pairs are not rules and the next kept
// token starts a segment.
extern "C" void bpe_retire_stuck(uint64_t slot, const uint16_t * ids, const uint64_t * live,
                                 const uint64_t * seg, uint64_t * kept, uint64_t * segOut,
                                 uint64_t numItems, uint64_t n, uint64_t final, uint64_t * state) {
    const StuckTables & t = gStuckTables[slot];
    const uint64_t numWords = numItems / 64;
    thread_local std::vector<uint64_t> tmp;
    tmp.resize(3 * numWords);
    uint64_t * const frozenW = tmp.data(), * const barrierW = frozenW + numWords,
             * const fwdW = barrierW + numWords;
    bool prevFrozen = state[0] & 2, seen = state[0] & 4;
    uint32_t prevId = (uint32_t) state[1];
    for (uint64_t wi = 0; wi < numWords; wi++) {
        uint64_t lv = wi * 64 < n ? live[wi] : 0, beyond = 0;
        if (wi * 64 < n && n - wi * 64 < 64) {
            beyond = ~0ull << (n - wi * 64);
            lv &= ~beyond;
        } else if (wi * 64 >= n) {
            beyond = ~0ull;
        }
        uint64_t L = 0, F = 0, AF = 0;
        for (uint64_t m = lv; m; m &= m - 1) {
            const unsigned p = __builtin_ctzll(m);
            const uint32_t id = ids[wi * 64 + p];
            const uint64_t fz = (t.frozen[id >> 6] >> (id & 63)) & 1;
            L |= uint64_t(stuckPairLive(t, (prevId << 16) | id)) << p;
            AF |= uint64_t(prevFrozen | !seen) << p;
            F |= fz << p;
            prevId = id; prevFrozen = fz; seen = true;
        }
        const uint64_t S = ((wi * 64 < n ? seg[wi] : 0) & lv) | AF;
        const uint64_t barrier = S | F | (final ? beyond : 0);
        kept[wi] = L & ~barrier;
        segOut[wi] = S & ~F;
        frozenW[wi] = F | ~lv;          // not a kept candidate: frozen, or no token here
        barrierW[wi] = barrier;
    }
    // forward: fwd = MatchStar(L, ~barrier), minus the barrier it stops on
    uint64_t carry = state[0] & 1;
    for (uint64_t wi = 0; wi < numWords; wi++) {
        const uint64_t C = ~barrierW[wi], L = kept[wi];
        uint64_t s1, s2;
        const bool c1 = __builtin_add_overflow(L, C, &s1);
        const bool c2 = __builtin_add_overflow(s1, carry, &s2);
        carry = c1 | c2;
        fwdW[wi] = ((s2 ^ C) | L) & C;
    }
    const uint64_t fwdOut = carry;
    // backward: the same on reversed bits, keeping the segment start it stops on
    carry = final ? 0 : 1;
    for (uint64_t wi = numWords; wi-- > 0; ) {
        const uint64_t C = llvm::reverseBits<uint64_t>(~barrierW[wi]), L = llvm::reverseBits<uint64_t>(kept[wi]);
        uint64_t s1, s2;
        const bool c1 = __builtin_add_overflow(L, C, &s1);
        const bool c2 = __builtin_add_overflow(s1, carry, &s2);
        carry = c1 | c2;
        const uint64_t bwd = llvm::reverseBits<uint64_t>((s2 ^ C) | L);
        const uint64_t keep = (fwdW[wi] | bwd) & ~frozenW[wi];
        kept[wi] = keep;
        segOut[wi] &= keep;
    }
    state[0] = fwdOut | (prevFrozen ? 2 : 0) | (seen ? 4 : 0);
    state[1] = prevId;
}

// --retire-stuck, in Fp: one call of bpe_retire_stuck per invocation; it writes keptPre and
// the kept frame's segment starts (still in Fp) directly, so nothing visits Fc. ids16 is the
// id stream packed by P2S16Kernel, one i16 per position.
class BPEStuckKernel final : public MultiBlockKernel {
public:
    BPEStuckKernel(LLVMTypeSystemInterface & ts, StreamSet * ids, StreamSet * live, StreamSet * seg,
                   StreamSet * kept, StreamSet * segOut, unsigned slot, uint64_t tableHash)
    : MultiBlockKernel(ts, "BPE_Stuck16_s" + std::to_string(slot) + "_h" + std::to_string(tableHash),
                       {Binding{"ids", ids}, Binding{"live", live}, Binding{"seg", seg}},
                       {Binding{"kept", kept}, Binding{"segOut", segOut}},
                       {}, {},
                       {InternalScalar{ArrayType::get(ts.getInt64Ty(), 2), "state"}}),
      mSlot(slot) {}
protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        Function * const fn = b.getModule()->getFunction("bpe_retire_stuck");
        if (fn == nullptr) report_fatal_error("BPE_Stuck: bpe_retire_stuck is not linked");
        Value * const zero = b.getInt32(0);
        Value * const numItems = b.CreateMul(numOfStrides, b.getSize(getStride()));
        Value * const n = b.CreateUMin(numItems, b.getAccessibleItemCount("live"));
        Value * const final = b.CreateZExt(b.isFinal(), b.getInt64Ty());
        b.CreateCall(fn->getFunctionType(), fn,
                     {b.getInt64(mSlot), b.getInputStreamBlockPtr("ids", zero),
                      b.getInputStreamBlockPtr("live", zero), b.getInputStreamBlockPtr("seg", zero),
                      b.getOutputStreamBlockPtr("kept", zero), b.getOutputStreamBlockPtr("segOut", zero),
                      numItems, n, final, b.getScalarFieldPtr("state").first});
    }
private:
    const unsigned mSlot;
};

// --bpe-tail runtime. gTail: open addressing over every rule of a kernel the pipeline did not
// build, one 64-bit slot each: (idA << 16 | idB) << 16 | idAB, all ones = empty (idAB = 256
// + the merge's rank, so the lower id is the higher priority). Load <= 1/4, so a pair that
// is no rule, the common case, mostly ends at its first slot. (A table grouped by idA with a
// binary search was 40% slower: one left part such as U+0120 starts thousands of rules.)
// left / right: one bit per id, set when the id is the left / right part of some rule in
// the table; a pair failing either (most non-rules) never touches the 2 MB table.
// pair (--tail-pair-table): the rule of every pair of byte ids (both < 256) in a direct
// 256 x 256 table, 0xFFFF for none, 128 KB. A pretoken's first round of pairs is all byte
// pairs: 47% of the lookups of a cache miss on val_4MB and webtext_cases (7 of 15).
struct TailTable {
    std::vector<uint64_t> slots;
    uint32_t mask = 0, shift = 0;
    unsigned idBits = 16;                // the width of the ids reaching the tail
    uint64_t left[1024], right[1024];
    std::vector<uint16_t> pair;
    bool usePair = false;
};
static constexpr uint64_t kTailEmpty = ~0ull;
static TailTable gTail;
static cl::opt<bool> TailPairTable(
    "tail-pair-table",
    cl::desc("With --bpe-tail or --tail-from-bytes: look up rules of two byte ids in a direct "
             "256 x 256 table instead of the hash table (default off)."),
    cl::init(false));
static BPETailSink gTailSink = nullptr;

void bpeTailSetSink(BPETailSink sink) { gTailSink = sink; }

static inline uint32_t tailSlot(uint32_t key) { return (key * 0x9E3779B1u) >> gTail.shift; }

static inline uint16_t tailMerge(uint16_t a, uint16_t b) {
    if (gTail.usePair && (a | b) < 256) return gTail.pair[(uint32_t(a) << 8) | b];
    if (!((gTail.left[a >> 6] >> (a & 63)) & (gTail.right[b >> 6] >> (b & 63)) & 1)) return 0xFFFF;
    const uint32_t key = (uint32_t(a) << 16) | b;
    for (uint32_t h = tailSlot(key); ; h = (h + 1) & gTail.mask) {
        const uint64_t e = gTail.slots[h];
        if (e == kTailEmpty) return 0xFFFF;
        if (uint32_t(e >> 16) == key) return uint16_t(e);
    }
}

// Standard BPE on one segment, in place: merge the lowest-ranked adjacent pair (leftmost on
// a tie, as HF and tiktoken do) until no pair is a rule. t[0..n) are the ids and pos[0..n)
// their positions; both lose the merged-away tokens, and n is updated. Segments of up to 64
// tokens (nearly all) shift in plain arrays. Longer ones (one word of 100k tokens, e.g. a
// run of one letter) use HF's merge_all: a linked list and a min-heap of (id, position)
// pairs, skipping a popped pair whose tokens changed since; O(n log n). A long segment in
// which no pair is a rule (a run of NULs, say) returns before building any of that.
static void tailMergeLong(uint16_t * t, uint32_t * pos, size_t & n) {
    size_t first = 0;
    while (first + 1 < n && tailMerge(t[first], t[first + 1]) == 0xFFFF) first++;
    if (first + 1 >= n) return;
    thread_local std::vector<uint32_t> prev, next;
    thread_local std::vector<uint8_t> gone;
    using Item = std::pair<uint32_t, uint32_t>;          // (idAB, left position)
    thread_local std::vector<Item> heap;
    prev.resize(n); next.resize(n);
    gone.assign(n, 0);
    heap.clear();
    for (size_t i = 0; i < n; i++) { prev[i] = uint32_t(i - 1); next[i] = uint32_t(i + 1); }
    auto push = [&](uint32_t i) {
        if (i >= n || next[i] >= n) return;
        const uint16_t m = tailMerge(t[i], t[next[i]]);
        if (m != 0xFFFF) { heap.emplace_back(m, i); std::push_heap(heap.begin(), heap.end(), std::greater<Item>()); }
    };
    for (uint32_t i = uint32_t(first); i + 1 < n; i++) push(i);
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<Item>());
        const auto [m, i] = heap.back();
        heap.pop_back();
        if (gone[i] || next[i] >= n || tailMerge(t[i], t[next[i]]) != m) continue;   // stale
        const uint32_t j = next[i];
        t[i] = uint16_t(m);
        gone[j] = 1;
        next[i] = next[j];
        if (next[j] < n) prev[next[j]] = i;
        if (prev[i] < n) push(prev[i]);
        push(i);
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i++) if (!gone[i]) { t[w] = t[i]; pos[w] = pos[i]; w++; }
    n = w;
}

static inline void tailMergeSegment(uint16_t * t, uint32_t * pos, size_t & n) {
    if (n < 2) return;
    if (n > 64) { tailMergeLong(t, pos, n); return; }
    uint16_t r[64];
    size_t m = n - 1;                    // pairs
    for (size_t i = 0; i < m; i++) r[i] = tailMerge(t[i], t[i + 1]);
    for (;;) {
        size_t best = 0;
        for (size_t i = 1; i < m; i++) if (r[i] < r[best]) best = i;
        if (m == 0 || r[best] == 0xFFFF) break;
        t[best] = r[best];
        for (size_t i = best + 1; i + 1 < n; i++) { t[i] = t[i + 1]; pos[i] = pos[i + 1]; }
        for (size_t i = best; i + 1 < m; i++) r[i] = r[i + 1];
        n--; m--;
        if (best > 0) r[best - 1] = tailMerge(t[best - 1], t[best]);
        if (best < m) r[best] = tailMerge(t[best], t[best + 1]);
    }
}

// A growable scratch array of segment tokens: ids and positions.
struct TailSegBuf {
    std::vector<uint16_t> t;
    std::vector<uint32_t> pos;
    size_t n = 0;
    void push(uint16_t id, uint32_t p) {
        if (n == t.size()) { t.resize(std::max<size_t>(256, 2 * n)); pos.resize(t.size()); }
        t[n] = id; pos[n] = p; n++;
    }
};

// Per-thread cache of finished segments: words repeat, so most segments are a lookup instead
// of a run of merges (HF's BPE keeps a word cache too). Key (TailKey<B>, B = gTail.idBits):
// k0 holds input ids 0 .. 64/B - 1, k1 the next 60/B ids and the count in its top 4 bits, so
// with 9-bit ids (--bpe-tail=1) segments of up to 13 tokens fit. Value: which input tokens
// start an output token (bit i = token i) and the output ids; a result of more than
// kTailCacheOut tokens is not cached. Two ways of 32 bytes per 64-byte set; a miss moves
// way 0 to way 1 and fills way 0.
struct TailCacheEntry {
    uint64_t k0, k1;                     // k0 == k1 == 0: empty (a key's count is never 0)
    uint16_t keep;
    uint16_t out[7];
};
// --tail-cache-bits=N: 2^N sets x 64 bytes per thread (default 15: 2 MB).
static cl::opt<unsigned> TailCacheBits(
    "tail-cache-bits",
    cl::desc("With --bpe-tail: the tail's per-thread segment cache has 2^N sets of 64 bytes "
             "(default 15, 2 MB)."),
    cl::init(15));
// --tail-cache-ways=4 (with --tail-from-bytes): 4 entries per set instead of 2, 128 bytes, one
// cache line on Apple M-series, and a hit moves its entry to the front (LRU) where the 2-way
// cache keeps insertion order (a miss pushes out the older entry even if it was just hit).
// Simulated on val_4MB, 2 MB per thread: 1 thread warm 28k -> 16k misses, cold 60k -> 53k;
// 4 threads warm 14k -> 3k. Runtime only, no cache tag.
static cl::opt<unsigned> TailCacheWays(
    "tail-cache-ways",
    cl::desc("With --tail-from-bytes: entries per tail cache set, 2 (default, insertion order) "
             "or 4 (least recently used out)."),
    cl::init(2));
// --tail-shared-cache-bits=N (with --tail-from-bytes): a second-level cache of 2^N sets x 4
// entries (128 bytes each) shared by all threads, looked up when a thread's own cache misses.
// Each thread's cache otherwise learns every word itself: on val_4MB at 4 threads a cold run
// has 89k misses against 52k at 1 thread. Entries are written once per run and never
// replaced, so readers need no lock: a writer claims an empty entry (k1 0 -> busy by CAS),
// fills it and publishes its k1 with a release store; a reader reads an entry only after an
// acquire load of a published k1. A full set takes no more words. 0 = off (default).
static cl::opt<unsigned> TailSharedCacheBits(
    "tail-shared-cache-bits",
    cl::desc("With --tail-from-bytes: a tail cache of 2^N 128-byte sets shared by all threads, "
             "behind each thread's own (0 = off, the default)."),
    cl::init(0));
// --tail-long-cache (with --tail-from-bytes): a per-thread cache for the pretokens too long
// for the segment cache's 15-byte key (kTailByteKeyMax+1 .. kTailLongMax bytes), which
// otherwise merge on every occurrence. They are 0.5-4% of pretokens but 11-21% of the bytes of
// val_4MB, code_4MB and multi_4MB (NUL runs, indentation, long words), and most repeat: on
// code_4MB 15% of their bytes are in distinct ones. Runtime only, no cache tag.
static cl::opt<bool> TailLongCache(
    "tail-long-cache",
    cl::desc("With --tail-from-bytes: cache the BPE of pretokens over 15 bytes (default off)."),
    cl::init(false));
static constexpr unsigned kTailLongMax = 4096;       // longer pretokens always merge
static constexpr unsigned kTailLongIndexBits = 14;   // 16K slots of 16 bytes
static constexpr size_t kTailLongArena = size_t(4) << 20;
// An entry takes at most 4 + kTailLongMax bytes + 4 per token (one per byte), so a cleared
// arena always holds the entry that filled it.
static_assert(4 + kTailLongMax + 4 * kTailLongMax <= kTailLongArena, "a long cache entry must fit the arena");
// A slot of the long cache's index: the pretoken's hash, length and arena offset (len 0: empty).
struct TailLongSlot {
    uint64_t h;
    uint32_t len, off;
};
static constexpr unsigned kTailCacheOut = 7;
static std::atomic<uint64_t> gTailGeneration{0};   // bumped whenever gTail is rebuilt

template <unsigned B> struct TailKey {
    static constexpr unsigned per0 = 64 / B, per1 = 60 / B;
    static constexpr unsigned max = per0 + per1;               // cached segments: 2 .. max tokens
    static constexpr unsigned fast = max < 8 ? max : 8;        // gathered without a loop
};

// The resolve kernel's per-thread state, fetched once per call: on macOS every access to a
// thread_local with a constructor is a _tlv_get_addr call.
struct TailScratch {
    std::vector<uint16_t> t;             // an uncached segment: ids ...
    std::vector<uint32_t> pos;           // ... and positions
    std::vector<TailCacheEntry> cache;
    TailCacheEntry * sets = nullptr;     // set 0 (cache.data(), or 128-aligned for 4 ways)
    uint64_t generation = 0;
    unsigned setBits = 0;                // log2 of the cache's sets
    unsigned ways = 0;
    // --tail-long-cache: an open-addressed index (load <= 1/2) into an arena holding, per entry,
    // the pretoken's bytes and then its tokens as id | offset in the pretoken << 16. Both are
    // cleared when either fills.
    std::vector<TailLongSlot> longIndex;
    std::vector<uint8_t> longArena;
    size_t longUsed = 0, longCount = 0;
};
// --tail-cache-pool: scratches not in use; a thread leases one and returns it as it exits.
static std::mutex gTailPoolMutex;
static std::vector<std::unique_ptr<TailScratch>> gTailPool;
struct TailScratchLease {
    TailScratch * s = nullptr;
    ~TailScratchLease() {
        if (s == nullptr) return;
        std::lock_guard<std::mutex> lock(gTailPoolMutex);
        gTailPool.emplace_back(s);
    }
};
static TailScratch & tailScratch() {
    TailScratch * sp;
    if (TailCachePool) {
        thread_local TailScratchLease lease;
        if (lease.s == nullptr) {
            std::lock_guard<std::mutex> lock(gTailPoolMutex);
            if (gTailPool.empty()) lease.s = new TailScratch;
            else { lease.s = gTailPool.back().release(); gTailPool.pop_back(); }
        }
        sp = lease.s;
    } else {
        thread_local TailScratch s;
        sp = &s;
    }
    const uint64_t g = gTailGeneration.load(std::memory_order_relaxed);
    const unsigned ways = TailFromBytes && TailCacheWays == 4 ? 4 : 2;
    if (sp->generation != g || sp->setBits != TailCacheBits || sp->ways != ways) {
        sp->setBits = std::min<unsigned>(std::max<unsigned>(TailCacheBits, 4), 24);
        sp->ways = ways;
        if (ways == 2) {
            sp->cache.assign(size_t(2) << sp->setBits, TailCacheEntry{});
            sp->sets = sp->cache.data();
        } else {   // 4 extra entries so that set 0 can start on a 128-byte line
            sp->cache.assign((size_t(ways) << sp->setBits) + 4, TailCacheEntry{});
            sp->sets = reinterpret_cast<TailCacheEntry *>(
                (reinterpret_cast<uintptr_t>(sp->cache.data()) + 127) & ~uintptr_t(127));
        }
        if (TailLongCache) {
            sp->longIndex.assign(size_t(1) << kTailLongIndexBits, TailLongSlot{0, 0, 0});
            sp->longArena.resize(kTailLongArena);
            sp->longUsed = sp->longCount = 0;
        }
        sp->generation = g;
    }
    return *sp;
}

// --tail-shared-cache-bits: the shared cache (anonymous memory, so it starts zeroed and its
// pages arrive as they are first written). Reset only while no pipeline runs. A reset of the
// same size clears it in place, as the threads' own caches are cleared under
// --bench-cold-cache (a one-shot run pays page faults for both).
static TailCacheEntry * gTailShared = nullptr;
static size_t gTailSharedBytes = 0;
static unsigned gTailSharedShift = 64;
static constexpr uint64_t kTailSharedBusy = ~0ull;   // a k1 no key has: its length byte is <= 15

static void tailSharedReset() {
    // one thread shares with no one: the second lookup would only add a miss (+5% measured)
    const bool on = TailSharedCacheBits && codegen::SegmentThreads > 1;
    const size_t bytes = on ? size_t(128) << std::min<unsigned>(TailSharedCacheBits, 24) : 0;
    if (gTailShared && bytes == gTailSharedBytes) {
        std::memset(gTailShared, 0, bytes);
        return;
    }
    if (gTailShared) munmap(gTailShared, gTailSharedBytes);
    gTailShared = nullptr;
    gTailSharedBytes = bytes;
    if (bytes == 0) return;
    void * p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) report_fatal_error("--tail-shared-cache-bits: mmap failed");
    gTailShared = static_cast<TailCacheEntry *>(p);
    gTailSharedShift = 64 - std::min<unsigned>(TailSharedCacheBits, 24);
}

void bpeDropTailCaches() {
    gTailGeneration++;
    if (gTailShared) tailSharedReset();
}

static inline void tailMark(uint64_t * emit, uint32_t p) { emit[p / 64] |= 1ull << (p % 64); }

// --tail-emit-from-segs (with --tail-from-bytes): every pretoken's first token starts at the
// pretoken's start, so the byte resolve loop starts its emit stream as a copy of the pretoken
// start bits (64 positions per word) and marks only the later tokens of multi-token pretokens,
// instead of one read-modify-write of emit per pretoken. Runtime only, no cache tag.
static cl::opt<bool> TailEmitFromSegs(
    "tail-emit-from-segs",
    cl::desc("With --tail-from-bytes: start the emit stream from the pretoken start bits and mark "
             "only later tokens (default off)."),
    cl::init(false));
// tailMark for the j-th token of a pretoken (j = 0: the pretoken's start).
static inline void tailMarkTok(uint64_t * emit, uint32_t p, size_t j) {
    if (j != 0 || !TailEmitFromSegs) tailMark(emit, p);
}
static inline uint64_t tailLowMask(size_t k) { return k >= 64 ? ~0ull : (1ull << k) - 1; }

// A cached segment on its way through the resolve loop: its tokens (positions, ids), its key
// and its cache set, whose line was prefetched when the descriptor was made.
struct TailDesc {
    uint64_t k0, k1;
    TailCacheEntry * set;
    uint32_t cnt;
    uint32_t p[16];
    uint16_t t[16];
};

// Look d up (or merge it and fill the cache) and write its output ids at their positions.
static inline void tailFinishDesc(const TailDesc & d, uint32_t * res, uint64_t * emit) {
    TailCacheEntry * const set = d.set;
    const TailCacheEntry * e;
    if (LLVM_LIKELY(set[0].k0 == d.k0 && set[0].k1 == d.k1)) e = &set[0];
    else if (set[1].k0 == d.k0 && set[1].k1 == d.k1) e = &set[1];
    else {
        uint16_t tt[16];
        uint32_t idx[16];
        size_t m = d.cnt;
        for (size_t i = 0; i < m; i++) { tt[i] = d.t[i]; idx[i] = uint32_t(i); }
        tailMergeSegment(tt, idx, m);
        if (m > kTailCacheOut) {
            for (size_t j = 0; j < m; j++) { res[d.p[idx[j]]] = tt[j]; tailMark(emit, d.p[idx[j]]); }
            return;
        }
        set[1] = set[0];
        TailCacheEntry & f = set[0];
        f.k0 = d.k0; f.k1 = d.k1; f.keep = 0;
        for (size_t j = 0; j < m; j++) { f.keep |= uint16_t(1u << idx[j]); f.out[j] = tt[j]; }
        e = &f;
    }
    unsigned j = 0;
    for (uint32_t keep = e->keep; keep; keep &= keep - 1) {
        const uint32_t q = d.p[__builtin_ctz(keep)];
        res[q] = e->out[j++];
        tailMark(emit, q);
    }
}

// The tail runs in two kernels. BPETailResolveKernel is data-parallel: each call finishes
// every segment that lies wholly inside its own blocks and writes one word per token start:
// the finished id, or the input id with kTailOpen for a token of a segment cut by the call's
// edges (bit kTailSeg on the first token of such a segment); a bit in `emit` marks each of
// these, and not the tokens merged into their left neighbour. BPETailKernel then walks the
// marked words in order on one thread, emits, and finishes the cut segments, a couple per call.
static constexpr uint32_t kTailOpen = 1u << 16, kTailSeg = 1u << 18;

// The resolve loop walks the segment starts. A segment of one token is its own output; one
// of 2 .. TailKey<B>::max tokens becomes a TailDesc, whose cache set is prefetched and which
// is finished kTailAhead segments later, so the cache's memory latency overlaps the work on
// the segments in between (a lookup right away stalls on it: 30% slower); a longer segment
// is merged on the spot. A segment's tokens are the `starts` bits up to the next segment
// start; up to `fast` of them are gathered from one 64-bit window without a loop.
static constexpr unsigned kTailAhead = 4;

template <unsigned B>
static void tailResolve(const uint16_t * ids, const uint64_t * starts, const uint64_t * segs,
                        uint32_t * res, uint64_t * emit, uint64_t numItems, uint64_t n, uint64_t final) {
    using K = TailKey<B>;
    TailScratch & S = tailScratch();
    std::fill(emit, emit + numItems / 64, 0);
    if (n > numItems) n = numItems;
    const size_t nWords = (n + 63) / 64;
    if (nWords == 0) return;
    uint64_t mask0[K::max + 1], mask1[K::max + 1];   // the key bits of a segment of k tokens
    for (unsigned k = 0; k <= K::max; k++) {
        mask0[k] = tailLowMask(std::min(k, K::per0) * B);
        mask1[k] = k <= K::per0 ? 0 : tailLowMask((k - K::per0) * B);
    }
    TailCacheEntry * const cache = S.sets;
    const unsigned setShift = 64 - S.setBits;
    auto bitsIn = [&](size_t w, size_t lo, size_t hi) {      // starts bits of word w in [lo, hi)
        uint64_t x = starts[w];
        if (w * 64 < lo) x &= ~tailLowMask(lo - w * 64);
        if (w * 64 + 64 > hi) x &= tailLowMask(hi - w * 64);
        return x;
    };
    auto segWord = [&](size_t w) { return segs[w] & (w * 64 + 64 > n ? tailLowMask(n - w * 64) : ~0ull); };
    size_t sw = 0;
    uint64_t sx = segWord(0);
    while (sx == 0 && ++sw < nWords) sx = segWord(sw);
    const size_t first = sx ? sw * 64 + __builtin_ctzll(sx) : n;
    for (size_t w = 0; w * 64 < first; w++)                     // cut by the left edge
        for (uint64_t x = bitsIn(w, 0, first); x; x &= x - 1) {
            const uint32_t p = uint32_t(w * 64 + __builtin_ctzll(x));
            res[p] = ids[p] | kTailOpen;
            tailMark(emit, p);
        }
    if (!sx) return;
    sx &= sx - 1;
    TailDesc ring[kTailAhead];
    unsigned head = 0, count = 0;
    auto drain = [&] {
        for (; count; count--, head = (head + 1) % kTailAhead) tailFinishDesc(ring[head], res, emit);
    };
    for (size_t s = first;;) {
        while (sx == 0 && ++sw < nWords) sx = segWord(sw);
        const bool last = sx == 0;
        const size_t e = last ? n : sw * 64 + __builtin_ctzll(sx);
        if (last && !final) {                                   // may go on in the next call
            drain();
            uint32_t seg = kTailSeg;
            for (size_t w = s / 64; w * 64 < n; w++)
                for (uint64_t x = bitsIn(w, s, n); x; x &= x - 1) {
                    const uint32_t p = uint32_t(w * 64 + __builtin_ctzll(x));
                    res[p] = ids[p] | kTailOpen | seg;
                    seg = 0;
                    tailMark(emit, p);
                }
            return;
        }
        const size_t len = e - s, w = s / 64;
        const unsigned b = s % 64;
        const uint64_t next = w + 1 < nWords ? starts[w + 1] : 0;
        uint64_t m = ((starts[w] >> b) | ((next << 1) << (63 - b))) & tailLowMask(len);
        const unsigned cnt = len <= 64 ? __builtin_popcountll(m) : K::max + 1;
        if (cnt == 1) {
            res[s] = ids[s];
            tailMark(emit, uint32_t(s));
        } else if (cnt <= K::max) {
            if (count == kTailAhead) {
                tailFinishDesc(ring[head], res, emit);
                head = (head + 1) % kTailAhead;
                count--;
            }
            TailDesc & d = ring[(head + count++) % kTailAhead];
            if (LLVM_LIKELY(cnt <= K::fast)) {
                for (unsigned i = 0; i < K::fast; i++) {              // past cnt: the start again
                    const unsigned o = m ? __builtin_ctzll(m) : 0;
                    d.p[i] = uint32_t(s + o);
                    d.t[i] = ids[s + o];
                    m &= m - 1;
                }
            } else {
                for (unsigned i = 0; m; i++, m &= m - 1) {
                    d.p[i] = uint32_t(s + __builtin_ctzll(m));
                    d.t[i] = ids[d.p[i]];
                }
            }
            uint64_t k0 = 0, k1 = 0;
            for (unsigned i = 0; i < K::max; i++) {
                const uint64_t id = i < cnt ? d.t[i] : 0;
                if (i < K::per0) k0 |= id << (i * B);
                else k1 |= id << ((i - K::per0) * B);
            }
            d.k0 = k0 & mask0[cnt];
            d.k1 = (k1 & mask1[cnt]) | (uint64_t(cnt) << 60);
            d.cnt = cnt;
            const uint64_t h = (d.k0 ^ (d.k1 * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
            d.set = cache + (h >> setShift) * 2;
            __builtin_prefetch(d.set);
        } else {                                                // longer: merge it here
            if (S.t.size() < e - s) { S.t.resize(e - s); S.pos.resize(e - s); }
            size_t k = 0;
            for (size_t x = w; x * 64 < e; x++)
                for (uint64_t y = bitsIn(x, s, e); y; y &= y - 1) {
                    const uint32_t p = uint32_t(x * 64 + __builtin_ctzll(y));
                    S.t[k] = ids[p]; S.pos[k] = p; k++;
                }
            tailMergeSegment(S.t.data(), S.pos.data(), k);
            for (size_t i = 0; i < k; i++) { res[S.pos[i]] = S.t[i]; tailMark(emit, S.pos[i]); }
        }
        if (last) break;
        sx &= sx - 1;
        s = e;
    }
    drain();
}

// emit: one bit per token the emitter must visit (finished or cut), so it skips the
// merged-away ones without reading them.
extern "C" void bpe_tail_resolve(const uint16_t * ids, const uint64_t * starts, const uint64_t * segs,
                                 uint32_t * res, uint64_t * emit, uint64_t numItems, uint64_t n,
                                 uint64_t final) {
    switch (gTail.idBits) {
#define TAIL_CASE(B) case B: tailResolve<B>(ids, starts, segs, res, emit, numItems, n, final); return;
        TAIL_CASE(8) TAIL_CASE(9) TAIL_CASE(10) TAIL_CASE(11) TAIL_CASE(12)
        TAIL_CASE(13) TAIL_CASE(14) TAIL_CASE(15) TAIL_CASE(16)
#undef TAIL_CASE
        default: report_fatal_error("--bpe-tail: unsupported id width");
    }
}

// --tail-from-bytes: the tokens are the input bytes, as their GPT-2 base ids (T1).
static uint16_t gTailBase[256];
static constexpr unsigned kTailByteKeyMax = 15;   // cached pretokens: 2 .. 15 bytes

// --tail-scan-kernel (with --tail-from-bytes): ZTF's LengthGroupCompression pattern for the
// tail. The resolve loop is a Parabix two-level scan kernel (TwoLevelScanKernel) over the
// pretoken starts, with the previous start in a loop variable, that finishes the common
// pretokens in generated code: one byte (its base id) and a vocabulary-table hit of
// 2 .. kTailByteKeyMax bytes (key from two loads, hash, the set's 4 entries compared at once,
// one id stored). Only the rest (vocabulary misses, long pretokens, the call's edges) calls
// the C++ walk. The emit stream starts as the start bits (--tail-emit-from-segs).
// Kernel name: BPE_TailScanIR.
static cl::opt<bool> TailScanKernel(
    "tail-scan-kernel",
    cl::desc("With --tail-from-bytes: walk the pretoken starts in a two-level scan kernel that "
             "finishes one-byte pretokens and vocabulary-table hits in generated code (default off)."),
    cl::init(false));
// --tail-scan-ir: the scan kernel's generated hit path, now the only scan kernel. Kept so
// older command lines still parse; it does nothing.
static cl::opt<bool> TailScanIR(
    "tail-scan-ir", cl::desc("Retired: --tail-scan-kernel always finishes hits in generated code."),
    cl::init(false), cl::Hidden);
// --tail-scan-ir-cache (with --tail-scan-kernel): after a vocabulary miss the generated code
// also checks the first entry of the pretoken's set in the thread's cache (its most recent: a
// hit there moves nothing) and writes that entry's tokens. Kernel name tag "c<ways>".
static cl::opt<bool> TailScanIRCache(
    "tail-scan-ir-cache",
    cl::desc("With --tail-scan-kernel: also finish hits on the first entry of the thread cache's "
             "set in generated code (default off)."),
    cl::init(false));
// --tail-bixhash (with --tail-scan-kernel): a pretoken's sets come from the framework's
// bit-parallel kernel::BixHash over the BPE input (run = the bytes after a pretoken's first,
// 4 steps: exact up to 16 bytes), B bit streams. The scan kernel reads the hash at the
// pretoken's last byte by gathering that bit of each of the B streams into an integer (no
// P2S16), instead of multiplying the key. --tail-bixhash-bits=B sets B; the vocabulary table
// and each thread's cache then have 2^B sets (B=12: 512 KB each, 14: 2 MB, 16: 8 MB, 18: 32 MB).
// TailBixHashOf computes the same hash in C++ for the vocabulary table. Kernel name tag "_bh<B>".
static cl::opt<bool> TailBixHash(
    "tail-bixhash",
    cl::desc("With --tail-scan-kernel: hash pretokens with the bit-parallel BixHash kernel, read "
             "in the scan kernel (default off)."),
    cl::init(false));
static constexpr unsigned kBixHashMinBits = 8, kBixHashMaxBits = 24, kBixHashSteps = 4;
static cl::opt<unsigned> TailBixHashBits(
    "tail-bixhash-bits",
    cl::desc("With --tail-bixhash: hash bits, 8..24; the tail tables get 2^bits sets (default 14)."),
    cl::init(14));
// --tail-bixhash-control=1|2 (measurement only, hidden): the BixHash kernels run but the sets
// still come from the multiplied key (tables as without --tail-bixhash); 1: the scan kernel
// ignores the hash streams, 2: it also gathers the hash and discards it (ANDed with a runtime 0),
// 3: no BixHash; the scan kernel computes a second multiply hash and discards it the same way.
static cl::opt<unsigned> TailBixHashControl(
    "tail-bixhash-control", cl::desc("Measurement only: run BixHash without using it (1), and gather it (2)."),
    cl::init(0), cl::Hidden);
static unsigned bixHashBits() {
    return std::min(std::max<unsigned>(TailBixHashBits, kBixHashMinBits), kBixHashMaxBits);
}

// kernel::BixHash of one pretoken at its last byte, with the kernel's seed and shuffles (see
// lib/kernel/util/bixhash.cpp). Bit p of a mask is byte p of the pretoken, Advance(x, k) is
// x << k, and the select masks keep every bit inside the pretoken, as the kernel's do.
struct TailBixHashOf {
    unsigned bits;
    int init[kBixHashMaxBits];
    int mix[kBixHashSteps][kBixHashMaxBits];
    TailBixHashOf() : bits(bixHashBits()) {
        std::vector<int> bitmix(bits);
        std::iota(bitmix.begin(), bitmix.end(), 0);
        std::mt19937 rng(179321);                       // BixHash's default seed
        std::shuffle(bitmix.begin(), bitmix.end(), rng);
        std::copy(bitmix.begin(), bitmix.end(), init);
        for (unsigned j = 0; j < kBixHashSteps; j++) {
            std::shuffle(bitmix.begin(), bitmix.end(), rng);
            std::copy(bitmix.begin(), bitmix.end(), mix[j]);
        }
    }
    uint32_t operator()(const uint8_t * w, unsigned len) const {
        uint32_t basis[8] = {0}, h[kBixHashMaxBits];
        for (unsigned p = 0; p < len; p++)
            for (unsigned k = 0; k < 8; k++) basis[k] |= uint32_t((w[p] >> k) & 1) << p;
        for (unsigned i = 0; i < bits; i++) h[i] = basis[i % 8] ^ basis[init[i] % 8];
        uint32_t select = ((1u << len) - 1) & ~1u;      // run: every byte but the first
        for (unsigned j = 0; j < kBixHashSteps; j++) {
            for (unsigned i = 0; i < bits; i++) h[i] ^= select & (h[mix[j][i]] << (1u << j));
            select &= select << (1u << j);
        }
        uint32_t r = 0;
        for (unsigned i = 0; i < bits; i++) r |= ((h[i] >> (len - 1)) & 1) << i;
        return r;
    }
};
static const TailBixHashOf & tailBixHashOf() { static const TailBixHashOf f; return f; }

// The BixHash value at position q of a call, gathered from its B hash streams: the framework
// stores a block of each stream in turn (bwBytes bytes each), so stream i's bit q is in the
// 64-bit word at block q / bw, stream i, word (q % bw) / 64.
static inline uint64_t tailBixHashAt(const uint8_t * hashes, size_t q, unsigned bits, unsigned bwLog2) {
    const size_t bwBytes = (size_t(1) << bwLog2) / 8;
    const uint8_t * const blk = hashes + (q >> bwLog2) * bits * bwBytes + ((q & ((size_t(1) << bwLog2) - 1)) >> 6) * 8;
    uint64_t h = 0;
    for (unsigned i = 0; i < bits; i++) {
        uint64_t w;
        std::memcpy(&w, blk + i * bwBytes, 8);
        h |= ((w >> (q & 63)) & 1) << i;
    }
    return h;
}

// BPE_DBG=bixhash: check the hash streams at every cacheable pretoken against TailBixHashOf,
// and the scan kernel's gathered hash at every pretoken it hands to C++.
static std::atomic<uint64_t> gBixHashChecked{0}, gBixHashWrong{0}, gBixHashIRChecked{0}, gBixHashIRWrong{0};
static const bool gBixHashCheck = [] {
    const char * e = std::getenv("BPE_DBG");
    if (!e || std::string(e) != "bixhash") return false;
    std::atexit([] {
        std::cerr << "[BPE] bixhash check: " << gBixHashWrong.load() << " of " << gBixHashChecked.load()
                  << " stream hashes differ from TailBixHashOf; " << gBixHashIRWrong.load() << " of "
                  << gBixHashIRChecked.load() << " scan-kernel hashes differ\n";
    });
    return true;
}();

// A cached pretoken on its way through the byte resolve loop: its start (relative to the
// call), length, key (k0 = bytes 0..7, k1 = bytes 8..14 and the length in the top byte)
// and prefetched cache set.
struct TailByteDesc {
    uint64_t k0, k1;
    TailCacheEntry * set;
    const TailCacheEntry * vset;         // its --tail-vocab-table set, or null
    uint64_t h;                          // the key's hash (--tail-shared-cache-bits)
    uint32_t s, len;
};

// Entries 0 .. w-1 of a W-entry set move down one, entry W-1 drops out (unrolled: a memmove
// call for 32..96 bytes was 3% of a 1-thread run).
template <unsigned W>
static inline void tailShiftDown(TailCacheEntry * set, unsigned w) {
#pragma clang loop unroll(full)
    for (unsigned k = W - 1; k > 0; k--)
        if (k <= w) set[k] = set[k - 1];
}

// --tail-vocab-table: the vocabulary tokens of 2..kTailByteKeyMax bytes that BPE turns back
// into themselves, as finished cache entries (one output id) in 4-entry sets, read-only once
// built. tiktoken returns any pretoken found in its vocabulary without merging; checking
// each token once at load makes the same shortcut exact for any merge list (for GPT-2 every
// merge token passes). Measured on val_4MB: 49% of the distinct cacheable pretokens (the
// cold misses) are one token, webtext_cases 33%.
static cl::opt<bool> TailVocabTable(
    "tail-vocab-table",
    cl::desc("With --tail-from-bytes: answer a cache miss on a pretoken that is itself a "
             "vocabulary token from a table checked at load (default off)."),
    cl::init(false));
// Built on a thread of its own while the pipeline compiles and runs, and published when done
// (release store; the resolve loop loads it with acquire): until then a miss just merges.
static std::vector<TailCacheEntry> gTailVocabMem;
static std::atomic<TailCacheEntry *> gTailVocab{nullptr};
static unsigned gTailVocabShift = 64;
static std::thread gTailVocabThread;
static std::string gTailVocabReport;   // the builder's summary line
static void tailVocabJoin() {
    if (!gTailVocabThread.joinable()) return;
    gTailVocabThread.join();
    std::cerr << gTailVocabReport;
}

// --tail-vocab-first (with --tail-vocab-table): look a pretoken up in the vocabulary table
// first, prefetched like the thread's own set, and keep only the other pretokens in the
// thread's cache. 90% of pretoken occurrences in val_4MB are one token, and the table is
// full from the start of a run while each thread's cache starts empty.
static cl::opt<bool> TailVocabFirst(
    "tail-vocab-first",
    cl::desc("With --tail-vocab-table: look pretokens up in the vocabulary table before the "
             "thread's own cache (default off)."),
    cl::init(false));

static inline const TailCacheEntry * tailVocabSet(const TailCacheEntry * vocab, uint64_t h) {
    return vocab + (h >> gTailVocabShift) * 4;
}

static inline const TailCacheEntry * tailVocabFind(const TailCacheEntry * vs, uint64_t k0, uint64_t k1) {
    for (unsigned i = 0; i < 4; i++) {
        if (vs[i].k1 == 0) return nullptr;
        if (vs[i].k1 == k1 && vs[i].k0 == k0) return &vs[i];
    }
    return nullptr;
}

// --tail-shared-cache-bits: the shared set of a key, and its lookup and insertion.
static inline TailCacheEntry * tailSharedSet(uint64_t h) { return gTailShared + (h >> gTailSharedShift) * 4; }

static inline const TailCacheEntry * tailSharedFind(TailCacheEntry * ss, uint64_t k0, uint64_t k1) {
    for (unsigned i = 0; i < 4; i++) {
        const uint64_t c = __atomic_load_n(&ss[i].k1, __ATOMIC_ACQUIRE);
        if (c == 0) return nullptr;                  // filled in order: the rest are empty too
        if (c == k1 && ss[i].k0 == k0) return &ss[i];
    }
    return nullptr;
}

static inline void tailSharedInsert(TailCacheEntry * ss, const TailCacheEntry & f) {
    for (unsigned i = 0; i < 4; i++) {
        uint64_t c = __atomic_load_n(&ss[i].k1, __ATOMIC_ACQUIRE);
        if (c == 0) {
            if (__atomic_compare_exchange_n(&ss[i].k1, &c, kTailSharedBusy, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                ss[i].k0 = f.k0;
                ss[i].keep = f.keep;
                std::memcpy(ss[i].out, f.out, sizeof(f.out));
                __atomic_store_n(&ss[i].k1, f.k1, __ATOMIC_RELEASE);
                return;
            }
        }
        if (c == f.k1 && ss[i].k0 == f.k0) return;  // another thread stored it first
    }
}

// W = entries per set: 2 keeps insertion order, 4 moves a hit to the front (LRU). VF:
// --tail-vocab-first.
// --tail-branchless-probe (with --tail-from-bytes): find a key in a 4-entry set (the
// vocabulary table's, and the thread cache's with --tail-cache-ways=4) by comparing all 4
// entries at once, not by a loop that stops at the first match or empty entry: the entry a
// word sits in varies from word to word, so that loop's exit was mispredicted. Runtime only,
// no cache tag.
static cl::opt<bool> TailBranchlessProbe(
    "tail-branchless-probe",
    cl::desc("With --tail-from-bytes: compare all entries of a 4-entry tail cache set at once "
             "(default off)."),
    cl::init(false));
// The entry of a 4-entry set holding the key, 4 if none (an empty entry's k1 is 0, a key's
// never is).
static inline unsigned tailMatch4(const TailCacheEntry * s, uint64_t k0, uint64_t k1) {
    unsigned m = 16;
#pragma clang loop unroll(full)
    for (unsigned i = 0; i < 4; i++) m |= unsigned((s[i].k0 == k0) & (s[i].k1 == k1)) << i;
    return unsigned(__builtin_ctz(m));
}

// --tail-inline-vocab-hit (with --tail-vocab-first): the resolve loop probes the vocabulary set
// itself (all 4 entries at once) and writes a hit's one id in place; only a miss calls
// tailFinishBytes (SV: the vocabulary set already failed). As a call per pretoken, the hit
// path, 90% of pretokens on val_4MB, spilled the loop's state. Runtime only, no cache tag.
static cl::opt<bool> TailInlineVocabHit(
    "tail-inline-vocab-hit",
    cl::desc("With --tail-vocab-first: finish a vocabulary-table hit inside the resolve loop "
             "(default off)."),
    cl::init(false));

// --tail-no-ring (with --tail-from-bytes): finish each pretoken of 2 .. kTailByteKeyMax bytes as
// soon as it is found, from registers, instead of keeping its descriptor in the ring of
// kTailAhead whose cache sets are being prefetched. With --tail-inline-vocab-hit the
// processor's out-of-order window already hides the set loads (ring of 1: no slower).
// Runtime only, no cache tag.
static cl::opt<bool> TailNoRing(
    "tail-no-ring",
    cl::desc("With --tail-from-bytes: finish each cached pretoken at once instead of after a "
             "prefetch ring (default off)."),
    cl::init(false));

template <unsigned W, bool VF, bool SV = false>
static inline void tailFinishBytes(const TailByteDesc & d, const uint8_t * bytes, uint32_t * res,
                                   uint64_t * emit) {
    TailCacheEntry * const set = d.set;
    const TailCacheEntry * e;
    unsigned w = W;
    if constexpr (VF && !SV) {
        if (d.vset) {
            if (TailBranchlessProbe) {
                const unsigned i = tailMatch4(d.vset, d.k0, d.k1);
                if (i < 4) { e = &d.vset[i]; goto emitIds; }
            } else {
                for (unsigned i = 0; i < 4 && d.vset[i].k1 != 0; i++)
                    if (d.vset[i].k1 == d.k1 && d.vset[i].k0 == d.k0) { e = &d.vset[i]; goto emitIds; }
            }
        }
    }
    if constexpr (W == 2) {
        if (LLVM_LIKELY(set[0].k0 == d.k0 && set[0].k1 == d.k1)) w = 0;
        else if (set[1].k0 == d.k0 && set[1].k1 == d.k1) w = 1;
    } else if (TailBranchlessProbe) {
        w = tailMatch4(set, d.k0, d.k1);
    } else {
        for (unsigned i = 0; i < W; i++)
            if (set[i].k0 == d.k0 && set[i].k1 == d.k1) { w = i; break; }
    }
    if (LLVM_LIKELY(w < W)) {
        if (W > 2 && w > 0) {                                   // move to the front
            const TailCacheEntry hit = set[w];
            tailShiftDown<W>(set, w);
            set[0] = hit;
            w = 0;
        }
        e = &set[w];
    } else if (const TailCacheEntry * g = !VF && d.vset ? tailVocabFind(d.vset, d.k0, d.k1) : nullptr) {
        tailShiftDown<W>(set, W - 1);                           // the last one goes
        set[0] = *g;
        e = &set[0];
    } else if (const TailCacheEntry * g = gTailShared ? tailSharedFind(tailSharedSet(d.h), d.k0, d.k1)
                                                      : nullptr) {
        tailShiftDown<W>(set, W - 1);
        set[0] = *g;
        e = &set[0];
    } else {
        uint16_t tt[kTailByteKeyMax];
        uint32_t idx[kTailByteKeyMax];
        size_t m = d.len;
        for (size_t i = 0; i < m; i++) { tt[i] = gTailBase[bytes[d.s + i]]; idx[i] = uint32_t(i); }
        tailMergeSegment(tt, idx, m);
        if (m > kTailCacheOut) {
            for (size_t j = 0; j < m; j++) { res[d.s + idx[j]] = tt[j]; tailMarkTok(emit, d.s + idx[j], j); }
            return;
        }
        tailShiftDown<W>(set, W - 1);                           // the last one goes
        TailCacheEntry & f = set[0];
        f.k0 = d.k0; f.k1 = d.k1; f.keep = 0;
        for (size_t j = 0; j < m; j++) { f.keep |= uint16_t(1u << idx[j]); f.out[j] = tt[j]; }
        if (gTailShared) tailSharedInsert(tailSharedSet(d.h), f);
        e = &f;
    }
emitIds:
    unsigned j = 0;
    for (uint32_t keep = e->keep; keep; keep &= keep - 1) {
        const uint32_t q = d.s + __builtin_ctz(keep);
        res[q] = e->out[j];
        tailMarkTok(emit, q, j++);
    }
}

// tailResolve for --tail-from-bytes: segments are the pretokens (segs = their starts), every
// byte a token. Same protocol for res / emit and the segments cut by the call's edges. A
// pretoken of one byte is its base id; 2 .. kTailByteKeyMax bytes go through the cache
// (keyed by the bytes, prefetched kTailAhead pretokens ahead); longer ones merge on the spot.

// The pretoken starts of a call below n, in order, scanned from its start bits; n when done.
struct TailStartBits {
    const uint64_t * segs;
    size_t n, nWords, w = 0;
    uint64_t x = 0;
    TailStartBits(const uint64_t * segs, size_t n) : segs(segs), n(n), nWords((n + 63) / 64) {
        if (nWords) x = word(0);
    }
    uint64_t word(size_t i) const { return segs[i] & (i * 64 + 64 > n ? tailLowMask(n - i * 64) : ~0ull); }
    size_t next() {
        while (x == 0 && ++w < nWords) x = word(w);
        if (x == 0) return n;
        const size_t p = w * 64 + __builtin_ctzll(x);
        x &= x - 1;
        return p;
    }
};

// --tail-long-cache: the hash of a long pretoken's bytes.
static inline uint64_t tailLongHash(const uint8_t * p, size_t len) {
    uint64_t h = len * 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t c;
        std::memcpy(&c, p + i, 8);
        h = (h ^ c) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 31;
    }
    if (i < len) {
        uint64_t c = 0;
        std::memcpy(&c, p + i, len - i);
        h = (h ^ c) * 0xC2B2AE3D27D4EB4Full;
    }
    return (h ^ (h >> 29)) * 0x9E3779B97F4A7C15ull;
}

// A pretoken of more than kTailByteKeyMax bytes, bytes[s, s + len): merge it and write its
// tokens. With --tail-long-cache, look it up first and keep what was merged.
static void tailLongPretoken(TailScratch & S, const uint8_t * bytes, size_t s, size_t len,
                             uint32_t * res, uint64_t * emit) {
    const bool cached = TailLongCache && len <= kTailLongMax;
    const size_t slots = size_t(1) << kTailLongIndexBits;
    uint64_t h = 0;
    size_t home = 0;
    if (cached) {
        h = tailLongHash(bytes + s, len);
        home = h >> (64 - kTailLongIndexBits);
        for (size_t i = home;; i = (i + 1) & (slots - 1)) {
            const TailLongSlot & q = S.longIndex[i];
            if (q.len == 0) { home = i; break; }
            const uint8_t * entry = S.longArena.data() + q.off;
            if (q.h == h && q.len == len && std::memcmp(entry + 4, bytes + s, len) == 0) {
                uint32_t ntok;
                std::memcpy(&ntok, entry, 4);
                const uint32_t * tok = reinterpret_cast<const uint32_t *>(entry + 4 + ((len + 3) & ~size_t(3)));
                for (uint32_t j = 0; j < ntok; j++) {
                    const uint32_t p = uint32_t(s) + (tok[j] >> 16);
                    res[p] = tok[j] & 0xFFFF;
                    tailMarkTok(emit, p, j);
                }
                return;
            }
        }
    }
    if (S.t.size() < len) { S.t.resize(len); S.pos.resize(len); }
    for (size_t i = 0; i < len; i++) { S.t[i] = gTailBase[bytes[s + i]]; S.pos[i] = uint32_t(s + i); }
    size_t k = len;
    tailMergeSegment(S.t.data(), S.pos.data(), k);
    for (size_t i = 0; i < k; i++) { res[S.pos[i]] = S.t[i]; tailMarkTok(emit, S.pos[i], i); }
    if (!cached) return;
    const size_t need = 4 + ((len + 3) & ~size_t(3)) + 4 * k;
    if (S.longUsed + need > S.longArena.size() || 2 * (S.longCount + 1) > slots) {   // full: start over
        std::fill(S.longIndex.begin(), S.longIndex.end(), TailLongSlot{0, 0, 0});
        S.longUsed = S.longCount = 0;
        home = h >> (64 - kTailLongIndexBits);
    }
    uint8_t * entry = S.longArena.data() + S.longUsed;
    const uint32_t ntok = uint32_t(k);
    std::memcpy(entry, &ntok, 4);
    std::memcpy(entry + 4, bytes + s, len);
    uint32_t * tok = reinterpret_cast<uint32_t *>(entry + 4 + ((len + 3) & ~size_t(3)));
    for (size_t i = 0; i < k; i++) tok[i] = S.t[i] | ((S.pos[i] - uint32_t(s)) << 16);
    S.longIndex[home] = TailLongSlot{h, uint32_t(len), uint32_t(S.longUsed)};
    S.longUsed += need;
    S.longCount++;
}

// The pretokens of one resolve call over bytes [0, n): its outputs, the thread's cache and the
// ring of pretokens whose cache sets are being prefetched. BH: --tail-bixhash, the sets come
// from the BixHash streams (hashes, set by tailWalkBeginIR) at the pretoken's last byte.
template <unsigned W, bool VF, bool BH = false>
struct TailByteWalk {
    TailScratch & S;
    const uint8_t * const bytes;
    uint32_t * const res;
    uint64_t * const emit;
    const size_t n;
    TailCacheEntry * const cache;
    const unsigned setShift;
    const TailCacheEntry * const vocab;   // null until built
    TailByteDesc ring[kTailAhead];
    unsigned head = 0, count = 0;
    const uint8_t * hashes = nullptr;     // BH: the call's B hash streams
    unsigned hashBits = 0, bwLog2 = 0;

    TailByteWalk(TailScratch & S, const uint8_t * bytes, uint32_t * res, uint64_t * emit, size_t n)
    : S(S), bytes(bytes), res(res), emit(emit), n(n), cache(S.sets),
      setShift(64 - S.setBits), vocab(gTailVocab.load(std::memory_order_acquire)) {}

    void openRange(size_t lo, size_t hi, bool segStart) {   // the bytes of a cut segment
        for (size_t p = lo; p < hi; p++) {
            res[p] = gTailBase[bytes[p]] | kTailOpen | (segStart && p == lo ? kTailSeg : 0);
            tailMark(emit, uint32_t(p));
        }
    }

    void drain() {
        for (; count; count--, head = (head + 1) % kTailAhead) finish(ring[head]);
    }

    LLVM_ATTRIBUTE_ALWAYS_INLINE void finish(const TailByteDesc & d) {
        if constexpr (VF) {
            if (TailInlineVocabHit && d.vset) {
                const unsigned i = tailMatch4(d.vset, d.k0, d.k1);
                if (LLVM_LIKELY(i < 4)) {                           // a vocabulary entry: one id
                    res[d.s] = d.vset[i].out[0];
                    tailMarkTok(emit, d.s, 0);
                } else {
                    tailFinishBytes<W, VF, true>(d, bytes, res, emit);
                }
                return;
            }
        }
        tailFinishBytes<W, VF>(d, bytes, res, emit);
    }

    // The whole pretoken [s, e). Always inlined: as a call, the loop's state (ring, head,
    // count) lives in memory instead of registers, +5% on val_4MB at 1 thread.
    LLVM_ATTRIBUTE_ALWAYS_INLINE void word(size_t s, size_t e) {
        const size_t len = e - s;
        if (len == 1) {                                             // one byte: its base id
            res[s] = gTailBase[bytes[s]];
            tailMarkTok(emit, uint32_t(s), 0);
        } else if (len <= kTailByteKeyMax) {
            if (TailNoRing) {                                       // finished at once
                TailByteDesc d;
                describe(d, s, e);
                finish(d);
                return;
            }
            if (count == kTailAhead) {
                finish(ring[head]);
                head = (head + 1) % kTailAhead;
                count--;
            }
            TailByteDesc & d = ring[(head + count++) % kTailAhead];
            describe(d, s, e);
            __builtin_prefetch(d.set);
            if (VF && d.vset) __builtin_prefetch(d.vset);
        } else {                                                    // longer: merge it here
            tailLongPretoken(S, bytes, s, len, res, emit);
        }
    }

    // The key, hash and sets of the pretoken [s, e) of 2 .. kTailByteKeyMax bytes.
    LLVM_ATTRIBUTE_ALWAYS_INLINE void describe(TailByteDesc & d, size_t s, size_t e) {
        const size_t len = e - s;
        uint64_t a = 0, b = 0;
        if (LLVM_LIKELY(s + 16 <= n)) {                             // stays inside the call
            std::memcpy(&a, bytes + s, 8);
            std::memcpy(&b, bytes + s + 8, 8);
        } else {
            for (size_t i = 0; i < len; i++)
                (i < 8 ? a : b) |= uint64_t(bytes[s + i]) << (8 * (i % 8));
        }
        d.k0 = len >= 8 ? a : a & tailLowMask(len * 8);
        d.k1 = (len > 8 ? b & tailLowMask((len - 8) * 8) : 0) | (uint64_t(len) << 56);
        d.s = uint32_t(s);
        d.len = uint32_t(len);
        uint64_t h;
        if constexpr (BH) h = tailBixHashAt(hashes, e - 1, hashBits, bwLog2) << (64 - hashBits);
        else h = (d.k0 ^ (d.k1 * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
        d.set = cache + (h >> setShift) * W;
        d.h = h;
        d.vset = vocab ? tailVocabSet(vocab, h) : nullptr;
    }
};

template <unsigned W, bool VF>
static void tailResolveBytes(TailScratch & S, const uint8_t * bytes, TailStartBits starts,
                             uint32_t * res, uint64_t * emit, uint64_t numItems, uint64_t n,
                             uint64_t final) {
    size_t copied = 0;
    if (TailEmitFromSegs) {                                         // every start of the call
        copied = starts.nWords;
        for (size_t i = 0; i < copied; i++) emit[i] = starts.word(i);
    }
    std::fill(emit + copied, emit + numItems / 64, 0);
    if (n == 0) return;
    TailByteWalk<W, VF> walk(S, bytes, res, emit, n);
    const size_t first = starts.next();
    walk.openRange(0, first, false);                                // cut by the left edge
    if (first == n) return;
    for (size_t s = first;;) {
        const size_t e = starts.next();
        const bool last = e == n;
        if (last && !final) {                                       // may go on in the next call
            walk.drain();
            walk.openRange(s, n, true);
            return;
        }
        walk.word(s, e);
        if (last) break;
        s = e;
    }
    walk.drain();
}

// --tail-scan-kernel: the same walk, driven by BPETailScanKernel, a two-level scan over the
// pretoken starts that finishes the common pretokens itself and hands the rest to
// tailWalkSlowIR. tailWalkBeginIR sets up the call's walk (thread-local: one kernel call at a
// time per thread), tailWalkStart takes the call's first start, tailWalkEnd finishes the call.
// prev = the previous start, kTailNoStart before the first.
static constexpr uint64_t kTailNoStart = ~0ull;
template <unsigned W, bool VF, bool BH>
struct TailWalkCall {
    TailByteWalk<W, VF, BH> walk;
    bool final;
};

template <unsigned W, bool VF, bool BH>
static void tailWalkStart(uint64_t call, uint64_t prev, uint64_t p) {
    TailByteWalk<W, VF, BH> & walk = reinterpret_cast<TailWalkCall<W, VF, BH> *>(call)->walk;
    if (LLVM_UNLIKELY(prev == kTailNoStart)) walk.openRange(0, p, false);   // cut by the left edge
    else walk.word(prev, p);
}

template <unsigned W, bool VF, bool BH>
static void tailWalkEnd(uint64_t call, uint64_t prev) {
    TailWalkCall<W, VF, BH> & c = *reinterpret_cast<TailWalkCall<W, VF, BH> *>(call);
    TailByteWalk<W, VF, BH> & walk = c.walk;
    if (prev == kTailNoStart) walk.openRange(0, walk.n, false);       // no start in the call
    else if (c.final) walk.word(prev, walk.n);
    else walk.openRange(prev, walk.n, true);                          // may go on in the next call
    walk.drain();
}

// What the generated hit path reads, per call: the walk for everything else, the vocabulary
// table (null until built) and its set shift, and the base ids of the bytes.
struct TailIRCtx {
    uint64_t call;
    const TailCacheEntry * vocab;
    uint64_t vshift;
    const uint16_t * base;
    const TailCacheEntry * cache;   // the thread's cache and its set shift (--tail-scan-ir-cache)
    uint64_t cshift;
    uint64_t zero;                  // 0 (--tail-bixhash-control=2)
};
static_assert(sizeof(TailCacheEntry) == 32 && offsetof(TailCacheEntry, out) == 18,
              "the generated vocabulary probe reads 32-byte entries, out[0] at byte 18");

// hashes: the call's BixHash streams (--tail-bixhash, else null); bwLog2: log2 of the block width.
template <unsigned W, bool VF, bool BH>
static uint64_t tailWalkBeginIR(const uint8_t * bytes, const uint64_t * segs, const uint8_t * hashes,
                                uint32_t * res, uint64_t * emit, uint64_t numItems, uint64_t n,
                                uint64_t final, uint64_t bwLog2) {
    using Call = TailWalkCall<W, VF, BH>;
    alignas(Call) thread_local unsigned char mem[sizeof(Call)];
    n = std::min(n, numItems);
    Call * const c = new (mem) Call{TailByteWalk<W, VF, BH>(tailScratch(), bytes, res, emit, n), final != 0};
    const TailStartBits st(segs, n);
    for (size_t i = 0; i < st.nWords; i++) emit[i] = st.word(i);   // every pretoken's first token
    std::fill(emit + st.nWords, emit + numItems / 64, 0);
    if constexpr (BH) {
        c->walk.hashes = hashes;
        c->walk.hashBits = bixHashBits();
        c->walk.bwLog2 = unsigned(bwLog2);
        if (LLVM_UNLIKELY(gBixHashCheck)) {                       // every whole cacheable pretoken
            TailStartBits t(segs, n);
            for (size_t s = t.next(), e; s < n; s = e) {
                e = t.next();
                if (e == n && !final) break;
                if (e - s < 2 || e - s > kTailByteKeyMax) continue;
                gBixHashChecked++;
                if (tailBixHashAt(hashes, e - 1, c->walk.hashBits, c->walk.bwLog2)
                        != tailBixHashOf()(bytes + s, unsigned(e - s))) gBixHashWrong++;
            }
        }
    }
    thread_local TailIRCtx ctx;
    ctx = TailIRCtx{reinterpret_cast<uint64_t>(c), gTailVocab.load(std::memory_order_acquire), gTailVocabShift,
                    gTailBase, c->walk.cache, c->walk.setShift, 0};
    return reinterpret_cast<uint64_t>(&ctx);
}

// The pretoken [s, e) the generated code left to the walk; h: the hash the generated code
// computed for it, kTailNoStart if none.
template <unsigned W, bool VF, bool BH>
static void tailWalkSlowIR(uint64_t call, uint64_t s, uint64_t e, uint64_t h) {
    TailByteWalk<W, VF, BH> & walk = reinterpret_cast<TailWalkCall<W, VF, BH> *>(call)->walk;
    if constexpr (BH) {
        if (LLVM_UNLIKELY(gBixHashCheck) && h != kTailNoStart) {
            gBixHashIRChecked++;
            if (h != tailBixHashAt(walk.hashes, e - 1, walk.hashBits, walk.bwLog2) << (64 - walk.hashBits))
                gBixHashIRWrong++;
        }
    }
    walk.word(s, e);
}

extern "C" void bpe_tail_resolve_bytes(const uint8_t * bytes, const uint64_t * segs, uint32_t * res,
                                       uint64_t * emit, uint64_t numItems, uint64_t n, uint64_t final) {
    n = std::min(n, numItems);
    TailScratch & S = tailScratch();
    const TailStartBits starts(segs, n);
    const bool vf = TailVocabFirst && TailVocabTable;
    if (S.ways == 4) {
        if (vf) tailResolveBytes<4, true>(S, bytes, starts, res, emit, numItems, n, final);
        else tailResolveBytes<4, false>(S, bytes, starts, res, emit, numItems, n, final);
    } else {
        if (vf) tailResolveBytes<2, true>(S, bytes, starts, res, emit, numItems, n, final);
        else tailResolveBytes<2, false>(S, bytes, starts, res, emit, numItems, n, final);
    }
}

// BPETailKernel's body: the tokens in order. A cut segment's tokens collect in gTailOpen and
// are finished when the next segment starts; everything else is emitted as it comes. The
// finished ids of a call go to gTailOutBuf (uninitialized, grown as needed: a call never
// emits more tokens than it has positions) and then to the sink in one batch.
static TailSegBuf gTailOpen;
static std::unique_ptr<uint16_t[]> gTailOutBuf;
static size_t gTailOutCap = 0;

static size_t tailFlushOpen(uint16_t * out, size_t k) {
    tailMergeSegment(gTailOpen.t.data(), gTailOpen.pos.data(), gTailOpen.n);
    for (size_t i = 0; i < gTailOpen.n; i++) out[k++] = gTailOpen.t[i];
    gTailOpen.n = 0;
    return k;
}

extern "C" void bpe_tail_block(const uint32_t * res, const uint64_t * starts,
                               uint64_t numItems, uint64_t n, uint64_t final) {
    const size_t room = std::min(n, numItems) + gTailOpen.n;
    if (room > gTailOutCap) {
        gTailOutCap = std::max<size_t>(room, 2 * gTailOutCap);
        gTailOutBuf.reset(new uint16_t[gTailOutCap]);
    }
    uint16_t * const out = gTailOutBuf.get();
    size_t k = 0;
    for (uint64_t wi = 0; wi * 64 < n && wi * 64 < numItems; wi++) {
        uint64_t st = starts[wi];
        if (n - wi * 64 < 64) st &= (1ull << (n - wi * 64)) - 1;
        for (; st; st &= st - 1) {
            const uint32_t v = res[wi * 64 + __builtin_ctzll(st)];
            if (LLVM_LIKELY((v & kTailOpen) == 0)) {
                if (LLVM_UNLIKELY(gTailOpen.n != 0)) k = tailFlushOpen(out, k);
                out[k++] = uint16_t(v);
            } else {
                if ((v & kTailSeg) && gTailOpen.n != 0) k = tailFlushOpen(out, k);
                gTailOpen.push(uint16_t(v), 0);
            }
        }
    }
    if (final && gTailOpen.n != 0) k = tailFlushOpen(out, k);
    if (gTailSink && k) gTailSink(out, k);
}

// --bpe-tail kernels, both thin wrappers around their C++ bodies above.
class BPETailResolveKernel final : public MultiBlockKernel {
public:
    BPETailResolveKernel(LLVMTypeSystemInterface & ts, StreamSet * ids16, StreamSet * starts,
                         StreamSet * segs, StreamSet * res, StreamSet * emit)
    : MultiBlockKernel(ts, "BPE_TailResolve",
                       {Binding{"ids", ids16}, Binding{"starts", starts}, Binding{"segs", segs}},
                       {Binding{"res", res}, Binding{"emit", emit}}, {}, {}, {}) {
        // No state between calls (the C++ body only uses thread_local scratch), so the
        // pipeline may run its calls on several threads at once.
        addAttribute(Statefree());
    }
protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        Function * const fn = b.getModule()->getFunction("bpe_tail_resolve");
        if (fn == nullptr) report_fatal_error("BPE_TailResolve: bpe_tail_resolve is not linked");
        Value * const zero = b.getInt32(0);
        Value * const numItems = b.CreateMul(numOfStrides, b.getSize(getStride()));
        Value * const n = b.CreateUMin(numItems, b.getAccessibleItemCount("starts"));
        b.CreateCall(fn->getFunctionType(), fn,
                     {b.getInputStreamBlockPtr("ids", zero), b.getInputStreamBlockPtr("starts", zero),
                      b.getInputStreamBlockPtr("segs", zero), b.getOutputStreamBlockPtr("res", zero),
                      b.getOutputStreamBlockPtr("emit", zero), numItems, n,
                      b.CreateZExt(b.isFinal(), b.getInt64Ty())});
    }
};

// Side-effecting, so the pipeline keeps it and runs it in order, one call at a time.
class BPETailKernel final : public MultiBlockKernel {
public:
    BPETailKernel(LLVMTypeSystemInterface & ts, StreamSet * res, StreamSet * starts)
    : MultiBlockKernel(ts, "BPE_Tail",
                       {Binding{"res", res}, Binding{"starts", starts}},
                       {}, {}, {}, {}) {
        addAttribute(SideEffecting());
    }
protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        Function * const fn = b.getModule()->getFunction("bpe_tail_block");
        if (fn == nullptr) report_fatal_error("BPE_Tail: bpe_tail_block is not linked");
        Value * const zero = b.getInt32(0);
        Value * const numItems = b.CreateMul(numOfStrides, b.getSize(getStride()));
        Value * const n = b.CreateUMin(numItems, b.getAccessibleItemCount("starts"));
        b.CreateCall(fn->getFunctionType(), fn,
                     {b.getInputStreamBlockPtr("res", zero), b.getInputStreamBlockPtr("starts", zero),
                      numItems, n, b.CreateZExt(b.isFinal(), b.getInt64Ty())});
    }
};

// --tail-from-bytes: BPETailResolveKernel on the input bytes and the pretoken starts.
class BPETailByteResolveKernel final : public MultiBlockKernel {
public:
    BPETailByteResolveKernel(LLVMTypeSystemInterface & ts, StreamSet * bytes, StreamSet * segs,
                             StreamSet * res, StreamSet * emit)
    : MultiBlockKernel(ts, "BPE_TailResolveBytes",
                       {Binding{"bytes", bytes}, Binding{"segs", segs}},
                       {Binding{"res", res}, Binding{"emit", emit}}, {}, {}, {}) {
        addAttribute(Statefree());   // see BPETailResolveKernel
    }
protected:
    void generateMultiBlockLogic(KernelBuilder & b, Value * const numOfStrides) override {
        Function * const fn = b.getModule()->getFunction("bpe_tail_resolve_bytes");
        if (fn == nullptr) report_fatal_error("BPE_TailResolveBytes: bpe_tail_resolve_bytes is not linked");
        Value * const zero = b.getInt32(0);
        Value * const numItems = b.CreateMul(numOfStrides, b.getSize(getStride()));
        Value * const n = b.CreateUMin(numItems, b.getAccessibleItemCount("segs"));
        b.CreateCall(fn->getFunctionType(), fn,
                     {b.getInputStreamBlockPtr("bytes", zero), b.getInputStreamBlockPtr("segs", zero),
                      b.getOutputStreamBlockPtr("res", zero), b.getOutputStreamBlockPtr("emit", zero),
                      numItems, n, b.CreateZExt(b.isFinal(), b.getInt64Ty())});
    }
};

// --tail-scan-kernel: BPETailByteResolveKernel as a two-level scan over the pretoken starts.
// The scan keeps the previous start in a loop variable; at each start it finishes the pretoken
// between the two in generated code (generateIRWord) or calls bpe_tail_ir_slow for it.
// hashes (--tail-bixhash, else null): the B BixHash streams, read at the pretoken's last byte.
class BPETailScanKernel final : public TwoLevelScanKernel {
public:
    BPETailScanKernel(LLVMTypeSystemInterface & ts, StreamSet * bytes, StreamSet * segs,
                      StreamSet * res, StreamSet * emit, unsigned cacheWays, StreamSet * hashes = nullptr,
                      unsigned control = 0)
    : TwoLevelScanKernel(ts, std::string("BPE_TailScanIR") + (cacheWays ? "c" + std::to_string(cacheWays) : "")
                             + (hashes ? "_bh" + std::to_string(hashes->getNumElements()) : "")
                             + (control ? "x" + std::to_string(control) : ""),
                         64, "segs", {LoopVar("prev", ts.getInt64Ty())})
    , mCacheWays(cacheWays), mHashBits(hashes ? hashes->getNumElements() : 0), mControl(control) {
        mInputStreamSets.push_back(Binding{"segs", segs, FixedRate(1), Principal()});
        mInputStreamSets.push_back(Binding{"bytes", bytes});
        if (hashes) mInputStreamSets.push_back(Binding{"hashes", hashes});
        mOutputStreamSets.push_back(Binding{"res", res});
        mOutputStreamSets.push_back(Binding{"emit", emit});
        addAttribute(Statefree());   // see BPETailResolveKernel
    }
protected:
    Function * linked(KernelBuilder & b, StringRef name) {
        Function * const fn = b.getModule()->getFunction(name);
        if (fn == nullptr) report_fatal_error(StringRef("BPE_TailScan: ") + name + " is not linked");
        return fn;
    }
    void initialize(KernelBuilder & b) override {
        Value * const zero = b.getInt32(0);
        Value * const numItems = b.CreateMul(b.getNumOfStrides(), b.getSize(getStride()));
        mN = b.CreateUMin(numItems, b.getAccessibleItemCount("segs"));
        mBase = b.getProcessedItemCount("segs");
        mLoopVarInitialValues = {b.getInt64(kTailNoStart)};
        Function * const fn = linked(b, "bpe_tail_ir_begin");
        mBytes = b.getInputStreamBlockPtr("bytes", zero);
        mRes = b.getOutputStreamBlockPtr("res", zero);
        mEmit = b.getOutputStreamBlockPtr("emit", zero);
        Type * const i8 = b.getInt8Ty(), * const i64 = b.getInt64Ty(), * const ptr = PointerType::getUnqual(b.getContext());
        mHashes = mHashBits ? b.getInputStreamBlockPtr("hashes", zero) : ConstantPointerNull::get(cast<PointerType>(ptr));
        Value * const ctx = b.CreateIntToPtr(
            b.CreateCall(fn->getFunctionType(), fn,
                         {mBytes, b.getInputStreamBlockPtr("segs", zero), mHashes, mRes, mEmit,
                          numItems, mN, b.CreateZExt(b.isFinal(), b.getInt64Ty()),
                          b.getInt64(__builtin_ctz(b.getBitBlockWidth()))}),
            ptr);
        mCall = b.CreateLoad(i64, ctx);
        mVocab = b.CreateLoad(ptr, b.CreateGEP(i8, ctx, b.getInt64(offsetof(TailIRCtx, vocab))));
        mVShift = b.CreateLoad(i64, b.CreateGEP(i8, ctx, b.getInt64(offsetof(TailIRCtx, vshift))));
        mBaseIds = b.CreateLoad(ptr, b.CreateGEP(i8, ctx, b.getInt64(offsetof(TailIRCtx, base))));
        mCache = b.CreateLoad(ptr, b.CreateGEP(i8, ctx, b.getInt64(offsetof(TailIRCtx, cache))));
        mCShift = b.CreateLoad(i64, b.CreateGEP(i8, ctx, b.getInt64(offsetof(TailIRCtx, cshift))));
        mZero = b.CreateLoad(i64, b.CreateGEP(i8, ctx, b.getInt64(offsetof(TailIRCtx, zero))));
    }
    void wordPrologueLogic(KernelBuilder &, Value *, std::vector<Value *>, std::vector<Value *> &) override {}
    void generateProcessingLogic(KernelBuilder & b, Value * absItemPos, std::vector<Value *> & loopVars) override {
        Value * const p = b.CreateSub(absItemPos, mBase);
        generateIRWord(b, loopVars[0], p);
        loopVars[0] = p;
    }
    // The pretoken [prev, p), as TailByteWalk's word() + finish() would for one byte or a
    // vocabulary hit; anything else goes to bpe_tail_ir_slow.
    void generateIRWord(KernelBuilder & b, Value * prev, Value * p) {
        Type * const i8 = b.getInt8Ty(), * const i16 = b.getInt16Ty(), * const i32 = b.getInt32Ty(), * const i64 = b.getInt64Ty();
        auto c64 = [&](uint64_t v) { return b.getInt64(v); };
        BasicBlock * const first = b.CreateBasicBlock("irFirst");
        BasicBlock * const word = b.CreateBasicBlock("irWord");
        BasicBlock * const one = b.CreateBasicBlock("irOne");
        BasicBlock * const multi = b.CreateBasicBlock("irMulti");
        BasicBlock * const probe = b.CreateBasicBlock("irProbe");
        BasicBlock * const hit = b.CreateBasicBlock("irHit");
        BasicBlock * const slow = b.CreateBasicBlock("irSlow");
        BasicBlock * const done = b.CreateBasicBlock("irDone");
        b.CreateUnlikelyCondBr(b.CreateICmpEQ(prev, c64(kTailNoStart)), first, word);

        b.SetInsertPoint(first);                    // the call's first start: its left edge
        Function * const start = linked(b, "bpe_tail_walk_start");
        b.CreateCall(start->getFunctionType(), start, {mCall, prev, p});
        b.CreateBr(done);

        b.SetInsertPoint(word);
        Value * const len = b.CreateSub(p, prev);
        b.CreateCondBr(b.CreateICmpEQ(len, c64(1)), one, multi);

        b.SetInsertPoint(one);                      // one byte: its base id
        Value * const byte = b.CreateZExt(b.CreateLoad(i8, b.CreateGEP(i8, mBytes, prev)), i64);
        Value * const baseId = b.CreateLoad(i16, b.CreateGEP(i16, mBaseIds, byte));
        b.CreateStore(b.CreateZExt(baseId, i32), b.CreateGEP(i32, mRes, prev));
        b.CreateBr(done);

        b.SetInsertPoint(multi);                    // 2 .. 15 bytes inside the call, table built
        Value * const ok = b.CreateAnd(b.CreateAnd(b.CreateICmpULE(len, c64(kTailByteKeyMax)),
                                                   b.CreateICmpULE(b.CreateAdd(prev, c64(16)), mN)),
                                       b.CreateIsNotNull(mVocab));
        b.CreateLikelyCondBr(ok, probe, slow);

        b.SetInsertPoint(probe);                    // the key as in TailByteWalk::multi
        Value * const src = b.CreateGEP(i8, mBytes, prev);
        Value * const lo = b.CreateAlignedLoad(i64, src, 1);
        Value * const hi = b.CreateAlignedLoad(i64, b.CreateGEP(i8, src, c64(8)), 1);
        Value * const bits = b.CreateShl(len, 3);
        Value * const long0 = b.CreateICmpUGE(len, c64(8)), * const long1 = b.CreateICmpUGT(len, c64(8));
        Value * const mask0 = b.CreateSub(b.CreateShl(c64(1), b.CreateSelect(long0, c64(0), bits)), c64(1));
        Value * const mask1 = b.CreateSub(b.CreateShl(c64(1), b.CreateSelect(long1, b.CreateSub(bits, c64(64)), c64(0))), c64(1));
        Value * const k0 = b.CreateSelect(long0, lo, b.CreateAnd(lo, mask0));
        Value * const k1 = b.CreateOr(b.CreateSelect(long1, b.CreateAnd(hi, mask1), c64(0)), b.CreateShl(len, 56));
        Value * h;
        if (mHashBits && mControl != 1) {           // --tail-bixhash: bit q of each hash stream,
            const unsigned bw = b.getBitBlockWidth(), bwBytes = bw / 8;   // q the last byte
            Value * const q = b.CreateSub(p, c64(1));
            Value * const off = b.CreateAdd(b.CreateMul(b.CreateLShr(q, c64(__builtin_ctz(bw))), c64(mHashBits * bwBytes)),
                                            b.CreateShl(b.CreateLShr(b.CreateAnd(q, c64(bw - 1)), c64(6)), c64(3)));
            Value * const at = b.CreateGEP(i8, mHashes, off);
            Value * const sh = b.CreateAnd(q, c64(63));
            std::vector<Value *> terms;             // into the top B bits, as tailBixHashAt << (64 - B)
            for (unsigned i = 0; i < mHashBits; i++) {
                Value * const w = b.CreateAlignedLoad(i64, b.CreateGEP(i8, at, c64(i * bwBytes)), 8);
                terms.push_back(b.CreateShl(b.CreateAnd(b.CreateLShr(w, sh), c64(1)), c64(64 - mHashBits + i)));
            }
            while (terms.size() > 1) {              // a balanced tree of ORs, not a chain
                std::vector<Value *> next;
                for (size_t i = 0; i + 1 < terms.size(); i += 2) next.push_back(b.CreateOr(terms[i], terms[i + 1]));
                if (terms.size() % 2) next.push_back(terms.back());
                terms.swap(next);
            }
            h = terms[0];
            if (mControl == 2)
                h = b.CreateAdd(b.CreateMul(b.CreateXor(k0, b.CreateMul(k1, c64(0xC2B2AE3D27D4EB4Full))), c64(0x9E3779B97F4A7C15ull)),
                                b.CreateAnd(h, mZero));
        } else {
            h = b.CreateMul(b.CreateXor(k0, b.CreateMul(k1, c64(0xC2B2AE3D27D4EB4Full))), c64(0x9E3779B97F4A7C15ull));
            if (mControl == 3)
                h = b.CreateAdd(h, b.CreateAnd(b.CreateMul(b.CreateXor(k1, b.CreateMul(k0, c64(0xFF51AFD7ED558CCDull))),
                                                           c64(0xC4CEB9FE1A85EC53ull)), mZero));
        }
        Value * const set = b.CreateGEP(i8, mVocab, b.CreateShl(b.CreateLShr(h, mVShift), 7));   // 4 entries of 32 bytes
        Value * found = c64(16);                    // all 4 compared into a mask, as tailMatch4: an
        for (unsigned i = 0; i < 4; i++) {          // or-chain of the 4 matches becomes 4 branches
            Value * const e = b.CreateGEP(i8, set, c64(32 * i));
            Value * const m = b.CreateAnd(b.CreateICmpEQ(b.CreateLoad(i64, e), k0),
                                          b.CreateICmpEQ(b.CreateLoad(i64, b.CreateGEP(i8, e, c64(8))), k1));
            found = b.CreateOr(found, b.CreateShl(b.CreateZExt(m, i64), i));
        }
        Value * const way = b.CreateCountForwardZeroes(found);
        BasicBlock * const vocabMiss = mCacheWays ? b.CreateBasicBlock("irVocabMiss") : slow;
        BasicBlock * hashedToSlow = b.GetInsertBlock();   // where the slow path gets h from
        b.CreateLikelyCondBr(b.CreateICmpULT(way, c64(4)), hit, vocabMiss);

        b.SetInsertPoint(hit);                      // a vocabulary token: one id
        Value * const out = b.CreateGEP(i8, set, b.CreateAdd(b.CreateShl(way, 5), c64(offsetof(TailCacheEntry, out))));
        b.CreateStore(b.CreateZExt(b.CreateLoad(i16, out), i32), b.CreateGEP(i32, mRes, prev));
        b.CreateBr(done);

        if (mCacheWays) {                           // the thread cache's most recent entry
            b.SetInsertPoint(vocabMiss);
            BasicBlock * const cacheHit = b.CreateBasicBlock("irCacheHit");
            BasicBlock * const tokens = b.CreateBasicBlock("irCacheTokens");
            Value * const e = b.CreateGEP(i8, mCache, b.CreateMul(b.CreateLShr(h, mCShift), c64(32 * mCacheWays)));
            Value * const m = b.CreateAnd(b.CreateICmpEQ(b.CreateLoad(i64, e), k0),
                                          b.CreateICmpEQ(b.CreateLoad(i64, b.CreateGEP(i8, e, c64(8))), k1));
            hashedToSlow = b.GetInsertBlock();
            b.CreateLikelyCondBr(m, cacheHit, slow);

            b.SetInsertPoint(cacheHit);
            Value * const keep0 = b.CreateZExt(b.CreateLoad(i16, b.CreateGEP(i8, e, c64(offsetof(TailCacheEntry, keep)))), i64);
            Value * const outs = b.CreateGEP(i8, e, c64(offsetof(TailCacheEntry, out)));
            b.CreateBr(tokens);

            b.SetInsertPoint(tokens);                // token j at the j-th set bit of keep; the
            PHINode * const keep = b.CreatePHI(i64, 2);   // first one's emit bit is already set
            PHINode * const j = b.CreatePHI(i64, 2);
            keep->addIncoming(keep0, cacheHit);
            j->addIncoming(c64(0), cacheHit);
            Value * const q = b.CreateAdd(prev, b.CreateCountForwardZeroes(keep));
            Value * const tok = b.CreateLoad(i16, b.CreateGEP(i16, outs, j));
            b.CreateStore(b.CreateZExt(tok, i32), b.CreateGEP(i32, mRes, q));
            Value * const ew = b.CreateGEP(i64, mEmit, b.CreateLShr(q, 6));
            b.CreateStore(b.CreateOr(b.CreateLoad(i64, ew), b.CreateShl(c64(1), b.CreateAnd(q, c64(63)))), ew);
            Value * const nextKeep = b.CreateAnd(keep, b.CreateSub(keep, c64(1)));
            keep->addIncoming(nextKeep, tokens);
            j->addIncoming(b.CreateAdd(j, c64(1)), tokens);
            b.CreateCondBr(b.CreateICmpNE(nextKeep, c64(0)), tokens, done);
        }

        b.SetInsertPoint(slow);                     // h for BPE_DBG=bixhash, kTailNoStart if none
        PHINode * const slowH = b.CreatePHI(i64, 2);
        slowH->addIncoming(c64(kTailNoStart), multi);
        slowH->addIncoming(h, hashedToSlow);
        Function * const fn = linked(b, "bpe_tail_ir_slow");
        b.CreateCall(fn->getFunctionType(), fn, {mCall, prev, p, slowH});
        b.CreateBr(done);

        b.SetInsertPoint(done);
    }
    void finalize(KernelBuilder & b, std::vector<Value *> & loopVarFinalValues) override {
        Function * const fn = linked(b, "bpe_tail_walk_end");
        b.CreateCall(fn->getFunctionType(), fn, {mCall, loopVarFinalValues[0]});
    }
    const unsigned mCacheWays, mHashBits, mControl;
    Value * mHashes = nullptr, * mZero = nullptr;
    Value * mBase = nullptr, * mCall = nullptr;
    Value * mBytes = nullptr, * mRes = nullptr, * mN = nullptr;
    Value * mVocab = nullptr, * mVShift = nullptr, * mBaseIds = nullptr;
    Value * mCache = nullptr, * mCShift = nullptr, * mEmit = nullptr;       // --tail-scan-ir-cache
};

template <unsigned W, bool VF, bool BH>
static void linkTailWalkIR(PipelineBuilder & P, Kernel * k) {
    P.LinkFunction(k, "bpe_tail_ir_begin", tailWalkBeginIR<W, VF, BH>);
    P.LinkFunction(k, "bpe_tail_ir_slow", tailWalkSlowIR<W, VF, BH>);
    P.LinkFunction(k, "bpe_tail_walk_start", tailWalkStart<W, VF, BH>);
    P.LinkFunction(k, "bpe_tail_walk_end", tailWalkEnd<W, VF, BH>);
}

template <bool BH>
static void linkTailWalkIR(PipelineBuilder & P, Kernel * k) {
    const bool four = TailCacheWays == 4, vf = TailVocabFirst && TailVocabTable;
    if (four) vf ? linkTailWalkIR<4, true, BH>(P, k) : linkTailWalkIR<4, false, BH>(P, k);
    else vf ? linkTailWalkIR<2, true, BH>(P, k) : linkTailWalkIR<2, false, BH>(P, k);
}

// --tail-bixhash: BixHash's run stream, the bytes that continue a pretoken.
class BPEPretokenRunKernel final : public PabloKernel {
public:
    BPEPretokenRunKernel(LLVMTypeSystemInterface & ts, StreamSet * segs, StreamSet * run)
    : PabloKernel(ts, "BPE_PretokenRun", {Binding{"segs", segs}}, {Binding{"run", run}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        pb.createAssign(pb.createExtract(getOutputStreamVar("run"), pb.getInteger(0)),
                        pb.createNot(getInputStreamSet("segs")[0]));
    }
};

void buildBPEByteTailEmitter(PipelineBuilder & P, StreamSet * bytes, StreamSet * segs, StreamSet * basis) {
    StreamSet * res = P.CreateStreamSet(1, 32);
    StreamSet * emit = P.CreateStreamSet(1, 1);
    if (TailScanKernel) {
        TailEmitFromSegs = true;                                     // tailWalkBeginIR copies the starts
        const unsigned cacheWays = TailScanIRCache ? (TailCacheWays == 4 ? 4 : 2) : 0;
        StreamSet * hashes = nullptr;
        if (TailBixHash || (TailBixHashControl && TailBixHashControl < 3)) {
            if (basis == nullptr) report_fatal_error("--tail-bixhash: no basis bits for the BPE input");
            StreamSet * run = P.CreateStreamSet(1);
            P.CreateKernelCall<BPEPretokenRunKernel>(segs, run);
            hashes = P.CreateStreamSet(bixHashBits());
            P.CreateKernelCall<BixHash>(basis, run, hashes, kBixHashSteps);
            if (TailBixHash) TailCacheBits = bixHashBits();          // the thread cache: 2^B sets
        }
        const unsigned control = TailBixHash ? 0 : unsigned(TailBixHashControl);
        Kernel * const r = P.CreateKernelCall<BPETailScanKernel>(bytes, segs, res, emit, cacheWays, hashes, control);
        if (hashes && !control) linkTailWalkIR<true>(P, r);
        else linkTailWalkIR<false>(P, r);
    } else {
        if (TailBixHash) report_fatal_error("--tail-bixhash needs --tail-scan-kernel");
        Kernel * const r = P.CreateKernelCall<BPETailByteResolveKernel>(bytes, segs, res, emit);
        P.LinkFunction(r, "bpe_tail_resolve_bytes", bpe_tail_resolve_bytes);
    }
    Kernel * const k = P.CreateKernelCall<BPETailKernel>(res, emit);
    P.LinkFunction(k, "bpe_tail_block", bpe_tail_block);
}

// --tail-direct-input: the tail's segment starts, the word starts that are live tokens (what
// BPERetirePackKernel's 18th stream holds).
class BPETailSegKernel final : public PabloKernel {
public:
    BPETailSegKernel(LLVMTypeSystemInterface & ts, StreamSet * boundary, StreamSet * live, StreamSet * segs)
    : PabloKernel(ts, "BPE_TailSeg",
                  {Binding{"boundary", boundary}, Binding{"live", live}},
                  {Binding{"segs", segs}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        pb.createAssign(pb.createExtract(getOutputStreamVar("segs"), pb.getInteger(0)),
                        pb.createAnd(getInputStreamSet("boundary")[0], getInputStreamSet("live")[0]));
    }
};

void buildBPETailEmitter(PipelineBuilder & P, StreamSet * ids, StreamSet * starts, StreamSet * segs) {
    StreamSet * ids16 = P.CreateStreamSet(1, 16);
    if (ids->getNumElements() < 16) {   // --tail-direct-input: P2S16Kernel zero-pads
        StreamSets idSets{ids};
        P.CreateKernelCall<P2S16Kernel>(idSets, ids16);
    } else {
        P.CreateKernelCall<P2S16Kernel>(ids, ids16);
    }
    StreamSet * res = P.CreateStreamSet(1, 32);
    StreamSet * emit = P.CreateStreamSet(1, 1);
    Kernel * const r = P.CreateKernelCall<BPETailResolveKernel>(ids16, starts, segs, res, emit);
    P.LinkFunction(r, "bpe_tail_resolve", bpe_tail_resolve);
    Kernel * const k = P.CreateKernelCall<BPETailKernel>(res, emit);
    P.LinkFunction(k, "bpe_tail_block", bpe_tail_block);
}

// Merge-back at one retirement point, in Fp: `later` = the tokens that survived the point,
// spread back to Fp by keptPre; `ids` = Fp's id stream. The retired tokens are the live
// starts that were not kept (liveIn & ~keptPre): OR in their id and start bit.
class BPERetireMergeKernel : public PabloKernel {
public:
    BPERetireMergeKernel(LLVMTypeSystemInterface & ts, StreamSet * later, StreamSet * ids,
                         StreamSet * liveIn, StreamSet * keptPre, StreamSet * merged)
    : PabloKernel(ts, "BPE_RetireMerge" + std::to_string(ids->getNumElements())
                        + (later->getNumElements() == kTailStreams ? "_t" : ""),
                  {Binding{"later", later}, Binding{"ids", ids}, Binding{"liveIn", liveIn},
                   Binding{"keptPre", keptPre}},
                  {Binding{"merged", merged}}) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> later = getInputStreamSet("later");
        std::vector<PabloAST *> ids = getInputStreamSet("ids");
        PabloAST * const fin = pb.createAnd(getInputStreamSet("liveIn")[0],
                                            pb.createNot(getInputStreamSet("keptPre")[0]), "retired");
        Var * out = getOutputStreamVar("merged");
        for (unsigned i = 0; i + 1 < kRetireStreams; i++)
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)),
                            i < ids.size() ? pb.createOr(later[i], pb.createAnd(ids[i], fin))
                                           : later[i]);
        for (unsigned i = kRetireStreams - 1; i < later.size(); i++)   // start (and segment)
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), pb.createOr(later[i], fin));
    }
};

// The final 16-bit id stream and its token-start mask as one 17-stream set. No mask
// (the last kernel was followed by a FilterByMask) means every position is a token start.
// segIn (--bpe-tail): the segment starts, packed as stream 17.
class BPERetirePackKernel : public PabloKernel {
public:
    BPERetirePackKernel(LLVMTypeSystemInterface & ts, StreamSet * idIn, StreamSet * maskIn,
                        StreamSet * packed, StreamSet * segIn = nullptr)
    : PabloKernel(ts, std::string("BPE_RetirePack") + (maskIn ? "_m1" : "_m0") + (segIn ? "_t" : ""),
                  bindings(idIn, maskIn, segIn),
                  {Binding{"packed", packed}}),
      mHasMask(maskIn != nullptr), mHasSeg(segIn != nullptr) {}
protected:
    static std::vector<Binding> bindings(StreamSet * idIn, StreamSet * maskIn, StreamSet * segIn) {
        std::vector<Binding> b{Binding{"idIn", idIn}};
        if (maskIn) b.emplace_back("maskIn", maskIn);
        if (segIn) b.emplace_back("segIn", segIn);
        return b;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> id = getInputStreamSet("idIn");
        Var * out = getOutputStreamVar("packed");
        for (unsigned i = 0; i + 1 < kRetireStreams; i++)
            pb.createAssign(pb.createExtract(out, pb.getInteger(i)), id[i]);
        PabloAST * mask = mHasMask ? getInputStreamSet("maskIn")[0]
                                   : pb.createInFile(pb.createOnes());
        pb.createAssign(pb.createExtract(out, pb.getInteger(kRetireStreams - 1)), mask);
        if (mHasSeg)
            pb.createAssign(pb.createExtract(out, pb.getInteger(kRetireStreams)),
                            pb.createAnd(getInputStreamSet("segIn")[0], mask));
    }
private:
    const bool mHasMask, mHasSeg;
};

// Splits the merged-back 17-stream set into the emitter's 16 id bits and start mask, and
// with --bpe-tail (18 streams) the segment starts into segOut.
class BPERetireUnpackKernel : public PabloKernel {
public:
    BPERetireUnpackKernel(LLVMTypeSystemInterface & ts, StreamSet * packed, StreamSet * idOut,
                          StreamSet * maskOut, StreamSet * segOut = nullptr)
    : PabloKernel(ts, std::string("BPE_RetireUnpack") + (segOut ? "_t" : ""),
                  {Binding{"packed", packed}},
                  segOut ? std::vector<Binding>{Binding{"idOut", idOut}, Binding{"maskOut", maskOut},
                                                Binding{"segOut", segOut}}
                         : std::vector<Binding>{Binding{"idOut", idOut}, Binding{"maskOut", maskOut}}),
      mHasSeg(segOut != nullptr) {}
protected:
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST *> in = getInputStreamSet("packed");
        Var * id = getOutputStreamVar("idOut");
        for (unsigned i = 0; i + 1 < kRetireStreams; i++)
            pb.createAssign(pb.createExtract(id, pb.getInteger(i)), in[i]);
        pb.createAssign(pb.createExtract(getOutputStreamVar("maskOut"), pb.getInteger(0)),
                        in[kRetireStreams - 1]);
        if (mHasSeg)
            pb.createAssign(pb.createExtract(getOutputStreamVar("segOut"), pb.getInteger(0)),
                            in[kRetireStreams]);
    }
private:
    const bool mHasSeg;
};

// --bit-frequency-statistics: over the surviving token starts (`live`), count all tokens
// (`total`) and, for each N in [kBitFreqLo, kBitFreqHi], the tokens whose id has at least
// N bits, i.e. id >= 2^(N-1) (`atLeastN`).
static cl::opt<bool> BitFrequencyStatistics(
    "bit-frequency-statistics",
    cl::desc("BPE mode: report how many output tokens have ids of at least N bits, for "
             "N = 10..16."),
    cl::init(false));

static constexpr unsigned kBitFreqLo = 10, kBitFreqHi = 16;

class BPEBitFrequencyKernel : public PabloKernel {
public:
    BPEBitFrequencyKernel(LLVMTypeSystemInterface & ts, StreamSet * id, StreamSet * live)
    : PabloKernel(ts, "BPE_BitFreq" + std::to_string(id->getNumElements()),
                  {Binding{"id", id}, Binding{"live", live}},
                  {},
                  {},
                  outputScalars(ts)) {}
protected:
    static std::vector<Binding> outputScalars(LLVMTypeSystemInterface & ts) {
        std::vector<Binding> out {Binding{ts.getSizeTy(), "total"}};
        for (unsigned n = kBitFreqLo; n <= kBitFreqHi; n++)
            out.push_back(Binding{ts.getSizeTy(), "atLeast" + std::to_string(n)});
        return out;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST*> id = getInputStreamSet("id");
        PabloAST * live = pb.createInFile(getInputStreamSet("live")[0]);
        pb.createAssign(getOutputScalarVar("total"), pb.createCount(live));
        // bits[N-1 .. W) has a 1, accumulated from the top bit down
        PabloAST * any = pb.createZeroes();
        for (unsigned n = kBitFreqHi; n >= kBitFreqLo; n--) {
            if (n - 1 < id.size()) any = pb.createOr(any, id[n - 1]);
            pb.createAssign(getOutputScalarVar("atLeast" + std::to_string(n)),
                            pb.createCount(pb.createAnd(any, live)));
        }
    }
};

static void bpe_report_bit_frequency(uint64_t total, uint64_t n10, uint64_t n11, uint64_t n12,
                                     uint64_t n13, uint64_t n14, uint64_t n15, uint64_t n16) {
    const uint64_t counts[] = {n10, n11, n12, n13, n14, n15, n16};
    std::cerr << "[BPE] bit frequency statistics: " << total << " tokens\n";
    for (unsigned n = kBitFreqLo; n <= kBitFreqHi; n++) {
        const uint64_t c = counts[n - kBitFreqLo];
        std::cerr << "[BPE]   ids of >= " << n << " bits (id >= " << (1u << (n - 1)) << "): "
                  << c << " (" << (total ? 100.0 * c / total : 0.0) << "%)\n";
    }
}

// --merge-frequency-statistics: after each merge kernel, count the merges it made
// (`merges`, the B starts it consumed) and, for each N in [kBitFreqLo, kBitFreqHi], the
// merges whose A and B ids both have at least N bits (`bothN`). B is a consumed start of
// the kernel's input live mask, A the live start before it, so A's size flag reaches B
// by an IndexedAdvance over that mask.
static cl::opt<bool> MergeFrequencyStatistics(
    "merge-frequency-statistics",
    cl::desc("BPE mode: report, per merge id bit tier, how many merges have both part ids "
             "of at least N bits, for N = 10..16."),
    cl::init(false));

static bool hasStreamInput(const Kernel * k, llvm::StringRef name);   // defined below

class BPEMergeFrequencyKernel : public PabloKernel {
public:
    BPEMergeFrequencyKernel(LLVMTypeSystemInterface & ts, StreamSet * id, StreamSet * liveIn,
                            StreamSet * liveOut)
    : PabloKernel(ts, "BPE_MergeFreq" + std::to_string(id->getNumElements()) + (liveIn ? "" : "_fa1"),
                  inputs(id, liveIn, liveOut),
                  {},
                  {},
                  outputScalars(ts)) {}
protected:
    // liveIn is null right after a FilterByMask compaction: every position is live then.
    static std::vector<Binding> inputs(StreamSet * id, StreamSet * liveIn, StreamSet * liveOut) {
        std::vector<Binding> in {Binding{"id", id}, Binding{"liveOut", liveOut}};
        if (liveIn) in.push_back(Binding{"liveIn", liveIn});
        return in;
    }
    static std::vector<Binding> outputScalars(LLVMTypeSystemInterface & ts) {
        std::vector<Binding> out {Binding{ts.getSizeTy(), "merges"}};
        for (unsigned n = kBitFreqLo; n <= kBitFreqHi; n++)
            out.push_back(Binding{ts.getSizeTy(), "both" + std::to_string(n)});
        return out;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST*> id = getInputStreamSet("id");
        PabloAST * liveIn = hasStreamInput(this, "liveIn")
            ? getInputStreamSet("liveIn")[0]
            : pb.createInFile(pb.createNot(pb.createZeroes()));
        PabloAST * consumed = pb.createInFile(pb.createAnd(liveIn, pb.createNot(getInputStreamSet("liveOut")[0])),
                                              "consumedB");
        pb.createAssign(getOutputScalarVar("merges"), pb.createCount(consumed));
        PabloAST * any = pb.createZeroes();   // id has a 1 in bits [N-1, W)
        for (unsigned n = kBitFreqHi; n >= kBitFreqLo; n--) {
            if (n - 1 < id.size()) any = pb.createOr(any, id[n - 1]);
            PabloAST * big = pb.createAnd(any, liveIn);
            PabloAST * prevBig = pb.createIndexedAdvance(big, liveIn, 1);
            pb.createAssign(getOutputScalarVar("both" + std::to_string(n)),
                            pb.createCount(pb.createAnd(consumed, pb.createAnd(big, prevBig))));
        }
    }
};

// merge statistics per output id width (tier): [0] merges, [1 + N - kBitFreqLo] bothN
static std::map<uint64_t, std::array<uint64_t, 2 + kBitFreqHi - kBitFreqLo>> gMergeFreq;

// One instance per tier: the tier is a template argument, not a call argument, because a
// pipeline constant scalar passed to a CreateCall crashes getFinalOutputScalars.
template <unsigned tier>
static void bpe_record_merge_frequency(uint64_t merges, uint64_t b10, uint64_t b11, uint64_t b12,
                                       uint64_t b13, uint64_t b14, uint64_t b15, uint64_t b16) {
    const uint64_t v[] = {merges, b10, b11, b12, b13, b14, b15, b16};
    auto & row = gMergeFreq[tier];
    for (unsigned i = 0; i < row.size(); i++) row[i] += v[i];
}

using MergeFrequencyRecorder = decltype(bpe_record_merge_frequency<16>);

template <unsigned... tiers>
static MergeFrequencyRecorder * mergeFrequencyRecorder(unsigned tier, std::integer_sequence<unsigned, tiers...>) {
    MergeFrequencyRecorder * fns[] = {bpe_record_merge_frequency<tiers>...};
    return tier < sizeof...(tiers) ? fns[tier] : nullptr;
}

void reportBPEStatistics() {
    if (gMergeFreq.empty()) return;
    std::array<uint64_t, 2 + kBitFreqHi - kBitFreqLo> all{};
    std::cerr << "[BPE] merge frequency statistics: merges whose A and B ids both have >= N bits\n"
              << "[BPE]   tier      merges";
    for (unsigned n = kBitFreqLo; n <= kBitFreqHi; n++) std::cerr << std::setw(11) << ("both>=" + std::to_string(n));
    std::cerr << "\n";
    auto printRow = [](const std::string & label, const auto & row) {
        std::cerr << "[BPE]   " << std::setw(4) << label << std::setw(12) << row[0];
        for (unsigned i = 1; i < row.size(); i++) std::cerr << std::setw(11) << row[i];
        std::cerr << "\n";
    };
    for (const auto & [tier, row] : gMergeFreq) {
        printRow(std::to_string(tier), row);
        for (unsigned i = 0; i < row.size(); i++) all[i] += row[i];
    }
    printRow("all", all);
    gMergeFreq.clear();
}

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

// ─── applyCompactionSchedule (--compaction) ──────────────────────────────────
// Decide WHERE to inject a FilterByMask compaction into the merge-kernel chain, and
// rewrite every rule's merge distance to match the resulting coordinate space. This
// is pure preprocessing.
//
// This is a preprocessing step that prepares the merge pipeline before any kernels are created.
// Returns compactAfter[i] = inject a compaction after merge kernel i. No compaction returns
// all-false and leaves every lenA untouched, so the byte-space path stays bit-exact.
static std::vector<bool> applyCompactionSchedule(std::vector<MergeRuleGroup> & ruleRanges) {
    //  The first block is always byte space (F=256), so the kernels before the first
    //  compaction see the original lenA = byte distance.
    const CompactionMode mode = compactionMode();
    const unsigned base = effCompactEvery();
    std::vector<bool> compactAfter(ruleRanges.size(), false);
    if (mode == CompactionMode::None) return compactAfter;

    // by-output-bits: kernel i ends its width when the next non-empty kernel writes a
    // wider id stream (the last kernel of its bit tier). Never after the last.
    std::vector<bool> widthGrowsAfter(ruleRanges.size(), false);
    if (mode == CompactionMode::ByOutputBits) {
        size_t prev = ruleRanges.size();
        for (size_t i = 0; i < ruleRanges.size(); i++) {
            if (ruleRanges[i].rules.empty()) continue;
            if (prev < ruleRanges.size()
                    && ceil_log2(ruleRanges[i].hi) > ceil_log2(ruleRanges[prev].hi))
                widthGrowsAfter[prev] = true;
            prev = i;
        }
    }

    // parts[idAB] = (idA, idB) for every merged token. Base ids (< 256) are absent —
    // they are atomic and terminate the recursion. lookup table
    // kernelOf[idAB] = index of the kernel that STAMPS that id, i.e. WHEN it is built.
    std::map<unsigned, std::pair<unsigned,unsigned>> parts;
    std::map<unsigned, size_t> kernelOf;
    for (size_t gi = 0; gi < ruleRanges.size(); gi++)
        for (const auto & r : ruleRanges[gi].rules) {
            if (ruleRanges[gi].tailOnly) break;   // never a kernel: nothing to measure
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
    // order, which --partition=level does not.
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
    CompactionCounter counter;
    unsigned nCompact = 0, firstBlockDisagree = 0, kernelNo = 0;
    for (size_t i = 0; i < ruleRanges.size(); i++) {
        auto & g = ruleRanges[i];
        if (g.rules.empty() || g.tailOnly) continue;
        ++kernelNo;
        unsigned maxDist = 0;
        for (auto & r : g.rules) {
            unsigned d = slotSpan(r.idA);
            if (nCompact == 0 && d != r.lenA) firstBlockDisagree++;
            r.lenA = d;
            if (d > maxDist) maxDist = d;
            // --partition=chain: the probe sits one slot past idB, so its slot-space distance
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
        const bool compact = (mode == CompactionMode::ByOutputBits)
            ? widthGrowsAfter[i] && kernelNo >= base
            : counter.step();
        if (!compact) continue;
        compactAfter[i] = true;
        lastCompactKernel = (long) i;
        memo.clear();
        nCompact++;
    }
    std::cerr << "[BPE] compaction: " << compactionName(mode) << " base=" << base << " -> "
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
//
// wordStart (--retire-words, else null): pretoken starts. A run then also breaks at a word
// start: once whole words are dropped from the stream, the last X of one word can sit right
// before the first X of another, and one joined run would shift the second word's pairing.
static PabloAST * selfMergeFireStarts(PabloBuilder & pb, PabloAST * isX, unsigned L,
                                      PabloAST * wordStart = nullptr) {
    // X-token body: the L bytes each X covers (start .. start+L-1). For L==1 this is
    // just isX; the body connects consecutive X tokens so MatchStar spans the gaps.
    PabloAST * body = isX;
    for (unsigned k = 1; k < L; k++)
        body = pb.createOr(body, pb.createAdvance(isX, (int64_t) k));
    // A run starts at an X with no X-token ending immediately L bytes before it.
    PabloAST * runStart = pb.createAnd(isX, pb.createNot(pb.createAdvance(isX, (int64_t) L)));
    if (wordStart) runStart = pb.createOr(runStart, pb.createAnd(isX, wordStart));
    PabloAST * A1 = pb.createEveryNth(isX, pb.getInteger(2));  // global 1st,3rd,5th…
    PabloAST * A2 = pb.createXor(isX, A1);                     // global 2nd,4th,6th…
    PabloAST * A1_start = pb.createAnd(runStart, A1);
    PabloAST * A2_start = pb.createAnd(runStart, A2);
    // Spread each run start over its run. With word starts, spread from the position after
    // the start through `cont` (the body minus word starts), so it stops before the next
    // word's first X instead of running into it.
    auto spread = [&](PabloAST * start) -> PabloAST * {
        if (!wordStart) return pb.createMatchStar(start, body);
        PabloAST * cont = pb.createAnd(body, pb.createNot(wordStart));
        PabloAST * reach = pb.createMatchStar(pb.createAdvance(start, (int64_t) 1), cont);
        return pb.createOr(start, pb.createAnd(reach, cont));
    };
    PabloAST * A1_runs = spread(A1_start);   // runs whose 1st X is global-odd
    PabloAST * A2_runs = spread(A2_start);   // runs whose 1st X is global-even
    // Per-run 1st,3rd,5th X = the fire (A-start) positions.
    return pb.createOr(pb.createAnd(A1_runs, A1), pb.createAnd(A2_runs, A2));
}

// Shared by every kernel below that takes an optional "meIn" (inPlayMask) input:
// omitted (nullptr) right after a FilterByMask compaction, where it's provably
// all-ones (every surviving position just passed that filter on the real mask).
static void bindOptional(std::vector<kernel::Binding> & in, const char * name, StreamSet * s) {
    if (s) in.push_back(Binding{name, s});
}
// true iff this kernel instance actually has a stream bound under `name` -- lets a
// kernel body ask "was meIn given?" without each kernel needing its own stored flag.
static bool hasStreamInput(const Kernel * k, llvm::StringRef name) {
    for (auto & b : k->getInputStreamSetBindings())
        if (b.getName() == name) return true;
    return false;
}

// Self-merges handled by emitSelfMergeBlock (--self-merge-block): every self-merge of the
// group, except a --chain-partition chain root (whose dependents nest in its own gate).
static std::vector<const MergeRule *> blockSelfMerges(const MergeRuleGroup & g) {
    std::vector<const MergeRule *> out;
    if (!SelfMergeBlock) return out;
    std::unordered_set<unsigned> chainParents;   // idA of every needsLiveId rule
    for (const auto & r : g.rules)
        if (r.needsLiveId) chainParents.insert(r.idA);
    for (const auto & r : g.rules)
        if (r.idA == r.idB && !chainParents.count(r.idAB)) out.push_back(&r);
    return out;
}

// emitSelfMergeBlock (--self-merge-block) — all of a kernel's self-merges X+X -> XX at once.
//   eq_L  = NOT OR_i (id_i XOR id_i L slots ahead): the token at p equals the next one,
//           when the token at p spans L slots. One per distinct span L of the rules' X.
//   E_L   = InFile(mask(L) AND eq_L AND CC(S_L)(id)), S_L = the X's of span L. The
//           membership test is required, not just a filter: for a token whose span is
//           not L, the id L slots ahead is a stale interior id and may match.
// A run of k equal tokens X at stride L gives k-1 consecutive E_L positions, and runs of
// different tokens never join (the E positions of XXYY are 2L apart), so the per-run
// pairing selfMergeFireStarts(E_L, L) fires at the 1st, 3rd, ... X of every run that has
// a partner. The OR of all E_L gates one if-block holding that pairing and the stamp,
// a bit transformation: change_j = CC_j(id) AND fire, where CC_j holds the X whose XX
// differs from X in bit j (X selects its rule, so one class serves every L).
// mask(L) = live start mask AND NOT (pretoken boundary at p+L); ahead(L) = the W id bits
// L slots ahead. Returns change Vars (W_out) and fire Vars per L, zero outside the block;
// the caller applies out_j = id_j XOR change_j and consumes Advance(fire_L, L).
struct SelfMergeBlockResult {
    std::vector<Var *> change;
    std::map<unsigned, Var *> fireByL;
};
static SelfMergeBlockResult emitSelfMergeBlock(
        PabloBuilder & pb, const std::vector<PabloAST *> & idBits, unsigned W_out,
        const std::vector<const MergeRule *> & rules,
        const std::function<std::vector<PabloAST *>(unsigned)> & ahead,
        const std::function<PabloAST *(unsigned)> & mask) {
    SelfMergeBlockResult res;
    if (rules.empty()) return res;
    const unsigned W = idBits.size();
    cc::Parabix_CC_Compiler ccc(idBits);
    std::map<unsigned, UCD::UnicodeSet> xByL;
    std::vector<UCD::UnicodeSet> changeSet(W_out);
    for (const MergeRule * r : rules) {
        xByL[r->lenA].insert(r->idA);
        for (unsigned j = 0; j < W_out; j++) {
            const unsigned inBit = (j < W) ? ((r->idA >> j) & 1u) : 0u;
            if (((r->idAB >> j) & 1u) != inBit) changeSet[j].insert(r->idA);
        }
    }
    std::map<unsigned, PabloAST *> E;
    PabloAST * anySelf = nullptr;
    for (auto & [L, xs] : xByL) {
        const std::vector<PabloAST *> nxt = ahead(L);
        PabloAST * diff = nullptr;
        for (unsigned i = 0; i < W; i++) {
            PabloAST * d = pb.createXor(idBits[i], nxt[i]);
            diff = diff ? pb.createOr(diff, d) : d;
        }
        PabloAST * isX = ccc.compileCC("selfX_L" + std::to_string(L), re::makeCC(std::move(xs)), pb);
        PabloAST * e = pb.createAnd(pb.createAnd(mask(L), isX), pb.createNot(diff));
        E[L] = pb.createInFile(e, "selfEq_L" + std::to_string(L));
        anySelf = anySelf ? pb.createOr(anySelf, E[L]) : E[L];
    }
    for (unsigned j = 0; j < W_out; j++)
        res.change.push_back(changeSet[j].empty() ? nullptr
                             : pb.createVar("selfChange_" + std::to_string(j), pb.createZeroes()));
    for (const auto & [L, e] : E)
        res.fireByL[L] = pb.createVar("selfFire_L" + std::to_string(L), pb.createZeroes());
    auto body = pb.createScope();
    PabloAST * fireAll = nullptr;
    for (const auto & [L, e] : E) {
        PabloAST * f = selfMergeFireStarts(body, e, L);
        body.createAssign(res.fireByL[L], f);
        fireAll = fireAll ? body.createOr(fireAll, f) : f;
    }
    for (unsigned j = 0; j < W_out; j++) {
        if (!res.change[j]) continue;
        PabloAST * cj = ccc.compileCC("selfChg_" + std::to_string(j), re::makeCC(std::move(changeSet[j])), body);
        body.createAssign(res.change[j], body.createAnd(cj, fireAll));
    }
    pb.createIf(anySelf, body);
    return res;
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
                        + (grouped ? (IfTestSignificantBits > 0
                                        ? "sbh" + std::to_string(IfTestSignificantBits) + "_"
                                          + (EmbeddedIfBits > 0 ? "eb" + std::to_string(EmbeddedIfBits) + "_" : "")
                                        : "g" + std::to_string(effGroupSize(group.rules.size())) + "_") : "")
                        + (LookaheadInGate ? "la1_" : "la0_")
                        + (LookaheadInGroup ? "lg1_" : "lg0_")
                        + (BatchWriteback ? "bw1_" : "bw0_")
                        + (isChainPartition() ? "cp1_" : "cp0_")
                        + (isChainVeto() ? "cv1_" : "cv0_")
                        + (ChainFuseSiblings ? "fs1_" : "fs0_")
                        + (ChainUngateRoots ? "ur1_" : "ur0_")
                        + (InFileOnce ? "io1_" : "")   // off = body identical to before → keep old names
                        + (GateMaskFold ? "gm1_" : "")
                        + (SkipInstCombine ? "ic1_" : "")
                        + (KeyCluster ? "kc1_" : "")
                        + (IfReduceTest ? "rt1_" : "")
                        + (grouped && IfUnlikelyWeight > 0 ? "uw" + std::to_string(IfUnlikelyWeight) + "_" : "")
                        + (RetireWords && boundaryIn ? "rw1_" : "")
                        + (meIn ? "" : "fa1_")   // meIn omitted: first kernel after a
                                                 // FilterByMask, its inPlayMask is built
                                                 // locally instead of bound as an input
                        + (!nextIdIn && !blockSelfMerges(group).empty() ? "smb_" : "")
                        + "w" + std::to_string(sourceIn->getNumElements())
                        + "o" + std::to_string(ceil_log2(group.hi))
                        + "L" + std::to_string(maxLen) + "_h" + std::to_string(shapeHash),
                  mergeInputs(sourceIn, meIn, boundaryIn, nextIdIn, maxLen),
                  {Binding{"sourceOut", sourceOut}, Binding{"meOut", meOut}}),
      mRuleGroup(group), mHasBoundary(boundaryIn != nullptr), mUseNextId(nextIdIn != nullptr),
      mGrouped(grouped) {}
protected:
    // --skip-instcombine: ask the driver to drop InstCombine for this kernel only.
    void addOptimizationPasses(KernelBuilder & b, SelectedOptimizationPasses & passes) const override {
        PabloKernel::addOptimizationPasses(b, passes);
        if (SkipInstCombine) passes.push_back(OptimizationPass::NoInstCombinePass);
    }
    // --if-unlikely-weight / --if-reduce-test (see their definitions)
    unsigned ifBodyUnlikelyWeight() const override { return mGrouped ? (unsigned) IfUnlikelyWeight : 0u; }
    bool reduceIfTest() const override { return IfReduceTest; }
    static std::vector<kernel::Binding> mergeInputs(
            StreamSet * sourceIn, StreamSet * meIn, StreamSet * boundaryIn,
            StreamSet * nextIdIn, unsigned maxLen) {
        std::vector<kernel::Binding> in {
            Binding{"sourceIn", sourceIn, FixedRate(), LookAhead(maxLen)} };
        bindOptional(in, "meIn", meIn);
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
        const unsigned W_out = ceil_log2(mRuleGroup.hi);
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
        // --infile-once: the end-of-file mask is applied here, once, instead of inside every
        // id compare (see eqId below).
        //
        // No meIn stream given (right after a FilterByMask). Every position here just
        // passed that filter, so it's already known to be live — build "all ones"
        // directly instead of reading it. This also lets every rule's AND(eqId,
        // inPlayMask) below collapse to just eqId for free.
        // Must use InFile (not a bare all-ones): the filtered stream's length rarely
        // lands on a block boundary, so a bare all-ones would leak into the leftover
        // padding and show up as junk tokens past the real end of input.
        PabloAST * const meInMasked = !hasStreamInput(this, "meIn")
            ? pb.createInFile(pb.createNot(pb.createZeroes()), "meInFile")
            : (InFileOnce ? pb.createInFile(getInputStreamSet("meIn")[0], "meInFile")
                          : getInputStreamSet("meIn")[0]);
        Var * inPlayMask = pb.createVar("inPlayMask", meInMasked);

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
        PabloAST * const runWordStart = RetireWords ? boundaryBit : nullptr;   // see selfMergeFireStarts
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
        PabloAST *  meInFrozen = meInMasked;

        // eqId(bits, v) = (bits == v): the same balanced AND tree the binary
        // cc::Parabix_CC_Compiler builds for a single value (bit_pattern_expr: one term per
        // bit, NOT for a 0 bit, pairwise AND reduction), so Pablo CSE shares the partial
        // products across compares in a scope exactly as compileCC's trees did. compileCC
        // then wraps the result in InFile; without --infile-once so does eqId (identical
        // body), with it the InFile is dropped — every gate and fire is ANDed with
        // meInFrozen/inPlayMask, which already carry it (meInMasked above).
        // `extra` (optional) is ANDed in as one more leaf, placed FIRST so it pairs with the
        // lowest tested bit: that product is shared by every compare over the same bits
        // (placed last, an even bit count would carry it to the root — one private AND).
        auto eqId = [&](auto & bld, const BixNum & bits, unsigned v,
                        const std::string & name = std::string(),
                        PabloAST * extra = nullptr) -> PabloAST * {
            std::vector<PabloAST*> terms;
            terms.reserve(bits.size() + 1);
            if (extra) terms.push_back(extra);
            for (unsigned i = 0; i < bits.size(); i++)
                terms.push_back(((v >> i) & 1u) ? bits[i] : bld.createNot(bits[i]));
            if (terms.empty()) terms.push_back(bld.createOnes());
            while (terms.size() > 1) {
                std::vector<PabloAST*> next;
                next.reserve(terms.size() / 2 + 1);
                for (size_t i = 0; i + 1 < terms.size(); i += 2)
                    next.push_back(bld.createAnd(terms[i], terms[i + 1]));
                if (terms.size() % 2 == 1) next.push_back(terms.back());
                terms.swap(next);
            }
            if (!InFileOnce)
                return bld.createInFile(terms[0], name.empty() ? std::string("expr") : name);
            if (!name.empty() && isa<And>(terms[0]))   // label only an And we built, never an input bit
                cast<Statement>(terms[0])->setName(bld.makeName(name));
            return terms[0];
        };

        // ── Batched write-back accumulators (--batch-writeback) ──────────────────
        // Each rule's gate only ORs its fire in here; the id-stamp write is applied
        // once at the end of the kernel. setBit[i] = positions where id bit i must
        // become 1, anyFire = positions where SOME rule fired (drives the bit clear).
        //
        // --partition=asymmetric can put two rules in the SAME kernel where the second
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
        // --partition=chain rules (a chain root and every rule nested under it) keep
        // the EAGER write-back: a parent stamps idAB and its child stamps idABC at the
        // SAME position, so the child must overwrite the parent, and the batch
        // accumulators only OR — they cannot express "the later one wins". Batching
        // stays ON for every OTHER rule in the same kernel, which is safe because the
        // two never collide:
        //   - STAMP: a rule fires at its A start, identified by the FROZEN id there.
        //     A chain position holds the root's idA, and all of a kernel's rules with
        //     that idA share one lenA and so probe one slot, where at most one idB can
        //     actually sit — so at most one of them fires. No batched rule ever fires
        //     where a chain rule does, hence anyFire is 0 there and flushWriteback's
        //     `idAcc & ~anyFire` preserves the eager chain stamp untouched.
        //   - MASK: a chain rule consumes at its idB's start. The maxLeft constraint
        //     forbids any same-kernel rule from having that idB as ITS idA (or, under
        //     --partition=chain, makes it stand down at runtime), so no batched rule reads
        //     the position a chain consume clears.
        //
        // childrenOf[idAB] = the rules in THIS group whose idA == idAB (i.e. every rule
        // that should nest inside idAB's own gate — see emitChain below). Empty, so
        // isChainRule is always false, whenever --partition=chain is off.
        std::unordered_map<unsigned, std::vector<const MergeRule*>> childrenOf;
        for (const auto & r : mRuleGroup.rules)
            if (r.needsLiveId) childrenOf[r.idA].push_back(&r);
        auto isChainRule = [&](const MergeRule & r) {
            return r.needsLiveId || childrenOf.count(r.idAB) != 0;
        };
        // --self-merge-block: these self-merges are resolved together after the other rules
        // (emitSelfMergeBlock) and skipped by every per-rule path below.
        const std::vector<const MergeRule *> blockSelf =
            mUseNextId ? std::vector<const MergeRule *>() : blockSelfMerges(mRuleGroup);
        const std::unordered_set<const MergeRule *> inSelfBlock(blockSelf.begin(), blockSelf.end());
        const bool batch     = BatchWriteback && !mUseNextId;  // defer the id-stamp
        const bool deferMask = batch && !groupNeedsLiveMask;   // also defer the mask consume
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
                    if (!isChainRule(r) && !inSelfBlock.count(&r) && fireByLen.find(r.lenA) == fireByLen.end())
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
        // --partition=level symmetric seam) this is a no-op — conflict-freedom already
        // guarantees no rule's fire touches a position another same-kernel rule reads 
        auto eqAstart = [&](auto & bld, unsigned id) -> PabloAST * {
            return bld.createAnd(eqId(bld, srcFrozen, id), inPlayMask, "Astart_" + std::to_string(id));
        };

        // --partition=chain: suppress this rule where a lower-rank competitor claims its idB.
        // Reads the id one slot PAST idB (r.vetoOff) from the frozen kernel input; if it
        // is that competitor's right part, the competitor applies here, wins on rank, and
        // this merge must not fire. One peek serves every competitor (all probe the same
        // slot), one EQ each. Returns `fire` untouched when the rule carries no veto, so
        // both emission paths call it unconditionally. Reads nothing but the kernel
        // input, which is why it needs neither nesting nor --partition=chain.
        auto applyVeto = [&](auto & body, const MergeRule & r, PabloAST * fire) -> PabloAST * {
            if (r.vetoIdB.empty()) return fire;
            if (mUseNextId)   // --indexed-shift: no byte-distance LookAhead exists here
                llvm::report_fatal_error("--partition=chain is incompatible with --indexed-shift");
            std::vector<PabloAST*> vbits(W);
            for (unsigned i = 0; i < W; i++)
                vbits[i] = body.createLookahead(srcBits[i], (int64_t) r.vetoOff);
            const BixNum vetoBits(vbits.begin(), vbits.end());
            PabloAST * veto = nullptr;
            for (unsigned vid : r.vetoIdB) {
                PabloAST * hit = eqId(body, vetoBits, vid,
                                      "veto_" + std::to_string(r.idB) + "_" + std::to_string(vid));
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
                            const std::map<unsigned, PabloAST*> * grpBoundary = nullptr,
                            const BixNum * bPeek = nullptr, unsigned bLowDone = 0) {
            //indexed mode reads the next-live id, byte mode reads lenA ahead via LookAhead.
            // Peek source precedence: indexed nextId > group-cache > per-rule in-gate > hoisted.
            PabloAST * BstartAtA;
            const std::string bstartName = "Bstart_" + std::to_string(r.idA) + "_" + std::to_string(r.idB);
            if (bPeek) {
                // --embedded-if-bits: the caller's inner-if condition already matched the
                // low bLowDone bits of idB on bPeek; test only the remaining high bits.
                if (bLowDone >= bPeek->size()) {
                    BstartAtA = nullptr;
                } else {
                    BstartAtA = eqId(body, BixNum(bPeek->begin() + bLowDone, bPeek->end()),
                                     r.idB >> bLowDone, bstartName);
                }
            } else if (mUseNextId) {
                BstartAtA = eqId(body, nextIdBN, r.idB, bstartName);
            } else if (grpAhead) {   // chunk-cached shared peek (built once per lenA in the gate)
                BstartAtA = eqId(body, grpAhead->at(r.lenA), r.idB, bstartName);
            } else if (LookaheadInGate) {
                std::vector<PabloAST*> bits(W);
                for (unsigned i = 0; i < W; i++)
                    bits[i] = body.createLookahead(srcBits[i], (int64_t) r.lenA);
                BstartAtA = eqId(body, BixNum(bits.begin(), bits.end()), r.idB, bstartName);
            } else {
                BstartAtA = eqId(body, aheadByLenA.at(r.lenA), r.idB, bstartName);
            }
            PabloAST * fire = !BstartAtA ? fireStart : body.createAnd(fireStart, BstartAtA, "fire1_" + std::to_string(r.idA) + "_" + std::to_string(r.idB) + "_" + std::to_string(r.idAB));
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
                fireStart = selfMergeFireStarts(pb, fireStart, r.lenA, runWordStart);
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

        // ── --partition=chain: REAL nested createIf ─────────────────────────────
        // A rule flagged needsLiveId (its idA was stamped by an earlier SAME-level
        // rule — see levelPartition's chain-partition branch) is emitted physically
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
        // childrenOf is built above the write-back accumulators — isChainRule needs it.
        //
        // One rule's B-detect + stamp + consume, built fresh (not shared with
        // emitBody — deliberately isolated). Returns `fire` (Astart AND B-detect) so
        // a nested child can use it as ITS gate condition directly.
        auto emitChainBody = [&](PabloBuilder & body, const MergeRule & r,
                                  PabloAST * fireStart) -> PabloAST * {
            std::vector<PabloAST*> bits(W);
            for (unsigned i = 0; i < W; i++)
                bits[i] = body.createLookahead(srcBits[i], (int64_t) r.lenA);
            PabloAST * BstartAtA = eqId(body, BixNum(bits.begin(), bits.end()), r.idB,
                                        "Bstart_" + std::to_string(r.idA) + "_" + std::to_string(r.idB));
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

        // emitChainInto: r's body straight into `scope` (no gate of its own), then r's
        // chain-children nested on r's `fire`. emitChain: same, wrapped in createIf(fireStart).
        std::function<void(PabloBuilder&, const MergeRule&, PabloAST*)> emitChainInto;
        auto emitChain = [&](PabloBuilder & bld, const MergeRule & r, PabloAST * fireStart) {
            auto body = bld.createScope();
            emitChainInto(body, r, fireStart);
            bld.createIf(fireStart, body);
        };
        emitChainInto = [&](PabloBuilder & scope, const MergeRule & r, PabloAST * fireStart) {
            PabloAST * fire = emitChainBody(scope, r, fireStart);
            auto it = childrenOf.find(r.idAB);
            if (it == childrenOf.end()) return;
            if (ChainFuseSiblings) {
                auto kids = scope.createScope();
                for (const MergeRule * child : it->second)
                    emitChainInto(kids, *child, fire);
                scope.createIf(fire, kids);
            } else {
                for (const MergeRule * child : it->second)
                    emitChain(scope, *child, fire);   // one createIf per child
            }
        };

        if (!mGrouped) {
            for (const auto & r : mRuleGroup.rules) {
                if (r.needsLiveId) continue;      // emitted as a nested child above, not a root
                if (inSelfBlock.count(&r)) continue;
                if (!childrenOf.count(r.idAB)) {
                    emitRule(r);                   // no chain involved — untouched path
                    continue;
                }
                // Root of a chain: same Astart as emitRule, then nest the dependents
                // inside via emitChain instead of a flat sibling loop.
                PabloAST * fireStart = eqAstart(pb, r.idA);
                if (r.idA == r.idB)
                    fireStart = selfMergeFireStarts(pb, fireStart, r.lenA, runWordStart);
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
            //
            // --if-test-significant-bits=K (high-bit prefix): groups are instead the runs of
            // sorted rules whose idA share the same K significant bits, counted from the
            // highest 1 bit of idA. For idA of bit-length L, loBits = max(0, L-K) and the
            // group key is idA >> loBits (which also fixes L, since its top bit is 1). The
            // gate tests bits [loBits, W_out) against that key (the K significant bits plus
            // the zero bits above them); inside the gate each rule's Astart ANDs in the EQ
            // of the low loBits bits, so the id test is split across the gate and the body
            // rather than paid in full outside (range gate) and again inside (per-rule EQ).
            // Sorted by idA → equal keys are contiguous (same L, same prefix).
            //
            // --partition=chain: only the nested CHILDREN sit out the chunk loop —
            // emitChain emits each of them physically inside its producer's gate, so
            // they must not also appear as a chunk rule. Chain ROOTS stay in, and get
            // their nest built inside the chunk gate (see emitChain call below), so a
            // chain costs one gate shared with ~14 neighbours instead of a flat if of
            // its own. Grouping is therefore decided per RULE, not per kernel: the old
            // per-kernel exclusion sent all ~170 rules of any chain-bearing kernel back
            // to one createIf each — 94.7% of all rules, measured.
            std::vector<const MergeRule*> sorted;
            sorted.reserve(mRuleGroup.rules.size());
            for (const auto & r : mRuleGroup.rules)
                if (!r.needsLiveId && !inSelfBlock.count(&r)) sorted.push_back(&r);
            std::sort(sorted.begin(), sorted.end(),
                      [](const MergeRule* a, const MergeRule* b){ return a->idA < b->idA; });

            const unsigned hiBits = std::min<unsigned>(IfTestSignificantBits, W_out);
            // Number of low bits of idA below its K significant bits.
            auto loBitsOf = [&](unsigned idA) -> unsigned {
                const unsigned L = (idA == 0) ? 0 : 32u - __builtin_clz(idA);
                return (L > hiBits) ? L - hiBits : 0;
            };

            // [s,e) ranges of `sorted` forming each gate.
            std::vector<std::pair<size_t, size_t>> groups;
            if (hiBits > 0) {
                for (size_t s = 0; s < sorted.size(); ) {
                    const unsigned loBits = loBitsOf(sorted[s]->idA);
                    const unsigned prefix = sorted[s]->idA >> loBits;
                    size_t e = s + 1;
                    while (e < sorted.size() && loBitsOf(sorted[e]->idA) == loBits
                           && (sorted[e]->idA >> loBits) == prefix) e++;
                    groups.emplace_back(s, e);
                    s = e;
                }
            } else {
                const unsigned GROUP_SIZE = effGroupSize(mRuleGroup.rules.size());
                for (size_t s = 0; s < sorted.size(); s += GROUP_SIZE)
                    groups.emplace_back(s, std::min(sorted.size(), s + GROUP_SIZE));
            }

            for (const auto & [s, e] : groups) {
                PabloAST * inRange;
                // Per-group split point (high-bit prefix only): all rules in the group share it.
                const unsigned loBits = (hiBits > 0) ? loBitsOf(sorted[s]->idA) : 0;
                // Every rule in the group has the same idA and the gate tests all of it.
                const bool gatePinsIdA = (hiBits > 0) ? (loBits == 0)
                                                      : (sorted[s]->idA == sorted[e-1]->idA);
                const BixNum loFrozen(frozenBits.begin(), frozenBits.begin() + loBits);
                if (hiBits > 0) {
                    const unsigned prefix = sorted[s]->idA >> loBits;
                    const BixNum hiFrozen(frozenBits.begin() + loBits, frozenBits.end());
                    // --gate-mask-fold: the mask rides inside the shared tree, so inRange
                    // IS the gate condition (prefixAstart still ANDs the live inPlayMask).
                    inRange = eqId(pb, hiFrozen, prefix,
                                   "idHi_" + std::to_string(loBits) + "_" + std::to_string(prefix),
                                   GateMaskFold ? meInFrozen : nullptr);
                } else {
                    unsigned gLo = sorted[s]->idA, gHi = sorted[e-1]->idA;   // sorted → tight range
                    cc::Parabix_CC_Compiler_Builder ccId(frozenBits);
                    inRange = ccId.compileCC(re::makeCC(gLo, gHi), pb);
                }
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
                // High-bit prefix: Astart = hiMatch AND EQ(low bits) AND inPlayMask, with the
                // low-bit EQ built once per distinct idA in the group.
                std::map<unsigned, PabloAST*> idMatchInGroup;
                auto prefixAstart = [&](unsigned idA) -> PabloAST * {
                    auto f = idMatchInGroup.find(idA);
                    if (f == idMatchInGroup.end()) {
                        PabloAST * m = inRange;
                        if (loBits > 0) {
                            const unsigned lo = idA & ((1u << loBits) - 1u);
                            m = body.createAnd(m, eqId(body, loFrozen, lo), "idMatch_" + std::to_string(idA));
                        }
                        f = idMatchInGroup.emplace(idA, m).first;
                    }
                    return body.createAnd(f->second, inPlayMask, "Astart_" + std::to_string(idA));
                };
                // --embedded-if-bits=E: the E-bit budget covers the loBits idA bits the
                // gate left untested, then the lowest bLow = E - loBits bits of idB.
                const unsigned embedBits = (hiBits > 0) ? EmbeddedIfBits.getValue() : 0;
                const unsigned bLow = (embedBits > loBits) ? embedBits - loBits : 0;
                // Peek at the id lenA ahead (B's start), using the same source emitBody
                // would: indexed nextId > group-cache > per-rule in-gate > hoisted.
                auto bPeekFor = [&](const MergeRule & r) -> BixNum {
                    if (mUseNextId) return nextIdBN;
                    if (grpAhead) return grpAhead->at(r.lenA);
                    if (LookaheadInGate) {
                        std::vector<PabloAST*> bits(W);
                        for (unsigned i = 0; i < W; i++)
                            bits[i] = body.createLookahead(srcBits[i], (int64_t) r.lenA);
                        return BixNum(bits.begin(), bits.end());
                    }
                    return aheadByLenA.at(r.lenA);
                };
                for (size_t k = s; k < e; k++) {
                    const MergeRule & r = *sorted[k];
                    PabloAST * fireStart = (hiBits > 0) ? prefixAstart(r.idA) : eqAstart(body, r.idA);
                    if (r.idA == r.idB)
                        fireStart = selfMergeFireStarts(body, fireStart, r.lenA, runWordStart);
                    // Chain root: build its own createIf inside THIS chunk gate and nest
                    // its children in it, instead of emitting flat at the outer scope.
                    // The root's gate is then doubly guarded (chunk prefix, then its own
                    // Astart), and a cold chunk skips the whole chain in one test.
                    // (--embedded-if-bits does not apply: the root's own gate already
                    // plays the part of its inner if.)
                    if (childrenOf.count(r.idAB)) {
                        if (ChainUngateRoots && gatePinsIdA && r.idA != r.idB)
                            emitChainInto(body, r, fireStart);  // own if would re-test the gate
                        else
                            emitChain(body, r, fireStart);
                        continue;
                    }
                    if (embedBits == 0) {
                        emitBody(body, r, fireStart, grpAhead, grpBoundary);
                        continue;
                    }
                    const BixNum bPeek = bPeekFor(r);
                    const unsigned bLowUsed = std::min<unsigned>(bLow, bPeek.size());
                    PabloAST * embedCond = fireStart;
                    if (bLowUsed > 0) {
                        const unsigned lowB = r.idB & ((1u << bLowUsed) - 1u);
                        PabloAST * bLoMatch = eqId(body, BixNum(bPeek.begin(), bPeek.begin() + bLowUsed), lowB,
                            "idBlo_" + std::to_string(bLowUsed) + "_" + std::to_string(lowB));
                        embedCond = body.createAnd(fireStart, bLoMatch,
                            "embed_" + std::to_string(r.idA) + "_" + std::to_string(r.idB));
                    }
                    auto inner = body.createScope();
                    emitBody(inner, r, embedCond, grpAhead, grpBoundary, &bPeek, bLowUsed);
                    body.createIf(embedCond, inner);
                }
                const bool maskInRange = GateMaskFold && hiBits > 0;
                pb.createIf(maskInRange ? inRange : pb.createAnd(inRange, meInFrozen), body);
            }
        }

        // ── Apply whatever's left in the batch accumulators ─────────────────────
        if (batch) flushWriteback();

        // ── --self-merge-block ───────────────────────────────────────────────────
        // Read from the frozen input like every other rule; its fire and consume
        // positions are disjoint from theirs (seam constraints), so XOR-ing its change
        // into the accumulated ids after them equals applying it in rank order.
        if (!blockSelf.empty()) {
            auto ahead = [&](unsigned L) -> std::vector<PabloAST *> {
                auto f = aheadByLenA.find(L);
                if (f != aheadByLenA.end()) return std::vector<PabloAST *>(f->second.begin(), f->second.end());
                std::vector<PabloAST *> bits(W);
                for (unsigned i = 0; i < W; i++) bits[i] = pb.createLookahead(srcBits[i], (int64_t) L);
                return bits;
            };
            auto mask = [&](unsigned L) -> PabloAST * {
                if (!mHasBoundary) return meInFrozen;
                return pb.createAnd(meInFrozen, pb.createNot(pb.createLookahead(boundaryBit, (int64_t) L)));
            };
            auto res = emitSelfMergeBlock(pb, srcBits, W_out, blockSelf, ahead, mask);
            for (unsigned i = 0; i < W_out; i++)
                if (res.change[i]) pb.createAssign(idAcc[i], pb.createXor(idAcc[i], res.change[i]));
            for (const auto & [L, fire] : res.fireByL)
                pb.createAssign(inPlayMask,
                    pb.createAnd(inPlayMask, pb.createNot(pb.createAdvance(fire, (int64_t) L))));
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
// ─── BPEXfrmKernel (--bit-xfrm-limit) ───────────────────────────────────────
// A merge kernel built as a set of bit transformations instead of per-rule gates. Same
// bindings and semantics as BPEMergeKernel; it handles groups whose rules read only the
// frozen kernel input (no --partition=chain nest, --partition=asymmetric live mask or
// --partition=chain; see bitXfrmEligible).
//
// For an input id stream of N <= kXfrmLowBits bits, rules are bucketed by L = lenA, the
// lookahead distance at which their B part starts. For each L the kernel forms one 2N-bit
// vector
//     V_L = [ id bits 0..N-1 at p ,  id bits 0..N-1 at p+L ]
// so a rule's (idA, idB) pair is the value idA | idB << N. For every output bit j, the
// rules of that bucket whose idAB bit j differs from idA bit j (for j >= N: whose idAB
// bit j is set, the input bit being 0) form a set of such values; cc::Parabix_CC_Compiler
// compiles it over V_L, and
//     change_j = OR over L of ( CC_{L,j}(V_L) AND live AND NOT boundary(p+L) )
//     out_j    = in_j XOR change_j.
// The union of a bucket's values, under the same mask, is its fire stream, and the B
// starts are consumed as in BPEMergeKernel: live &= NOT Advance(fire_L, L).
//
// Subtiers (N > kXfrmLowBits): re::CC codepoints stop at 0x10FFFF, so a pair value can
// hold 20 bits. The classes take the low 20-N bits of idA and all N bits of idB; the
// remaining high 2N-20 bits of idA name the rule's subtier, and each subtier present in
// the kernel is one block: its classes ANDed with (idA's subtier bits at p == the key),
// all L sharing the block. --subtier-gating-bits puts a block in an if when its key
// shows a large idA; --subtier-grouping-by-min-ID keys rules with idA > idB on idB.
//
// Exactness: within one kernel at most one rule fires at a position (the frozen id there
// selects idA, and rules sharing idA share L and so read one B slot), and same-kernel
// rules never touch each other's positions (dependency + seam constraints), so XOR-ing
// all change streams from the frozen input equals applying the rules one by one.
//
// Self-merges X+X need per-run pairing (selfMergeFireStarts), which a character class
// cannot express; they keep a per-rule stamp here, or join --self-merge-block. The seam
// constraints keep every other rule off their positions, so the stamp and the change
// streams never meet.
static constexpr unsigned kXfrmLowBits = 10;

class BPEXfrmKernel : public PabloKernel {
public:
    BPEXfrmKernel(LLVMTypeSystemInterface & ts,
                  StreamSet * sourceIn, StreamSet * meIn, StreamSet * boundaryIn,
                  StreamSet * sourceOut, StreamSet * meOut,
                  MergeRuleGroup group, uint64_t shapeHash, unsigned maxLen)
    : PabloKernel(ts, std::string("BPEXfrm_") + (boundaryIn ? "b1_" : "")
                        + (SkipInstCombine ? "ic1_" : "")
                        + (RetireWords && boundaryIn ? "rw1_" : "")
                        + (sourceIn->getNumElements() > kXfrmLowBits
                               ? std::string("st_")
                                 + (subtierGroupingApplies(ceil_log2(group.hi)) ? "gm_" : "")
                                 + (SubtierGatingBits > 0 ? "sg" + std::to_string(SubtierGatingBits) + "_" : "")
                               : std::string())
                        + (!blockSelfMerges(group).empty() ? "smb_" : "")
                        + (meIn ? "" : "fa1_")   // meIn omitted: first kernel after a
                                                 // FilterByMask, inPlayMask built locally
                        + "w" + std::to_string(sourceIn->getNumElements())
                        + "o" + std::to_string(ceil_log2(group.hi))
                        + "L" + std::to_string(maxLen) + "_h" + std::to_string(shapeHash),
                  xfrmInputs(sourceIn, meIn, boundaryIn, maxLen),
                  {Binding{"sourceOut", sourceOut}, Binding{"meOut", meOut}}),
      mRuleGroup(std::move(group)), mHasBoundary(boundaryIn != nullptr) {}
protected:
    void addOptimizationPasses(KernelBuilder & b, SelectedOptimizationPasses & passes) const override {
        PabloKernel::addOptimizationPasses(b, passes);
        if (SkipInstCombine) passes.push_back(OptimizationPass::NoInstCombinePass);
    }
    static std::vector<kernel::Binding> xfrmInputs(StreamSet * sourceIn, StreamSet * meIn,
                                                   StreamSet * boundaryIn, unsigned maxLen) {
        std::vector<kernel::Binding> in {
            Binding{"sourceIn", sourceIn, FixedRate(), LookAhead(maxLen)} };
        bindOptional(in, "meIn", meIn);
        if (boundaryIn)
            in.push_back(Binding{"boundaryIn", boundaryIn, FixedRate(), LookAhead(maxLen)});
        return in;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST*> srcBits = getInputStreamSet("sourceIn");
        const unsigned N = srcBits.size();
        const unsigned W_out = ceil_log2(mRuleGroup.hi);
        // No meIn stream given (right after a FilterByMask): every position here just
        // passed that filter, so it's already known to be live -- build "all ones"
        // directly (InFile, so leftover block padding doesn't read as live).
        PabloAST * const live = hasStreamInput(this, "meIn")
            ? getInputStreamSet("meIn")[0]
            : pb.createInFile(pb.createNot(pb.createZeroes()), "meInFile");
        PabloAST * const boundaryBit = mHasBoundary ? getInputStreamSet("boundaryIn")[0] : nullptr;

        // Per lookahead distance L: the CC rules and the self-merges.
        std::map<unsigned, std::vector<const MergeRule *>> byLen;
        std::vector<const MergeRule *> selfMerges;
        for (const auto & r : mRuleGroup.rules)
            (r.idA == r.idB ? selfMerges : byLen[r.lenA]).push_back(&r);

        std::map<unsigned, std::vector<PabloAST *>> aheadBits;   // id bits L slots ahead
        auto ahead = [&](unsigned L) -> const std::vector<PabloAST *> & {
            auto & v = aheadBits[L];
            if (v.empty())
                for (unsigned i = 0; i < N; i++) v.push_back(pb.createLookahead(srcBits[i], (int64_t) L));
            return v;
        };
        // gate(L) = live AND NOT (a pretoken boundary at B's start)
        auto gate = [&](unsigned L) -> PabloAST * {
            if (!mHasBoundary) return live;
            return pb.createAnd(live, pb.createNot(pb.createLookahead(boundaryBit, (int64_t) L)),
                                "gate_L" + std::to_string(L));
        };

        std::vector<PabloAST *> change(W_out, nullptr);
        PabloAST * consumed = nullptr;   // B starts to clear from the live mask
        auto orInto = [&](PabloAST *& acc, PabloAST * v) { acc = acc ? pb.createOr(acc, v) : v; };

        // Ids of N bits: the classes take the low NloA bits of idA and all N bits of idB.
        // Subtiers (H): N > kXfrmLowBits, NloA = 2*kXfrmLowBits - N, and the high N-NloA
        // bits of idA (the subtier key) select a block.
        const bool H = N > kXfrmLowBits;
        const unsigned NloA = H ? 2 * kXfrmLowBits - N : N, NloB = N;
        const unsigned loMaskA = (1u << NloA) - 1u, loMaskB = (1u << NloB) - 1u;
        // --subtier-grouping-by-min-ID: rules with idA > idB (group B) swap the roles: idB
        // gives the NloA keyed bits, idA all NloB bits.
        const bool byMin = H && subtierGroupingApplies(W_out);

        // Compile one block's sets in `bld` over the low-bit pair vector, masked by `mask`,
        // handing each result to addFire / addChange. keyB: the block is keyed on idB
        // (group B), so idB's bits come first in the pair vector.
        auto buildSets = [&](auto & bld, const std::vector<const MergeRule *> & rs,
                             const std::vector<PabloAST *> & bBits, PabloAST * mask, bool keyB,
                             const std::string & tag, auto && addFire, auto && addChange) {
            const std::vector<PabloAST *> & keyed = keyB ? bBits : srcBits;
            const std::vector<PabloAST *> & other = keyB ? srcBits : bBits;
            std::vector<PabloAST *> pairBits(keyed.begin(), keyed.begin() + NloA);
            pairBits.insert(pairBits.end(), other.begin(), other.begin() + NloB);
            cc::Parabix_CC_Compiler ccc(pairBits);
            UCD::UnicodeSet fireSet;
            std::vector<UCD::UnicodeSet> changeSet(W_out);
            for (const MergeRule * r : rs) {
                const unsigned k = keyB ? r->idB : r->idA, o = keyB ? r->idA : r->idB;
                const re::codepoint_t v = (k & loMaskA) | ((o & loMaskB) << NloA);
                fireSet.insert(v);
                for (unsigned j = 0; j < W_out; j++) {
                    const unsigned inBit = (j < N) ? ((r->idA >> j) & 1u) : 0u;
                    if (((r->idAB >> j) & 1u) != inBit) changeSet[j].insert(v);
                }
            }
            addFire(bld.createAnd(ccc.compileCC("fire" + tag, re::makeCC(std::move(fireSet)), bld), mask));
            for (unsigned j = 0; j < W_out; j++) {
                if (changeSet[j].empty()) continue;
                PabloAST * cj = ccc.compileCC("chg" + std::to_string(j) + tag,
                                              re::makeCC(std::move(changeSet[j])), bld);
                addChange(j, bld.createAnd(cj, mask));
            }
        };

        // H: accumulators the subtier blocks OR into (Pablo joins them at each if's end).
        std::vector<Var *> changeVar(W_out, nullptr);
        auto changeAcc = [&](unsigned j) -> Var * {
            if (!changeVar[j]) changeVar[j] = pb.createVar("change_" + std::to_string(j), pb.createZeroes());
            return changeVar[j];
        };
        // (high bits [lo, N) of `bits`) == v, as one AND tree; null when lo == N
        auto eqHigh = [&](const std::vector<PabloAST *> & bits, unsigned lo, unsigned v) -> PabloAST * {
            PabloAST * e = nullptr;
            for (unsigned i = lo; i < N; i++) {
                PabloAST * t = ((v >> (i - lo)) & 1u) ? bits[i] : pb.createNot(bits[i]);
                e = e ? pb.createAnd(e, t) : t;
            }
            return e;
        };

        if (!H) {
            for (const auto & [L, rules] : byLen) {
                PabloAST * fire = nullptr;
                buildSets(pb, rules, ahead(L), gate(L), false, "_L" + std::to_string(L),
                          [&](PabloAST * f) { fire = f; },
                          [&](unsigned j, PabloAST * c) { orInto(change[j], c); });
                orInto(consumed, pb.createAdvance(fire, (int64_t) L));
            }
        } else {
            // One block per subtier, all L sharing it. Group B blocks (--subtier-grouping-
            // by-min-ID) are keyed on idB's high bits, read L slots ahead, so each has one L.
            // Key: (group: 0 = keyed on idA, 1 = on idB; L, or 0 for group 0; keyed high bits).
            std::map<std::tuple<unsigned, unsigned, unsigned>,
                     std::map<unsigned, std::vector<const MergeRule *>>> blocks;
            for (const auto & [L, rules] : byLen)
                for (const MergeRule * r : rules) {
                    if (byMin && r->idA > r->idB)
                        blocks[{1u, L, r->idB >> NloA}][L].push_back(r);
                    else
                        blocks[{0u, 0u, r->idA >> NloA}][L].push_back(r);
                }
            std::map<unsigned, Var *> fireL;
            for (const auto & [L, rules] : byLen)
                fireL[L] = pb.createVar("fire_L" + std::to_string(L), pb.createZeroes());
            // Emit one block's sets into `bld`: the if body, or the top scope when ungated.
            auto emitSets = [&](auto & bld, const std::map<unsigned, std::vector<const MergeRule *>> & rulesByL,
                                PabloAST * cond, bool keyB, const std::string & btag) {
                for (const auto & [L, rs] : rulesByL) {
                    // the condition already carries `live`; add L's boundary gate if any
                    PabloAST * mask = (keyB || !mHasBoundary) ? cond : bld.createAnd(cond, gate(L));
                    Var * fire = fireL[L];
                    buildSets(bld, rs, ahead(L), mask, keyB, btag + (keyB ? "" : "_L" + std::to_string(L)),
                              [&](PabloAST * f) { bld.createAssign(fire, bld.createOr(fire, f)); },
                              [&](unsigned j, PabloAST * c) {
                                  Var * acc = changeAcc(j);
                                  bld.createAssign(acc, bld.createOr(acc, c));
                              });
                }
            };
            for (const auto & [key, rulesByL] : blocks) {
                const auto & [grp, keyL, hiK] = key;
                const bool keyB = grp == 1;
                const std::string btag = (keyB ? "_L" + std::to_string(keyL) + "_kb" : "_a") + std::to_string(hiK);
                PabloAST * hiEq = eqHigh(keyB ? ahead(keyL) : srcBits, NloA, hiK);
                PabloAST * cond = pb.createAnd(hiEq, keyB ? gate(keyL) : live, "hi" + btag);
                // --subtier-gating-bits: gate only a subtier whose keyed part has >= K bits.
                const unsigned keyedBits = hiK ? (32u - __builtin_clz(hiK)) + NloA : 0u;
                const bool gated = SubtierGatingBits == 0 || keyedBits >= SubtierGatingBits;
                if (!gated) {
                    emitSets(pb, rulesByL, cond, keyB, btag);
                    continue;
                }
                auto body = pb.createScope();
                emitSets(body, rulesByL, cond, keyB, btag);
                pb.createIf(cond, body);
            }
            for (const auto & [L, fire] : fireL)
                orInto(consumed, pb.createAdvance(fire, (int64_t) L));
        }
        for (unsigned j = 0; j < W_out; j++)
            if (changeVar[j]) orInto(change[j], changeVar[j]);

        // --self-merge-block: the self-merges join the bit transformation as one gated block.
        const std::vector<const MergeRule *> blockSelf = blockSelfMerges(mRuleGroup);
        if (!blockSelf.empty()) {
            auto res = emitSelfMergeBlock(pb, srcBits, W_out, blockSelf,
                                          [&](unsigned L) { return ahead(L); }, gate);
            for (unsigned j = 0; j < W_out; j++)
                if (res.change[j]) orInto(change[j], res.change[j]);
            for (const auto & [L, fire] : res.fireByL)
                orInto(consumed, pb.createAdvance(fire, (int64_t) L));
            selfMerges.clear();
        }

        // out_j = in_j XOR change_j
        std::vector<PabloAST *> outBits(W_out);
        for (unsigned j = 0; j < W_out; j++) {
            PabloAST * in = (j < N) ? srcBits[j] : pb.createZeroes();
            outBits[j] = change[j] ? pb.createXor(in, change[j], "xfrm_" + std::to_string(j)) : in;
        }

        // Self-merges: per-run pairing, then an explicit stamp of idAB.
        for (const MergeRule * r : selfMerges) {
            std::vector<PabloAST *> terms;
            auto eqTerms = [&](const std::vector<PabloAST *> & bits, unsigned id) {
                PabloAST * e = nullptr;
                for (unsigned i = 0; i < N; i++) {
                    PabloAST * t = ((id >> i) & 1u) ? bits[i] : pb.createNot(bits[i]);
                    e = e ? pb.createAnd(e, t) : t;
                }
                return e;
            };
            PabloAST * isX = pb.createInFile(pb.createAnd(eqTerms(srcBits, r->idA), live));
            PabloAST * fire = selfMergeFireStarts(pb, isX, r->lenA,
                                                  RetireWords ? boundaryBit : nullptr);
            fire = pb.createAnd(fire, eqTerms(ahead(r->lenA), r->idB));
            fire = pb.createAnd(fire, gate(r->lenA), "selffire_" + std::to_string(r->idAB));
            orInto(consumed, pb.createAdvance(fire, (int64_t) r->lenA));
            for (unsigned j = 0; j < W_out; j++)
                outBits[j] = ((r->idAB >> j) & 1u) ? pb.createOr(outBits[j], fire)
                                                   : pb.createAnd(outBits[j], pb.createNot(fire));
        }

        Var * sOut = getOutputStreamVar("sourceOut");
        for (unsigned j = 0; j < W_out; j++)
            pb.createAssign(pb.createExtract(sOut, pb.getInteger(j)), outBits[j]);
        PabloAST * meOut = consumed ? pb.createAnd(live, pb.createNot(consumed), "liveOut") : live;
        pb.createAssign(pb.createExtract(getOutputStreamVar("meOut"), pb.getInteger(0)), meOut);
    }
private:
    MergeRuleGroup mRuleGroup;
    bool mHasBoundary;
};

// A group can be built by BPEXfrmKernel iff every rule reads only the frozen kernel input.
static bool bitXfrmEligible(const MergeRuleGroup & g) {
    for (const auto & r : g.rules)
        if (r.needsLiveId || r.needsFlush || !r.vetoIdB.empty()) return false;
    return true;
}

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
void bpeSetFrameworkHooks() {
    if (VMRingBuffers) std::cerr << "[BPE] --vm-ring-buffers is retired: vm_remap is the macOS default\n";
    if (MemSourceBlockCapacity) kernel::MemorySourceBlockCapacity = true;
}
// builds the rule table
// The --bpe-tail rule table (gTail) from the rules of groups `from` on; idBits: the width
// of the ids reaching the tail. Returns the number of rules.
static size_t buildTailTable(const std::vector<MergeRuleGroup> & ruleRanges, size_t from, unsigned idBits) {
    size_t numRules = 0;
    for (size_t gi = from; gi < ruleRanges.size(); gi++) numRules += ruleRanges[gi].rules.size();
    const unsigned bits = std::max<unsigned>(4, ceil_log2(4 * numRules + 1));
    gTail.slots.assign(size_t(1) << bits, kTailEmpty);
    gTailGeneration++;
    std::fill(std::begin(gTail.left), std::end(gTail.left), 0);
    std::fill(std::begin(gTail.right), std::end(gTail.right), 0);
    gTail.mask = (1u << bits) - 1;
    gTail.shift = 32 - bits;
    gTail.idBits = idBits;
    gTail.usePair = TailPairTable;
    gTail.pair.assign(TailPairTable ? 65536 : 0, 0xFFFF);
    for (size_t gi = from; gi < ruleRanges.size(); gi++)
        for (const auto & r : ruleRanges[gi].rules) {
            if (TailPairTable && r.idA < 256 && r.idB < 256) gTail.pair[(r.idA << 8) | r.idB] = uint16_t(r.idAB);
            const uint32_t key = (r.idA << 16) | r.idB;
            uint32_t h = tailSlot(key);
            while (gTail.slots[h] != kTailEmpty && uint32_t(gTail.slots[h] >> 16) != key)
                h = (h + 1) & gTail.mask;
            gTail.slots[h] = (uint64_t(key) << 16) | r.idAB;
            gTail.left[r.idA >> 6] |= 1ull << (r.idA & 63);
            gTail.right[r.idB >> 6] |= 1ull << (r.idB & 63);
        }
    return numRules;
}

// --tail-vocab-table: build the table from the rules (gTail must be built) and publish it.
// Tokens go in by rank, so when a set is full it is a rarer token that is left to the merge
// path. Runs on gTailVocabThread.
static void tailVocabBuild(std::vector<MergeRule> rules) {
    const auto t0 = std::chrono::steady_clock::now();
    std::sort(rules.begin(), rules.end(), [](const MergeRule & a, const MergeRule & b) { return a.idAB < b.idAB; });
    unsigned maxId = 256;
    for (const MergeRule & r : rules) maxId = std::max(maxId, r.idAB + 1);
    struct Bytes { uint8_t len; uint8_t b[kTailByteKeyMax]; };   // len 0: longer than the key
    std::vector<Bytes> bytesOf(maxId, Bytes{0, {}});
    for (unsigned by = 0; by < 256; by++) { bytesOf[bpeBaseId(by)].len = 1; bytesOf[bpeBaseId(by)].b[0] = uint8_t(by); }
    size_t want = 0;
    for (const MergeRule & r : rules) {
        const Bytes & a = bytesOf[r.idA], & b = bytesOf[r.idB];
        if (a.len == 0 || b.len == 0 || a.len + b.len > kTailByteKeyMax) continue;
        Bytes & t = bytesOf[r.idAB];
        t.len = uint8_t(a.len + b.len);
        std::memcpy(t.b, a.b, a.len);
        std::memcpy(t.b + a.len, b.b, b.len);
        want++;
    }
    unsigned bits = ceil_log2(std::max<size_t>(want / 4, 16));
    if (const char * e = std::getenv("BPE_VOCAB_BITS")) if (std::atoi(e) >= 4) bits = unsigned(std::atoi(e));
    if (TailBixHash) bits = bixHashBits();          // indexed by the B-bit hash itself
    gTailVocabMem.assign((size_t(4) << bits) + 4, TailCacheEntry{});
    TailCacheEntry * const vocab = reinterpret_cast<TailCacheEntry *>(
        (reinterpret_cast<uintptr_t>(gTailVocabMem.data()) + 127) & ~uintptr_t(127));
    gTailVocabShift = 64 - bits;
    size_t stored = 0, unreachable = 0, full = 0;
    for (const MergeRule & r : rules) {
        const Bytes & s = bytesOf[r.idAB];
        const size_t len = s.len;
        if (len < 2) continue;
        uint16_t tt[kTailByteKeyMax];
        uint32_t idx[kTailByteKeyMax];
        size_t m = len;
        for (size_t i = 0; i < len; i++) { tt[i] = gTailBase[s.b[i]]; idx[i] = uint32_t(i); }
        tailMergeSegment(tt, idx, m);
        if (m != 1 || tt[0] != r.idAB) { unreachable++; continue; }
        uint64_t a = 0, b = 0;
        std::memcpy(&a, s.b, std::min<size_t>(len, 8));
        if (len > 8) std::memcpy(&b, s.b + 8, len - 8);
        const uint64_t k0 = a, k1 = b | (uint64_t(len) << 56);
        const uint64_t h = TailBixHash ? uint64_t(tailBixHashOf()(s.b, unsigned(len))) << (64 - bits)
                                       : (k0 ^ (k1 * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
        TailCacheEntry * vs = vocab + (h >> gTailVocabShift) * 4;
        unsigned i = 0;
        while (i < 4 && vs[i].k1 != 0) i++;
        if (i == 4) { full++; continue; }
        vs[i].k0 = k0; vs[i].k1 = k1; vs[i].keep = 1; vs[i].out[0] = uint16_t(r.idAB);
        stored++;
    }
    gTailVocab.store(vocab, std::memory_order_release);
    // printed by tailVocabJoin on the main thread: from here it could split another stderr line
    gTailVocabReport = "[BPE] --tail-vocab-table: " + std::to_string(stored) + " tokens in "
        + std::to_string((size_t(128) << bits) / 1024) + " KB (" + std::to_string(unreachable)
        + " not reached by their own merges, " + std::to_string(full) + " left out: full sets), "
        + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count())
        + " ms\n";
}

BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe,
        kernel::StreamSet * boundary) {

    compactionMode();   // validate the compaction settings first: conflicts halt here   
    // --tail-from-bytes: every rule goes to the tail, which reads the bytes itself
    // (buildBPEByteTailEmitter); no merge kernel and no id stream.
    if (TailFromBytes && boundary) {
        const auto all = bpe.buildMergeRuleRanges(BPETokenizer::kTailAllRules);
        const size_t numRules = buildTailTable(all, 0, 8);
        for (unsigned by = 0; by < 256; by++) gTailBase[by] = uint16_t(bpeBaseId(by));
        gTailOpen.n = 0;
        tailSharedReset();
        tailVocabJoin();
        gTailVocab.store(nullptr, std::memory_order_relaxed);
        if (TailVocabTable) {
            std::vector<MergeRule> rules;
            for (const auto & g : all) rules.insert(rules.end(), g.rules.begin(), g.rules.end());
            // joined at exit before the statics it reads (gTail, gTailBase) are destroyed
            static const bool joinAtExit = (std::atexit(tailVocabJoin), true);
            (void) joinAtExit;
            if (std::getenv("BPE_VOCAB_SYNC")) tailVocabBuild(std::move(rules));
            else gTailVocabThread = std::thread(tailVocabBuild, std::move(rules));
        }
        std::cerr << "[BPE] --tail-from-bytes: no merge kernels; the tail applies all "
                  << numRules << " rules per pretoken\n";
        return {nullptr, nullptr, boundary};
    }
    if (TailFromBytes)
        std::cerr << "[BPE] --tail-from-bytes ignored: it needs a pretokenizer\n";
    auto ruleRanges = bpe.buildMergeRuleRanges(boundary ? (unsigned) BPETail : 0u);
    gStuckTables.clear();

    // Inject FilterByMask compactions per --compaction and rewrite the rules' merge
    // distances into the compacted (slot) frame.
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
    auto compactAfter = applyCompactionSchedule(ruleRanges);

    // debug: dump the merge-range groups to stderr. BPE_GROUPS=1 for the
    // per-group [lo,hi) xN maxLen=M lines too (verbose, 1 line/kernel).
    std::cerr << "[BPE] " << ruleRanges.size() << " merge-range kernels ("
              << (isLevelPartition() ? "ASAP level schedule" : "contiguous clean ranges")
              << (isBitTierPartition() ? ", per merge id bit width" : "") << ")\n";
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
                          << " lenA=" << r.lenA << " lenB=" << r.lenB << " -> idAB=" << r.idAB
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
    unsigned nXfrm = 0, nXfrmIneligible = 0;   // --bit-xfrm-limit kernel counts

    // --retire-words: one entry per FilterByMask point from the first retiring one on, for
    // the merge-back below.
    //   prevMask: the mask that FilterByMask used: live token starts in Fp, the frame
    //             before the point
    //   keptPre:  the live starts the point kept, in Fp; idsPre: Fp's id stream. Both null
    //             at a point that does not retire (--retire-points): the merge-back only
    //             steps through it.
    // A retiring point sends only 1-bit streams to the compacted frame (Fc): word starts
    // and frozen marks are filtered by prevMask, BPERetireKernel decides keep there, and
    // keep and the segment starts are spread back to Fp. The ids are then filtered once,
    // by keptPre, instead of by prevMask and again by keep.
    struct RetirePoint {
        StreamSet * prevMask; StreamSet * keptPre; StreamSet * idsPre;
    };
    std::vector<RetirePoint> retirePoints;
    const bool retire = RetireWords && boundary && compactionMode() != CompactionMode::None;
    if (RetireWords && !retire)
        std::cerr << "[BPE] --retire-words ignored: it needs --compaction and a pretokenizer\n";
    // --retire-frozen: lastPartUse[id] = 1 + the last kernel with a rule that has id as its
    // left or right part (absent = never a part). After kernel i, an id is frozen iff its
    // entry is absent or <= i + 1.
    std::unordered_map<unsigned, size_t> lastPartUse;
    if (retire && (RetireFrozen || RetireStuck))
        for (size_t gi = 0; gi < ruleRanges.size(); gi++)
            for (const auto & r : ruleRanges[gi].rules)
                for (unsigned id : {r.idA, r.idB}) {
                    size_t & u = lastPartUse[id];
                    u = std::max(u, gi + 1);
                }
    unsigned compactionPoint = 0, nRetiring = 0;
    bool tailCut = false;   // --bpe-tail stopped the kernels; `boundary` is the segment starts
    StreamSet * tailSegs = nullptr;   // --bpe-tail: the segment starts, in the emitter's frame
    gTailOpen.n = 0;
    if (BPETail && !boundary)
        std::cerr << "[BPE] --bpe-tail ignored: it needs a pretokenizer\n";
    auto retiresAt = [&](unsigned point) {
        return RetirePoints.empty()
            || std::find(RetirePoints.begin(), RetirePoints.end(), point) != RetirePoints.end();
    };

    for (size_t i = 0; i < ruleRanges.size(); i++) {
        auto & g = ruleRanges[i];
        if (g.rules.empty()) continue;
        if (g.tailOnly) report_fatal_error("BPE: the --bpe-tail rules were reached as a kernel");
        unsigned output_bits = ceil_log2(g.hi);
        StreamSet * sOut  = P.CreateStreamSet(output_bits, 1);
        StreamSet * meOut = P.CreateStreamSet(1, 1);
        // 1. Make the copied/shifted stream
        // Indexed mode: precompute next-live id for this kernel (frozen source+mask).
        // Only the first indexedShiftN kernels convert (isolation); rest stay byte-space.
        StreamSet * nextId = nullptr;
        if (useIndexedShift && i < indexedShiftN) {
            // CAUTION: inPlayMask may be nullptr here (first kernel after a
            // FilterByMask). Unlike BPEMergeKernel/BPEXfrmKernel below, IndexedShiftBack
            // is a shared framework kernel (include/kernel/streamutils/stream_shift.h)
            // and does NOT know how to build inPlayMask locally when it's missing --
            // not reachable with the current --compaction schedule + --indexed-shift
            // combination, but would need a framework-side change (opt-in, default off)
            // to support, not just a bpe.cpp one.
            nextId = P.CreateStreamSet(source->getNumElements(), 1);
            P.CreateKernelCall<IndexedShiftBack>(inPlayMask, source, nextId);
        }
        // Grouped-if for kernels at/after the lower limit (later kernels); -1 = off.
        // A --partition=chain rule no longer disqualifies the whole kernel: the
        // grouped-if path now skips just the chain-involved rules and emits them
        // its chain roots' nests inside its chunk gates (see isChainRule / emitChain in
        // BPEMergeKernel::generatePabloMethod).
        const int groupLowerLimit = effIfGroupLowerLimit();
        bool grouped = (groupLowerLimit >= 0) && ((long) i >= (long) groupLowerLimit);
        const bool xfrmWidth = output_bits <= BitXfrmLimit;
        if (xfrmWidth && !nextId && bitXfrmEligible(g)) {
            // inPlayMask may be nullptr here (first kernel after a FilterByMask) --
            // BPEXfrmKernel builds it locally in that case (see hasStreamInput in its
            // generatePabloMethod), same as BPEMergeKernel.
            P.CreateKernelCall<BPEXfrmKernel>(source, inPlayMask, boundary, sOut, meOut,
                                              g, hashRuleSet(g.rules), g.maxLen);
            nXfrm++;
        } else {
            if (xfrmWidth) nXfrmIneligible++;
            P.CreateKernelCall<BPEMergeKernel>(source, inPlayMask, boundary, nextId, sOut, meOut,
                                               g, hashRuleSet(g.rules), g.maxLen, grouped);
        }
        if (MergeFrequencyStatistics) {
            Kernel * k = P.CreateKernelCall<BPEMergeFrequencyKernel>(source, inPlayMask, meOut);
            MergeFrequencyRecorder * rec =
                mergeFrequencyRecorder(output_bits, std::make_integer_sequence<unsigned, 17>());
            if (!rec) throw std::runtime_error("--merge-frequency-statistics: id width over 16 bits");
            // one call name per kernel: the pipeline merges calls that share a name
            P.CreateCall("bpe_record_merge_frequency_" + std::to_string(i), *rec,
                         {k->getOutputScalarAt(0), k->getOutputScalarAt(1), k->getOutputScalarAt(2),
                          k->getOutputScalarAt(3), k->getOutputScalarAt(4), k->getOutputScalarAt(5),
                          k->getOutputScalarAt(6), k->getOutputScalarAt(7)});
        }
        source     = sOut;
        inPlayMask = meOut;

        // Scheduled compaction (applyCompactionSchedule): drop the positions this
        // block consumed. source and boundary move to the PopcountOf(inPlayMask) rate;
        // `boundary` MUST be filtered by the SAME mask or the gating lookahead reads
        // the wrong token. The fresh mask is all-ones because every position that
        // survived the filter was, by construction, a live token start.
        if (!compactAfter[i]) continue;
        ++compactionPoint;
        if (BPETail && compactionPoint == BPETail && boundary) {
            // --bpe-tail: no more kernels. The emitter finishes the rules of kernels i+1 on.
            // The ids reaching the tail are below 2^output_bits.
            const size_t numRules = buildTailTable(ruleRanges, i + 1, output_bits);
            tailCut = true;
            std::cerr << "[BPE] --bpe-tail: " << (i + 1) << " of " << ruleRanges.size()
                      << " merge kernels built; the emitter applies the other " << numRules << " rules\n";
            break;
        }
        const bool retireHere = retire && retiresAt(compactionPoint);
        StreamSet * sourceC = P.CreateStreamSet(output_bits, 1);
        if (!retireHere) {
            FilterByMask(P, inPlayMask, source, sourceC);
            if (boundary) {
                StreamSet * boundaryC = P.CreateStreamSet(1, 1);
                FilterByMask(P, inPlayMask, boundary, boundaryC);
                boundary = boundaryC;
            }
            if (retire && !retirePoints.empty())
                retirePoints.push_back({inPlayMask, nullptr, nullptr});
        } else {
            // Drop the finished words too (see the RetirePoint comment above). Every
            // surviving position stays a live token start, so the next kernel still builds
            // its all-ones mask locally.
            ++nRetiring;
            auto frozenId = [&](unsigned id) {
                const auto f = lastPartUse.find(id);
                return f == lastPartUse.end() || f->second <= i + 1;
            };
            if (RetireStuck && compactionPoint >= RetireStuck) {
                // --retire-stuck: the frozen-id bitmap and the pairs of every later rule.
                StuckTables tab;
                uint64_t hash = 1469598103934665603ull;
                auto mix = [&](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
                tab.frozen.assign(65536 / 64, 0);
                for (unsigned id = 0; id < 65536; id++)
                    if (frozenId(id)) tab.frozen[id >> 6] |= 1ull << (id & 63);
                for (uint64_t w : tab.frozen) mix(w);
                tab.pairs.assign((size_t(1) << kStuckBloomBits) / 64, 0);
                for (size_t gi = i + 1; gi < ruleRanges.size(); gi++)
                    for (const auto & r : ruleRanges[gi].rules) {
                        const uint32_t key = (r.idA << 16) | r.idB;
                        const uint32_t h = stuckHash(key);
                        tab.pairs[h >> 6] |= 1ull << (h & 63);
                        mix(key);
                    }
                const unsigned slot = gStuckTables.size();
                gStuckTables.push_back(std::move(tab));
                StreamSet * keptPre = P.CreateStreamSet(1, 1);
                StreamSet * segPre = P.CreateStreamSet(1, 1);
                // P2S16Kernel's StreamSets form names the kernel by the input's shape; the
                // single-set form names every width "p2s_16", like the emitter's 16-bit one.
                StreamSet * ids16 = P.CreateStreamSet(1, 16);
                StreamSets idSets{source};
                P.CreateKernelCall<P2S16Kernel>(idSets, ids16);
                Kernel * const sk = P.CreateKernelCall<BPEStuckKernel>(ids16, inPlayMask, boundary,
                                                                      keptPre, segPre, slot, hash);
                P.LinkFunction(sk, "bpe_retire_stuck", bpe_retire_stuck);
                FilterByMask(P, keptPre, source, sourceC);
                StreamSet * boundaryK = P.CreateStreamSet(1, 1);
                FilterByMask(P, keptPre, segPre, boundaryK);
                retirePoints.push_back({inPlayMask, keptPre, source});
                boundary = boundaryK;
                source = sourceC;
                inPlayMask = nullptr;
                continue;
            }
            // --retire-frozen: the frozen ids below 2^output_bits, as intervals, and their
            // complement; the kernel tests whichever has fewer intervals.
            UCD::UnicodeSet frozenSet, liveSet;
            uint64_t setHash = 1469598103934665603ull;
            bool negate = false;
            if (RetireFrozen) {
                const unsigned idLimit = 1u << output_bits;
                unsigned runStart = 0;
                bool runFrozen = true;
                for (unsigned id = 0; id <= idLimit; id++) {
                    const bool fz = (id < idLimit) && frozenId(id);
                    if (id < idLimit && id > 0 && fz == runFrozen) continue;
                    if (id > 0) {
                        (runFrozen ? frozenSet : liveSet).insert_range(runStart, id - 1);
                        setHash = (setHash ^ (((uint64_t) runStart << 1) | runFrozen)) * 1099511628211ull;
                    }
                    runStart = id;
                    runFrozen = fz;
                }
                negate = liveSet.size() < frozenSet.size();
            }
            // 1-bit streams only to Fc: the word starts and (--retire-frozen) frozen marks.
            StreamSet * boundaryC = P.CreateStreamSet(1, 1);
            FilterByMask(P, inPlayMask, boundary, boundaryC);
            StreamSet * frozenC = nullptr;
            StreamSet * segPre = boundary;   // segment starts in Fp: word starts, plus barriers
            if (RetireFrozen) {
                StreamSet * frozenPre = P.CreateStreamSet(1, 1);
                segPre = P.CreateStreamSet(1, 1);
                P.CreateKernelCall<BPEFrozenKernel>(source, inPlayMask, boundary,
                                                    negate ? liveSet : frozenSet, negate, setHash,
                                                    frozenPre, segPre);
                frozenC = P.CreateStreamSet(1, 1);
                FilterByMask(P, inPlayMask, frozenPre, frozenC);
            }
            StreamSet * keep = P.CreateStreamSet(1, 1);
            P.CreateKernelCall<BPERetireKernel>(boundaryC, frozenC, keep);
            // keep back to Fp, then one filter of the ids and segment starts.
            StreamSet * keptPre = P.CreateStreamSet(1, 1);
            SpreadByMask(P, inPlayMask, keep, keptPre);
            FilterByMask(P, keptPre, source, sourceC);
            StreamSet * boundaryK = P.CreateStreamSet(1, 1);
            FilterByMask(P, keptPre, segPre, boundaryK);
            retirePoints.push_back({inPlayMask, keptPre, source});
            boundary = boundaryK;
        }
        source     = sourceC;
        // nullptr, not BPEOnesKernel: whichever kernel consumes this next builds the
        // constant locally (see hasStreamInput in each kernel's generatePabloMethod)
        // instead of reading it from a materialized stream -- one fewer kernel, and
        // LLVM folds away every rule's now-trivial AND(eqId, inPlayMask) for free.
        inPlayMask = nullptr;
    }

    if (BitXfrmLimit > 0)
        std::cerr << "[BPE] bit-xfrm-limit=" << BitXfrmLimit << ": " << nXfrm
                  << " bit-transformation kernels" << (nXfrmIneligible
                      ? ", " + std::to_string(nXfrmIneligible) + " kernels kept per-rule "
                        "(chain/asymmetric-seam/veto rules or --indexed-shift)" : std::string())
                  << "\n";
    // --tail-direct-input: no merge-back to do, so skip the widen/pack/unpack copies.
    if (TailDirectInput && tailCut && retirePoints.empty() && inPlayMask != nullptr) {
        StreamSet * segs = P.CreateStreamSet(1, 1);
        P.CreateKernelCall<BPETailSegKernel>(boundary, inPlayMask, segs);
        return {inPlayMask, source, segs};
    }
    // The emitter packs exactly 16 id bits (P2S16Kernel); widen a narrower final stream.
    if (source->getNumElements() < 16) {
        StreamSet * source16 = P.CreateStreamSet(16, 1);
        P.CreateKernelCall<BPEWidenKernel>(source, source16);
        source = source16;
    }
    // --retire-words merge-back, last point first, one 17-stream spread per point: at a
    // retiring point SpreadByMask(keptPre) puts the later survivors back at their positions
    // in Fp and BPERetireMergeKernel ORs in the retired tokens, whose ids Fp still holds; at
    // any other point SpreadByMask(prevMask) steps back to Fp, with 0 (no token) at the
    // positions it had dropped as consumed. The emitter scans Fp of the first retiring point.
    // Not MergeByMask(keep, later, retired): it counts keep both ways (PopcountOf and
    // PopcountOfNot), and on some inputs the pipeline's two-way popcount kernel then
    // over-counts the filtered frame: later kernels ran on up to 1.9x phantom positions
    // (right output, 1.7x slower; val_4MB LF with --retire-points=5, 2026-10-15).
    // --bpe-tail: the same, with the segment starts as an 18th stream (see kTailStreams).
    if (!retirePoints.empty() || tailCut) {
        const unsigned numStreams = tailCut ? kTailStreams : kRetireStreams;
        StreamSet * cur = P.CreateStreamSet(numStreams, 1);
        P.CreateKernelCall<BPERetirePackKernel>(source, inPlayMask, cur, tailCut ? boundary : nullptr);
        for (size_t m = retirePoints.size(); m-- > 0; ) {
            const RetirePoint & rp = retirePoints[m];
            StreamSet * spread = P.CreateStreamSet(numStreams, 1);
            SpreadByMask(P, rp.keptPre ? rp.keptPre : rp.prevMask, cur, spread);
            cur = spread;
            if (rp.keptPre) {
                StreamSet * merged = P.CreateStreamSet(numStreams, 1);
                P.CreateKernelCall<BPERetireMergeKernel>(cur, rp.idsPre, rp.prevMask, rp.keptPre, merged);
                cur = merged;
            }
        }
        StreamSet * ids = P.CreateStreamSet(16, 1);
        StreamSet * starts = P.CreateStreamSet(1, 1);
        tailSegs = tailCut ? P.CreateStreamSet(1, 1) : nullptr;
        P.CreateKernelCall<BPERetireUnpackKernel>(cur, ids, starts, tailSegs);
        source = ids;
        inPlayMask = starts;
        if (!retirePoints.empty()) {
            std::cerr << "[BPE] --retire-words: retiring at " << nRetiring << " of "
                      << compactionPoint << " compaction points";
            if (!gStuckTables.empty())
                std::cerr << " (" << gStuckTables.size() << " with --retire-stuck)";
            std::cerr << "\n";
        }
    }
    if (tailCut) return {inPlayMask, source, tailSegs};
    if (BitFrequencyStatistics) {
        Kernel * k = P.CreateKernelCall<BPEBitFrequencyKernel>(source, inPlayMask);
        P.CreateCall("bpe_report_bit_frequency", bpe_report_bit_frequency,
                     {k->getOutputScalarAt(0), k->getOutputScalarAt(1), k->getOutputScalarAt(2),
                      k->getOutputScalarAt(3), k->getOutputScalarAt(4), k->getOutputScalarAt(5),
                      k->getOutputScalarAt(6), k->getOutputScalarAt(7)});
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
// levelPartition — ASAP level schedule of the rank-sorted rules (see --partition=level).
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
// --partition=chain bookkeeping, reported on the [BPE] stderr dump.
static unsigned gChainVetoRelaxed = 0;   // rules carrying a runtime veto
static unsigned gChainVetoTerms   = 0;   // total competitor EQs those rules emit

static void keyCluster(std::vector<std::vector<MergeRule>> & levels, const PartitionContext & ctx);

static std::vector<MergeRuleGroup> levelPartition(const std::vector<MergeRule> & rules,
                                                  const PartitionContext & ctx = PartitionContext()) {
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
        // only for the --partition=asymmetric maxRight relaxation: the live-mask read makes
        // same-kernel coexistence correct (rank order inside the kernel still runs the
        // consumer before the dependent), but the rule must never land STRICTLY EARLIER
        // than the token's consumer.
        auto sameOrAfter = [&](const std::unordered_map<unsigned, unsigned> & m, unsigned key) {
            auto it = m.find(key);
            if (it != m.end() && it->second > lvl) lvl = it->second;
        };
        // dependency: idA must be stamped by a lower-OR-SAME kernel under
        // --partition=chain (excluding self-merges, see the enum's comment);
        // otherwise (default) strictly a lower kernel, same as idB below.
        if (isChainPartition() && r.idA != r.idB) sameOrAfter(prodLevel, r.idA);
        else after(prodLevel, r.idA);
        after(prodLevel, r.idB);      // dependency: idB stamped by a lower kernel
        // seam: an earlier rule consumed this token as its B. Under --partition=asymmetric
        // (or chain), softened to same-level-or-after — the runtime gate now reads
        // inPlayMask LIVE (see eqAstart), so a same-kernel earlier consume is visible
        // without a level split, but the rule still can't jump to an EARLIER level than
        // its consumer. EXCEPT self-merges (idA==idB): selfMergeFireStarts' run-parity
        // math assumes a STATIC isX (see its call site comment — "no same-kernel rule
        // can have consumed an X-run position... so frozen==live").
        if (isAsymmetricSeam() && r.idA != r.idB) sameOrAfter(maxRight, r.idA);
        else after(maxRight, r.idA);
        // seam: an earlier rule claimed this token as its A. Without --partition=chain
        // this is a hard split — the forward LookAhead reads frozen input only (T5), so a
        // same-kernel competitor's consume of idB is invisible. With it, the rule tests
        // the competitor itself and stands down at runtime (see the enum's comment).
        std::vector<unsigned> veto;
        bool relaxed = false;
        if (isChainVeto() && r.idA != r.idB) {
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
        // (always false) whenever --partition=chain is off.
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
    if (KeyCluster) keyCluster(levels, ctx);
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

// --key-cluster: re-level the ASAP schedule so rules sharing a gate key (idA's
// --if-test-significant-bits=K key, as in the grouped path) land in ONE kernel and
// share one gate test, instead of each kernel of a segment testing the same key.
// A rule only moves LATER, and only inside its ASAP compaction segment (so every
// stream length is unchanged). Every rank-order dependency and seam constraint of
// levelPartition's strict pass is re-checked against the NEW levels while placing,
// so the result is as valid as ASAP. Port of analysis/bpe_model/partition/
// chain_sched.py key_cluster ('KC: chain, keep today nests'): model -38%
// block-weighted gate tests, same 283 kernels, 0 fire mismatches vs HF.
static void keyCluster(std::vector<std::vector<MergeRule>> & levels, const PartitionContext & ctx) {
    const int lowerLimit = effIfGroupLowerLimit();
    if (isAsymmetricSeam() || isChainVeto() || IfTestSignificantBits == 0 || lowerLimit < 0) {
        std::cerr << "[BPE] --key-cluster ignored: needs grouping with --if-test-significant-bits, "
                     "and --partition=level (not asymmetric or chain)\n";
        return;
    }
    const bool chain = isChainPartition();
    const unsigned NL = levels.size();
    // Level l (1-based) is global kernel ctx.kernelOffset + l - 1, which is grouped iff that
    // index >= lowerLimit, i.e. iff l > localLimit.
    const long localLimit = (long) lowerLimit - (long) ctx.kernelOffset;
    // Rules in rank order (= idAB order) with their ASAP level E0 (1-based).
    std::vector<MergeRule> rules;
    std::vector<unsigned> E0;
    for (unsigned l = 0; l < NL; l++)
        for (const auto & r : levels[l]) { rules.push_back(r); E0.push_back(l + 1); }
    const size_t R = rules.size();
    {
        std::vector<size_t> ord(R);
        for (size_t i = 0; i < R; i++) ord[i] = i;
        std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return rules[a].idAB < rules[b].idAB; });
        std::vector<MergeRule> rs; std::vector<unsigned> es;
        for (size_t i : ord) { rs.push_back(rules[i]); es.push_back(E0[i]); }
        rules.swap(rs); E0.swap(es);
    }
    // segEnd[l] = last level of l's compaction segment (same points applyCompactionSchedule
    // picks, continuing the count from ctx; the last level always ends a segment).
    std::vector<unsigned> segEnd(NL + 1, NL);
    {
        std::vector<unsigned> ends;
        CompactionCounter counter = ctx.counter;
        for (unsigned l = 1; l <= NL; l++)
            if (counter.step()) ends.push_back(l);
        for (unsigned l = 1; l <= NL; l++) {
            auto it = std::lower_bound(ends.begin(), ends.end(), l);
            segEnd[l] = (it != ends.end()) ? *it : NL;
        }
    }
    using Map = std::unordered_map<unsigned, unsigned>;
    auto get = [](const Map & m, unsigned k) -> unsigned { auto it = m.find(k); return it == m.end() ? 0 : it->second; };
    auto keepMax = [](Map & m, unsigned k, unsigned v) { unsigned & s = m[k]; if (v > s) s = v; };
    auto keepMin = [](Map & m, unsigned k, unsigned v) { auto it = m.find(k); if (it == m.end() || v < it->second) m[k] = v; };
    auto bound = [](const Map & m, unsigned k, unsigned off, unsigned & u) {
        auto it = m.find(k); if (it != m.end()) u = std::min(u, it->second - off);
    };
    // ALAP (reverse rank order): the latest level each rule can take with every later rule
    // at or below ITS latest level, capped at the end of the rule's ASAP segment.
    std::vector<unsigned> L(R);
    {
        Map minLns, minLs, minR;   // token -> min level of a later rule using it as A (non-self / self), as B
        for (size_t p = R; p-- > 0; ) {
            const MergeRule & r = rules[p];
            unsigned u = segEnd[E0[p]];
            bound(minLns, r.idAB, chain ? 0 : 1, u);   // a later rule built on idAB (nestable under chain)
            bound(minLs,  r.idAB, 1, u);
            bound(minR,   r.idAB, 1, u);
            bound(minLns, r.idB, 1, u);                // seam: a later rule's A is our B
            bound(minLs,  r.idB, 1, u);
            bound(minR,   r.idA, 1, u);                // seam: a later rule's B is our A
            L[p] = std::max(u, E0[p]);                 // ASAP is feasible, so u >= E0 (defensive)
            keepMin(r.idA == r.idB ? minLs : minLns, r.idA, L[p]);
            keepMin(minR, r.idB, L[p]);
        }
    }
    // Rules left in place: the flat kernels (below the grouping limit) and today's nests.
    std::vector<char> skip(R);
    for (size_t q = 0; q < R; q++) skip[q] = ((long) E0[q] <= localLimit) || rules[q].needsLiveId;
    // Per key: fewest levels that stab every rule's window [lo, L] (greedy by right end),
    // each point pulled down to the latest left end it covers.
    std::unordered_map<uint64_t, std::vector<size_t>> byKey;
    for (size_t q = 0; q < R; q++) if (!skip[q]) byKey[gateKeyOf(rules[q].idA)].push_back(q);
    auto stab = [&](const std::vector<unsigned> & lo) {
        std::vector<unsigned> tgt = lo;
        for (auto & kv : byKey) {
            std::vector<size_t> idx = kv.second;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return L[a] < L[b]; });
            while (!idx.empty()) {
                const unsigned point = L[idx[0]];
                unsigned p2 = 0;
                std::vector<size_t> rest;
                for (size_t v : idx) if (lo[v] <= point) p2 = std::max(p2, lo[v]); else rest.push_back(v);
                for (size_t v : idx) if (lo[v] <= point) tgt[v] = p2;
                idx.swap(rest);
            }
        }
        return tgt;
    };
    // Rank-order placement: earliest legal level ep from the rules ALREADY placed, then
    // the target clipped into [ep, L]. nested[q] = idA's producer sits at the same level.
    const unsigned cap = effKeyClusterCap();
    std::vector<unsigned> lv(R), cnt(NL + 2);
    std::vector<char> nested(R);
    auto realize = [&](const std::vector<unsigned> & tgt) {
        Map prod, maxL, maxR;
        std::fill(cnt.begin(), cnt.end(), 0);
        for (size_t q = 0; q < R; q++) {
            const MergeRule & r = rules[q];
            const bool nestable = chain && r.idA != r.idB;
            const unsigned ep = std::max({1u, get(prod, r.idA) + (nestable ? 0u : 1u), get(prod, r.idB) + 1u,
                                          get(maxR, r.idA) + 1u, get(maxL, r.idB) + 1u});
            unsigned l;
            if (rules[q].needsLiveId && get(prod, r.idA) == ep) l = ep;   // keep today's nest
            else l = std::max(ep, std::min(tgt[q], L[q]));
            if (cap > 0 && l > ep && cnt[l] >= cap) l = ep;
            if (l >= cnt.size()) cnt.resize(l + 1);
            lv[q] = l; cnt[l]++;
            nested[q] = nestable && get(prod, r.idA) == l;
            prod[r.idAB] = l;
            keepMax(maxL, r.idA, l);
            keepMax(maxR, r.idB, l);
        }
    };
    // Static gate count: distinct (level, key) over grouped, non-nested rules.
    auto gates = [&](const std::vector<unsigned> & lvls, const std::vector<char> & nst) {
        std::unordered_set<uint64_t> s;
        for (size_t q = 0; q < R; q++)
            if ((long) lvls[q] > localLimit && !nst[q]) s.insert((uint64_t(lvls[q]) << 40) ^ gateKeyOf(rules[q].idA));
        return s.size();
    };
    std::vector<char> nested0(R);
    for (size_t q = 0; q < R; q++) nested0[q] = rules[q].needsLiveId;
    const size_t gates0 = gates(E0, nested0);
    size_t bestGates = gates0;
    std::vector<unsigned> best = E0;
    std::vector<char> bestNested = nested0;
    std::vector<unsigned> lo = E0;
    for (int it = 0; it < 3; it++) {
        realize(stab(lo));
        const size_t g = gates(lv, nested);
        if (g < bestGates) { bestGates = g; best = lv; bestNested = nested; }
        // next round's windows: the lower bound from every predecessor except the
        // chain parent (it can follow the rule), under the schedule just built
        Map prod, maxL, maxR;
        for (size_t q = 0; q < R; q++) {
            const MergeRule & r = rules[q];
            unsigned x = std::max({1u, get(prod, r.idB) + 1u, get(maxR, r.idA) + 1u, get(maxL, r.idB) + 1u});
            if (!chain || r.idA == r.idB || r.idA < 256) x = std::max(x, get(prod, r.idA) + 1u);
            lo[q] = std::max(x, E0[q]);
            prod[r.idAB] = lv[q]; keepMax(maxL, r.idA, lv[q]); keepMax(maxR, r.idB, lv[q]);
        }
    }
    size_t maxBefore = 0, maxAfter = 0;
    for (auto & v : levels) maxBefore = std::max(maxBefore, v.size());
    std::vector<std::vector<MergeRule>> out(*std::max_element(best.begin(), best.end()));
    for (size_t q = 0; q < R; q++) {
        MergeRule rc = rules[q];
        rc.needsLiveId = bestNested[q];
        out[best[q] - 1].push_back(rc);        // q runs in rank order → each level stays rank-sorted
    }
    for (auto & v : out) maxAfter = std::max(maxAfter, v.size());
    std::cerr << "[BPE] --key-cluster: static gates " << gates0 << " -> " << bestGates
              << ", max rules/kernel " << maxBefore << " -> " << maxAfter << "\n";
    levels.swap(out);
}

// splitOversizedGroups — apply --max-merges-per-kernel (see MaxMergesPerKernel). Each
// chunk keeps the group's `hi` (a valid width bound, and it keeps `hi` monotone), and its
// rules stay in rank order. nSplit counts the groups split.
//
// Key-aware split (with --if-test-significant-bits, unless --partition=asymmetric or
// --partition=chain): chunks are filled by gate key, so rules sharing a gate stay in one kernel
// (what --key-cluster gathered is not scattered again). Any assignment of a group's rules
// to consecutive kernels is valid when no two of them are rank-order dependent — no
// dependency or seam lies inside a group — except a --partition=chain nest, whose child
// reads its producer's stamp in the same kernel; a chain tree is therefore kept whole.
// --partition=asymmetric and --partition=chain relate same-group rules by rank, so with either of
// those the split stays in rank order.
static std::vector<MergeRuleGroup> splitOversizedGroups(std::vector<MergeRuleGroup> groups,
                                                        unsigned & nSplit) {
    const size_t cap = MaxMergesPerKernel;
    if (cap == 0) return groups;
    const bool byKey = IfTestSignificantBits > 0 && !isAsymmetricSeam() && !isChainVeto();
    std::vector<MergeRuleGroup> out;
    for (auto & g : groups) {
        const size_t n = g.rules.size();
        if (n <= cap) { out.push_back(std::move(g)); continue; }
        const size_t k = (n + cap - 1) / cap;
        nSplit++;
        std::vector<std::vector<MergeRule>> chunks;
        if (!byKey) {
            for (size_t c = 0; c < k; c++)
                chunks.emplace_back(g.rules.begin() + n * c / k, g.rules.begin() + n * (c + 1) / k);
        } else {
            // units: a chain tree (root + every needsLiveId descendant) or a single rule,
            // keyed by the root's gate key, in rank order of their roots
            std::vector<std::vector<MergeRule>> units;
            std::vector<uint64_t> unitKey;
            std::unordered_map<unsigned, size_t> unitOf;   // idAB -> unit stamping it
            for (const auto & r : g.rules) {
                auto p = r.needsLiveId ? unitOf.find(r.idA) : unitOf.end();
                size_t u;
                if (p != unitOf.end()) u = p->second;
                else { u = units.size(); units.emplace_back(); unitKey.push_back(gateKeyOf(r.idA)); }
                units[u].push_back(r);
                unitOf[r.idAB] = u;
            }
            std::vector<size_t> ord(units.size());
            for (size_t u = 0; u < ord.size(); u++) ord[u] = u;
            std::stable_sort(ord.begin(), ord.end(),
                             [&](size_t a, size_t b) { return unitKey[a] < unitKey[b]; });
            // Fill chunks toward the even size n/k, never splitting a key unless it alone
            // exceeds the cap, and never exceeding the cap except for one oversized tree.
            const size_t target = (n + k - 1) / k;
            std::vector<MergeRule> cur;
            auto close = [&] { if (!cur.empty()) { chunks.push_back(std::move(cur)); cur.clear(); } };
            for (size_t i = 0; i < ord.size(); ) {
                size_t j = i, keySize = 0;
                for (; j < ord.size() && unitKey[ord[j]] == unitKey[ord[i]]; j++) keySize += units[ord[j]].size();
                if (cur.size() + keySize > cap) close();
                for (; i < j; i++) {
                    const auto & unit = units[ord[i]];
                    if (!cur.empty() && cur.size() + unit.size() > cap) close();
                    cur.insert(cur.end(), unit.begin(), unit.end());
                }
                if (cur.size() >= target) close();
            }
            close();
            for (auto & c : chunks)
                std::sort(c.begin(), c.end(),
                          [](const MergeRule & x, const MergeRule & y) { return x.idAB < y.idAB; });
        }
        for (auto & c : chunks) {
            MergeRuleGroup sub;
            sub.hi = g.hi;
            sub.rules = std::move(c);
            sub.lo = sub.rules.front().idAB;
            std::unordered_set<unsigned> stamped;   // idABs stamped in this chunk
            for (auto & r : sub.rules) {
                if (r.needsLiveId) r.needsLiveId = stamped.count(r.idA) != 0;
                stamped.insert(r.idAB);
                sub.maxLen = std::max(sub.maxLen, std::max(r.lenA + r.lenB, r.vetoOff));
            }
            out.push_back(std::move(sub));
        }
    }
    return out;
}

// cleanRangePartition — the default contiguous clean-range walk: lo = first rule's
// idAB (its parts always precede it, so it always fits); extend while the rule is
// dependency-independent (idA<lo && idB<lo) AND token-adjacency-overlap-free vs every
// rule already in the group. The first rule violating either starts the next range.
static std::vector<MergeRuleGroup> cleanRangePartition(const std::vector<MergeRule> & rules) {
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

// Mark rules whose gate needs a POST-write live mask: an earlier (lower-rank) rule
// in the SAME group already consumed this rule's idA as its own idB. Under the
// default symmetric seam test (--partition=clean or level) this can never occur —
// the seam test forbids both overlap directions within one group — so this pass is
// a no-op there. Under --partition=asymmetric (or chain) it is exactly the ONE
// relaxed direction (see isAsymmetricSeam()'s
// `sameOrAfter(maxRight, r.idA)` in levelPartition). --batch-writeback defers the
// inPlayMask clear to the end of the kernel; a flagged rule must force a flush of
// the pending write-back first, or its eqAstart sees the kernel's frozen entry
// mask instead of the producer's consume and misfires.
static void tagFlushPoints(std::vector<MergeRuleGroup> & groups) {
    unsigned totalFlags = 0, totalRules = 0, groupsWithFlags = 0;
    for (auto & g : groups) {
        if (g.tailOnly) continue;
        std::unordered_set<unsigned> consumedAsB;   // idB values used by earlier rules in this group
        unsigned before = totalFlags;
        for (auto & r : g.rules) {
            // Two reasons a rule's gate must read the LIVE mask rather than the kernel's
            // frozen entry mask:
            //  1. --partition=asymmetric: an earlier rule in this group consumed this rule's
            //     idA as ITS idB, so idA is already gone and the gate has to see that.
            //  2. --partition=chain: this rule now shares its kernel with the lower-rank rules
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

std::vector<MergeRuleGroup> BPETokenizer::buildMergeRuleRanges(unsigned tailPoint) const {
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

    // --tail-from-bytes: every rule goes to the tail, so nothing is partitioned.
    if (tailPoint == kTailAllRules) {
        MergeRuleGroup g;
        g.rules = std::move(rules);
        if (!g.rules.empty()) { g.lo = g.rules.front().idAB; g.hi = g.rules.back().idAB + 1; }
        g.tailOnly = true;
        return {std::move(g)};
    }

    // 3. Partition. --partition=level/asymmetric/chain/bit-tier take the ASAP level
    //    schedule (minimum kernel count, same constraints); --partition=clean (default) is
    //    the clean_range walk (see cleanRangePartition). --partition=bit-tier partitions
    //    each idAB bit tier separately (via the ASAP level schedule) and concatenates the
    //    tiers in order (see isBitTierPartition). Oversized groups are split right after
    //    each partition (--max-merges-per-kernel), so a tier's context counts the kernels
    //    that will really precede it.
    unsigned nSplit = 0;
    auto partition = [&](const std::vector<MergeRule> & rs, const PartitionContext & ctx) {
        return splitOversizedGroups(isLevelPartition() ? levelPartition(rs, ctx) : cleanRangePartition(rs),
                                    nSplit);
    };
    std::vector<MergeRuleGroup> groups;
    if (isBitTierPartition()) {
        // rules are rank-sorted, so each bit tier is a contiguous run of them
        PartitionContext ctx;
        size_t i = 0, n = rules.size();
        // --bpe-tail under by-output-bits: the compaction points sit at the ends of tiers
        // (applyCompactionSchedule), so the tail's cut is known as the tiers are partitioned.
        // The tiers past it never become kernels: their rules go to the tail as one
        // unpartitioned group, which saves partitioning ~50k rules (~75 ms per run) and
        // leaves the kernels before the cut as they were.
        const bool tailCutKnown = tailPoint > 0 && compactionMode() == CompactionMode::ByOutputBits;
        unsigned points = 0, kernelsSoFar = 0;
        while (i < n) {
            if (tailCutKnown && points == tailPoint) {
                MergeRuleGroup g;
                g.rules.assign(rules.begin() + i, rules.end());
                g.lo = g.rules.front().idAB;
                g.hi = g.rules.back().idAB + 1;
                g.tailOnly = true;
                std::cerr << "[BPE] --bpe-tail: " << g.rules.size() << " rules past the cut left "
                          "unpartitioned\n";
                groups.push_back(std::move(g));
                break;
            }
            const unsigned bits = ceil_log2(rules[i].idAB + 1);   // bit length of idAB
            size_t j = i;
            while (j < n && ceil_log2(rules[j].idAB + 1) == bits) ++j;
            auto tier = partition(std::vector<MergeRule>(rules.begin() + i, rules.begin() + j), ctx);
            std::cerr << "[BPE] merge id bits " << bits << ": " << (j - i) << " rules in "
                      << tier.size() << " kernels\n";
            // advance the context past this tier: the count steps once per kernel, as in
            // applyCompactionSchedule
            ctx.kernelOffset += tier.size();
            for (size_t t = 0; t < tier.size(); t++) ctx.counter.step();
            for (auto & g : tier) {
                if (!g.rules.empty()) kernelsSoFar++;
                groups.push_back(std::move(g));
            }
            // by-output-bits places a point after a tier's last kernel when a wider tier
            // follows and at least --compact-base kernels have run (applyCompactionSchedule)
            if (j < n && kernelsSoFar >= effCompactEvery()) points++;
            i = j;
        }
    } else {
        groups = partition(rules, PartitionContext());
    }
    if (nSplit)
        std::cerr << "[BPE] max-merges-per-kernel=" << MaxMergesPerKernel << ": split " << nSplit
                  << " kernels -> " << groups.size() << " kernels total"
                  << (IfTestSignificantBits > 0 && !isAsymmetricSeam() && !isChainVeto() ? " (by gate key)" : "")
                  << "\n";
    tagFlushPoints(groups);
    if (isLevelPartition()) {
        if (std::getenv("BPE_CHAIN_STATS")) {
            unsigned n = 0, total = 0;
            for (auto & g : groups) {
                total += g.rules.size();
                for (auto & r : g.rules) if (r.needsLiveId) n++;
            }
            std::cerr << "[BPE] chain-partition: " << n << "/" << total
                       << " rules flagged needsLiveId\n";
        }
        if (isChainVeto())
            std::cerr << "[BPE] chain-veto: " << gChainVetoRelaxed
                      << " rules kept their level via a runtime veto ("
                      << gChainVetoTerms << " competitor EQs)\n";
    }
    return groups;
}
