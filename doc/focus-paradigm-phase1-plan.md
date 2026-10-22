# Focus / transform / reintegrate: Phase 1 plan (restructuring)

Updated after Phase 0 (baseline, `doc/focus-paradigm-phase0.md`) and the first part
of Phase 2 (run-based reintegration for nfd, commit 0219801cf and following).

## Why restructure

The paradigm (focus, filter, transform, placement, reintegrate) is implemented
separately in each tool, with its own options and its own copies of the shared
parts:

| program | where the stages live | modes | notes |
|---|---|---|---|
| nfd | focus+filter, placement and transform in `NFD_PipelineBuilder` (library); source and reintegration in `tools/transcoders/nfd.cpp` | NoFocus; ByteMerging, ByteReplace, RunFilter, RunMerge, RunReplace; fill-run-gaps; SeparatedPipelineStages=2/4; UseLayers | focus and filter are one function, so the filter cannot be chosen separately |
| nfc | all in `tools/transcoders/nfc.cpp` (NFC_Focus, working-space expansion, transform, final placement and merge) | NoFocus; ByteFiltering; MultiStage; UseLayers | reintegration is MergeByMask only; placement needs the transform's ValidWorkMask |
| nfkd | `tools/transcoders/nfkd.cpp` | none | no focus: transforms the whole input |
| tconv | `tools/ldml/tconv.cpp`: NFD/NFKD via NFD_PipelineBuilder, NFC via a copy of nfc's kernels | none | copies NFC_Focus and nfd's bit-level reintegration |
| lex/normalize | own variants | | out of scope for Phase 1 |

Consequences:

- Improvements to one tool (e.g., the Phase 2 run-based reintegration and gap
  filling in nfd) do not reach the others.
- Options are tool-local globals with different names for similar choices
  (nfd `-ByteMerging` vs nfc `-ByteFiltering`; nfd `-SeparatedPipelineStages` vs
  nfc `-MultiStage`).
- Stage boundaries, which the later phases need for layers and separate tasks,
  are not explicit.

## Two placement models

The stages must allow both:

- **Placement from the source (nfd, nfkd).** Output lengths per character are
  known from the source (the UTF-8 insertion/deletion counts), so placement can
  be computed independently of, and concurrently with, the transform.
- **Placement after the transform (nfc).** Composition decides which positions
  of the working space survive; the transform returns a ValidWorkMask in focused
  space, from which placement is derived. Placement then depends on the transform.

## Target design

A library builder, `FocusedTransformBuilder` (in `kernel/unicode/normalization` or
a new `kernel/streamutils/focus` module), that assembles the stages, labels them
(`SetStageLabel`) and selects implementations by mode:

```
source:      bytes, basis bits
focus:       callback(basis) -> WorkSelectionMask       [+ fill-run-gaps]
filter:      mode bit | byte | run                       -> focused input
transform:   callback(focused input) -> transformed bytes or basis
                                        [+ ValidWorkMask for post-transform placement]
placement:   callback from source (pre) or from transform outputs (post)
reintegrate: mode bit-merge | byte-spread | byte-replace | run -> output bytes
layering:    none | phase boundaries at chosen stage boundaries | separated pipelines
```

Principles:

- **Keep every existing implementation selectable.** FilterByMask/SpreadByMask,
  byte merging and byte replacement remain alongside the run-based kernels, for
  experiments on other architectures, data mixes and thread counts.
- **Same options for all tools.** A shared option category, for example
  `-focus-filter=bit|byte|run`, `-focus-merge=bit|byte-spread|byte-replace|run`,
  `-fill-run-gaps=N`, `-NoFocus`, `-focus-layers=...`. The existing tool option
  names stay as aliases, so existing scripts and tests keep working.
- **The tools become thin:** each provides its focus, transform and placement
  callbacks and its transform-specific options.
- **Stage boundaries are explicit,** so that Phases 4–5 can place a layer
  boundary or a separate task at any of them without restructuring again.

## Steps

Each step ends with byte-identical output, a `ctest -j3` pass, and (on mains power)
a timing check with `QA/focus_baseline.py` against the step before.

