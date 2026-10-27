# Issue #26, part 3: crash in the LLVM greedy register allocator

Issue: https://github.com/prof-emeritus-robc/parabix-devel/issues/26
("Pipeline phase boundaries: lost final items, a spurious cycle, and an LLVM
register-allocator crash"), section 3.

The issue reports that `nfc -focus-layers=focus` exits with status 139 and no
output. The crash is in `HoistSpillHelper::runHoistSpills`, called from
`InlineSpiller::postOptimization` and `RAGreedy`, while the pipeline kernel is
being compiled. The issue suspects an LLVM 18 bug.

**It is not an LLVM bug.** The Parabix pipeline compiler produces invalid IR:
a value is used in blocks that its definition does not dominate. The register
allocator crashes on the resulting machine code. This note gives a
reproduction, shows the invalid IR, explains the cause, and describes the fix.

## Summary

* **The cause is invalid IR from the pipeline compiler.** `-VerifyIR` rejects
  the IR before any optimization ("Instruction does not dominate all uses!"),
  and `-verify-machineinstrs` rejects the machine code right after
  instruction selection ("Virtual register defs don't dominate all uses").
  The register allocator crash is only where the bad code does damage.
* **Every pipeline with a phase boundary right after the focus stage has
  invalid IR,** whether or not it crashes. nfc at the branch tip with LLVM 18
  ran and gave correct output, but `-VerifyIR` rejects its IR too. Phase
  boundaries after `filter`, `placement` or `transform` give valid IR.
* **Whether it crashes depends on the LLVM version and the shape of the code,
  not on the input.** A 6-byte input crashes too, because the crash happens
  during compilation. Which tool crashes changed from one commit to the next,
  and between LLVM 18.1.8 and 22.1.8.
* **The workarounds only hide the problem.** `-regalloc=basic` and
  `--backend-optimization-level=none` avoided every crash. Neither one makes
  the IR valid. `-split-spill-mode=size` did not avoid any crash except one
  (nfc at efe701fca with LLVM 22). `-disable-spill-hoist` has no effect: in
  LLVM 18.1.8 the option is declared in `InlineSpiller.cpp` but never read,
  and LLVM 22 removed it.
* **Fixed** by reading the item counts and termination signals of
  earlier-phase producers once per segment, at the entry of the phase (see
  "Fix" below). With the fix, nfc, nfd and nfkd pass `-VerifyIR
  -verify-machineinstrs` and give correct output with every `-focus-layers`
  setting, on both LLVM versions.

## Reproduction (before the fix)

Environment: Apple M1 (arm64, `ARM_Neon` target), macOS 14.6.1, Apple clang
16.0.0. Release builds against `/opt/llvm/libllvm18` (18.1.8) and
`/opt/llvm/libllvm22` (22.1.8).

### Branch and commits

* Branch `focus-paradigm`.
* **efe701fca** ("Phase 1 steps 4 and 5: shared -NoFocus and -focus-layers;
  focus mode tests"). This is the commit the issue was written against: it
  adds `-focus-layers`, and the issue was filed 2026-10-20 17:49 PDT, after
  this commit and before 7005984ab. The crash is in **nfc** with both LLVM
  versions.
* **47e059423** (tip of `focus-paradigm` when this note was written). The
  crash is in **nfd** and **nfkd** with both LLVM versions, and in **nfc**
  with LLVM 22 only.

### Steps

    git checkout efe701fca            # or 47e059423
    make -C build18 -j8 nfc nfd nfkd
    make -C build22 -j8 nfc nfd nfkd
    python3 -c "import sys; sys.path.insert(0, 'QA/focus'); import focus_modes_test as f; open('fm.txt', 'wb').write(f.text().encode())"
    mkdir -p /tmp/emptycache
    build18/bin/nfc --object-cache-dir=/tmp/emptycache -focus-layers=focus fm.txt; echo $?
    # 139, no output
    build18/bin/nfc --object-cache-dir=/tmp/emptycache -focus-layers=focus -VerifyIR fm.txt
    # Instruction does not dominate all uses!

Use an empty object cache directory each time, or a new
`--object-cache-salt`. The other kernels are cached normally. The pipeline
kernel never reaches the cache because its compilation crashes, so with a
reused cache the crash still recurs.

`QA/focus/issue26_regalloc_repro.sh [build-dir ...]` runs nfc, nfd and nfkd,
with and without `-focus-layers=focus` and with each workaround. It uses a
fresh cache for every run and checks the output against Python's
`unicodedata.normalize`.

### Backtrace (LLVM 18.1.8, nfc at efe701fca)

    stop reason = EXC_BAD_ACCESS (code=1, address=0x8)
    #0 (anonymous namespace)::HoistSpillHelper::runHoistSpills(...) + 3436
    #1 (anonymous namespace)::InlineSpiller::postOptimization() + 892
    #2 llvm::RegAllocBase::postOptimization() + 44
    #3 llvm::RAGreedy::runOnMachineFunction(llvm::MachineFunction&) + 2344
    #4 llvm::MachineFunctionPass::runOnFunction(llvm::Function&)
    ...
    #8 CPUDriverCompiler::materializeObject(...)
    #9 CPUDriverCompiler::materializeDecl(...)

LLVM 22.1.8 crashes at `InlineSpiller::postOptimization + 2664` (with
`runHoistSpills` inlined) ← `RegAllocBase::postOptimization` ←
`RAGreedy::run` ← `RAGreedyLegacy::runOnMachineFunction`. The faulting address
is again `0x8`.

The crashing module is the main pipeline kernel. Compare the object cache
from a crashing run with one from a `-regalloc=basic` run: every kernel object
is the same except the pipeline kernel's (`parabix-P2c045e34…_ARM_Neon_*.o`,
188 KB at efe701fca), which is missing after the crash.

### Results before the fix

Input: `QA/focus/focus_modes_test.py` text (8480 bytes). "139" means a crash
(SIGSEGV); "ok" means the output matches `unicodedata.normalize`.

At efe701fca:

| tool | `-focus-layers=` | LLVM 18 | LLVM 22 |
|------|------------------|---------|---------|
| nfc  | (none)           | ok      | ok      |
| nfc  | focus            | **139** | **139** |
| nfc  | filter / placement / transform | ok | ok |
| nfc  | focus,filter,placement,transform | ok | ok |
| nfd  | focus, filter    | wrong output (part 1 of the issue) | same |
| nfd  | placement / transform | ok | ok |
| nfkd | each stage       | ok      | ok      |

Workarounds for nfc `-focus-layers=focus` at efe701fca:

| option | LLVM 18 | LLVM 22 |
|--------|---------|---------|
| `-regalloc=basic` | ok | ok |
| `--backend-optimization-level=none` | ok | ok |
| `--backend-optimization-level=less` | 139 | 139 |
| `-split-spill-mode=size` | 139 | ok |
| `-disable-spill-hoist` | 139 (option has no effect) | unknown option |
| `-focus-filter=bit -reintegrate=bit` | ok | ok |
| `-NoFocus` | ok | ok |

At 47e059423 (tip of `focus-paradigm`):

| tool | `-focus-layers=` | LLVM 18 | LLVM 22 |
|------|------------------|---------|---------|
| nfc  | focus            | ok (but the IR is invalid) | **139** |
| nfd  | focus            | **139** | **139** |
| nfkd | focus            | **139** | **139** |
| nfc, nfd, nfkd | filter / placement / transform | ok | ok |

At the tip, `-regalloc=basic` and `--backend-optimization-level=none` avoided
every crash, and `-split-spill-mode=size` avoided none of them.

## The invalid IR

At 47e059423, `-verify-machineinstrs` fails in each crashing case, right after
instruction selection, in one `ComputeThread` function of the pipeline kernel:

    # After Instruction Selection
    # Machine code for function Pb98679bb…_ComputeThread8_93: IsSSA, TracksLiveness
    *** Bad machine code: Virtual register defs don't dominate all uses. ***
    - v. register: %222
    LLVM ERROR: Found 1 machine code errors.

`-VerifyIR` reports the same problem in the LLVM IR, before optimization (nfd
with LLVM 18):

    Instruction does not dominate all uses!
      %"1O0.IN" = load i64, ptr %28, align 8
      %2270 = sub i64 %"1O0.IN", %"18I2_alreadyProcessed"
    Instruction does not dominate all uses!
      %"1O0.IN" = load i64, ptr %28, align 8
      %"18I2_finalProcessed" = phi i64 [ %"1O0.IN", %"18:post_LSN_acquired82" ]

`1O0.IN` is the produced item count of output 0 of kernel 1, the source
kernel, which runs in an earlier phase. Kernel 18 uses a load of that count
that one of its predecessors made in a block that does not dominate kernel 18.

## Cause

`readAvailableItemCounts` (`buffer_management_logic.cpp`) caches each consumed
stream set's available item count in `mLocallyAvailableItems`. The first
kernel that reads the stream set fills the cache. For a stream set produced
in an earlier phase, `readAvailableItemCount` loads the count from the
producer's `.IN` scalar at the current insertion point: the first consumer's
entry block. The cache is cleared only once per compute function, in
`PipelineCompiler::start`.

For a stream set produced in the *current* phase, the compiler handles a
consumer in a later partition that can be reached without passing through the
earlier ones. It adds a PHI node to every partition between producer and
reader (`mPartitionProducedItemCountPhi`), and when it jumps over a partition
it loads the count at the jump (`phiOutPartitionItemCounts`). Stream sets from
an earlier phase get no such PHI nodes. A later consumer therefore reuses the
first consumer's load even when control can reach the later consumer without
passing through the first one.

A phase boundary right after the focus stage puts the source kernel and the
focus in phase 1, and the rest of the pipeline in phase 2. Many kernels in
several partitions of phase 2 then read stream sets from phase 1, such as the
byte stream. Boundaries after later stages leave fewer such consumers. With
those boundaries in nfc, nfd and nfkd, no consumer reused a load that did not
dominate it, and `-VerifyIR` accepted the IR.

## Fix

`readEarlierPhaseItemCounts` (`buffer_management_logic.cpp`) is called at the
end of `branchToInitialPartition`, once per segment. In a multithreaded
pipeline, the first kernel of the phase has acquired its segment lock by that
point. The entry of the phase's
first partition dominates every kernel of the phase. The function:

1. finds every stream set consumed in the current phase whose producer is in
   an earlier phase. A truncated stream set counts with the stream set it is
   a truncation of. Constant stream sets and pipeline inputs are excluded;
2. reads the termination signals of those producers, and then the produced
   item counts, with an acquire fence between them. A producer that is seen
   as closed therefore has a final count;
3. stores both in `mEarlierPhaseTerminationSignal` and
   `mEarlierPhaseAvailableItems`.

`readAvailableItemCounts` then starts each consumer from the snapshot, never
from a value computed by an earlier consumer. `hasKernelTerminated` uses the
snapshot signal for earlier-phase producers. Every kernel in a segment
therefore sees the same item count and closed state for a stream set from an
earlier phase.

A snapshot taken at the start of the segment can be older than a count a
kernel saw in its previous segment. Another thread may have run the kernel's
previous segment, and read a later count, after the snapshot was taken. A
non-data-parallel consumer therefore takes the maximum of the snapshot and its
processed item count, so it never sees fewer items available than it has
already processed. Data-parallel and relative-rate ports are not clamped: for
those, the processed count is not a lower bound on the produced count.

Trade-offs: later partitions in a segment see the earlier phase's count as it
was at the start of the segment, so they may process less per segment, and a
producer's termination may be noticed one segment later. The snapshot values
are live through the whole compute function. That is a handful of `i64`
values per phase.

## Results after the fix

Branch `doc/issue-26-regalloc-repro` (47e059423 plus the fix). Each tool ran
on the same input with an empty object cache, with `-VerifyIR
-verify-machineinstrs`, and with no phase boundary, with each of `focus`,
`filter`, `placement` and `transform`, and with all four together. This gives
36 runs at the default thread count: all compiled without verifier errors and
gave output equal to `unicodedata.normalize`, with both LLVM 18.1.8 and
22.1.8.

A full rebuild of `build18` and `build22` with the fix passed all 42 `ctest`
tests in each build. These include `focus_modes_{nfd,nfc,nfkd}_test`, which
run with `--thread-num=3 --segment-size=1024` and
`-focus-layers=filter,transform`, and the `-SeparatedPipelineStages` and
`-MultiStage` tests.

## Other findings

* Every tool aborts on an empty input file with
  `LLVM ERROR: Unable to allocate aligned file buffer` (exit 134), with or
  without phase boundaries.
* The phase-boundary tests should run with `-VerifyIR`. That would have
  reported this problem when `-focus-layers` was added.
