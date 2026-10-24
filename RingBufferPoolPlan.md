# Phase 3 plan: reuse ring buffers across pipeline runs

Phase 1 (commit d75b494a1) made Mach `vm_remap` the macOS default for circular buffers.
Phase 3 stops allocating them again on every pipeline run, on all platforms.

## What the code does today

- **Allocation.** Every pipeline call allocates its non-linear internal buffers through
  `make_circular_buffer`. With `icgrep`, that is 6 per file: 600 for 100 files.
- **Release.** Buffers are freed through the IR symbol `munmap` in two places:
  - `ManagedDynamicBuffer::releaseBuffer` (`lib/kernel/core/streamset.cpp:893`), which passes
    `2 × capacity` bytes;
  - the pending-deletion list (`streamset.cpp:1095–1210`), which handles buffers retired by
    expansion. Its length is also the full mapped length, recorded at `streamset.cpp:1869`.
- **`munmap` is shared, so it can't simply be intercepted.** The source kernel links the same
  name to unmap input files (`lib/kernel/io/source_kernel.cpp:37`). The disabled fd-backed
  buffer also calls it, through the same pending-deletion code, with a single-size length
  (`streamset.cpp:2356`).
- **Linear buffers** use `malloc`/`free`, which already reuses memory, so they are out of scope.

## Design

1. **Release function.** Add `extern "C" void release_circular_buffer(void * addr, size_t mappedLength)`,
   linked as `__ss_fcb` next to `__ss_mcb`. It has the same signature as `munmap`, so each IR
   call site changes by one symbol.
   - Switch the non-linear branch of `releaseBuffer` to it.
   - Give `removeFromPendingDeletions` (and its callers) the release function to call, so
     expansion-retired circular buffers use `__ss_fcb` while fd-backed buffers keep `munmap`.
2. **Pool.** A static free list inside `streamset.cpp`, keyed by mapped length: a
   `std::unordered_map<size_t, std::vector<void*>>` plus a running byte total, guarded by a
   `std::mutex`. Worker threads expand buffers concurrently, and the lock is uncontended in
   the common case.
   - `make_circular_buffer(size, 0)` pops an exact match for `2 × size` before calling the
     user hook, the Mach path or memfd.
   - `hasUnderflow == 1` always skips the pool. Only the hook and the test use it.
   - An exact-size match is enough, because sizes are page multiples set by buffer analysis
     and repeat from run to run.
3. **Cap.** Add `-CircularBufferPool=<MiB>` (default 64; `0` turns pooling off, as an escape
   hatch and for A/B timing). If a release would push the total over the cap, the buffer is
   unmapped instead of pooled.
4. **Exit.** An `atexit` drain unmaps everything and sets a `drained` flag. Any release after
   that unmaps directly, so a late pipeline teardown can't touch a destroyed pool. The same
   drain is exposed as `kernel::releaseCircularBufferPool()` for long-running library users.
5. **Stale data.** A fresh mapping is all zeros; a reused one holds the previous run's bytes.
   The pipeline already zeroes the space past each stream's final items
   (`lib/kernel/pipeline/compiler/codegen/buffer_manipulation_logic.cpp:32, 374, 651, 720`),
   so I expect nothing depends on fresh zeros, but this has to be shown, not assumed.
   - Add a debug flag, `-EnableAsserts` or a new `-PoisonReusedBuffers`, that fills each
     reused buffer with `0xA5` before handing it back.
   - Run `make check` once with it.
   - If anything fails, the fallback is to zero reused buffers with `memset`. That still skips
     the syscalls and page faults, so most of the gain remains.
6. **Cache keys.** Kernel IR changes (`munmap` → `__ss_fcb`). The source hash covers
   `kernel.core`, so cached kernels recompile automatically on first run. Confirm with one
   `-trace-object-cache` run, and note in the commit that the first run is cold. Old cached
   objects can't be left calling `munmap` on pooled buffers, because their hash changes too.

## Tests and measurement

- **Extend `tests/test_circular_buffer.cpp`** (must stay under a second):
  - Set `-CircularBufferPool=0` for the existing mirror checks, since a pooled Mach buffer
    would fail the memfd file-backed check.
  - Add pool cases:
    - release then allocate returns the same address with its mirroring intact;
    - a different size misses the pool;
    - the cap is respected;
    - after a drain, allocation maps fresh memory.
- **`make -j3 check`** twice: default, and with the poison flag.
- **A/B timing** with `-CircularBufferPool=0` vs the default: `icgrep -c` over the same 100
  files, plus one single-file run. Main expected gain: reused buffers are already paged in,
  so the first-touch cost measured in phase 1 (2.5× of allocation cost at 1 MB) goes away
  along with the 600 Mach calls.

## Files touched

- `lib/kernel/core/streamset.cpp`: pool, `__ss_fcb`, IR call sites
- `include/kernel/core/streamset.h`: drain API, comment
- `include/toolchain/toolchain.h`, `lib/toolchain/toolchain.cpp`: pool and poison options
- `tests/test_circular_buffer.cpp`: new pool cases

## Outcome

- Implemented as planned. One change from the design: a reused buffer is zeroed before it is
  handed out, not returned as-is. Without that, `test_scan` (`filter_no_spans`, an empty
  filter stream) and, under ctest, `test_bytefilterandspread` failed: some pipelines read
  bytes no kernel wrote and rely on fresh pages being zero.
- `-PoisonReusedBuffers` fills reused buffers with `0xA5` instead of zeros. Under it
  `test_scan`, `test_bytefilterandspread` and `test_sorting` fail, which locates that
  reliance; it is a diagnostic, not expected to pass.
- `make -j3 check`: 49/49 pass with the defaults.
- `icgrep -c '12.*5'` over 100 small files, 20 runs each: system time 0.55 s → 0.31 s,
  wall 0.79 s → 0.74 s (`-CircularBufferPool=0` vs default); output identical.