1. **Builder and nfd.**
   - Create the builder with the stages above.
   - Split NFD_FilterStage into its focus and filter parts; move nfd's source and
     reintegration code into the builder; keep the NFD-specific parts (focus
     kernels, placement CCs, transform) in NFD_PipelineBuilder.
   - nfd.cpp keeps its options as aliases and its modes (including
     SeparatedPipelineStages=2/4 and UseLayers) on top of the builder.
   - Gate: the correctness matrix used for Phase 2 (all output modes × fill-run-gaps
     0/1/4/16 × 1 and 3 threads × small, 300 KB, vi and ko inputs), plus
     `nfd_test` and `nfd_separated_test`.
2. **nfc on the builder (post-transform placement).**
   - Move NFC_Focus, NFC_CandidateClass, the working-space expansion, the NFC
     transform and the final placement from nfc.cpp into the library.
   - nfc then gains the run-based filter and merge, gap filling and the other
     modes; MultiStage and ByteFiltering remain as aliases.
   - Gate: `nfc_test`, plus a matrix over nfc's modes on de.nfd and ko.nfd (the
     inputs where nfc has work) and on de (no work).
3. **nfkd with focus, and tconv.**
   - Give nfkd a focused mode (work: characters with a compatibility or
     canonical decomposition, or nonzero CCC) on the builder; `-NoFocus` keeps
     the current behaviour.
   - Switch tconv's Any-NFD, Any-NFKD and Any-NFC forms to the builder and
     delete its copies of the NFC kernels.
   - Gate: `ldml_trules_test`, nfkd output identical to the current nfkd on the
     corpora.
4. **Shared options, labels and layer boundaries.**
   - Add the shared option category and the aliases.
   - Labels come from the builder.
   - Add `-focus-layers=` to insert phase boundaries at chosen stage boundaries.
     Measuring layering is Phase 4's job; Phase 1 only makes it possible.
   - Extend `QA/focus_baseline.py` with the new modes and with nfkd.
5. **Tests.**
   - A small QA test that runs each tool's main modes on small inputs, with
     segment boundaries inside work regions and one dense input.
   - It must stay inexpensive: each mode is a separate pipeline and compiles once
     per cache, so keep the mode list short.

## Out of scope for Phase 1

- Performance changes, apart from nfc and nfkd gaining modes that already exist.
- `lex/normalize` and xch, which have different focus patterns; revisit once the
  builder exists.
- Making placement cheaper or a separate task (Phase 0 found it is 17–28% of time,
  and on Korean nfd's insertion character-class kernel alone is 18.5% and caps
  scaling at 5.4x). The builder should make it a separable stage; the
  optimization belongs to a later phase.

## Status of the steps

- **Step 1 (builder and nfd):** done.  `FocusedTransform`
  (`kernel/streamutils/focused_transform.h`) with the options `-focus-filter`,
  `-reintegrate`, `-unmodified-filter` and `-fill-run-gaps`; since 4341eae27 the
  defaults are `-focus-filter=run -reintegrate=run`.  nfd's separated pipeline
  stages now pass the source bytes to the filter stage, so they work with every
  filter and reintegration mode.
- **Step 2 (nfc):** done.  `FocusedTransform` has a second form for placement after
  the transform: the transform returns a valid mask (a single bit stream, passed
  unchanged) from which the placement callback computes the placement mask.
  NFC's focus kernel, transform (working-space expansion and composition) and
  placement are in the library (`DetermineNFC_WorkSelection`, `NFC_Transform`,
  `NFC_WorkPlacement` in `lib/kernel/unicode/normalization/nfc.cpp`); nfc.cpp keeps
  `-NoFocus` (the whole text, with room for decompositions only within the work
  regions), `-UseLayers`, `-ByteFiltering` (alias of `-focus-filter=byte`) and
  `-MultiStage`, now three pipelines (source/focus/filter, transform,
  placement/reintegration).  Output is byte-identical to the previous nfc for every
  mode, at 1 and 3 threads, on de, ko, vi, their NFD forms and small inputs.  New
  test: `nfc_multistage_test`.

