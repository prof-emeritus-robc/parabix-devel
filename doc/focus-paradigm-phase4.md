# Focus / transform / reintegrate: Phase 4 interim report (phase boundaries)

Phase 4 asks whether stages of a focused transform (nfd, nfc) can run as separate
pipeline layers, with phase boundaries (`-focus-layers=focus,filter,placement,transform`),
without losing time. This report summarizes the results so far. The plan, with the
details of each step, is `focus-paradigm-phase4-plan.md`.

**Status:** steps 1–4 of the plan are done. All timings below were taken **on battery**
(Apple M1, at most 3 threads, best of 2–3 runs, warm object cache), so they are
indicative; step 5 repeats them on mains power. nfkd behaves like nfd and is in the
plan's tables.

## Summary

- **At the start of Phase 4, boundaries were correct but costly.** Every boundary was
  slower than none, up to 2.6 times at 3 threads (nfd on ko after the filter: 0.69 s
  against 0.27 s), and memory grew with the input size (350 MB for a 50 MB input).
- **Three pipeline-compiler fixes** remove almost all of the cost:
  1. consumed item counts now cross phase boundaries, so buffers no longer keep the
     whole stream (f863dfdc3);
  2. each segment of a later phase claims its own range of the earlier phase's data
     (104b2f1ba);
  3. a later phase claims a segment only after the earlier phase has started 4 more
     segments (`--phase-claim-segments=4`, f9ceaf4e1).
- **Now a boundary after the transform costs nothing measurable at 3 threads,** and
  0–3% at 1 thread, for nfd and nfc on every input measured. Other boundaries cost
  0–19% at 3 threads for nfd and nfc (nfd on ko after the placement and the
  transform), and up to 9% at 1 thread.
- **No boundary is faster than none yet.** The gain expected from running the
  transform on full segments has not appeared at 3 threads; whether it appears on
  mains power or with more threads is for steps 5 and Phase 5.
- **Memory is bounded** but higher with a boundary: 82 MB instead of 52 MB for nfd on
  ko after the transform, up to 384 MB with a boundary after the filter, because data
  crossing a boundary is buffered for 100 segments (`--phase-segment-limit`).

## Results

### nfd

Seconds at 3 threads (1 thread in the last column). "Start" is the branch before Phase 4
(9920c169b), "step 3" adds the consumed-count fix, "claims" the claimed ranges, and
"K=4" the current default.

| input | boundary after | start | step 3 | claims | K=4 | K=4, 1 thread |
|---|---|---|---|---|---|---|
| ko | (none) | 0.27 | 0.27 | 0.27 | 0.27 | 0.66 |
| ko | filter | 0.69 | 0.66 | 0.40 | 0.31 | 0.70 |
| ko | transform | 0.35 | 0.32 | 0.31 | 0.28 | 0.67 |
| ko | placement, transform | 0.63 | 0.58 | 0.37 | 0.32 | 0.72 |
| de | (none) | 0.09 | 0.09 | 0.09 | 0.09 | 0.16 |
| de | filter | 0.16 | 0.14 | 0.13 | 0.09 | 0.16 |
| de | transform | 0.13 | 0.11 | 0.09 | 0.09 | 0.15 |
| de | placement, transform | 0.13 | 0.11 | 0.11 | 0.09 | 0.16 |
| vi | (none) | | | | 0.17 | 0.37 |
| vi | filter | | | | 0.18 | 0.39 |
| vi | transform | | | | 0.17 | 0.38 |
| vi | placement, transform | | | | 0.18 | 0.39 |

At the start, 1-thread times with a boundary were 6–14% slower than without (ko: 0.71–0.75 s
against 0.65 s).

### nfc

nfc places after the transform, so its boundary "after the transform" separates the
transform from placement and reintegration. The start of Phase 4 was not measured for nfc;
"step 3" is the earliest column.

