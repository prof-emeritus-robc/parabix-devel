# Focus / transform / reintegrate: Phase 0 baseline

This is the baseline for restructuring the normalization tools (nfd, nfc) around
four stages:

1. a **focus** stage that finds the regions that may change;
2. a **filter** that extracts those regions into a short stream;
3. a **transform** of the short stream;
4. a **reintegration** of the transformed data with the unchanged source data,
   guided by **placement** masks.

The goal is to find where time goes today, on inputs of different work density, and
how far each configuration could scale on machines with many more cores than were
used here.

## Setup

- **Machine and settings:**
  - Apple M1 (4 performance + 4 efficiency cores), 128-bit blocks.
  - `--backend-optimization-level=standard` (the new default).
  - At most 3 threads.
- **Timing:** best of 5 runs with a warm object cache, so JIT compilation is
  excluded.
- **Profiling:** stage figures come from `-EnableCycleCounter`, whose "cycles"
  on the M1 are ticks of a 24 MHz timer.
- **Tools** (on branch `focus-paradigm`):
  - **Stage labels:** `PipelineBuilder::SetStageLabel` labels kernels. nfd and
    nfc label their source, focus, filter, placement, transform and
    reintegrate stages, and the cycle counter report prints `[label] name`.
  - **`QA/stage_report.py`** sums a report by stage. Given the
    `--PrintPipelineGraph` output, it also computes a **scaling ceiling**:
    kernels not marked `<StateFree>` process one segment at a time, so no
    number of threads can beat the single-threaded time of the largest such
    kernel. The ceiling is total single-threaded time divided by that kernel's
    time.
  - **`QA/focus_baseline.py`** runs every input and configuration and checks
    each N-thread output against the tool's default output with 1 thread. It
    records wall times and stage tables.
- **Timing pitfall found while building this:** `subprocess.run(..., timeout=)`
  waits for the child by polling with sleeps of up to 50 ms, which rounded every
  measured time up by tens of milliseconds. Timed runs must not use a timeout.

### Inputs (about 50 MB each, cut from `~/wiki-books-all.xml`)

| input | content | non-ASCII bytes | nfd work density | nfc work density |
|---|---|---|---|---|
| ascii | German wikibooks with non-ASCII bytes removed | 0% | 0.0% | 0.0% |
| de | German wikibooks | 2.6% | 2.1% | 0.0% |
| de.nfd | `de` converted to NFD | 2.6% | 2.0% | 2.0% |
| ja | Japanese wikibooks | 68% | 6.0% | 0.0% |
| vi | Vietnamese wikibooks | 16% | 20.1% | 0.0% |
| ko | Korean wikibooks (precomposed Hangul) | 59% | 63.6% | 0.1% |
| ko.nfd | `ko` converted to NFD (96 MB) | 78% | 0.2% | 76.4% |

Work density is the size of the focused stream that reaches the transform stage,
as a percentage of the input. The source wikis are already NFC, so nfc finds work
only in the NFD inputs.

## Wall times

All outputs matched the reference (1-thread default) at 3 threads. Times are in
seconds; "ceiling" is the projected maximum speedup at any thread count.

### nfd

| input | config | 1 thread | 3 threads | speedup | ceiling |
|---|---|---|---|---|---|
| ascii | default | 0.199 | 0.114 | 1.74 | 6.3 |
| ascii | -ByteReplace | 0.169 | 0.109 | 1.54 | 5.0 |
| ascii | -NoFocus | 0.361 | 0.159 | 2.27 | 13.6 |
| de | default | 0.220 | 0.126 | 1.74 | 14.1 |
| de | -ByteReplace | 0.185 | 0.103 | 1.80 | 14.5 |
| de | -NoFocus | 0.414 | 0.176 | 2.35 | 15.3 |
| ja | default | 0.257 | 0.134 | 1.92 | 12.3 |
| ja | -ByteReplace | 0.223 | 0.115 | 1.94 | 10.3 |
| vi | default | 0.341 | 0.162 | 2.10 | 17.0 |
| vi | -ByteReplace | 0.308 | 0.148 | 2.08 | 15.1 |
| vi | -NoFocus | 0.465 | 0.195 | 2.39 | 17.1 |
| ko | default | 0.755 | 0.314 | 2.40 | 5.4 |
| ko | -ByteReplace | 0.704 | 0.294 | 2.39 | 5.0 |
| ko | **-NoFocus** | **0.549** | **0.224** | 2.45 | 20.1 |
| ko.nfd | default | 0.370 | 0.213 | 1.74 | 13.6 |
| ko.nfd | -ByteReplace | 0.300 | 0.147 | 2.04 | 10.6 |

