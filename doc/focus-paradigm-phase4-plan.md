# Focus / transform / reintegrate: Phase 4 plan (layers)

Written after Phase 1 (the `FocusedTransform` builder and `-focus-layers`), Phase 2
(run-based filter and reintegration), the UTF-8 bit-movement change
(`focus-paradigm-bit-movement.md`) and the fix for issue #26 part 3
(`issue-26-regalloc-crash.md`, bbf6179a8).

Interim results: `focus-paradigm-phase4.md`.

## Goal

Run a stage of a focused transform, above all the transform itself, as a separate
pipeline layer (a phase, `PipelineBuilder::InsertPhaseBoundary`), so that:

- the transform processes full segments of focused data instead of the small part
  of a segment that one source segment yields (2% on de, 64% on ko), which should
  lower its per-segment overhead on dense text (Phase 0: the transform is 47–77% of
  the time there);
- stages of different rates (source, focused, output) can later get threads in
  proportion to their work (Phase 5).

Phase 1 made layering possible (`-focus-layers=focus,filter,placement,transform`);
measuring and improving it is this phase.

## How phases run today

From `multithreading_model_logic.cpp`, `pipeline_logic.cpp` and
`buffer_management_logic.cpp`:

- Each phase has its own segment numbers (`PHASE_NEXT_LOGICAL_SEGMENT_NUMBER`).
  Every thread runs phase 1, then phase 2, and so on, in a loop.
- In a phase, a thread takes segment numbers until a segment makes no progress, the
  pipeline terminates, or it reaches its step limit: its first segment number in
  this visit plus `DEFAULT_INITIAL_PHASE_SEGMENT_LIMIT` (100, `config.h`). The
  limit is never adapted (a TODO in the code).
- Stream sets that cross a phase boundary get buffers 100 times larger
  (the same constant), so that one phase can run 100 segments ahead.
  `DISABLE_FD_BACKED_BUFFERS` is defined, so they are ordinary dynamic buffers.
- A segment of phase p runs every kernel of phase p, each under the kernel's
  segment lock, in segment order, whether or not the earlier phase has produced
  anything new.
- The "expected I/O ratio" restrictions of `InsertPhaseBoundary(streamSet, ratio)`
  are recorded in the pipeline signature only; the compiler does not use them.

## First measurements (2026-10-21)

Battery power, best of 1–3 runs, so indicative only. Inputs of 50 MB rebuilt as in
Phase 0. nfd with the current defaults.

| input | `-focus-layers=` | 1 thread s | 3 threads s | peak RSS MB |
|---|---|---|---|---|
| de | (none) | 0.15 | 0.09 | 72 |
| de | filter | 0.19 | 0.16 | 320 |
| de | placement | 0.17 | 0.13 | 224 |
| de | transform | 0.17 | 0.13 | 224 |
| de | placement,transform | 0.17 | 0.13 | |
| ko | (none) | 0.65 | 0.27 | 54 |
| ko | filter | 0.74 | 0.69 | 523 |
| ko | placement | 0.72 | 0.55 | 408 |
| ko | transform | 0.71 | 0.35 | 356 |
| ko | placement,transform | 0.75 | 0.63 | |

Every boundary is correct (nfd, nfc and nfkd, each boundary and all four together,
`-VerifyIR`, 3 threads, `--segment-size=1024`, against Python), but every boundary
is slower, and much slower at 3 threads, and uses 3–10 times the memory.

Observations:

- **Threads wait in the later phases.** On ko with a boundary after the filter, user
  time at 3 threads is 1.92 s against 0.71 s without it. The cycle counter puts 77%
  of the first phase-2 kernel's time in pipeline overhead, and that kernel takes half
  of all ticks. (A first reading of `-TraceStridesPerSegment`, counting zero entries
  per kernel, suggested many later-phase segments that process nothing. That was
  wrong: within a phase a partition may process nothing in one segment and twice as
  much in the next, and no segment of a later phase is empty in every kernel; see
  step 4.)
- **Memory grows with the input, not with the buffer factor.** With a boundary,
  nfd's peak RSS on ko is 92, 166 and 350 MB for 5, 20 and 50 MB inputs (52 MB for
  each without a boundary), and the same with a phase step of 4, 16 or 100
  segments (step 2). Some stream set that crosses the boundary keeps nearly all its
  data, so a 1 GB input would need several GB. System time rises from 0.02 s to
  about 0.10 s on ko (19,000–27,000 page faults against 379).
