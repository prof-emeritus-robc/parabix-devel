# Issue #26, part 3: crash in the LLVM greedy register allocator

Issue: https://github.com/prof-emeritus-robc/parabix-devel/issues/26
("Pipeline phase boundaries: lost final items, a spurious cycle, and an LLVM
register-allocator crash"), section 3.

The issue reports that `nfc -focus-layers=focus` exits with status 139 and no
output. The crash is in `HoistSpillHelper::runHoistSpills`, called from
`InlineSpiller::postOptimization` and `RAGreedy`, while the pipeline kernel is
being compiled. The issue suspects an LLVM 18 bug. This note gives a
reproduction and assesses the crash with LLVM 18.1.8 (`build18`) and LLVM
22.1.8 (`build22`).

## Summary

* **Reproduced with both LLVM 18.1.8 and LLVM 22.1.8.** The crash is the same
  in both: a load from address `0x8` in `runHoistSpills` (in LLVM 22 the
  function is inlined into `InlineSpiller::postOptimization`). The bug is not
  fixed in LLVM 22, so upgrading LLVM is not a fix.
* **The crash depends on the shape of the code, not on the input.** A 6-byte
  input (`hello\n`) crashes just as the test text does, because the crash
  happens while compiling, before any data is read. Small pipeline changes
  decide whether a given tool crashes (see the table: nfc stopped crashing
  with LLVM 18 at 7005984ab, while nfd and nfkd started).
* **Only `-focus-layers=focus` crashes.** Phase boundaries after `filter`,
  `placement` or `transform`, or after all four stages together, compile with
  both LLVM versions.
* **Reliable workarounds:** `-regalloc=basic` and
  `--backend-optimization-level=none`. Both give correct output in every
  failing case.
* **`-split-spill-mode=size` is not a reliable workaround.** It did not avoid
  any of the crashes reproduced here with either LLVM version. The one
  exception is nfc at efe701fca with LLVM 22.
* **`-disable-spill-hoist` cannot help.** In LLVM 18.1.8, `DisableHoisting` is
  declared in `llvm/lib/CodeGen/InlineSpiller.cpp` but is never read. LLVM 22
  removed the option, and `build22` binaries reject it.
* `--backend-optimization-level=less` still crashes. The crash needs the
  greedy register allocator with spill hoisting, which runs at every level
  above `none`.

## Reproduction

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
188 KB at efe701fca), which is missing after the crash. The pipeline kernel
contains the compute-thread functions of all partitions.

## Results

Input: `QA/focus/focus_modes_test.py` text (8480 bytes). The input does not
affect whether a tool crashes. "139" means a crash (SIGSEGV); "ok" means the
output matches `unicodedata.normalize`.

### efe701fca

| tool | `-focus-layers=` | LLVM 18 | LLVM 22 |
|------|------------------|---------|---------|
| nfc  | (none)           | ok      | ok      |
| nfc  | focus            | **139** | **139** |
| nfc  | filter / placement / transform | ok | ok |
| nfc  | focus,filter,placement,transform | ok | ok |
| nfd  | focus, filter    | wrong output (part 1 of the issue) | same |
| nfd  | placement / transform | ok | ok |
| nfkd | each stage       | ok      | ok      |

Workarounds for nfc `-focus-layers=focus`:

| option | LLVM 18 | LLVM 22 |
|--------|---------|---------|
| `-regalloc=basic` | ok | ok |
| `--backend-optimization-level=none` | ok | ok |
| `--backend-optimization-level=less` | 139 | 139 |
| `-split-spill-mode=size` | 139 | ok |
| `-disable-spill-hoist` | 139 (option has no effect) | unknown option |
| `-focus-filter=bit -reintegrate=bit` | ok | ok |
| `-NoFocus` | ok | ok |

### 47e059423 (tip of `focus-paradigm`)

| tool | `-focus-layers=` | LLVM 18 | LLVM 22 |
|------|------------------|---------|---------|
| nfc  | focus            | ok      | **139** |
| nfd  | focus            | **139** | **139** |
| nfkd | focus            | **139** | **139** |
| nfc, nfd, nfkd | filter / placement / transform | ok | ok |

At the tip, `-regalloc=basic` and `--backend-optimization-level=none` avoid
every crash, and `-split-spill-mode=size` avoids none of them.
`-u8-bit-movement=advance` (the default before 47e059423) changes the code
enough to avoid the crash in nfd and nfkd with LLVM 18, and in nfkd with
LLVM 22. nfd with LLVM 22 still crashes.

Commit 7005984ab made `ExpandFilter` a single kernel and added the
LookAhead-mode UTF-8 kernels. Both change the pipeline function: nfc stopped
crashing with LLVM 18, and nfd and nfkd started.

## Assessment

* This is a latent LLVM bug in spill hoisting (`InlineSpiller.cpp`,
  `HoistSpillHelper`), present from 18.1.8 through 22.1.8. A null
  `MachineDomTreeNode` or block pointer is dereferenced at offset 8. Parabix
  triggers it with the large, branchy pipeline function that a phase boundary
  right after the focus stage produces. That the crash moves between tools
  from one commit to the next shows how sensitive it is to code shape.
* The issue rejected `-split-spill-mode=size` as a global workaround on the
  grounds that it affects every program. In addition, it does not work here.
  A workaround would have to use `-regalloc=basic`, or `-O0`
  code generation for the pipeline kernel only. Applying either just to
  pipeline kernels that use phase boundaries would limit the cost to those
  pipelines.
* To report the bug upstream, a standalone `.ll`/`.bc` reproducer for `llc`
  is needed. `-ShowIR` does not give one: it prints the optimized IR one
  function at a time, without declarations or attribute groups, and only
  after code generation, so a crashing run writes nothing. A small hook that
  writes the module bitcode before `materializeObject` (or `-ShowIR` from a
  `-regalloc=basic` run, made into a complete module) would allow
  `llc -O2 -mtriple=arm64-apple-macosx` reproduction and then `llvm-reduce`.
* Unrelated, found while testing: every tool aborts on an empty input file
  with `LLVM ERROR: Unable to allocate aligned file buffer` (exit 134), with
  or without phase boundaries.
