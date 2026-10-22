# Focus / transform / reintegrate: Phase 4 plan (layers)

Written after Phase 1 (the `FocusedTransform` builder and `-focus-layers`), Phase 2
(run-based filter and reintegration), the UTF-8 bit-movement change
(`focus-paradigm-bit-movement.md`) and the fix for issue #26 part 3
(`issue-26-regalloc-crash.md`, bbf6179a8).

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
  of all ticks. `-TraceStridesPerSegment` shows many phase-2 and phase-3 segments
  that process nothing, and the transform working in bursts.
  The likely cause is that segment n of a phase must wait, kernel by kernel, for
  segment n−1. A thread that takes a later-phase segment number when there is no
  new input therefore waits behind the thread that has the data, instead of running
  phase 1.
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
     they are needed to find it.
4. **No empty segments in later phases** (pipeline compiler).
   - Before a thread takes a segment number of phase p > 1, check that the
     earlier-phase stream sets it consumes have grown since the last segment of
     phase p was claimed, or that their producers have terminated; otherwise go on
     to the next phase. `readEarlierPhaseItemCounts` (issue #26 fix) already reads
     these counts once per segment.
   - Alternatives if that is not enough: let only one thread at a time claim
     segments of a phase whose input has not grown; adapt the step limits as the
     TODO in `multithreading_model_logic.cpp` proposes (more segments for a phase
     that keeps making progress, fewer for one that stalls).
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
