# Plan: transforms with masking

Status (2026-11-18): plan. The first step, a mask input for the class and
property kernels, is done (`MaskKind`, `StreamMask` in
`include/kernel/unicode/charclasses.h`, commit fc7aaa956) and used by tconv's
case-map marks (c85065bb7). Step 2, the pipeline-level `TransformMask` with
position maps, is done in tconv (`TransformMask`, `PositionMap`,
`TransformMasks` in `tools/ldml/tconv.cpp`).

## Goal

One interface for applying a transform, or a kernel computing streams from a
basis, to part of a text:

- **Filters.** A global filter `:: [set] ;`, a filtered step `:: [set] T ;`, and
  the filters of the transforms a transform invokes.
- **`--lines`.** Each line is a text of its own. Today
  `applyToInput` (`tools/ldml/tconv.cpp`) runs the whole pipeline once per line,
  through a pipe.
- **Function calls.** `&T($k)` applies T to a captured text. Case maps are done
  by marks (`CaseMapSplit`); other functions are expanded per character
  (`ldml::ExpandFunctionCalls`) or deferred by marks (shape groups,
  `DeferredCall`).
- **Parts of a group.** Positions where a rule applies, with the change done by a
  shared transform kernel, as the case maps are now.

The aims are fewer special mechanisms, kernels that are shared and cached
across transforms (a transform's code does not depend on where it is applied),
one pipeline run for `--lines`, and more kernels that can run in parallel.

## Current mechanisms (to be replaced or absorbed)

| Mechanism | Where | What it does |
|---|---|---|
| Mask stack | `TransformMasks` (`pushFilter`, `activeMask`, `popFilter`) | the filter's characters, intersected with the outer filters |
| Masks kept in step | stages return a `PositionMap`; `inStep` applies it (`InheritMask` for inherited positions) | after positions are inserted or deleted |
| Barrier replacement | `barrierNormalization`, `maskedNormalization`, `fcd` | characters outside the mask replaced by `BarrierMark` (a starter), normalized, then restored |
| Masked case maps | `maskedStringOverrides` | lc, uc or tc at masked positions, with its own spread |
| Classes outside the mask | `OutsideMask` kernel | a class also holds outside the filter |
| Run ends | `ldml::FilterRunEndMark` (U+FDD2), `marksFilterRunEnd` | an after context's boundary set matching at the end of a filter run |
| Insertions under a filter | `mInsertAnywhere`, eligibility (`insertionsEligible`) | an insertion rule inserts only before a filtered character |
| Per-line runs | `applyToInput` | `--lines`: one pipeline run per line |
| Engine regions | `RE_Compiler` region start and follow streams | matches confined to regions: every class, `.` and external is intersected with the region span `SpanUpTo(starts, follows)`, so matches and lookaround bodies (lookbehinds too) stop at region boundaries; `^` and `$` are the region start and follow; the lookahead chain kernels and region reversal take the follows as barriers |
| Kernel masks | `StreamMask` (Zeroing, PassThru) | class and property kernels (done) |

## The interface

### Two notions, kept apart

1. **Application mask**: the positions where the transform applies (the 1
   bits). Each run of 1 bits is applied as a run, as ICU does under a filter.
2. **Context scope**: what a run's contexts and anchors see.
   - **Text scope** (filters): contexts see the whole text, including characters
     outside the mask, unchanged. Anchors are those of the text. The end of a run
     still matches an after context's boundary set (as `FilterRunEndMark` does
     now).
   - **Region scope** (`--lines`, function arguments): each run is a text of its
     own. Contexts stop at its boundaries, and anchors are its start and end.

A filter in ICU is text scope, and `--lines` is region scope. Both are runs of
1 bits, but they behave differently at the boundaries.

### Declaration

At the pipeline level (`RulePipelineBuilder`, possibly a library type later):

