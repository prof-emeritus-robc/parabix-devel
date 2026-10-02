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
#include <ucd/core/unicode_set.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/stream_shift.h>   // IndexedShiftBack (BPE_INDEXED_SHIFT)
#include <stdexcept>
#include <boost/intrusive/detail/math.hpp>
using boost::intrusive::detail::ceil_log2;

using namespace llvm;
// Every optional BPE optimization defaults OFF — a bare run is the plain, unoptimized
// pipeline, and each optimization is opted into on the command line. 
// --compaction: where FilterByMask compaction goes between merge kernels.
//   linear         — after every N kernels (N = --compact-base).
//   geometric      — after N kernels, then 2N more, 4N more, ... (N = --compact-base).
//   by-output-bits — between two kernels whose output id widths differ, i.e. after the
//                    last kernel of each width (with --partition-by-merge-id-bits: after
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

// Bit-tier partition: confine every kernel to merged ids of ONE bit length. Tier b holds
// the rules with idAB in [2^(b-1), 2^b) — tier 9 = ids 256..511, tier 10 = 512..1023, …
// Each tier is partitioned on its own (clean ranges, or --level-partition levels) and
// the tiers run in order, so the first kernel of tier N+1 reads an N-bit id stream and
// writes N+1 bits, and every later kernel of that tier reads and writes N+1 bits. Both
// correctness constraints carry across a tier boundary for free: every rule of a lower
// tier has lower rank and runs in a strictly earlier kernel, which is exactly what the
// dependency and seam constraints ask of an earlier rule. Only the packing changes.
static cl::opt<bool> PartitionByMergeIdBits(
    "partition-by-merge-id-bits",
    cl::desc("Confine each merge kernel to merged ids of a single bit length, so each "
             "kernel grows the id stream by at most one bit."),
    cl::init(false));

// Cap the rules per merge kernel: a partition group with more than this many rules is
// split into ceil(n/cap) consecutive kernels of near-equal size, in rank order. Pablo's
// compile time is superlinear in kernel size, and --level-partition with
// --partition-by-merge-id-bits puts most of a bit tier into its first level (8023 rules
// in the 16-bit tier's first kernel). Splitting is exact: the rules of one group have
// no dependencies or seams among each other that a later kernel could break —
//   - dependency / seam: a lower-rank rule moves to an EARLIER kernel, which is what
//     both constraints ask of it anyway;
//   - --chain-partition: needsLiveId is recomputed per chunk; a child whose producer
//     landed in an earlier chunk reads the producer's stamp from its frozen input;
//   - --chain-veto: a competitor in an earlier chunk has already stamped its merged id
//     over this rule's idB slot, so B-detect fails exactly where the veto would fire;
//   - --asymmetric-seam: an earlier chunk's consume reaches this kernel through meIn.
// 0 = no cap.
static cl::opt<unsigned> MaxMergesPerKernel(
    "max-merges-per-kernel",
    cl::desc("Split any merge kernel with more than this many rules into consecutive "
             "kernels of near-equal size (0 = no cap). Default 500."),
    cl::init(500));

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

// --chain-partition emission shape. Output-identical: Pablo ORs pending carries into
// every if test (CarryManager::generateEntrySummaryTest), so fusing same-condition ifs,
// or dropping one whose body is a no-op when its condition is zero, changes nothing.
static cl::opt<bool> ChainFuseSiblings(
    "chain-fuse-siblings",
    cl::desc("With --chain-partition, nest all chain-children of a rule under ONE createIf "
             "on its fire instead of one createIf per child."),
    cl::init(false));

