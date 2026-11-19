# Plan: transforms with masking

Status (2026-11-18): plan. The first step, a mask input for the class and
property kernels, is done (`MaskKind`, `StreamMask` in
`include/kernel/unicode/charclasses.h`, commit fc7aaa956) and used by tconv's
case-map marks (c85065bb7). Step 2, the pipeline-level `TransformMask` with
position maps, is done in tconv (`TransformMask`, `PositionMap`,
`TransformMasks` in `tools/ldml/tconv.cpp`). Step 3, `--lines` in one run for
the transforms whose components support region scope, is done
(`regionScope`, `pushRegions`, `--lines-regions`). Step 4, contexts bounded by
the regions in the fused kernels (correction kernels included) and the engine,
is done (`boundRegions`, `FusedRulesConfig::region`, `--bound-regions`).

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
| Per-line runs | `applyToInput` | `--lines`: one pipeline run per line, for the transforms not supporting region scope (`regionScopeProblem`) |
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

### Execution: focused or in place

A transform under a mask can run in two ways, and both are kept for every
masked transform:

- **Focused:** the masked positions (or the work they need) are filtered into a
  short stream, transformed there, and spread back. `localCharacterMapping` and
  the focused normalization transforms work this way.
- **In place:** the kernels run over the whole text under the mask, with no
  filter or spread. `maskedStringOverrides`, the masked class kernels and
  masked NFD work this way.

The cheaper choice depends on the work per position in the transform, against
the cost of filtering and spreading, and on the density of the mask. Filter and
spread are an area for improvement in the pipeline, which will move the
balance, so neither path is retired: the same masked kernels serve both, with
the choice made by an option (and possibly estimated later). Focusing
concatenates regions, so a focused transform must also keep its regions from
interacting at the seams; working in place has no seams (see
doc/masked-nfc-plan.md, The focus seam).

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

   Done. With `--lines`, a transform whose components all support region scope
   (`regionScopeProblem`, shown by `--plan --lines`) is applied in one run: the
   builders push a mask of region scope outside every filter (`pushRegions`),
   the characters other than the line feeds, which pass through unchanged.
   `--lines-regions=0` keeps the per-line runs. No kernel bounds contexts yet
   (step 4), so the components supported are those that cannot see past a line:
   - the class kernels; the case maps (Any-Upper, Any-Lower, Any-Title: neither
     Final_Sigma's contexts nor a word crosses a line feed); NFD, NFKD, NFC and
     NFKC (each run of the mask normalized by itself by the barrier
     substitution); Any-Hex and its forms, Any-Remove, Any-Null;
   - conversion groups without revisits, insertions, deferred rules, prefix
     runs, chain revisits, mark run edits, capture replacements or front
     letters, whose contexts are *line-bound*. A match lies within the mask. In
     the text, the position beyond a line holds a line feed where the line as a
     text has its end, which a class matches if it has U+FFFF (as in ICU), and
     an anchor matches too. So the outermost item of a context (a class, an
     alternation or difference of classes and the anchor of its end) must match
     both the line feed and the end, or neither. An inner item must match
     neither. A lookaround of such an item (as the engine has a complement)
     must match both or neither. No item may match a mark of tconv's or the
     deletion mark.

   Of the 252 CLDR transforms with test data, 112 are applied in one run. The
   others have contexts that are not line-bound (e.g. `[$ \-] {`, which matches
   at the start of a line taken as a text but not after a line feed), capture
   replacements, prefix runs or insertion rules. On the test data of the 112,
   one run gives the output of the per-line runs, also with `-thread-num=4
   -segment-size=1` (`tconv_lines_regions_test` covers the edges). Warm run times
   on the largest test data, per-line runs to one run: 0.45 s to 0.04 s
   (und-Ethi-t-sgw-ethi-m0-gurage-2013, 8532 lines), 0.22 s to 0.05 s
   (es-419-fonipa-t-es-fonipa), 0.24 s to 0.05 s (ug-fonipa-t-ug); 36.8 s to
   27.2 s for am-Ethi-t-am-ethi-m0-geminate, whose 7718 string rules take about
   27 s to analyze either way.
4. **Fused kernels and DivCode with region scope.** Bounded before and after
   contexts, the While loop's start and run items, and the correction kernel's
   rounds. Test with rules files whose contexts reach across line boundaries.

   Done for the fused kernels and the engine; DivCode is not used for a bounded
   group. A conversion group whose contexts are not line-bound (step 3) is
   applied with its contexts bounded by the regions (`RegionScope::bounded`,
   `--plan --lines`: "contexts bounded by the lines"); `--bound-regions=0`
   keeps step 3's per-line runs for them.
   - **Fused kernels** (`FusedRulesConfig::region`): InText gives the region
     positions. An item at distance d holds only if every position between it
     and the match is in the region; otherwise it holds as beyond the end (or
     start) of a text, only if it is a boundary item.
   - **Correction kernels**: the converted text is walked back within the
     region (`stepBack`; deletion carries and possessive runs stop at its
     start), and the first position of a region is the start of a text
     (`firstPosition`), in the rounds and in the While loop (whose initial and
     loop steps back are bounded alike; the values outside the regions stay 0).
   - **Engine**: the region starts and follows (`RegionEdges`) as
     `RE_context::matchStarts` and `matchFollows` (step 5).
   - A bounded group is not applied by DivCode, the transform not by its shape
     groups, and not in UTF-8 mode (the fused path gives the same output).

   Of the 252 CLDR transforms with test data, 228 are now applied in one run
   (112 after step 3), 116 of them with bounded contexts; the others have
   capture replacements, prefix runs, insertion or deferred rules. On the test
   data of the 116, one run gives the output of the per-line runs, also with
   `-thread-num=4 -segment-size=1`. `tconv_lines_regions_test` has rules files
   whose contexts reach across lines (after and before, the engine's, and
   before contexts matched against the converted text, which differ without
   the bounds). Warm run times, per-line runs to one run: zh-t-es 2.86 s to
   0.17 s, zh-t-es-419 2.46 s to 0.18 s, ja-t-es-419 2.42 s to 0.08 s,
   zh-t-es-fonipa 0.83 s to 0.16 s (3514 lines each).
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

   Done for NFD and NFKD in Unicode mode (`--masked-normalization`, default
   on): `NFD_U21_Pipeline` and `NFKD_U21_Pipeline` take a mask. Only the
   characters of the mask are decomposed (masked class kernels in
   `U21_CharToShortStringPipeline`, and `LVT2NFD` for the Hangul syllables),
   and only the runs of nonzero combining classes within it are sorted, so a
   character outside it is unchanged and nothing moves across it. The positions
   inserted for a decomposition belong to the run of the character it
   decomposes (the spread mask is returned for the position map). FCD uses it
   too. On the CLDR test data of the 112 transforms with NFD, NFKD or FCD under a
   filter, the output equals the barrier substitution's (also multithreaded and
   with `--lines`); el-Latn-t-el-m0-bgn: 83 to 75 kernels.
   Remaining:
   - **UTF-8 mode** (95 of those 112 transforms) still substitutes barriers
     (`UTF8PipelineBuilder::maskedNormalization`). A mask on the byte-level
     focused NFD needs work regions within the runs of the mask, with no
     reordering across the seams of the focused stream.
   - **NFC, NFKC and FCC** still substitute barriers. Their composition kernels
     are generated (`scripts/UCD/nfc_generator.py`); a mask needs the pairs
     composed only within a run (the pair finding, long composition and Hangul
     composition).
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