- **At 1 thread the boundary costs 6–14%,** so a full segment of focused data does
  not yet pay for the extra pipeline work and memory.

## Steps

Each step keeps output byte-identical and `ctest -j3` passing. Timings that go into
the report are taken on mains power with `QA/focus_baseline.py` (best of 5, warm
cache), at 1 and 3 threads, with the scaling ceiling.

1. **Measurement.**
   - `QA/focus_baseline.py`: report peak RSS and system time; add a `--layers`
     option that runs each configuration with the given `-focus-layers` settings.
   - `QA/phase_segments.py`: from `-TraceStridesPerSegment`, the number of segments
     per phase and how many of them processed nothing.
2. **Phase step limit as an option.**
   - `--phase-segment-limit=N` (default 100, the current constant) sets both the
     step limit and the cross-phase buffer factor. It is part of the pipeline
     signature when the pipeline has phase boundaries, so cached pipelines stay
     correct.
   - Measure N = 4, 16, 100: memory, empty segments and time. This separates the
     cost of large buffers from the cost of empty segments.
3. **Bounded memory across phase boundaries** (pipeline compiler).
   - Find the stream set that keeps its data (a consumer in a later phase whose
     consumed count does not reach the producer, or a buffer that grows instead of
     wrapping), and fix it. Memory with a boundary should not depend on the input
     size.
   - `-TraceDynamicBuffers` crashes nfd (exit 139) with or without a boundary, and
     `-TraceUnconsumedItemCounts` reports only zeros; fix or replace them first if
     they are needed to find it. (Both fixed after step 4; see the report.)