`-ByteMerging` is within 2% of `-ByteReplace` throughout.

### nfc

| input | config | 1 thread | 3 threads | speedup | ceiling |
|---|---|---|---|---|---|
| ascii | default | 0.181 | 0.118 | 1.54 | 5.0 |
| ascii | -ByteFiltering | 0.166 | 0.113 | 1.46 | 4.4 |
| de | default | 0.194 | 0.148 | 1.31 | 5.8 |
| de | -ByteFiltering | 0.176 | 0.144 | 1.22 | 5.0 |
| de | -NoFocus | 0.652 | 0.269 | 2.42 | 17.7 |
| de.nfd | default | 0.200 | 0.124 | 1.61 | 11.9 |
| de.nfd | -ByteFiltering | 0.184 | 0.119 | 1.54 | 10.8 |
| ko.nfd | default | 0.878 | 0.363 | 2.42 | 23.3 |
| ko.nfd | -ByteFiltering | 0.865 | 0.358 | 2.42 | 23.0 |
| ko.nfd | -NoFocus | 1.156 | 0.455 | 2.54 | 12.5 |

The full tables, with the remaining inputs and configurations, come from running
`QA/focus_baseline.py`.

## Stage breakdowns (selected)

The "1-thread %" column gives each stage's share of single-threaded time. "Sync" and
"pipe" are each stage's waiting and pipeline-overhead shares at 3 threads.
"Serialized %" is the time in kernels that process one segment at a time.

### nfd default, de (low density)

| stage | 1-thread % | sync % | pipe % | serialized % | largest serialized kernel |
|---|---|---|---|---|---|
| source | 5.9 | 16.1 | 0.3 | 3.3 | read_source (3.3%) |
| focus | 12.8 | 5.2 | 1.2 | 12.4 | UCD property kernel (4.6%) |
| filter | 11.3 | 4.4 | 0.3 | 5.3 | streamCompress64_8 (5.1%) |
| placement | 19.9 | 7.9 | 1.6 | 10.7 | insertion character-class kernel (4.0%) |
| transform | 7.6 | 6.7 | 7.4 | 3.9 | PopCount (0.8%) |
| **reintegrate** | **42.4** | 13.5 | 16.2 | 7.9 | streamCompress64_8 (7.1%) |

### nfd -ByteReplace, de

The byte-level merge cuts reintegration from 42% to 28.5% of a smaller total
(wall time −16% at 3 threads). Placement (25%) and focus (16%) now dominate.

### nfd default, ko (high density, 64%)

| stage | 1-thread % | serialized % | largest serialized kernel |
|---|---|---|---|
| **placement** | **27.8** | 24.0 | insertion character-class kernel (**18.5%**) |
| transform | 47.3 | 14.2 | InsertionSpreadMask (3.3%) |
| reintegrate | 15.5 | 1.7 | streamCompress64_8 (1.3%) |
| focus + filter + source | 9.4 | | |

Without focus (`-NoFocus`), the transform is 96% of the time and the scaling
ceiling rises from 5.4× to 20×.

### nfc -ByteFiltering, de (no work)

| stage | 1-thread % | sync % | pipe % | largest serialized kernel |
|---|---|---|---|---|
| **reintegrate** | **48.1** | 38.7 | 13.2 | elemFilter_8 (**19.9%**) |
| placement | 27.7 | 9.9 | 51.8 | NFC_Initial_Insertion (7.3%) |
| focus | 11.5 | 15.5 | 1.3 | U8Spans (5.0%) |
| transform | 5.1 | 24.0 | 3.7 | PopCount (2.2%) |

### nfc default, ko.nfd (high density, 76%)

The transform is 77% of the time. Its many serialized kernels each take no more
than 4.3%, so the ceiling is 23×.

## Findings