static cl::opt<bool> ChainUngateRoots(
    "chain-ungate-roots",
    cl::desc("With --chain-partition on grouped kernels, drop a chain root's own createIf "
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

// Bit-transformation kernels (BPEXfrmKernel): a merge kernel whose input id stream has
// at most this many bits computes every output bit as input XOR a "change" stream, where
// each change stream is a character class compiled over an (A id, B id) pair. Ids wider
// than kXfrmLowBits are split: an if-block per (high A bits, high B bits) pair, with the
// character classes over the low bits inside (see BPEXfrmKernel). 0 = off.
static cl::opt<unsigned> MaxBitXfrmLimit(
    "max-bit-xfrm-limit",
    cl::desc("Build merge kernels whose input id stream has at most N bits as bit "
             "transformations (0 = off, the default). Ids over 10 bits are gated by "
             "if-blocks on their high bits."),
    cl::init(0));

static cl::opt<bool> KeyCluster(
    "key-cluster",
    cl::desc("With --level-partition and --if-test-significant-bits: move rules (only later, only "
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
// kernel and the compaction count on entry. --partition-by-merge-id-bits partitions each
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
    CompactionCounter counter;
    unsigned nCompact = 0, firstBlockDisagree = 0, kernelNo = 0;
    for (size_t i = 0; i < ruleRanges.size(); i++) {
        auto & g = ruleRanges[i];
        if (g.rules.empty()) continue;
        ++kernelNo;
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
                        + (grouped ? (IfTestSignificantBits > 0
                                        ? "sbh" + std::to_string(IfTestSignificantBits) + "_"
                                          + (EmbeddedIfBits > 0 ? "eb" + std::to_string(EmbeddedIfBits) + "_" : "")
                                        : "g" + std::to_string(effGroupSize(group.rules.size())) + "_") : "")
                        + (LookaheadInGate ? "la1_" : "la0_")
                        + (LookaheadInGroup ? "lg1_" : "lg0_")
                        + (BatchWriteback ? "bw1_" : "bw0_")
                        + (ChainPartition ? "cp1_" : "cp0_")
                        + (ChainVeto ? "cv1_" : "cv0_")
                        + (ChainFuseSiblings ? "fs1_" : "fs0_")
                        + (ChainUngateRoots ? "ur1_" : "ur0_")
                        + (InFileOnce ? "io1_" : "")   // off = body identical to before → keep old names
                        + (GateMaskFold ? "gm1_" : "")
                        + (SkipInstCombine ? "ic1_" : "")
                        + (KeyCluster ? "kc1_" : "")
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
        PabloAST * const meInMasked = InFileOnce
            ? pb.createInFile(getInputStreamSet("meIn")[0], "meInFile")
            : getInputStreamSet("meIn")[0];
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
        // --chain-partition rules (a chain root and every rule nested under it) keep
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
        //     --chain-veto, makes it stand down at runtime), so no batched rule reads
        //     the position a chain consume clears.
        //
        // childrenOf[idAB] = the rules in THIS group whose idA == idAB (i.e. every rule
        // that should nest inside idAB's own gate — see emitChain below). Empty, so
        // isChainRule is always false, whenever --chain-partition is off.
        std::unordered_map<unsigned, std::vector<const MergeRule*>> childrenOf;
        for (const auto & r : mRuleGroup.rules)
            if (r.needsLiveId) childrenOf[r.idA].push_back(&r);
        auto isChainRule = [&](const MergeRule & r) {
            return r.needsLiveId || childrenOf.count(r.idAB) != 0;
        };
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
                    if (!isChainRule(r) && fireByLen.find(r.lenA) == fireByLen.end())
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
            return bld.createAnd(eqId(bld, srcFrozen, id), inPlayMask, "Astart_" + std::to_string(id));
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
            // --chain-partition: only the nested CHILDREN sit out the chunk loop —
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
                if (!r.needsLiveId) sorted.push_back(&r);
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
                        fireStart = selfMergeFireStarts(body, fireStart, r.lenA);
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
// ─── BPEXfrmKernel (--max-bit-xfrm-limit) ───────────────────────────────────
// A merge kernel built as a set of bit transformations instead of per-rule gates. Same
// bindings and semantics as BPEMergeKernel; it handles groups whose rules read only the
// frozen kernel input (no --chain-partition nest, --asymmetric-seam live mask or
// --chain-veto; see bitXfrmEligible).
//
// For an input id stream of N bits, rules are bucketed by L = lenA, the lookahead
// distance at which their B part starts. For each L the kernel forms one 2N-bit vector
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
// Exactness: within one kernel at most one rule fires at a position (the frozen id there
// selects idA, and rules sharing idA share L and so read one B slot), and same-kernel
// rules never touch each other's positions (dependency + seam constraints), so XOR-ing
// all change streams from the frozen input equals applying the rules one by one.
//
// Self-merges X+X need per-run pairing (selfMergeFireStarts), which a character class
// cannot express; they keep a per-rule stamp here. The seam constraints keep every other
// rule off their positions, so the stamp and the change streams never meet.
//
// Ids wider than kXfrmLowBits (re::CC codepoints stop at 0x10FFFF = 21 bits, so the pair
// vector can hold two 10-bit ids): with H = N - 10 high bits, a bucket's rules are
// further split by (idA >> 10, idB >> 10). Each such pair gets one createIf whose
// condition tests those H high bits at p and at p+L (AND the bucket's live/boundary
// gate); inside, the change and fire sets are compiled over the 20-bit vector of the low
// 10 bits of both ids, and ANDed with that condition. Every rule belongs to exactly one
// block, so the result equals the ungated form.
static constexpr unsigned kXfrmLowBits = 10;

class BPEXfrmKernel : public PabloKernel {
public:
    BPEXfrmKernel(LLVMTypeSystemInterface & ts,
                  StreamSet * sourceIn, StreamSet * meIn, StreamSet * boundaryIn,
                  StreamSet * sourceOut, StreamSet * meOut,
                  MergeRuleGroup group, uint64_t shapeHash, unsigned maxLen)
    : PabloKernel(ts, std::string("BPEXfrm_") + (boundaryIn ? "b1_" : "")
                        + (SkipInstCombine ? "ic1_" : "")
                        + (sourceIn->getNumElements() > kXfrmLowBits
                               ? "lo" + std::to_string(kXfrmLowBits) + "_" : "")
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
            Binding{"sourceIn", sourceIn, FixedRate(), LookAhead(maxLen)},
            Binding{"meIn", meIn} };
        if (boundaryIn)
            in.push_back(Binding{"boundaryIn", boundaryIn, FixedRate(), LookAhead(maxLen)});
        return in;
    }
    void generatePabloMethod() override {
        PabloBuilder pb(getEntryScope());
        std::vector<PabloAST*> srcBits = getInputStreamSet("sourceIn");
        const unsigned N = srcBits.size();
        const unsigned W_out = ceil_log2(mRuleGroup.hi);
        PabloAST * const live = getInputStreamSet("meIn")[0];
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

        // Ids of N bits: the low Nlo bits go into the character classes, the high H bits
        // (if any) into the if-block conditions.
        const unsigned H = (N > kXfrmLowBits) ? N - kXfrmLowBits : 0;
        const unsigned Nlo = N - H;
        const unsigned loMask = (1u << Nlo) - 1u;

        // Compile one block's sets in `bld` over the low-bit pair vector, masked by `mask`,
        // handing each result to addFire / addChange.
        auto buildSets = [&](auto & bld, const std::vector<const MergeRule *> & rs,
                             const std::vector<PabloAST *> & bBits, PabloAST * mask,
                             const std::string & tag, auto && addFire, auto && addChange) {
            std::vector<PabloAST *> pairBits(srcBits.begin(), srcBits.begin() + Nlo);
            pairBits.insert(pairBits.end(), bBits.begin(), bBits.begin() + Nlo);
            cc::Parabix_CC_Compiler ccc(pairBits);
            UCD::UnicodeSet fireSet;
            std::vector<UCD::UnicodeSet> changeSet(W_out);
            for (const MergeRule * r : rs) {
                const re::codepoint_t v = (r->idA & loMask) | ((r->idB & loMask) << Nlo);
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

        // H > 0: accumulators the if-blocks OR into (Pablo joins them at each block's end).
        std::vector<Var *> changeVar(W_out, nullptr);
        auto changeAcc = [&](unsigned j) -> Var * {
            if (!changeVar[j]) changeVar[j] = pb.createVar("change_" + std::to_string(j), pb.createZeroes());
            return changeVar[j];
        };
        // (high bits [Nlo, N) of `bits`) == v, as one AND tree
        auto eqHigh = [&](const std::vector<PabloAST *> & bits, unsigned v) -> PabloAST * {
            PabloAST * e = nullptr;
            for (unsigned i = Nlo; i < N; i++) {
                PabloAST * t = ((v >> (i - Nlo)) & 1u) ? bits[i] : pb.createNot(bits[i]);
                e = e ? pb.createAnd(e, t) : t;
            }
            return e;
        };

        for (const auto & [L, rules] : byLen) {
            const auto & bBits = ahead(L);
            PabloAST * const g = gate(L);
            const std::string tag = "_L" + std::to_string(L);
            if (H == 0) {
                PabloAST * fire = nullptr;
                buildSets(pb, rules, bBits, g, tag,
                          [&](PabloAST * f) { fire = f; },
                          [&](unsigned j, PabloAST * c) { orInto(change[j], c); });
                orInto(consumed, pb.createAdvance(fire, (int64_t) L));
                continue;
            }
            std::map<std::pair<unsigned, unsigned>, std::vector<const MergeRule *>> blocks;
            for (const MergeRule * r : rules) blocks[{r->idA >> Nlo, r->idB >> Nlo}].push_back(r);
            Var * fireL = pb.createVar("fire" + tag, pb.createZeroes());
            for (const auto & [hi, rs] : blocks) {
                const std::string btag = tag + "_a" + std::to_string(hi.first) + "_b" + std::to_string(hi.second);
                PabloAST * cond = pb.createAnd(pb.createAnd(eqHigh(srcBits, hi.first), eqHigh(bBits, hi.second)),
                                               g, "hi" + btag);
                auto body = pb.createScope();
                buildSets(body, rs, bBits, cond, btag,
                          [&](PabloAST * f) { body.createAssign(fireL, body.createOr(fireL, f)); },
                          [&](unsigned j, PabloAST * c) {
                              Var * acc = changeAcc(j);
                              body.createAssign(acc, body.createOr(acc, c));
                          });
                pb.createIf(cond, body);
            }
            orInto(consumed, pb.createAdvance(fireL, (int64_t) L));
        }
        for (unsigned j = 0; j < W_out; j++)
            if (changeVar[j]) orInto(change[j], changeVar[j]);

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
            PabloAST * fire = selfMergeFireStarts(pb, isX, r->lenA);
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
BPEPassResult buildBPEPassPipeline(
        kernel::PipelineBuilder & P,
        kernel::StreamSet * basis,
        const BPETokenizer & bpe,
        kernel::StreamSet * boundary) {

    compactionMode();   // validate the compaction settings first: conflicts halt here
    auto ruleRanges = bpe.buildMergeRuleRanges();

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
              << (LevelPartition ? "ASAP level schedule" : "contiguous clean ranges")
              << (PartitionByMergeIdBits ? ", per merge id bit width" : "") << ")\n";
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
    unsigned nXfrm = 0, nXfrmIneligible = 0;   // --max-bit-xfrm-limit kernel counts
    for (size_t i = 0; i < ruleRanges.size(); i++) {
        auto & g = ruleRanges[i];
        if (g.rules.empty()) continue;
        unsigned output_bits = ceil_log2(g.hi);
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
        // A --chain-partition rule no longer disqualifies the whole kernel: the
        // grouped-if path now skips just the chain-involved rules and emits them
        // its chain roots' nests inside its chunk gates (see isChainRule / emitChain in
        // BPEMergeKernel::generatePabloMethod).
        const int groupLowerLimit = effIfGroupLowerLimit();
        bool grouped = (groupLowerLimit >= 0) && ((long) i >= (long) groupLowerLimit);
        if (source->getNumElements() <= MaxBitXfrmLimit && !nextId && bitXfrmEligible(g)) {
            P.CreateKernelCall<BPEXfrmKernel>(source, inPlayMask, boundary, sOut, meOut,
                                              g, hashRuleSet(g.rules), g.maxLen);
            nXfrm++;
        } else {
            if (source->getNumElements() <= MaxBitXfrmLimit) nXfrmIneligible++;
            P.CreateKernelCall<BPEMergeKernel>(source, inPlayMask, boundary, nextId, sOut, meOut,
                                               g, hashRuleSet(g.rules), g.maxLen, grouped);
        }
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

    if (MaxBitXfrmLimit > 0)
        std::cerr << "[BPE] max-bit-xfrm-limit=" << MaxBitXfrmLimit << ": " << nXfrm
                  << " bit-transformation kernels" << (nXfrmIneligible
                      ? ", " + std::to_string(nXfrmIneligible) + " narrow kernels kept per-rule "
                        "(chain/asymmetric-seam/veto rules or --indexed-shift)" : std::string())
                  << "\n";
    // The emitter packs exactly 16 id bits (P2S16Kernel); widen a narrower final stream.
    if (source->getNumElements() < 16) {
        StreamSet * source16 = P.CreateStreamSet(16, 1);
        P.CreateKernelCall<BPEWidenKernel>(source, source16);
        source = source16;
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
    if (AsymmetricSeam || ChainVeto || IfTestSignificantBits == 0 || lowerLimit < 0) {
        std::cerr << "[BPE] --key-cluster ignored: needs grouping with --if-test-significant-bits, "
                     "and no --asymmetric-seam / --chain-veto\n";
        return;
    }
    const bool chain = ChainPartition;
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
// Key-aware split (with --if-test-significant-bits, unless --asymmetric-seam or
// --chain-veto): chunks are filled by gate key, so rules sharing a gate stay in one kernel
// (what --key-cluster gathered is not scattered again). Any assignment of a group's rules
// to consecutive kernels is valid when no two of them are rank-order dependent — no
// dependency or seam lies inside a group — except a --chain-partition nest, whose child
// reads its producer's stamp in the same kernel; a chain tree is therefore kept whole.
// --asymmetric-seam and --chain-veto relate same-group rules by rank, so with either of
// those the split stays in rank order.
static std::vector<MergeRuleGroup> splitOversizedGroups(std::vector<MergeRuleGroup> groups,
                                                        unsigned & nSplit) {
    const size_t cap = MaxMergesPerKernel;
    if (cap == 0) return groups;
    const bool byKey = IfTestSignificantBits > 0 && !AsymmetricSeam && !ChainVeto;
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
    //    count, same constraints); the default is the clean_range walk (see
    //    cleanRangePartition). --partition-by-merge-id-bits partitions each idAB bit
    //    tier separately and concatenates the tiers in order (see PartitionByMergeIdBits).
    //    Oversized groups are split right after each partition (--max-merges-per-kernel),
    //    so a tier's context counts the kernels that will really precede it.
    unsigned nSplit = 0;
    auto partition = [&](const std::vector<MergeRule> & rs, const PartitionContext & ctx) {
        return splitOversizedGroups(LevelPartition ? levelPartition(rs, ctx) : cleanRangePartition(rs),
                                    nSplit);
    };
    std::vector<MergeRuleGroup> groups;
    if (PartitionByMergeIdBits) {
        // rules are rank-sorted, so each bit tier is a contiguous run of them
        PartitionContext ctx;
        size_t i = 0, n = rules.size();
        while (i < n) {
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
            for (auto & g : tier) groups.push_back(std::move(g));
            i = j;
        }
    } else {
        groups = partition(rules, PartitionContext());
    }
    if (nSplit)
        std::cerr << "[BPE] max-merges-per-kernel=" << MaxMergesPerKernel << ": split " << nSplit
                  << " kernels -> " << groups.size() << " kernels total"
                  << (IfTestSignificantBits > 0 && !AsymmetricSeam && !ChainVeto ? " (by gate key)" : "")
                  << "\n";
    tagFlushPoints(groups);
    if (LevelPartition) {
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
    }
    return groups;
}
