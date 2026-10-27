# Focus / transform / reintegrate: Phase 2 results for nfd

Phase 2 added run-based alternatives for the byte-level filter and merge stages
of focused transforms, and measured them in nfd.

## What was added

- **`RunFilterKernel` and `RunMergeKernel`** (`kernel/streamutils/run_replace`)
  are byte-stream equivalents of FilterByMask and MergeByMask.
  - Each scans a run-start stream (`mask XOR Advance(mask)`) with
    TwoLevelScanKernel, which skips any number of 64-bit words without a run
    start. It then copies each run with `memcpy`.
  - Both are data parallel.
- **`FillRunGaps`** adds gaps of at most N unmodified bytes between work
  regions to the work.
- **Options.** Since Phase 1 step 1, the options are shared by all focused
  transforms: `-focus-filter=bit|byte|run`,
  `-reintegrate=bit|byte-spread|byte-replace|run`, `-unmodified-filter=byte|run`
  and `-fill-run-gaps=N`. nfd's earlier `-ByteMerging`, `-ByteReplace`,
  `-RunFilter`, `-RunMerge` and `-RunReplace` remain as aliases. The default is
  unchanged (bit filter, bit merge).

Below, "RR" is `-reintegrate=run -unmodified-filter=run`, that is, run filtering
of the unmodified bytes and run merging.

## Setup

- **Machine and settings:** Apple M1, mains power, backend
  `-backend-optimization-level=standard`, at most 3 threads.
- **Inputs:** about 50 MB each, as in Phase 0.
- **Timing:** best of 5, with a warm object cache.
- **Correctness:** every configuration below produced the same output as default
  nfd at 3 threads on every input.
- **Commit:** `focus-paradigm` at 863fcdb64.
- **Reproduce:** `QA/focus_baseline.py --tools nfd --config=...`.

## Wall times (seconds)

Density is the fraction of the input sent to the transform.

| input (density) | default t1 / t3 | byte-replace t1 / t3 | RR t1 / t3 | RR + focus-filter=run t1 / t3 | NoFocus t1 / t3 |
|---|---|---|---|---|---|
| ascii (0%) | 0.200 / 0.115 | 0.170 / 0.108 | 0.139 / 0.094 | **0.120 / 0.088** | 0.361 / 0.158 |
| de (2.1%) | 0.220 / 0.126 | 0.185 / 0.103 | 0.173 / 0.100 | **0.158 / 0.095** | 0.412 / 0.177 |
| de.nfd (2.0%) | 0.210 / 0.127 | 0.176 / 0.100 | 0.161 / 0.096 | **0.146 / 0.091** | 0.408 / 0.176 |
| ja (6.0%) | 0.255 / 0.134 | 0.224 / 0.115 | 0.220 / 0.113 | **0.204 / 0.110** | 0.334 / 0.148 |
| ko.nfd (0.2%) | 0.377 / 0.218 | 0.300 / 0.147 | 0.260 / 0.143 | **0.226 / 0.127** | 0.515 / 0.214 |
| vi (20.1%) | 0.338 / 0.160 | **0.306 / 0.145** | 0.366 / 0.170 | 0.380 / 0.177 | 0.463 / 0.192 |
| ko (63.6%) | 0.764 / 0.314 | 0.704 / 0.292 | 0.713 / 0.291 | 0.706 / 0.292 | **0.547 / 0.221** |

**Best configuration versus the default, at 3 threads:**

- ascii −23%, de −25%, de.nfd −28%, ja −18%, ko.nfd −42% (all run filter and
  merge, with `-focus-filter=run`; `-focus-filter=byte` is within 2%).
- vi −9% (byte-replace).
- ko −30% (no focus).

At 1 thread the gains are larger (ascii −40%, ko.nfd −40%).

### Other variants

- **`-reintegrate=run` with FilterByMask of the unmodified bytes** is within a
  few percent of RR at low density. On ja and vi it is slightly better than RR.
- **`-fill-run-gaps`** brought no gain:
  - with N = 4, times within ±3% of RR, density rising slightly (vi 20% → 30%);
  - with N = 16, slightly slower everywhere.

  Merging regions saves little once runs are copied with `memcpy`, and the
  extra bytes go through the transform.

## Scaling ceilings

Projected maximum speedup at any thread count: single-threaded time divided by
the largest serialized kernel's time.

| input | default | RR | RR + focus-filter=run | NoFocus |
|---|---|---|---|---|
| ascii | 4.8 | 10.3 | **14.1** | 13.6 |
| de | 14.2 | 14.3 | 14.1 | 15.3 |
| ko.nfd | 13.8 | 9.1 | 7.5 | 10.5 |
| vi | 17.0 | 18.6 | 19.4 | 17.2 |
| ko | 5.4 | 5.0 | 5.0 | **19.7** |

- **ascii:** run reintegration removes the serialized filter that capped the
  default at 4.8×.
- **ko.nfd:** the ceiling falls with RR. Reintegration time drops, so the
  serialized placement and focus kernels become a larger share of what remains.
  It is a ratio, not a regression.
- **ko:** the ceiling stays at 5× whenever focus is on, set by the placement
  insertion character-class kernel (20% of single-threaded time).

## Where the time goes now

**de, RR + focus-filter=run (1-thread share):**

| stage | share | largest serialized kernel |
|---|---|---|
| placement | 31.2% | insertion character-class kernel (6.2%) |
| reintegrate | 23.2% | RunStarts (1.0%) |
| focus | 20.0% | UCD property kernel (7.1%) |
| transform | 11.9% | |
| source | 9.3% | read_source (5.3%) |
| filter | 4.3% | |

Placement and focus are now 51% of the time. Their serialized kernels, which
carry state from block to block, bound scaling at about 14×.

**vi, RR versus byte-replace:**

- Run reintegration takes 31.8% of single-threaded time, against 16.4% for
  byte-replace.
- At 20% density the runs are short (accented letters every few characters),
  and a `memcpy` per run costs more than per-pack merging.

**ko:**

- The transform is 51%. Focus overhead (placement 30% plus reintegration) makes
  every focused configuration slower than `-NoFocus`.

## Conclusions

1. **Run-based reintegration is the best choice for typical text,** where work
   regions are sparse. It cuts nfd wall time by 18–42% at 3 threads and 40% at 1
   thread. On sparse ascii input it also raises the scaling ceiling from 4.8× to
   14×. Run focus filtering adds a further 3–12%.
2. **No single configuration is best across densities:**
   - up to about 6%: run filter and merge;
   - around 20%: byte-replace;
   - above about 40–60%: no focus.

   That calls for an adaptive choice, per segment or per block of segments, by
   measured density or run count. A hybrid merge that copies long runs with
   `memcpy` and merges short ones per pack would remove the vi regression on its
   own.
3. **`-fill-run-gaps` is not useful** with run kernels on these inputs. It stays
   available for other data mixes and architectures.
4. **Placement and focus are now the main costs and the main scaling limits**
   (51% of time at low density; the placement character-class kernel caps ko at
   5×). They are the next targets: make placement cheaper or a separate task,
   and look at the stateful UCD focus kernels.

## Suggested defaults (decision pending)

The default is unchanged. If nfd's default were changed to RR with
`-focus-filter=run`, typical text would gain 18–42% and vi would lose about 10%,
until an adaptive or hybrid choice exists.