| input | boundary after | step 3 | claims | K=4 | K=4, 1 thread |
|---|---|---|---|---|---|
| ko.nfd | (none) | 0.40 | 0.40 | 0.40 | 0.97 |
| ko.nfd | filter | 0.95 | 0.56 | 0.42 | 0.99 |
| ko.nfd | transform | 0.51 | 0.50 | 0.41 | 0.98 |
| ko.nfd | placement, transform | 0.51 | 0.45 | 0.41 | 0.98 |
| de.nfd | (none) | 0.10 | | 0.11 | 0.17 |
| de.nfd | filter | 0.15 | | 0.10 | 0.16 |
| de.nfd | transform | 0.14 | | 0.10 | 0.16 |
| de.nfd | placement, transform | 0.14 | | 0.10 | 0.17 |

### Memory (peak RSS, MB)

| tool, input | none | filter | transform | placement, transform |
|---|---|---|---|---|
| nfd ko, at the start (1 thread) | 53 | 487 | 351 | 515 |
| nfd ko, K=4 (3 threads) | 54 | 276 | 97 | 297 |
| nfc ko.nfd, K=4 (3 threads) | 236 | 384 | 269 | 281 |

At the start, memory with a boundary grew in proportion to the input (nfd on ko after the
transform: 92, 166, 350 MB for 5, 20, 50 MB of input); now it does not. With
`--phase-segment-limit=16` the remaining excess drops to 76–78 MB for nfd on ko, at a
small cost in time with some boundaries (plan, step 2).

## What was wrong, and the fixes

1. **Consumed counts did not cross phases (memory).** Each phase has its own compute
   function, and the consumed count of a stream set is gathered in a per-function
   variable that the producer initializes. A stream set last consumed in a later phase
   than its producer therefore always reported nothing consumed, and its buffer grew to
   hold the whole stream. Fix: later phases initialize those counts from the produced
   counts read at phase entry, and consumers in different phases write separate slots
   whose minimum the producer reads.
2. **Later-phase segments took all available data (threads serialized).** A partition
   root that reads earlier-phase data has no per-segment limit, so a segment took
   everything the earlier phase had produced. The next segment found nothing, skipped
   its partitions, and had to wait for the previous segment to release their locks in
   order (seen with lldb as spin loops in the partition-jump path), so later phases ran
   one segment at a time. Fix: a thread claims a later-phase segment only when the
   earlier phase's output has grown past the last claim (compare-and-swap), and the
   segment sees no more than it claimed.
3. **Claims could be too small.** Even with claimed ranges, a claim could cover so little
   that some partitions had nothing to do, with the same waits; times varied from run to
   run (0.41–0.70 s for nfd on ko after the filter). Fix: require the earlier phase to
   have started K segments since the last claim. K=2 gives most of the gain; K=4 is the
   default.

The first diagnosis, that later phases ran empty segments, was wrong: after fix 1, no
later-phase segment is empty, and the earlier count looked at kernels separately
(`QA/phase_segments.py` now counts per phase).

## Correctness

- nfd, nfc and nfkd with no boundary, each boundary and all four, at 1 and 3 threads,
  `--segment-size=1024`, `-VerifyIR`, on the focus-modes test text, against Python's
  `unicodedata` (36 runs), after each fix.
- Full-size inputs (nfd on ko, de and vi; nfc on ko.nfd and de.nfd; nfkd on ko) with
  boundaries after the filter, after the transform, and after both placement and
  transform, at 3 threads, identical to the 1-thread output without a boundary.
- `ctest -j3`: 42/42 after each fix, including greptest, which covers icgrep's own phase
  boundaries.

## Open points

- **Measure on mains power** (step 5): all inputs, boundaries and scaling ceilings with
  `QA/focus_baseline.py --layers`, and the memory/time trade-off of
  `--phase-segment-limit`.
- **icgrep** also uses phase boundaries and now the claim rules; its correctness is
  tested but its timing has not been measured.
- **Remedy 2 of the plan** (releasing skipped partitions without waiting for the previous
  segment) needs a new lock protocol; deferred to Phase 5, where more threads would make
  such waits more frequent.
- **Tools found broken along the way:** `-TraceDynamicBuffers` crashes nfd with or without
  boundaries, and `-TraceUnconsumedItemCounts` reports only zeros. The cycle counter
  reports waits in the partition-jump path as pipeline overhead of a phase's first kernel.