```
struct TransformMask {
    StreamSet * apply = nullptr;        // 1: the transform applies here
    MaskKind kind = MaskKind::PassThru; // outside: unchanged (PassThru) or 0 (Zeroing)
    ContextScope scope = ContextScope::Text;
};
```

- **Region starts and follows** are derived from the mask:
  `start = apply ∧ ¬Advance(apply)` and `follow = ¬apply ∧ Advance(apply)`.
- **Regions are not adjacent.** Each region's follow is a position outside
  every region (a line break, or a character outside the mask). This is the
  precondition for fully functional division into regions (see step 5). Two
  function arguments side by side are therefore not two regions as they stand.
- **Positions.** A transform under a PassThru mask may insert or delete
  positions within its runs. It returns its insertion and deletion masks (or one
  position map), so the caller can keep its other streams in step, as
  `spreadMasks` and `filterMasks` do now. Inserted positions belong to the run of
  the character they follow.
- **In place.** With no insertions or deletions, a PassThru output may modify its
  base in place (InOut, as the class kernels do).
- **Zeroing** applies to kernels producing classes, properties and changes. For
  character transforms it is useful only before a merge.

### Kernel-level conventions

- **Unicode mode:** one mask bit per code point.
- **UTF-8 mode:** the mask is set on every code unit of a character, and regions
  start at a character's first code unit. This needs checking against the UTF-8
  class compilers' marking conventions (Advance marks the last unit, LookAhead
  the first).
- **Mode in the name:** a kernel's name encodes the mask mode (`+MaskZ`,
  `+MaskP`) and the scope, never the mask's contents.

## What each component needs