- **Step 3 (nfkd and tconv):** done.  nfkd has a focused mode, now its default
  (`-NoFocus` keeps the previous whole-text pipeline).  Its work is the characters
  that NFKD changes (canonical or compatibility decomposition, including Hangul
  syllables) or with a nonzero combining class, and the character after each
  (the NFD_Focus kernel with the decomposable set in place of dt=Can).  Its
  placement uses NFD_BixData for NFKD, whose UTF-8 insertion BixNum now has as many
  bits as needed (5 for NFKD: up to 30 bytes are inserted).  The library provides
  `NFD_FocusedTransform`, `NFKD_FocusedTransform` and `NFC_FocusedTransform`, used by
  nfd, nfkd, nfc and tconv.  tconv's Any-NFD, Any-NFKD, Any-NFC and Any-NFKC use them
  with the focused-transform options; its copies of the NFC kernels are gone.  Its rule
  pipeline still uses the U21 pipelines for NFD and NFKD within rules.
  Output: focused nfkd = `-NoFocus` = Python's NFKD on the conformance files and
  corpora, in all modes at 1 and 3 threads; tconv's forms match the tools (and Python
  for NFKC).  New tests: `nfkd_test`, `nfkd_nofocus_test`.  Timing (3 threads):
  focused nfkd is 52% faster than `-NoFocus` on de (0.111 s against 0.230 s), 17% on
  vi, and 26% slower on ko (64% density, as for nfd).

- **Step 4 (shared options, labels, layers):** done.  The shared option category
  (`FocusedTransformOptions()`) now also has `-NoFocus` and
  `-focus-layers=focus,filter,placement,transform` (a phase boundary after each
  given stage, placed by `FocusedTransform::build`); the tools' own options are
  aliases (`-ByteFiltering`; nfc's `-UseLayers` is `-focus-layers=filter`; nfd's
  `-UseLayers` keeps its meaning, a boundary in the filter pipeline of
  `-SeparatedPipelineStages=4`).  Labels come from the builder.
  `QA/focus_baseline.py` runs nfd, nfc and nfkd with the current modes.
  The pipeline's PopCount kernels now take the phase of their consumers (a
  boundary between a work mask and its FilterByMask gave empty output).
  Phase boundaries still have problems in the pipeline framework (drafted as an
  issue): a boundary after the filter or the focus lost the end of nfd's
  transformed data on dense input with work near the end (avoided since by the
  single-kernel ExpandFilter, see `focus-paradigm-bit-movement.md`); the separated filter
  pipeline with `-UseLayers` and a byte-reading filter reports a cycle (nfd rejects
  that combination); and `nfc -focus-layers=focus` crashes LLVM 18's greedy
  register allocator (`-split-spill-mode=size` avoids it).  Boundaries after the
  placement or the transform are correct for all three tools.
- **Step 5 (tests):** done.  `focus_modes_{nfd,nfc,nfkd}_test` run six modes each
  (default, bit filter/merge, byte filter with byte replacement, byte spread with
  gap filling, boundaries after the filter and the transform, `-NoFocus`) at 3 threads with
  `--segment-size=1024` on a generated 8 KB text of sparse and dense work,
  against Python's `unicodedata` (0.3–0.4 s each).

## Status of the other phases

- **Phase 0:** done (baseline, stage labels, `stage_report.py`, `focus_baseline.py`).
- **Phase 2 (nfd):** run-based filter and merge (`RunFilterKernel`,
  `RunMergeKernel` on TwoLevelScanKernel), `-fill-run-gaps`, and two
  TwoLevelScanKernel fixes are done and tested; measurements and the report wait
  for mains power. Phase 2 for nfc follows naturally from step 2 here.
- **Phase 3** (cheaper focused input): unchanged.
- **Phase 4** (layers): planned in `doc/focus-paradigm-phase4-plan.md`, interim
  results in `doc/focus-paradigm-phase4.md`. After three pipeline-compiler fixes, a
  boundary after the transform costs nothing measurable at 3 threads; no boundary is
  faster than none yet.
- **Phase 5** (task and thread allocation, scaling to many cores): unchanged.

## Risks

- **Rarely used modes** (SeparatedPipelineStages=4, MultiStage, UseLayers) could
  regress unnoticed. They stay in the correctness matrix.
- **Pipeline signatures change,** so cached pipelines recompile once. This is
  harmless but makes first runs after each step slow; timings must use a warm
  cache.
- **tconv is large (6,600 lines) and transform-heavy;** its tests are the gate
  for step 3.
