# Plan: transforms with masking

Status (2026-11-18): plan. The first step, a mask input for the class and
property kernels, is done (`MaskKind`, `StreamMask` in
`include/kernel/unicode/charclasses.h`, commit fc7aaa956) and used by xlit's
case-map marks (c85065bb7). Step 2, the pipeline-level `TransformMask` with
position maps, is done in xlit (`TransformMask`, `PositionMap`,
`TransformMasks` in `tools/ldml/xlit.cpp`). Step 3, `--lines` in one run for
the transforms whose components support region scope, is done
(`regionScope`, `pushRegions`, `--lines-regions`). Step 4, contexts bounded by
the regions in the fused kernels (correction kernels included) and the engine,
is done (`boundRegions`, `FusedRulesConfig::region`, `--bound-regions`).
Step 5, engine regions, is done for regions that are not adjacent. Step 6,
normalization with a mask, is done for every form in both modes
(`--masked-normalization`, `--u21-nfc`; doc/masked-nfc-plan.md). Step 7 is
done in Unicode mode, and step 9 for the class and property kernels. Since
step 4, capture replacements are applied in region scope too (229fb48c0).

**Next (2026-11-20): every transform masked in both modes, and one run for
`--lines`.** See [Completing the interface](#completing-the-interface-both-modes-one-run-for---lines):
the per-line runs are to be removed, and the UTF-8 mode is to apply every
component that can be applied in it, switching to Unicode mode only where a
component needs it, and back as early as possible.

## Goal

One interface for applying a transform, or a kernel computing streams from a
basis, to part of a text:

- **Filters.** A global filter `:: [set] ;`, a filtered step `:: [set] T ;`, and
  the filters of the transforms a transform invokes.
- **`--lines`.** Each line is a text of its own. Today
  `applyToInput` (`tools/ldml/xlit.cpp`) runs the whole pipeline once per line,
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
| Barrier replacement | `barrierNormalization`, `UTF8PipelineBuilder::maskedNormalization` | characters outside the mask replaced by `BarrierMark` (a starter), normalized, then restored; replaced by the masked kernels (step 6), kept only for `--masked-normalization=0` |
| Masked case maps | `maskedStringOverrides` | lc, uc or tc at masked positions, with its own spread |
| Classes with run ends | `WithRunEnds` kernel, from `TransformMasks::runEnds` (step 7) | a class also holds outside the filter |
| Run ends | `ldml::FilterRunEndMark` (U+FDD2), `marksFilterRunEnd`; `TransformMasks::runEnds` (step 7) | an after context's boundary set matching at the end of a filter run |
| Insertions under a filter | `TransformMask::insertBefore`, eligibility (`insertionsEligible`) (step 7) | an insertion rule inserts only before a filtered character |
| Per-line runs | `applyToInput` | `--lines`: one pipeline run per line, for the transforms not supporting region scope (`regionScope`); to be removed (step 12) |
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
   barriers, within the masks. Three stages still carried the masks themselves:
   the engine's search and replace (shape groups), `kernel::UTF8_CharMapPipeline`
   (whose inserted bytes inherit by `UTF8_InheritMask`), and the front-letter
   revisits, which kept their placeholders out by an outermost mask.
   **Done (2026-11-21):** the position maps are the library's
   (`kernel::PositionMap`, mapped by `kernel::PositionMapper`, with an
   inserted position inheriting from the next position too, `InheritNext`);
   the engine's search and replace, the UTF-8 character maps and the hybrid's
   selectors (`UTF8_SelectorPipeline`) return their maps instead of carrying
   the masks; and the front-letter placeholders and the original positions of
   `--region-separators` are streams tracked by `TransformMasks`
   (`pushTracked`), kept in step as the masks are but not masks, instead of
   outermost masks.
   Output unchanged: the xlit tests, and the CLDR test data of the transforms
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
     must match both or neither. No item may match a mark of xlit's or the
     deletion mark.

   Of the 252 CLDR transforms with test data, 112 are applied in one run. The
   others have contexts that are not line-bound (e.g. `[$ \-] {`, which matches
   at the start of a line taken as a text but not after a line feed), capture
   replacements, prefix runs or insertion rules. On the test data of the 112,
   one run gives the output of the per-line runs, also with `-thread-num=4
   -segment-size=1` (`xlit_lines_regions_test` covers the edges). Warm run times
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
   `-thread-num=4 -segment-size=1`. `xlit_lines_regions_test` has rules files
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
   NFC, NFKC and FCC followed (2026-11-19, doc/masked-nfc-plan.md), with the
   NFC kernels rebuilt in C++ from the UCD (`NFC_Tables`, `nfc_kernels.cpp`)
   and a mask input, focused or in place, in UTF-8 and 21-bit modes, with an
   output mask:
   - **Unicode mode:** NFD and NFKD by `NFD_U21`, NFC by `NFC_U21` (no
     conversion through UTF-8), NFKC as the masked NFKD then the masked NFC,
     FCC as the masked NFD then its passes' masked NFC. The position map is
     the selection deleted and the output mask inserted.
   - **UTF-8 mode:** NFD and NFKD by `NFD_Masked`, NFC by `NFC_Masked`, NFKC by
     both, on the bytes, with the output byte mask for the position map.
   - **FCD:** its regions are found under the mask (`FCD_RegionMask` with a
     mask: lccc = tccc = 0 outside it, as for a barrier), then decomposed by the
     masked NFD.
   - **`--lines`:** FCD and FCC now support region scope as well, so every
     normalization form is applied in one run (none of the CLDR transforms
     uses them).

   Barrier substitution (`barrierNormalization`, `BarrierMark`,
   `ReplaceUnmasked`, `UTF8_ReplaceUnselected`) is left only behind
   `--masked-normalization=0` (and `--u21-nfc=0`) for comparison, and can be
   removed with the NFC generator (masked NFC plan, step 7; the generated
   kernels are removed).

   Checks: the CLDR test data of the transforms with NFC, NFKC, NFD, NFKD or
   FCC under a filter give the previous route's output, also multithreaded and
   with `--lines` (see doc/masked-nfc-plan.md, step 6 and NFD and NFKD,
   aligned). FCD and FCC under global and step filters cutting mark
   sequences, on 4,000 random lines: the masked route, the barrier route, ICU
   (FCD), per-line runs and one run with `--lines` all agree, also with
   `-thread-num=4 -segment-size=1`.
7. **Filters through the interface.** Fold `OutsideMask`, `FilterRunEndMark` and
   the insertion eligibility into text scope, and retire the special cases one at
   a time.

   Done (2026-11-19) in xlit's Unicode mode:
   - **Insertion eligibility** is a property of a mask of text scope,
     `TransformMask::insertBefore`: an insertion within its runs is made only
     before one of its positions (ICU's compound transforms under a filter).
     The filter of a transform of one conversion group, its own or the one it
     is invoked with, does not restrict them (`TransformMasks::insertAnywhere`).
     `insertionsEligible` takes the innermost mask that restricts insertions
     (`insertionMask`). This replaces `mInsertAnywhere`, a flag saved and
     restored around each transform, and the index arithmetic on the stack.
   - **Run ends** are a stream of the mask stack, `TransformMasks::runEnds`
     (the positions outside the innermost mask, cached). The classes of the
     sets with `FilterRunEndMark` are ORed with it (`WithRunEnds`, which
     replaces `OutsideMask`, a kernel that complemented the mask itself), and
     the engine's `xlitFilterRunEnd` external is the same stream instead of
     a complement of its own.

   `FilterRunEndMark` itself stays: it is how the rule analysis
   (`ldml::MoveTextAnchors`) tells the pipeline that a set matches at the end
   of a run, which the rules alone do not say.
   Checks: the xlit tests (`xlit_filter_run_end_test`, the insertion and
   DivCode tests) pass, and insertions under global filters, with one group
   and with several, give the previous output. On the CLDR test data of the
   151 transforms with a filter (am_Ethi-d0_Morse included), with `--lines`,
   the output equals the previous build's.
   Remaining:
   - **UTF-8 mode** has no run ends: a group with single-character rules
     whose contexts end a filtered run is applied in Unicode mode. Of the CLDR
     transforms, only am_Ethi-d0_Morse uses run ends, and it is in Unicode mode
     for its insertion rules.
   - **DivCode** takes the mask and complements it within its kernels; it
     could take the run ends instead.
8. **Function calls by masks.** `&T($k)` on a captured character applies T at the
   marked position, in region scope with each argument a region (separated
   from its neighbours, see the open question on adjacent arguments), as the
   case maps are now, but for any transform xlit implements.
   Deferred calls in shape groups use the same.
9. **Masked kernels skipping blocks without mask bits** (a Pablo `If` on the
   mask), and narrower masks where the work is rare (case maps' expanding
   slots). This step is independent and can come earlier.

   In progress (2026-11-19): the class and property kernels with a mask
   (`CharClassesKernel`, `UnicodePropertyKernelBuilder`, xlit's
   `PropertyClasses`) compute their streams within an If on the mask
   (`kernel::computeMasked`; `-masked-block-skip`, default on, which the
   kernel names encode as `+MaskZIf`/`+MaskPIf`). The normalization and xlit
   tests pass, and the CLDR test data of the 151 transforms with a filter give
   the previous output.

   el-Latn-t-el-m0-bgn (a filter of the Greek letters) on 10 MB of Wikibooks
   text, cycles of its two masked class kernels over the whole text
   (`-EnableCycleCounter`, one thread), without / with the skip:

   | Text | Greek in the text | Kernel 1 | Kernel 2 |
   |---|---|---|---|
   | German | 0% | 31,999 / 2,551 | 28,778 / 2,332 |
   | Greek | 50% | 22,293 / 20,758 | 19,480 / 18,010 |

   The skip costs nothing where the mask is dense. The run times are the
   same (0.28 s and 0.17 s with 4 threads): these kernels are 0.5% of the
   transform. Remaining:
   - the NFC stages and the masked case maps, whose masks are applied within
     their own kernels;
   - narrower masks for the case maps' expanding slots.

## Completing the interface: both modes, one run for `--lines`

Goals (2026-11-20):

- **One run for `--lines`, always.** Every transform is applied to all the
  lines in one run, each line a region. The per-line runs (`applyToInput`'s
  loop, a pipeline invocation per line through a pipe, and
  `--lines-regions=0`) are removed.
- **Both modes, mixed.** A transform is no longer in UTF-8 mode or Unicode
  mode as a whole. Each component is applied in UTF-8 mode if it can be, and
  the pipeline switches to Unicode mode only at the first component that needs
  it, and back to UTF-8 mode as early as possible. Leading and trailing
  normalizations, case maps and UTF-8-capable conversion groups stay on the
  bytes.
- **Masks in both modes.** Every component honours the text scope of filters
  and the region scope of `--lines` in the mode it runs in, and the masks
  (with their run ends, insertion eligibility and regions) cross each switch.

### Survey (2026-11-20, `xlit --plan` and `--plan --lines` for all 453 names)

Of the 424 implementable transforms defined by rules, 183 are in UTF-8 mode
and 241 in Unicode mode: 111 for a component of their own, 130 only because
a transform they invoke is. The first reason, followed through the invoked
transforms to its root:

| Reason for Unicode mode | Transforms |
|---|--:|
| a conversion group with string rules that DivCode cannot plan for UTF-8 | 186 |
| insertion rules | 14 |
| prefix runs | 13 |
| single-character rules with before contexts that are not sequences of sets | 8 |
| characters of several subgroups | 7 |
| rules with captures | 7 |
| mark run edits | 3 |
| before contexts matched against the converted text (single-character rules) | 1 |
| the built-ins Any-BreakInternal, Any-Title | 2 |

The DivCode problems of the string-rule groups of the 70 transforms where they
are the direct reason (127 groups, 101 ineligible): string rule overlaps,
two-way or self (54 groups); string rules with runs (23); a converted before
context that may begin within a string match (22); string rules keeping
characters of their texts (21); rules with captures (20); converted before
contexts of string rules (14); contexts that are not fixed sequences of sets
(14); self-overlapping string rules (14); and a few others. The fused kernels
handle all of these, but only in Unicode mode.

With `--lines`:

| Mode | One run | One run, contexts bounded | Per-line runs |
|---|--:|--:|--:|
| UTF-8 | 139 | 44 (then applied in Unicode mode) | 0 |
| Unicode | 33 | 165 | 43 |

The 43 per-line runs (counting the transforms invoking the component): prefix
runs 19, insertion rules 16, deferred rules 4, mark run edits 3. The built-ins
without region scope: Any-BreakInternal, Any-CaseFold, Any-Name, Name-Any and
the seven forms of Hex-Any. Revisits, chain revisits and front-letter
revisits, which also force per-line runs, are in no CLDR transform's
`--lines` path at present, but rules files may have them. Transforms applied
by ICU (`--use-ICU`, whole or as stages through `stagedTransformRunner`) are
also run line by line through `applyToInput`.

### Steps

**Region scope relies only on the mask.** A region is a run of 1 bits, with
at least one position outside the mask (mask = 0) between any two regions,
which may hold any character: no component may rely on it being a line feed.
Where the analyses of contexts take it to be one (`groupLineBound`: a
context that cannot match a line feed needs no bounds), a line feed is
inserted, outside the mask, at each edge of each region whose outside
positions are not line feeds, and deleted after the transform
(`RulePipelineBuilder::pushRegions`, `popRegions`; the original positions are
an outermost mask, kept in step). With `--lines` the separators are line
feeds and nothing is inserted. The hidden option `--region-separators=<hex,...>`
sets other characters outside the regions, for testing: one run must equal
each region transformed as a text of its own, with separators taken from
the alphabet of the transform (a cased letter for Final_Sigma, a combining
mark for the normalizations, a run character for prefix runs, an iteration
mark for the deferred rules). Text scope (filters) is unaffected: its
contexts see the characters outside the runs.

10. **Region scope for the remaining components, in Unicode mode.** Each is
    checked as steps 3 and 4 were: one run against the per-line runs on the
    CLDR test data of the transforms it enables (also `-thread-num=4
    -segment-size=1`), and rules files reaching across lines at both edges.
    - **Insertion rules** (16): an insertion at the start of a region needs no
      position before it in the region; eligibility (`insertionsEligible`)
      within the region; the correction kernel's insertions are already
      bounded with the region.
    - **Prefix runs** (19): a run stops at the region's follow; the blocking
      marks (`DeletedRunBlockMark`) and the streams of deleted runs restart
      at each region start.
    - **Deferred rules** (4): the rounds' compaction to the live positions and
      `DeferredPlan` keep the region mask mapped (as they keep the active
      mask), and a copied capture never reaches across a region.
    - **Mark run edits** (3), revisits, chain revisits and front-letter
      revisits: their runs and walks bounded by the regions.
    - **Built-ins**: Hex-Any and Name-Any parse within a line (their syntax
      has no line feed), Any-Name and Any-CaseFold are per character, so they
      need only to be checked and listed (`regionScope`); Any-BreakInternal
      needs its break kernel bounded by the regions.
    **Done (2026-11-20).** Of the 453 transform names, 448 are now applied in
    one run with `--lines` (251 with contexts bounded by the lines); the other
    five are ICU's (step 11). Accepted in region scope, bounded: insertion
    rules, prefix runs, deferred rules, mark run edits, chain revisits and
    front-letter revisits; and the built-ins Hex-Any (seven forms) and
    Any-CaseFold (now in the rule pipeline, by the `cf` mapping within the
    mask: `maskedStringOverrides`). The revisits that are not composed make a
    plan not implementable (applied by ICU), so the check of a rule with a
    revisit is left as a safeguard. Fixed on the way: the rounds of the
    deferred rules matched their rules with the bounds of the regions in the
    positions of the text rather than of each round's compacted text
    (`mRegionsOverride`; `ハヽヽ` gave `hahaヽ`); `boundedRegions` took the mask
    at the bottom of the stack, which an outermost mask (the front letters,
    the separators) may be; and the front letters' placeholders, kept out of
    every mask, cut the regions (`TransformMask::bounds`, where an inserted
    position belongs to the region of the position it follows: Grek-Latn gave
    `CHha` for `Χἁ`). Checks: one run equals the per-line runs on the CLDR
    test data of the transforms each component enables, and on 1,500 lines
    each of Wikibooks Japanese and Persian text (Kana-Latn, Hira-Latn,
    fa-fa_FONIPA, also equal to ICU per line) and 3,000 random Greek lines
    (Grek-Latn, Grek-Latn/UNGEGN); with `--region-separators` taken from each
    transform's alphabet, one run equals each region as a text of its own
    (`QA/xlit_region_separators_test.py`).
11. **ICU under `--lines`.** ICU's `uconv -x` does not give `--lines`
    semantics. It transliterates the input a line at a time, but each line's
    text includes its line feed (checked with ICU 78.3's `uconv`, the version
    xlit links, `/opt/homebrew/opt/icu4c@78/bin/uconv`):
    - the start of a line is the start of a text: `[a\u000A] { b > X ;`
      leaves the `b` after a line feed, and `[$] { b > X ;` matches at the
      start of each line;
    - the end of a line is not the end of a text: `b } [$] > X ;` matches only
      on a last line without a line feed, and `b } \u000A > Z ;` matches
      before each line feed.

    So rules that look at the end of a text, or at a line feed after it,
    differ from the CLDR test data, which takes each line as a text without
    its line feed: my-my_FONIPA drops a word-final `a̰` at the end of 57 of its
    1,228 test lines under uconv (`က` gives `k` instead of `ka̰`). (Nor could
    `-x` take every transform as xlit has it: a transform ID uses ICU's own
    CLDR data rather than the checkout's rules, which carry local fixes, and
    rule text is applied forward, so a backward transform of an XML file
    cannot be passed as it is.) A transform applied by ICU is therefore
    applied to each line as a text of its own, in the process, by xlit's own
    `ICUTransform` (the same transliterator, from the checkout's rules),
    without the pipe and thread of `applyToInput`. A transform with ICU stages
    (`stagedTransformRunner`) runs each xlit stage once over all the lines,
    in region scope, and each ICU stage likewise on the intermediate text.

    **Done (2026-11-20), in the process.** `icuLinesRunner` applies an ICU
    transform to each line (the same transliterator, from the checkout's
    rules, so `uconv` is not needed), and `stagedTransformRunner` with
    `--lines` generates its xlit stages once with regions and applies its ICU
    stages line by line. Any-Name, Name-Any, Any-BreakInternal, Thai-Latn and
    Latn-Grek (and rules files xlit does not implement) are applied in one
    run (`--plan --lines`: "the steps of ICU line by line"), with the output of
    the per-line runs and of ICU (`QA/xlit_icu_lines_test.py`). Remaining:
    a transform with ICU stages and a global filter (Thai-Latn, the only one)
    applies all its stages to each run of the filter (`filteredRunner`, as ICU
    does), so its xlit stages are still run once per run of the filter (no
    faster than per line: 1.65 s against 1.54 s on 2,000 lines). Applying them
    once needs the runs' boundaries carried through the xlit stages to the
    ICU stage.
12. **Remove the per-line runs.** When steps 10 and 11 are done, one run gives
    the per-line output on the CLDR test data of every transform, in both
    modes and with four threads. Then `applyToInput`'s per-line loop and
    `--lines-regions` are removed, `regionScope` becomes a check that must
    succeed (a component without region scope is an error in `--plan
    --lines` and in the tests), and the tests that compared one run with
    per-line runs (`xlit_lines_regions_test`) compare with expected
    outputs (from ICU, or from the last per-line build).
    **Done (2026-11-20).** One run gave the output of the per-line runs on
    the CLDR test data of all 290 test files (the eight that differ from
    their expected results do so both ways: six transforms xlit does not
    know, ka-Latn-t-ka-m0-bgn-2009 and my-fonipa-t-my). `--lines-regions`
    and `--bound-regions` are removed, `applyToInput` runs once, the ICU
    paths use the line runners with `--lines`, and a transform without
    region scope is an error with `--lines` (`--plan --lines`: "not
    supported"). `xlit_lines_regions_test` compares one run with the
    outputs of the last build with per-line runs (`EXPECTED`).
13. **Mixed modes.** The plan of a transform becomes a sequence of segments,
    each in UTF-8 or Unicode mode, through the transforms it invokes:
    - **Choice.** A component is in UTF-8 mode if it can be (the built-ins of
      `utf8BuiltIn`, `utf8CharMap` groups, DivCode groups for UTF-8), unless
      a Unicode segment on both sides makes it cheaper to stay (each switch
      costs a decode and an encode: compare kernels, then measure). `--plan`
      lists the segments, their modes and the reason for each Unicode
      segment, instead of one mode line.
      **Revised (2026-11-24): one switch to Unicode mode.** Once a switch to
      Unicode mode is made, the text goes back to UTF-8 mode only where all
      the rest of the work is in UTF-8 mode, the transforms it invokes
      included (`mixedSegments`): a Unicode segment runs from a step not
      wholly in UTF-8 mode to the first step from which every step is, or to
      the end. A step invoking a transform in mixed modes itself stays in
      UTF-8 mode only where all the steps after it are wholly in UTF-8 mode
      (its own segments then follow the same rule); otherwise the transform
      is applied within the Unicode segment. Zawgyi-my went from four Unicode
      segments (steps 10, 12, 14, 16, with UTF-8 steps between) to one
      (steps 10-16); 24 transforms changed their segments (Latn-Grek,
      ja_Latn-ru, cy-cy_FONIPA, …; Jpan-Latn and Latn-Thai no longer switch
      within Hira-Latin and Latin-ThaiLogical). 22 of them give ICU's output
      (CLDR test data, or 2,000 lines of their perf corpora); Han-Latin/Names
      and Jpan-Latn give the output they had, which differs from ICU's on
      full-width punctuation (xlit gives `!` for `！`, as before).
      `--plan` reports the segments the pipeline uses (it did not show the
      steps a segment takes after prefix runs or front-letter revisits).
    - **Switches.** UTF-8 to Unicode: the 21-bit basis decoded from the
      bytes; Unicode to UTF-8: encoded. These are stages with position maps
      (a character's code units to one position, and back).
    - **Masks across a switch.** The mask stack (`TransformMasks`) is mapped
      with the text: on decoding, a character's mask bit is that of its code
      units (which agree; see step 14); on encoding, a position's bit is
      spread to each of its code units. Run ends, insertion eligibility and
      the regions of `--lines` are properties of the masks, so they follow.
      The builders share one mask stack type and one pass over the plan.
    - **Checks.** Every transform in mixed mode against Unicode mode
      (`--utf8-mode=0`) on the CLDR test data, with and without `--lines`,
      and against ICU on random text for the transforms whose segments
      change.
    **Implemented (2026-11-20), being checked.** `--mixed-modes` (default on):
    the UTF-8 builder applies each step in UTF-8 mode where it can
    (`utf8StepProblem`, shared with `--plan`), and the steps that need
    Unicode mode in segments (`UTF8PipelineBuilder::unicodeSegment`): the
    text decoded (`U21_Of`), the mask stack mapped to one position per
    character (each character's bit at its final byte, `UTF8_index`), the
    steps applied by one `RulePipelineBuilder` of the pipeline
    (`applySegment`, its label numbered by segment for the names of its
    kernels), then the text encoded and the masks spread back over the
    bytes of each character (`U8Spans`, LookAhead mode, which takes marks at
    final bytes). A segment is extended over the steps that read the state
    its steps leave: the step after prefix runs (past `:: Null ;`), and the
    front-letter revisits to their end. A built-in of Unicode mode, and a
    transform applied by its shape groups or not by the regular plan, is a
    segment of its own. With `--lines`, the groups whose contexts are bounded
    by the lines are Unicode segments, so that the transform stays in UTF-8
    mode elsewhere. `--plan` gives the segments (`mode: mixed: step 1 UTF-8;
    step 2 Unicode (a conversion group with string rules); steps 3-4 UTF-8`).
    Found on the way, older than this work: in UTF-8 mode, a subgroup with
    contexts whose characters map to themselves (`\+ { g → g ;`, from
    prefix-run triggers) gave its matches to another subgroup's mapping of
    the character (`g → RR`); `utf8Applies` now matches only the characters
    a subgroup changes. Also older: the regular expression engine's
    `FixedMatchSpansKernel` declared a lookahead of a whole block
    (`round_up_to_blocksize(length)`, 128 on this machine), and in some
    pipelines it lost the spans of the final partial stride (positions
    128 and after, with the input marks complete). Mixed mode exposed it with
    `--lines am-am_FONIPA` (15 of 607 lines of `am-fonipa-t-am`, the ፞/፟
    gemination rules not applied near the end): a mask decoded from UTF-8 has
    its region bounds and its positions as two streams (equal, but separate),
    and the region edges made from the bounds changed the pipeline's layout
    (the same loss in Unicode mode with the bounds copied). The kernel now
    declares its actual lookahead, `length + offset - 1` (the old one also
    ignored the offset). The pipeline's handling of a block-sized lookahead
    at the final stride is still to be investigated.
14. **UTF-8 mask conventions** (the open question below). All the bytes of a
    character outside a region (or a filter) are 0 in the mask. A mask
    computed from a character class is made from the characters outside it:
    their class in LookAhead mode (marking their first bytes), `U8Spans` over
    `UTF8_index` in its Advance mode (which takes marks at first bytes and
    spreads them forward to the final byte; its LookAhead mode takes marks at
    last bytes), and the complement
    (`UTF8PipelineBuilder::utf8Mask`, used by `pushFilter` and so by
    `pushRegions`; done 2026-11-20). Still to check against the UTF-8 class
    compilers' marking (Advance marks the last unit, LookAhead the first),
    the UTF-8 character maps and DivCode on bytes, so that a mask decoded in
    step 13 is exact.
    **Done (2026-11-20).** The hidden option `--check-utf8-masks` checks the
    convention where it matters: after each filter's mask is made, after the
    regions of `--lines`, and after each step (a character map, a DivCode
    group, a built-in on the bytes, a Unicode segment with its masks spread
    back), each mask of the stack and the bounds of the regions. A kernel
    (`UTF8MaskSplits`) counts the continuation bytes whose bit differs from
    that of the byte before, and a pipeline call reports each mask with a
    count other than 0 (exit status 3). It held for: filters with characters
    of one to four bytes outside them; character maps that change the number
    of bytes (1 to 2, 2 to 1, 1 to 4, one character to two) before and after
    a capture rule in a Unicode segment; a DivCode group of string rules on
    the bytes; and `am-am_FONIPA` with `--lines` (two Unicode segments), each
    with output equal to Unicode mode's
    (`QA/xlit_utf8_masks_test.py`). A negative control (the check applied to
    `UTF8_index`, which marks only final bytes) was reported. The class
    compilers' marking does not enter the masks: the masks are made by
    `utf8Mask` from a class in LookAhead mode, and kept in step by the
    position maps of the character maps and DivCode (a byte inserted after a
    character takes its bit). So a mask decoded at a character's final byte
    is exact. Found on the way: a constant scalar (`CreateConstant`) given to a
    pipeline call (`CreateCall`) crashes the pipeline compiler
    (`PipelineCompiler::getFinalOutputScalars`); the check uses a callback
    per check instead.
15. **UTF-8 components with masks of both scopes.** So that fewer components
    need Unicode segments:
    - **Run ends in UTF-8 mode** (step 7's remaining item), for single-character
      rules whose contexts end a filtered run.
    - **Bounded contexts in UTF-8 mode and in DivCode** (step 4 kept both out):
      the 44 UTF-8-mode transforms whose contexts must be bounded by the lines
      now switch wholly to Unicode mode; with region bounds in the UTF-8
      character maps' contextual groups and in DivCode (both modes), they stay
      in UTF-8 mode.
    **Partly done (2026-11-20).** Bounded contexts in the UTF-8 character
    maps: a contextual group whose contexts are not line-bound is matched by
    the regular expression engine on the bytes with the regions as its match
    regions (`utf8Applies`, the starts and follows of the region mask, as grep
    gives the engine its lines), so with `--lines` it stays in UTF-8 mode
    (`utf8StepProblem` sends a bounded group to Unicode mode only if it needs
    DivCode). Checked with contexts that match the line feed but not the end of
    a text (`a } [\u000A c] → A ;`): the same output as Unicode mode, with no
    decoding. Fixed on the way: `--plan --lines` printed the mode as without
    `--lines` (`printMode` took no bounds), so it showed none of the Unicode
    segments of bounded groups. With that fixed, the survey of the 290 CLDR
    test transforms with `--lines`: 65 have Unicode segments for bounded
    groups; the character maps' bounds remove one segment from 2 of them
    (zh-t-es-fonipa, zh-t-ru); the others' bounded groups are DivCode groups.
    Not done:
    - **Bounded contexts in DivCode.** The segments they would remove cost
      little: zh-t-es-fonipa, on 2 MB with one thread, takes 0.33 s with
      `--lines` (three bounded groups in Unicode segments) and 0.40 s without
      (all in UTF-8 mode). Left to step 16.
    - **Run ends in UTF-8 mode.** No CLDR test transform has a Unicode
      segment for it ("single-character rules with contexts at the end of a
      filtered run"): the engine on the bytes would need externals of the
      client (the positions outside the mask), which it supports for Unicode
      sources only.
16. **More components in UTF-8 mode.** The string-rule
    groups that DivCode cannot plan (186 transforms through them) run in a
    Unicode segment after step 13. Reducing that set means either DivCode
    support for their problems (overlaps, runs, kept characters, captures) or
    the fused kernels on UTF-8 code units.

    **Goal (2026-11-20): UTF-8 support for every component.** Survey and
    profile (2026-11-20):

    | Reason for a Unicode segment (290 CLDR test transforms, own steps) | Transforms | Segments |
    |---|--:|--:|
    | a conversion group with string rules DivCode cannot plan | 44 | 67 |
    | characters of several subgroups | 9 | 10 |
    | prefix runs | 9 | 10 |
    | rules with captures | 5 | 8 |
    | single-character rules with before contexts matched against the converted text | 5 | 5 |
    | a transform applied by its shape groups (am-am_FONIPA) | 3 | 3 |
    | single-character rules with before contexts that are not sequences of sets | 3 | 3 |
    | insertion rules | 2 | 2 |
    | deferred rules | 1 | 1 |
    | with `--lines`: a bounded DivCode group (step 15) | 65 | |

    (Own steps only: the segments of the transforms a transform invokes are
    counted under those, so the table is a lower bound; 66 have segments of
    their own, 183 in all with `--lines`.)

    Where a segment's time goes: el-Latn-t-el-m0-bgn (one segment, a group of
    string rules; 2 MB of Greek, one thread, cycles by kernel):

    | Part | Share |
    |---|--:|
    | decoding: `UTF8_Decoder`, and the 21-bit basis filtered to one position per character | 7.7% |
    | encoding: deposit masks and assembly | 6% |
    | applying the replacements on 21 streams (expand, deposit, fill) | 15% |
    | the fused kernel's matching and selection (`FusedRules`) | 30% |

    The text has 2.2 M bytes and 1.2 M characters. So the switches cost about
    14%, but the same kernels run over the bytes would do about 1.8 times the
    work for this script: a direct port of the fused kernels to UTF-8 code
    units (option B below) would be slower than the switches for non-Latin
    scripts, and faster only for mostly-ASCII text.

    Options:
    - **A. DivCode for each problem** (overlaps, runs, kept characters,
      captures, converted contexts): stays on the bytes, but each problem is
      a new feature of a decision-tree design, and runs and captures fit it
      poorly.
    - **B. The fused kernels on UTF-8 code units** (positions are bytes,
      characters stepped by `IndexedAdvance` over `UTF8_index`): one port
      covers every problem the fused kernels handle, at a cost proportional
      to the bytes (see above), and their outputs (changes as XOR against the
      21 bits of each position) become per byte.
    - **C. Decisions in character space, text in byte space.** The matching
      and selection run as now, one position per character, but on the class
      streams they need (computed on the bytes, marked at each character's
      final byte, and compressed by `UTF8_index`: a few streams instead of
      the 21-bit basis), and their result is a selector per position (which
      rule, which replacement applies), spread back to the bytes, where the
      replacements are written as UTF-8 (as the UTF-8 character maps and
      DivCode write theirs). No decoding of the text and no encoding; the
      analyses stay as they are. The fused kernels need a selector output
      instead of XOR changes, and components that move text (captures,
      revisits) need byte-level moves of variable-length characters.

    Recommended: C, as one mechanism for all components, adopted component
    by component (string-rule groups first, then several subgroups, prefix
    runs, converted before contexts, insertions, captures, deferred rules,
    shape groups). **Chosen (2026-11-20): C.**

    **Design of C.** The Unicode-mode group builder
    (`RulePipelineBuilder::conversionGroup`) reads the text through a few
    helpers, which become those of a *character source*: the 21-bit text
    (Unicode mode), or the UTF-8 bytes with `UTF8_index` (the hybrid).
    - `classes(sets, …)`: on the bytes, the class kernel in Advance mode
      (each character marked at its final byte), then compressed by
      `UTF8_index` to one position per character. One stream per class,
      instead of the 21-bit basis.
    - The engine's matches (`matchPositions`, `matchStarts`,
      `matchPattern`): the engine on the bytes (its UTF-8 modes), its marks at
      the final bytes, compressed likewise. The masks (filters, regions) are
      compressed as for a Unicode segment (`unicodeMasks`).
    - `TextPositions`: the compressed index (all ones, one per character).
    - The fused kernels write *selectors* instead of changes as XOR against
      the 21 bits (`FusedRulesConfig::absolute`): before the kernels are
      generated, each rule's replacement is rewritten to one selector per
      position of its match, the id of the UTF-8 string that position becomes
      (the part of the replacement the position writes, as the kernels divide
      a replacement among the positions: the last position takes the rest of
      a longer one, and the positions beyond a shorter one become empty); a
      position the rule keeps (its text unchanged) has no selector. So the
      kernels insert and delete no positions, and read no basis.
    - The application, on the bytes (a selector variant of
      `UTF8_CharMapApply`): at the first byte of each character with a
      selector, the first byte of its string is assigned (not XORed, so it
      does not depend on the character replaced), the character's other bytes
      are deleted, and the rest of the string is inserted after it, by the
      tails machinery of `UTF8_CharMapPipeline`; an empty string deletes the
      first byte too. The selectors are spread from the characters to their
      first bytes by the complement of the continuation bytes.

    Slices: (1) string-rule groups of the fused kernels without correction
    kernels, captures, insertion rules or case-map marks; (2) correction
    kernels (the converted text in selectors: the classes of a selector's
    string are static); (3) captures (bytes moved by character); (4) the
    other components. Each slice is checked against Unicode mode on the CLDR
    test data of the transforms it moves.

    *Done (2026-11-20), behind `--hybrid-groups` (then off by default):* slices 1
    and 2. Slice 2: the correction kernels (staged rounds and the While loop)
    read the converted text from the selectors (`FusedRulesConfig::
    selectorStrings`: the length and the classes of each slot are classes of
    the selector values), and groups of single-character rules only are taken
    too. `--plan --hybrid-groups` gives the hybrid's reason where it does not
    apply.

    *Then:* sequential selection (the match-only kernels, `LeftmostMatches`
    and the staged rounds needed nothing new), and string rules with runs (the
    run's deletion written as the selector of the empty string,
    `FusedRulesConfig::deletionSelector`). The selector application and the
    tails of the UTF-8 character maps compile each output bit as a class over
    the numbers (selectors, tails) instead of a test of each number, and the
    hybrid compresses its classes in chunks of 32 streams: a group of 7718
    string rules (am-Ethi-t-am-ethi-m0-geminate) had made both kernels too
    large to compile in minutes. Its cold compile is still 100 s against
    52 s in Unicode mode (the fused kernels' absolute writes are 1.4 times
    larger: every bit of a selector rather than the bits that differ from a
    known character); once, then cached.

    On the CLDR test transforms the hybrid now removes 43 of the 67 Unicode
    segments of string-rule groups and all 5 of single-character rules with
    converted before contexts: 34 transforms lose all such segments. Outputs
    equal Unicode mode's on the CLDR data of 26 of them, as a text and with
    `--lines` (`QA/xlit_hybrid_test.py` has small cases). What still keeps
    string-rule groups out (segments): captures (14), contexts with xlit's
    marks (5), a kept character with inserted ones (4), insertion rules (1).

    *Slice 3, partly (2026-11-21):* the MultipleCaptureMultipleReplacement
    rules whose captures stay in place (not reversed, not moved). Their
    replacements are selectors like those of string rules; a captured
    character kept with text inserted around it is the selector of a string
    with `kernel::UTF8_SelectorKept` (xlit's `StringRuleKept`) at the place
    of the character: its bytes are kept, the text after the mark inserted
    after its last byte (as a tail), and the text before the mark inserted
    before its first byte by a second spread (`InsertPosition::Before`, only
    where such a prefix occurs; the bytes inserted have the character's
    masks). String rules with a kept character before inserted ones are the
    same selectors. The correction kernels read a kept slot as the character
    itself. The engine gives the hybrid the spans of matches, and the starts
    of patterns other than sequences of sets, on the bytes, compressed to the
    characters. This found an engine bug: `LongestSpan` (unique-prefix spans)
    dropped the end of a match marked after it (a lookahead) where the next
    match's prefix began, so that the spans of adjacent matches lost the
    first character of the second (fixed).

    The engine's spans were partial where no span algorithm applied to an
    alternative of the pattern (the shortest matches ending at each end), as
    for a capture set of cy-fonipa-t-cy on the bytes; planning now reports
    such patterns for the MatchSpans goal, unless `RE_ModeOptions::
    partialSpans` accepts them (icgrep's coloring does), and the hybrid leaves
    such capture sets to Unicode mode (none of the CLDR transforms needs
    them in Unicode mode). In the UTF-8 indexed mode the engine's match marks
    after a lookahead are a character, not a byte, after the match (the
    hybrid and the UTF-8 character maps with contexts shift them back by
    characters), and its spans are completed to whole characters.

    The hybrid now removes all the Unicode segments of string-rule groups of
    39 CLDR test transforms (from 34). What keeps the rest out (segments):
    contexts with xlit's marks (5), capture sets whose spans the engine
    cannot match on the bytes (3), captures reversed (3) and moved (2), the
    rest of "bytes moved by character", and insertion rules (1).

    *Multiplexed classes, and on by default (2026-11-21).* The hybrid
    compressed one stream for each class of a fused kernel (in chunks of 32,
    padded), which cost about as much as the Unicode segment it replaced
    (es-fonipa-t-es: 23% of the cycles, against 29% for the segment). The
    classes are now partitioned into their exclusive sets
    (`cc::MultiplexedAlphabet`), only the bits of their codes are computed on
    the bytes and compressed, and each fused kernel compiles its classes from
    the code bits (`FusedRulesConfig::classCodes`): class compression fell to
    0.8% of es-fonipa-t-es's cycles. `--hybrid-groups` is now on by default
    (`--hybrid-groups=0` for the Unicode segments). Found by the tests with it
    on: a rule that replaces a character by itself writes no change in Unicode
    mode (an XOR of 0), but wrote a selector in the hybrid, which another
    rule's selector at the same position combined with (by OR): the string
    rule `[^﷯] { ့ ့* → ့ ;`, its before context matched against the text
    itself, and a subgroup deleting `့` after `﷯` in the converted text
    (`xlit_split_runs_test`). Such a replacement is now kept (no selector):
    a string rule's character of a single character replaced by itself
    (`selectorReplacement`), and a character mapped to itself (the selector
    0, `selectorMappings`).

Order: steps 10 to 12 first (the per-line runs removed, in Unicode mode, which
every component already has); then 14 and 13 (mixed modes); then 15 and 16,
which move work from Unicode segments to UTF-8 segments.

## Outstanding issues (2026-11-21)

**Step 16, UTF-8 mode for every component.**
- The hybrid (`--hybrid-groups`) is on by default since 2026-11-21. Its
  fused kernels are larger than Unicode mode's for groups with many rules
  (selectors written whole): the cold compile of the largest group
  (am-Ethi-t-am-ethi-m0-geminate) was 100 s against 52 s before the classes
  were multiplexed; not measured since.
- String-rule groups the hybrid still leaves to Unicode mode (segments, CLDR
  test transforms): contexts with xlit's marks (5); capture sets whose spans
  the engine cannot give exactly on the bytes (3, cy-fonipa-t-cy); captures
  reversed (3) and moved (2), the rest of slice 3 (bytes moved by character);
  insertion rules (1).
- Compile time of very large groups: am-Ethi-t-am-ethi-m0-geminate (7718
  string rules in one group) takes 100 s cold against 52 s in Unicode mode;
  the fused kernels' absolute writes are about 1.4 times larger.
- Slice 4 (the other components in UTF-8 mode) not started: characters of
  several subgroups (10 segments), prefix runs (10), single-character rules
  with before contexts matched against the converted text (5), shape groups
  (am-am_FONIPA, 3), before contexts that are not sequences of sets (3),
  insertion rules (2), deferred rules (1).
- Bounded DivCode groups with `--lines` (65 transforms, left from step 15).

**Remaining items of the earlier steps.**
- Step 5: adjacent regions are not separated by the engine (region spans,
  the lookahead chain kernels and region reversal).
- Step 7: no run ends in UTF-8 mode (only am_Ethi-d0_Morse uses them, in
  Unicode mode for its insertion rules); DivCode complements the mask itself
  instead of taking the run ends.
- Step 8: function calls by masks, not started.
- Step 9: block skipping in the NFC stages and the masked case maps; narrower
  masks for the case maps' expanding slots.
- Step 11: Thai-Latn (ICU stages under a global filter) still runs its xlit
  stages once per run of the filter.
- Step 12: 2 of the 290 test files differ from their expected results, both
  issues of the test data (ka-Latn-t-ka-m0-bgn-2009 and my-fonipa-t-my, see
  doc/ICU-uconv-CLDR-issues.md, CLDR). The six whose names xlit did not know
  (BCP 47 IDs with aliases of registered values, and one malformed) are found
  since 2026-11-21 and equal their expected results.

**Found on the way, not yet investigated.**
- The pipeline's handling of a block-sized lookahead at the final stride
  (step 13: `FixedMatchSpansKernel` lost the spans of the final partial
  stride until it declared its actual lookahead).
- A constant scalar (`CreateConstant`) given to a pipeline call crashes the
  pipeline compiler (`PipelineCompiler::getFinalOutputScalars`; step 14).
- An anomaly of the combinatorial tester (859c10cfc0): am-t-am-fonipa in
  Unicode mode, the filter-half input, `-segment-size=1`.
- The combinatorial tester does not vary `--hybrid-groups`.

The open questions below remain: ICU's run semantics under filters, the
position map, and where `TransformMask` lives.

## Testing

- **`--lines`:** the single-run output must equal the per-line output on the CLDR
  test data of each transform the step enables, and ICU's output.
- **Region boundaries:** rules files with contexts that would match across a line
  boundary, both edges, empty lines, and lines ending without LF.
- **Filters:** the existing filter tests (`xlit_filter_run_end_test`, the
  DivCode filter cases) unchanged at each step.
- **In place and threads:** `-VerifyIR`, `-thread-num=4 -segment-size=1`
  (`inout_thread_local` covers the pipeline's in-place handling).
- **Performance:** execution cycles on 10 MB inputs (the Wikibooks texts) and
  run time on the test data with `--lines`, against the current mechanisms.

## Open questions

- **ICU's run semantics under filters.** Contexts see beyond a run, anchors are
  the text's, and the characters inserted belong to the run they follow
  (ldml-xlit-coverage.md). Are there further cases, such as a run end within a
  multi-character match, that text scope must reproduce?
- **The position map.** A pair of insertion and deletion masks, or one
  spread-and-filter description, so that a chain of transforms composes their
  maps once instead of each caller re-spreading its streams?
- **Adjacent arguments** (step 8). The arguments of the calls of one function
  are the regions of one mask, and two of them may touch: the arguments of
  adjacent matches (`(.) → &Any-Lower($1) ;`, where each character is a text
  of its own, so that a Σ lowercases to ς), or two arguments of one rule
  (`(a)(b) → &T($1) &T($2) ;`). Without a position outside the mask between
  them they would be one region, and the function's contexts would see across.
  (Arguments of different functions are in different masks.) Resolved by the
  mechanism of region scope above: a position (a line feed, outside the mask)
  is inserted before each argument that starts right after another, and
  deleted afterwards, rather than a pass for each argument.
- **Where the type lives.** `TransformMask` in the library (with kernel support
  for region scope) or in xlit's pipeline builder until a second tool needs it?
- **UTF-8 mode.** Mask conventions per code unit, and how they combine with the
  UTF-8 class compilers' Advance and LookAhead marking. Resolved by step 14: all
  the bytes of a character have its bit, and `--check-utf8-masks` checks it.