1. **Reintegration dominates low-density inputs.**
   - nfd default spends 42–56% of single-threaded time reassembling output: its
     bit-level filter of the unchanged data plus `streamMerge`.
   - The byte-level `-ByteReplace` path is 16–19% faster on de, ascii and ja.
     It still filters the unchanged bytes (`elemFilter_8`) and spreads them back.
   - In nfc (no work on NFC input), reintegration is 48% of the time even with
     `-ByteFiltering`; it is mostly `elemFilter_8` of the unchanged bytes.
2. **Placement is larger than expected: 17–28%.**
   - Computing the integration masks takes 20–25% on low-density text and 28%
     on Korean.
   - It includes the UTF-8 insertion/deletion character-class kernels; one of
     them alone is 18.5% on Korean.
3. **Focus can cost more than it saves at high density.**
   - On Korean (64% density), nfd with focus is 37% slower at 1 thread (0.755 s
     vs 0.549 s) and 40% slower at 3 threads than `-NoFocus`.
   - The focus, filter, placement and reintegration overhead (52%) exceeds the
     transform work avoided.
   - At low density, focus is 1.4–2.2× faster.
4. **Focused configurations scale worse on many cores.** Several have scaling
   ceilings of 4.4–6.3×, each set by one serialized kernel:
   - `streamCompress64_8` in nfd's reintegration on ascii (16%);
   - `elemFilter_8` in nfc's reintegration (20%);
   - the insertion character-class kernel in nfd's placement on Korean (18.5%).

   Unfocused (`-NoFocus`) runs, and focused runs where the transform dominates,
   have ceilings of 10–23×. Measured 3-thread speedups already show the
   difference: 1.2–1.8× for focused low-density runs, 2.2–2.5× for unfocused
   ones. On a machine with many cores, these serialized reintegration and
   placement kernels, not the transform, would set the limit.
5. **The transform stage is small on typical text and large on dense text.**
   - It is 2–8% of time at low density, so running it in full strides as a
     separate layer would gain little there.
   - It is 47–77% on dense inputs. There its serialized kernels are many but
     small (≤5% each), so it scales reasonably.
6. **Correctness.** All 49 configurations produced correct output. One defect
   was found and fixed: `-SeparatedPipelineStages=2` without `-ByteMerging`
   emitted 0/1 bytes. It now has a regression test, `nfd_separated_test`.

## Implications for the next phases

- **Phase 2 (run-based byte reintegration) is the top priority.** Copying
  unchanged runs straight from source to output removes the full-length filter
  and spread of unchanged data. That is the largest cost at low density and the
  serialized kernel that caps scaling at 5–6×.
  - The copies of separate runs are independent, so the stage can be data
    parallel. That matters for many-core machines.
  - The parked `filter-data-parallel-experiment` branch, which makes
    `elemFilter`/`streamCompress` data parallel, is a fallback for the same
    ceiling.
- **Placement needs its own work item,** previously not planned:
  - make the insertion/deletion character-class work cheaper (it costs 18.5%
    on Korean), or derive placement from the transform's per-region output
    lengths;
  - remove its serialized kernels, or move it to a separate task as the user
    suggested.
- **Choose between focus and no focus adaptively,** by measured density (for
  example per segment or per block of segments). The crossover lies between
  20% density (vi: focus wins, 0.34 vs 0.47 s) and 64% (ko: no-focus wins).
- **Phase 3 (cheaper focused input) still applies,** but filtering is only 3–14%;
  it is a secondary target.
- **Phase 4 (transform as a full-stride layer) matters mainly for dense text.**
  There the transform is large but already scales; the gain is in lower
  pipeline overhead, not in parallelism.
- **Every later phase should report the scaling ceiling** alongside 1- and
  3-thread times, since the design must scale beyond the 3 threads measured
  here.

## Reproducing

```bash
python3 QA/focus_baseline.py --bin build18/bin --inputs <dir of *.txt inputs> --out <out dir> --repeats 5
```

The inputs were cut from `~/wiki-books-all.xml`, using its `<dbname>` offsets for
dewikibooks, jawikibooks, kowikibooks and viwikibooks:

- each is 50 MiB, ending at a line boundary, with the shorter dumps repeated to
  reach that size;
- `ascii` is `de` with bytes ≥ 0x80 removed;
- `*.nfd` were produced with `nfd`.