4. **Threads serialized in later phases** (pipeline compiler). Revised after the
   investigation recorded under "Status of the steps".
   - The cause is not empty segments but partition jumps: a later-phase segment
     whose partition finds no new input in the earlier phase's stream sets skips the
     partition, and must first wait for the previous segment to release every
     skipped kernel's lock. Consecutive segments then run one after another.
   - Remedies to try, smallest first:
     - claim a later-phase segment only if the unconsumed earlier-phase input is
       enough for it to have its own data beyond what the segments in flight will
       take (needs a per-stream-set estimate of a segment's consumption);
     - let a skipped partition's locks be released without waiting for the
       previous segment when the partition has no state that depends on order
       (for example, when all its kernels are state-free);
     - run a later phase with fewer threads, or only after the earlier phase is a
       given number of segments ahead (Phase 5 territory).
   - Gate: the issue #26 matrix (three tools × every boundary × `-VerifyIR`),
     `focus_modes_*_test`, and the test programs that use phases elsewhere
     (icgrep's phase boundaries in `grep_engine.cpp`).
5. **Measure the layer configurations** on mains power:
   - nfd and nfkd on de, vi, ko; nfc on de.nfd, ko.nfd, de;
   - `-focus-layers` none, transform, placement,transform (nfd/nfkd), filter
     (nfc: the transform and placement in phase 2), at 1 and 3 threads;
   - stage tables and ceilings for the best configuration per input.
6. **Report and defaults.** `doc/focus-paradigm-phase4.md`. Make a boundary the
   default only where it wins at both 1 and 3 threads.

## Out of scope

- Thread allocation per layer and scaling past 3 threads: Phase 5. Step 4 should
  not make that harder, for example by tying a phase to a fixed thread.
- Separate tasks for the stages (`-SeparatedPipelineStages`, `-MultiStage`): they
  stay as they are, as a comparison for step 4.
- The adaptive choice between focus and no focus by density (Phase 0, Phase 2).

## Status of the steps

- **Step 1 (measurement):** done. `QA/focus_baseline.py` reports the system time
  of the best N-thread run and the peak RSS, and `--layers` runs each configuration
  with the given `-focus-layers` settings. `QA/phase_segments.py` summarizes
  `-TraceStridesPerSegment` per kernel (segments, empty segments, mean strides).
- **Step 2 (phase step limit):** done. `--phase-segment-limit=N` replaces
  `DEFAULT_INITIAL_PHASE_SEGMENT_LIMIT`; it is added to the pipeline signature
  (`$L<N>`) when the pipeline has phase boundaries. Results (nfd, battery, best of
  2; RSS of the last run):

  | input | `-focus-layers=` | N | 1 thread s | 3 threads s | RSS MB |
  |---|---|---|---|---|---|
  | ko | transform | 4 / 16 / 100 | 0.70 / 0.70 / 0.70 | 0.34 / 0.33 / 0.34 | 348 / 336 / 356 |
  | ko | placement,transform | 4 / 16 / 100 | 0.72 / 0.72 / 0.74 | 0.53 / 0.56 / 0.61 | 439 / 380 / 551 |
  | ko | filter | 4 / 16 / 100 | 0.71 / 0.70 / 0.74 | 0.57 / 0.63 / 0.70 | 359 / 307 / 522 |
  | de | transform | 4 / 16 / 100 | 0.18 / 0.17 / 0.17 | 0.12 / 0.11 / 0.12 | 209 / 173 / 224 |
  | de | placement,transform | 4 / 16 / 100 | 0.17 / 0.16 / 0.17 | 0.12 / 0.12 / 0.13 | 211 / 178 / 223 |
  | de | filter | 4 / 16 / 100 | 0.19 / 0.18 / 0.19 | 0.15 / 0.14 / 0.16 | 316 / 247 / 321 |

  (Without a boundary: ko 0.65 / 0.27 s, 54 MB; de 0.15 / 0.09 s, 72 MB.) A
  smaller step helps a little at 3 threads with two boundaries or a boundary after
  the filter (up to 19%), and changes little otherwise. It does not remove the loss
  against no boundary or the memory growth (fixed in step 3; the loss is step 4).
- **Step 3 (bounded memory):** fixed.
  - **Cause.** A stream set's consumed item count is gathered in a transitory
    (stack) variable of the compute function. The producer sets it to its produced
    count, each consumer lowers it to its processed count, and the last consumer
    stores it in the persistent count that the producer reads when it needs buffer
    space. Each phase has its own compute function. For a stream set produced in
    one phase and last consumed in a later one, the later phase's transitory count
    was never set by the producer; it stayed 0, and a stored 0 means "consumers
    skipped, keep the prior count". So the producer always saw 0 consumed and its
    buffer grew to hold the whole stream. Temporary prints in
    `ManagedDynamicBuffer::reserveCapacity` showed it: with a boundary after nfd's
    transform, `UTF8assembly` (the transformed basis) and `streamCompress64_1`
    expanded repeatedly with a consumed count of 0, to 14.75 MB on a 5 MB input.
    Consumer counts of earlier-phase consumers were lost the same way.
  - **Fix** (`consumer_analysis.cpp`, `consumer_logic.cpp`):
    `initializeEarlierPhaseTransitoryConsumedItemCounts` sets the transitory count
    of each earlier-phase stream set that this phase consumes to the produced count
    read at phase entry (`readEarlierPhaseItemCounts`). If a stream set has
    consumers in more than one phase, the last consumer in each phase before the
    phase of its last consumer writes that phase's count to a slot of its own
    (`@CON<id>_P<phase>`), and `readConsumedItemCount` takes the minimum of the
    count and the slots.
  - **Results** (nfd on ko, 1 thread, peak RSS for 5 / 20 / 50 MB inputs):
    boundary after the transform 78 / 80 / 82 MB (was 92 / 166 / 350); after the
    filter, the focus, or the placement and the transform, 141–186 MB, constant
    from 20 MB on. The rest is the 100-segment cross-phase buffers: 76–78 MB with
    `--phase-segment-limit=16`. No buffer expands more than twice.
  - **Correctness:** nfd, nfc and nfkd with no boundary, each boundary and all four,
    at 1 and 3 threads, `--segment-size=1024`, `-VerifyIR`, on the focus-modes text
    against Python (36 runs); nfd on de and ko, nfkd on ko, nfc on ko.nfd and
    de.nfd with boundaries after the filter, the transform, and the placement and
    the transform, at 3 threads, against the 1-thread output without a boundary;
    `ctest -j3` 42/42.
  - **Time** (battery, best of 2, 1 / 3 threads). At 1 thread a boundary now costs
    0–6% instead of 6–14%. At 3 threads it is still slower (step 4):

    | tool, input | none | filter | transform | placement,transform |
    |---|---|---|---|---|
    | nfd de | 0.15 / 0.09 | 0.15 / 0.14 | 0.15 / 0.11 | 0.15 / 0.11 |
    | nfd ko | 0.65 / 0.27 | 0.69 / 0.66 | 0.66 / 0.32 | 0.69 / 0.58 |
    | nfkd ko | 0.75 / 0.32 | 0.81 / 0.83 | 0.76 / 0.36 | 0.82 / 0.71 |
    | nfc ko.nfd | 0.97 / 0.40 | 0.98 / 0.95 | 0.98 / 0.51 | 0.97 / 0.51 |
    | nfc de.nfd | 0.16 / 0.10 | 0.16 / 0.15 | 0.17 / 0.14 | 0.16 / 0.14 |
- **Step 4 (later phases):** investigated; no change committed.
  - **No empty segments.** With step 3's fix, `QA/phase_segments.py` (now counting
    a segment as empty only if every traced kernel of its phase processed nothing)
    finds none: nfd on ko with a boundary after the transform runs 65 phase-1 and
    65 phase-2 segments at 1 thread and 141 / 140 at 3 threads, none of them empty;
    the trace that suggested empty segments has none either.
  - **A claim check was tried and reverted.** Before each segment of a later phase,
    a thread compared the earlier phase's produced counts with those at the phase's
    last claim, and claimed only if they had grown, or the last segment made progress
    and input remained unconsumed, or the producers had terminated. It was correct
    (the 36-run matrix with `-VerifyIR`) but changed neither the segment counts nor
    the times (nfd ko at 3 threads: 0.31 s with a boundary after the transform,
    0.68 s after the filter, against 0.32 / 0.66 s).
  - **Where the threads wait.** On a 600 MB input (nfd, boundary after the filter,
    3 threads), `sample` puts about half of one thread's samples on three JIT
    instructions, and lldb shows both other compute threads at the same loop:
    `ldapr x9, [lock]; cmp x9, segNo; b.ne`, followed by stores of segNo+1 to
    the locks of a run of consecutive kernels. That is the partition-jump path: the
    segment skips partitions that have no input and waits for the previous segment
    to release them. The cycle counter does not measure this wait separately; it
    appears as pipeline overhead of the first kernel of the phase (89% of that
    kernel's 29.5M ticks, against 2.3M ticks of execution at 1 thread).
  - Why partitions find no input: each segment of a later phase reads the earlier
    phase's counts once, at entry (the issue #26 fix). Segment n+1 starts right after
    segment n's first kernel, sees nearly the same counts, and finds that segment n
    has taken them.
- **Step 4, remedy 1 (claimed ranges):** done.
  - **Why an estimate of a segment's consumption does not work:** a partition root
    whose inputs are not all constant has no static limit on its strides per segment
    (`kernel_io_calculation_logic.cpp`), so a later-phase segment takes every item
    the earlier phase has produced when it enters the phase. The reverted claim check
    failed for this reason: a segment read the counts after its claim, and so also
    took the items that justified the next thread's claim.
  - **Change** (`synchronization_logic.cpp`, `buffer_management_logic.cpp`,
    `multithreading_model_logic.cpp`, `pipeline_logic.cpp`):
    - Before each segment of a later phase, at phase entry and before each further
      segment of a visit, `claimPhaseSegment` reads the earlier-phase stream sets'
      termination signals and produced counts. The thread claims a segment only if
      their total exceeds the phase's last claimed total (a compare-and-swap of a
      shared scalar, so that no two threads claim the same items) or all their
      producers have terminated. Otherwise it goes on to the next phase.
    - The claimed counts are kept in thread-local scalars, and
      `readEarlierPhaseItemCounts` limits each count the segment sees to them.
      Segments therefore get disjoint ranges of the earlier phase's data. A
      producer whose count was limited is treated as not yet terminated.
    - It applies to non-nested pipelines with phase boundaries whose earlier-phase
      producers have termination signals.
  - **Correctness:** the 36-run matrix with `-VerifyIR`; nfd on ko and de, nfkd on
    ko and nfc on ko.nfd with no boundary and boundaries after the filter, the
    transform, and the placement and transform, at 3 threads, against the 1-thread
    output without a boundary; `ctest -j3` 42/42.
  - **Time** (battery, best of 2, 3 threads; 1-thread times unchanged):

    | tool, input | none | filter | transform | placement,transform |
    |---|---|---|---|---|
    | nfd ko | 0.27 | 0.40 (was 0.66) | 0.31 (0.32) | 0.37 (0.58) |
    | nfd de | 0.09 | 0.13 (0.14) | **0.09** (0.11) | 0.11 (0.11) |
    | nfkd ko | 0.35 | 0.52 (0.83) | 0.36 (0.36) | 0.45 (0.71) |
    | nfc ko.nfd | 0.40 | 0.56 (0.95) | 0.50 (0.51) | 0.45 (0.51) |

    nfd on ko with a boundary after the transform runs 104 phase-1 and 103 phase-2
    segments at 3 threads (141 / 140 before), none empty. A boundary now costs
    0–40% at 3 threads instead of up to 140%, but it is still never faster than
    none. The next candidates are remedies 2 and 3 of step 4, and measurements
    on mains power (step 5).
- **Step 4, remedy 3 (claim after K earlier segments):** done; `--phase-claim-segments=4`
  is the default.
  - **Why:** after remedy 1, threads still waited in the partition-jump path (lldb on
    a 600 MB run, as before). A claim could cover little new data, so some partitions
    of the segment had nothing to do and waited for the previous segment to release
    them.
  - **Change:** `claimPhaseSegment` also requires the most recent earlier phase that
    produces the claimed stream sets to have started at least K segments since the
    phase's last claim (its `PHASE_NEXT_LOGICAL_SEGMENT_NUMBER` against a shared
    `@PCSN<phase>`), unless all their producers have terminated. K is clamped to
    `--phase-segment-limit`, so a later phase never waits for more segments than an
    earlier phase runs in one visit, and an earlier phase never waits for a later one.
    K is part of the pipeline signature with the phase segment limit.
  - **K sweep** (3 threads, battery, best of 2; all outputs equal to the 1-thread
    output without a boundary):

    | tool, input, boundary | K=1 | K=2 | K=4 | K=8 |
    |---|---|---|---|---|
    | nfd ko, filter | 0.69 | 0.31 | 0.31 | 0.29 |
    | nfd ko, transform | 0.31 | 0.28 | 0.28 | 0.28 |
    | nfd ko, placement,transform | 0.37 | 0.32 | 0.31 | 0.32 |
    | nfkd ko, filter | 0.49 | 0.38 | 0.37 | 0.37 |
    | nfc ko.nfd, filter | 0.57 | 0.43 | 0.43 | 0.42 |
    | nfc ko.nfd, transform | 0.51 | 0.41 | 0.41 | 0.40 |

    K=1 (remedy 1 alone) varies from run to run: 0.41–0.70 s for nfd ko with a
    boundary after the filter, over five runs. From K=2 on, times are steady.
  - **With K=4** (battery, best of 3, seconds and peak RSS):

    | tool, input | none, 1 / 3 threads | filter | transform | placement,transform |
    |---|---|---|---|---|
    | nfd ko | 0.66 / 0.27 | 0.70 / 0.31 | 0.67 / 0.28 | 0.72 / 0.32 |
    | nfd de | 0.16 / 0.09 | 0.16 / 0.09 | 0.15 / 0.09 | 0.16 / 0.09 |
    | nfd vi | 0.37 / 0.17 | 0.39 / 0.18 | 0.38 / 0.17 | 0.39 / 0.18 |
    | nfkd ko | 0.76 / 0.32 | 0.82 / 0.40 | 0.77 / 0.32 | 0.83 / 0.38 |
    | nfc ko.nfd | 0.97 / 0.40 | 0.99 / 0.42 | 0.98 / 0.41 | 0.98 / 0.41 |
    | nfc de.nfd | 0.17 / 0.11 | 0.16 / 0.10 | 0.16 / 0.10 | 0.17 / 0.10 |

    Peak RSS with a boundary after the transform is 74–269 MB (52–236 MB without a
    boundary); with a boundary after the filter, or after the placement and the
    transform, up to 384 MB, from the 100-segment cross-phase buffers.
    A boundary after the transform now costs nothing measurable at 3 threads, and
    0–3% at 1 thread. Other boundaries cost up to 25% at 3 threads (nfkd ko after
    the filter) and 4–9% at 1 thread. No boundary is clearly faster than none yet; on
    battery, differences under about 0.01 s are noise.
  - **Correctness:** the 36-run matrix with `-VerifyIR` at the default; `ctest -j3`
    42/42 (greptest covers icgrep's own phase boundaries, which now also use K=4;
    icgrep's timing with phases has not been measured).
- **Step 4, remedy 2 (release skipped partitions without waiting):** not done.
  - The wait in the jump path is what keeps the segment locks in order: a segment
    may set a kernel's lock to its own number plus one only after the previous
    segment has set it to its number. Releasing without waiting needs a different
    lock protocol, for example a per-kernel record of skipped segments that the
    previous segment completes when it releases the lock.
  - After remedy 3, the remaining loss is small, so this is left for Phase 5, where
    more threads would make such waits more frequent.
- **Next:** step 5 (measure the layer configurations on mains power with
  `QA/focus_baseline.py --layers`, including vi, ja, de.nfd and ko.nfd, and the
  scaling ceilings), then step 6 (report and defaults). The 100-segment cross-phase
  buffers should be measured against `--phase-segment-limit=16` for memory.