| Component | Text scope | Region scope |
|---|---|---|
| Class and property kernels | done (no contexts) | the same |
| Fused kernels (contexts by Advance and Lookahead, While loop, star items) | apply mask on the anchors; contexts unchanged; run ends for boundary sets | contexts cut at region boundaries: an item at distance d holds only if no region start lies in (p − d, p]; after items only up to the follow; `MatchStar` runs stop at starts; `start` (the text start) becomes the region start |
| DivCode kernels | as fused | as fused (the converted-context loop's start is the region start) |
| Regular expression engine (`matchPositions`) | the apply mask on the matches | region start and follow streams: matches, lookarounds and anchors confined to regions, which must not be adjacent (step 5) |
| Normalization (NFD, NFC, FCD) | the mask as a composition barrier, replacing `BarrierMark` substitution | the same (no contexts beyond combining sequences) |
| Case maps (lc, uc, tc; `maskedStringOverrides`) | per character | per character; Final_Sigma (Any-Lower) bounded by the region |
| Insertion and deletion stages (spreads, filters) | return their position maps | the same |
| Capture moves and swaps | regions not crossed by moves | the same |
| Invoked transforms (`:: T ;`) | the outer mask passed down, intersected with their own filters | region scope inherited |

## Steps

Each step is checked against the per-line or unmasked behaviour on the
transforms it can affect, and against ICU.

1. **Class and property kernels with masks.** Done.
2. **Pipeline-level `TransformMask`.** Done. The mask stack (`TransformMasks`)
   holds `TransformMask`s of text scope, in both the Unicode and the UTF-8
   pipeline builders. The stages that insert or delete positions return
   `Staged` (the text and its `PositionMap`: deletions, and insertions whose
   positions inherit the mask, are within it, or are outside it), and the
   caller keeps the masks in step (`inStep`). Mapped streams are cached, so a
   stage that needs its mapped active mask (`localCharacterMapping`'s placement,
   the DivCode translation, the deferred rules' rounds) shares the kernels of the
   stack's update. Barrier normalization is a map too: the characters outside
   the mask deleted, and the normalized characters inserted between the
   barriers, within the masks. Three stages still carry the masks themselves:
   the engine's search and replace (shape groups), `kernel::UTF8_CharMapPipeline`
   (whose inserted bytes inherit by `UTF8_InheritMask`), and the front-letter
   revisits, which keep their placeholders out by an outermost mask.
   Output unchanged: the tconv tests, and the CLDR test data of the transforms
   with filters (single-threaded and `-thread-num=4 -segment-size=1`),
   compared with the previous build.
3. **`--lines` as region scope, for the transforms whose components support it.**
   - The plan of a transform says whether every component it uses supports region
     scope. Initially: class kernels, case maps, hex, and fused groups without
     contexts.
   - The others keep the per-line runs. Check equality with the per-line runs on
     the CLDR test data of those transforms, and measure; the per-line runs pay a
     full pipeline invocation per line.
4. **Fused kernels and DivCode with region scope.** Bounded before and after
   contexts, the While loop's start and run items, and the correction kernel's
   rounds. Test with rules files whose contexts reach across line boundaries.
5. **Engine regions.** Done, for regions that are not adjacent. The engine
   confines matches, lookaheads and lookbehinds to the regions given by start and
   follow streams (see Engine regions above). icgrep's lines were checked on
   2026-11-18: nothing crosses a line break in the byte, UTF-8-indexed and
   Unicode-indexed modes, with `-z`, and with `-Unicode-lines`. Adjacent
   regions, where one region's follow is on the next region's start, are not
   separated:
   - **Region span.** `SpanUpTo(starts, follows)`, i.e. follows − starts, is one
     span across both regions. For regions [0,3) and [3,6):
     0b1001000 − 0b0001001 = 0b111111.
   - **Chain kernels.** The lookahead chain kernels and region reversal treat a
     follow as a position outside every region, so they would also exclude the
     second region's first character.
6. **Normalization with a mask**, replacing the barrier substitution in
   `barrierNormalization`. One masked NFD/NFC kernel set, shared by every
   filter.
7. **Filters through the interface.** Fold `OutsideMask`, `FilterRunEndMark` and
   the insertion eligibility into text scope, and retire the special cases one at
   a time.
8. **Function calls by masks.** `&T($k)` on a captured character applies T at the
   marked position, in region scope with each argument a region (separated
   from its neighbours, see the open question on adjacent arguments), as the
   case maps are now, but for any transform tconv implements.
   Deferred calls in shape groups use the same.
9. **Masked kernels skipping blocks without mask bits** (a Pablo `If` on the
   mask), and narrower masks where the work is rare (case maps' expanding
   slots). This step is independent and can come earlier.

## Testing

- **`--lines`:** the single-run output must equal the per-line output on the CLDR
  test data of each transform the step enables, and ICU's output.
- **Region boundaries:** rules files with contexts that would match across a line
  boundary, both edges, empty lines, and lines ending without LF.
- **Filters:** the existing filter tests (`tconv_filter_run_end_test`, the
  DivCode filter cases) unchanged at each step.
- **In place and threads:** `-VerifyIR`, `-thread-num=4 -segment-size=1`
  (`inout_thread_local` covers the pipeline's in-place handling).
- **Performance:** execution cycles on 10 MB inputs (the Wikibooks texts) and
  run time on the test data with `--lines`, against the current mechanisms.

## Open questions

- **ICU's run semantics under filters.** Contexts see beyond a run, anchors are
  the text's, and the characters inserted belong to the run they follow
  (ldml-tconv-coverage.md). Are there further cases, such as a run end within a
  multi-character match, that text scope must reproduce?
- **The position map.** A pair of insertion and deletion masks, or one
  spread-and-filter description, so that a chain of transforms composes their
  maps once instead of each caller re-spreading its streams?
- **Adjacent arguments.** Regions must not be adjacent, so how are function
  arguments side by side separated: by a position inserted between them and
  deleted afterwards, or by applying them in separate passes?
- **Where the type lives.** `TransformMask` in the library (with kernel support
  for region scope) or in tconv's pipeline builder until a second tool needs it?
- **UTF-8 mode.** Mask conventions per code unit, and how they combine with the
  UTF-8 class compilers' Advance and LookAhead marking.
