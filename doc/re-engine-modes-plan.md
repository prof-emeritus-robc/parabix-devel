# Regular expression engine modes: MatchStarts and SearchAndReplace (plan)

Written after the engine refactoring that made RE preparation its first step
(7961b9fdd), match spans as the union of match extents (1c78923b0), lookahead
chains computed in `matchStartPipeline` (3137e5e04) and exact spans by length
classes (5a2cc5a8f). Code references are to `Rework_RE` at 4f721235f.

## Goal

Give the regular expression engine (`RE_PipelineBuilder`,
`lib/kernel/re/regexp_engine.cpp`) two new public modes beside the existing two:

| Mode | Output | Today |
|---|---|---|
| **Search** (match ends) | a mark at (or `grepOffset` after) the last code unit of every match | `matchSearchPipeline` |
| **Span** | the union of the positions of all nonempty matches | `matchSpanPipeline` |
| **MatchStarts** *(new)* | a mark at the first position of every match | internal only: `matchStartPipeline`, used for lookahead externals |
| **SearchAndReplace** *(new)* | the text with the selected (leftmost, non-overlapping) matches replaced by a replacement pattern | hand-built in tconv |

and classify every RE analysis and transformation by the mode(s), and the role
(search pattern / replacement pattern), for which it is valid and useful, so that
each mode runs exactly the passes that preserve what it computes.

tconv is the first client of both: its `RulePipelineBuilder::matchStarts` and
`matchPattern` (`tools/ldml/tconv.cpp:4709-4767`) already emulate the two modes
with the existing ones, and its conversion groups are a search-and-replace
engine specialised to LDML rules.

## How the engine works today

### Pipeline of an RE

1. **Prepare** (`prepareRE`, `regexp_engine.cpp:971`)
   - `resolveModesAndExternalSymbols` (`lib/re/unicode/regex_passes.cpp`):
     `resolveEscapeNames`, `resolveGraphemeMode`, `UCD::linkAndResolve`,
     `removeUnneededCaptures`, `inlineSimpleProperties`, `validateNamesDefined`,
     `resolveRanges`, [U2: `toNFD`, `addClusterMatches`,
     `addEquivalentCodepoints`], `resolveCaseInsensitiveMode`,
     `resolvePossessiveQuantifiers`.
   - `regular_expression_passes`: `standardizeAssertions`,
     `convertToStarNormalForm`, `minimizeRE` (or `simplifyRE` at -O0/-O1),
     `resolveDiffs`.
   - `determineREMode`: byte (UTF-8 length and index), UTF-8 indexed (Unicode
     length over UTF-8 code units with `u8index`) or full Unicode (21-bit basis,
     one position per character). Driven by `validateFixedUTF8`,
     `hasPropertyReference`, `getLengthRange`, `hasAssertion`,
     `hasLookaheadChain`, `ByteModeLookaheads`, `hasReference`,
     `hasGraphemeClusterBoundary`, `maxLookaheadLength`.
2. **Source preparation** (`ensurePrepared`, :1094): the text is made one match
   region if the RE refers to its end anywhere (`hasEndAnchorAnywhere`), and
   the basis, `u8index` and the Unicode basis and regions are built for the mode.
3. **Mode passes** (`applyModePasses`, :2076): `expandPermutes`, the RE passes
   again for the length alphabet, `PropertyExternalizer`,
   `VariableLengthCCNamer` + `toUTF8` (UTF-8 code units), `StringClassRepNamer`,
   `LookAheadNamer`. Then `processReferences` (`FixedReferenceTransformer`) and
   `prepareExternals`.
4. **Compile**: `RE_Kernel` → `RE_Compiler` (`lib/re/compile/re_compiler.cpp`),
   forward marker propagation; markers sit at the last code unit of a match
   (`Position::AtEnd`) or one past it.
5. **Span mode only**: `spanFactoring` (`StringClassRep` substitution,
   `emptyMatchElimination`, `BoundedRepExpander`, `variableAltPromotion`,
   `FixedSpanNamer`) and `getSpan` per alternative: `FixedMatchSpansKernel`
   for named fixed spans, `chainSpans` (`ChainCoverage`), `uniquePrefixSpans`
   (`ParseUniquePrefix`, `LongestSpan`), `lengthClassSpans` (`lengthClasses`),
   else a minimum-length approximation.

### Where match starts already exist

The compiler only propagates forward, so **match ends are native and match
starts are not.** Lookbehinds are cheap (`compileAssertion` compiles the body
everywhere and ANDs it in); lookaheads of more than one character need the
*starts* of the body, which `matchStartPipeline` (:1323) provides for three
shapes only:

- fixed length: ends, offset by the length;
- `Seq[P, S]` with a named unique prefix `P`: ends shifted back along
  `P | ends` (`uniquePrefixEndsBack`) and ANDed with `P`;
- a lookahead chain (`parseLookaheadChain`): `chainMatchStarts`, right to left,
  Unicode code units only;

and calls `report_fatal_error` otherwise. Its result is the start *plus* an
offset (`s + offset - 1`), the convention of `StartIndexed` externals.

The key duality: **the starts of the matches of R are the positions where
`(?=R)` holds**, just as the ends are where `(?<=R)` holds. Everything that
makes lookbehinds easy for the search mode makes lookaheads easy for the
starts mode, and vice versa.

### What tconv does with the engine

- `matchStarts(before, after, text)`: compiles
  `(?<=before) (?= text after)` in search mode, i.e. a starts query disguised as
  a zero-width search.
- `matchPattern(before, after, items, &spans)`: compiles
  `(?<=before) items (?=after)` in span mode, then shifts the ends back by
  `grepOffset`; `MatchStartsFromEnds` (`tconv.cpp:837`) derives starts from spans
  and ends when no match can end within another (`endsUnique`).
- Selection and replacement are its own kernels: `SelectStringMatches`,
  `MatchStartsByLength`, `LeftmostMatches` (sequential leftmost selection, a
  `MultiBlockKernel`), `SpanContinuation`/`SelectedSpans`, `CaptureWalk`
  (capture boundaries from starts), `InsertionCounts`, `FusedRules`, the
  `Deferred*` kernels, plus deletion/spread/pdep from `kernel/streamutils`.
- Captures are prepared by `lib/ldml/transform_rules_captures.cpp`, which uses
  the two-RE form `removeUnneededCaptures(capturingRE, referencingRE)`.

## Classification of analyses and transformations

Columns:

- **Ends**: search mode (match ends; grep's matched lines are a coarser use of it).
- **Span**: span mode (coverage of nonempty matches).
- **Starts**: MatchStarts mode.
- **S&R‑S**: the search pattern of SearchAndReplace (needs starts, the selected
  matches' exact ends, and the boundaries of referenced captures; empty matches
  count).
- **S&R‑R**: the replacement pattern (literals, capture references, functions).

✔ valid and used · ✔\* valid with the stated restriction · ✘ invalid (changes
the answer) · — not applicable · ★ new use proposed by this plan.

### Front end: modes and external symbols

| Pass | Ends | Span | Starts | S&R‑S | S&R‑R | Notes |
|---|---|---|---|---|---|---|
| `resolveEscapeNames` | ✔ | ✔ | ✔ | ✔ | ✔★ | replacement literals may use `\n`, `\x{..}`, `\N{..}` |
| `resolveGraphemeMode` | ✔ | ✔ | ✔ | ✔ | — | |
| `UCD::linkAndResolve`, `inlineSimpleProperties`, `PropertyExternalizer` | ✔ | ✔ | ✔ | ✔ | — | |
| `validateNamesDefined`, `resolveRanges`, `resolveDiffs`, `expandPermutes` | ✔ | ✔ | ✔ | ✔ | ✔ (names) | |
| `removeUnneededCaptures(r)` | ✔ | ✔ | ✔ | ✘ | — | drops captures the replacement references |
| `removeUnneededCaptures(r, repl)` | — | — | — | ✔★ | — | keep captures referenced by back-references *or* the replacement |
| U2: `toNFD`, `addClusterMatches`, `addEquivalentCodepoints` | ✔ | ✔ | ✔ | ✔\* | — | captures must survive the rewriting (verify); a referenced capture then copies the text's form |
| `resolveCaseInsensitiveMode` | ✔ | ✔ | ✔ | ✔ | ✘ | `-i` never folds the replacement |
| `resolvePossessiveQuantifiers` (`X*+` → `X*(?!X)`) | ✔ | ✔ | ✔ | ✔ | — | produces lookaheads: cheap in Starts, external in Ends |

### Simplification (`regular_expression_passes`)

| Pass | Ends | Span | Starts | S&R‑S | Notes |
|---|---|---|---|---|---|
| `standardizeAssertions` | ✔ | ✔ | ✔ | ✔ | |
| `convertToStarNormalForm` | ✔ | ✔ | ✔ | ✔\* | language-preserving; changes which substring a capture under a star binds, and alternation order. Apply only to capture-free subtrees; never under leftmost-first (PCRE) semantics |
| `minimizeRE` / `simplifyRE` | ✔ | ✔ | ✔ | ✔\* | same restriction: captures are barriers; factoring must not reorder alternatives under leftmost-first |
| `resolveDiffs` | ✔ | ✔ | ✔ | ✔ | |

### Search-only reductions (not used today)

| Pass | Ends | Span | Starts | S&R‑S | Notes |
|---|---|---|---|---|---|
| `removeNullablePrefix` (`a*b` → `b`) | ✔★ | ✘ | ✘ | ✘ | the set of match ends is unchanged |
| `removeNullableSuffix` (`ab*` → `a`) | ✘ (line-match ✔) | ✘ | ✔★ | ✘ | dual: the set of match starts is unchanged; for grep's matched lines it is valid too |
| `zeroBoundElimination` | ✔ | ✔ | ✔ | ✘ | `x{0}` matters only for captures |
| `emptyMatchElimination` | ✘ | ✔ (spans) | ✘ | ✘ | empty matches have no span, but they are matches (`s/x*/-/g` inserts) |
| leading lookbehind → filter | native | ✔ (`chainSpans`) | ✔★ | ✔★ | `(?<=B)R`: compile `B` everywhere, AND with starts of `R` |
| trailing lookahead → body | external | ✔ | ✔★ (native) | ✔★ | `R(?=C)`: starts of `RC` |

### Mode determination and code units

| Analysis | Ends | Span | Starts | S&R‑S | S&R‑R |
|---|---|---|---|---|---|
| `determineREMode` (byte / UTF-8 indexed / Unicode) | ✔ | ✔ (coloring sets `unicodeBasisOverride`) | ✔\* fixed length and unique prefix in any mode; chains force Unicode code units | ✔\* as Starts, plus the output encoding | output is UTF-8 or U21 |
| `getLengthRange`, `grepOffset` | ✔ | ✔ | ✔ (offset convention differs) | ✔ | ✔★ replacement length, with capture lengths |
| `hasEndAnchorAnywhere` → one region (`ExtendByOne`, …) | ✔ | ✔ | ✔ | ✔ | — |
| `validateFixedUTF8`, `ByteModeLookaheads`, `byteTestsWithinLimit`, `maxLookaheadLength`, `hasReference`, `hasGraphemeClusterBoundary` | ✔ | ✔ | ✔ | ✔ | — |
| `VariableLengthCCNamer` + `toUTF8` | ✔ | ✔ | ✔ | ✔ | ✔★ literals encoded to the output code units |
| `StringClassRepNamer` | ✔ | ✔ (substituted back) | ✔ (chain string-class stars) | ✔ | — |
| `LookAheadNamer` | ✔ | ✔ | ✔\* the top-level lookahead *is* the query | ✔ | — |
| `processReferences` / `FixedReferenceTransformer` | ✔ | ✔ | ✔\* via ends (references point backward) | ✔ | — |
| `collectCCs` / multiplexing (disabled) | ✔ | ✔ | ✔ | ✔ | — |

### Span machinery

| Analysis / transform | Ends | Span | Starts | S&R‑S | Notes |
|---|---|---|---|---|---|
| `spanFactoring`: `BoundedRepExpander`, `variableAltPromotion`, `FixedSpanNamer` | — | ✔ | ✔★ | ✔★ | split an RE into alternatives each with a starts strategy |
| `ParseUniquePrefix` / `UniquePrefixNamer` / `uniquePrefixEndsBack` / `LongestSpan` | — | ✔ | ✔ | ✔ (longest end per start) | |
| `parseLookaheadChain` / `chainMatchStarts` / `ChainCoverage` | — | ✔ | ✔ | ✔ | |
| `lengthClasses` | — | ✔ | ✔★ | ✔★ (exact end per class) | each class is fixed length: starts = ends − (k − 1) |
| `parseStringClass`, `isRepeatableStringClass`, `firstCharClass`, `CC_Sequence_Search` | ✔ | ✔ | ✔ | ✔ (overlap analysis) | |
| `reverse` (`re_reverse.cpp`, unused) | — | — | ✔\* | — | only as a last-resort fallback over a reversed stream; not planned |

### Replacement pattern (S&R‑R only)

| Analysis / transform | Purpose |
|---|---|
| parse (new): `$n`, `${name}`, `\n`, `$&`, case escapes; LDML result side via `ldml::RuleSide` | an RE of literal CCs, `Reference`s and function `Name`s (`&Any-Hex($1)` as in `transform_rules.h`) |
| reference validation | every reference names a capture of the search pattern; feeds `removeUnneededCaptures(search, repl)` |
| literal folding | adjacent single-codepoint CCs → one string; a reference to a capture of one fixed string → that literal |
| length analysis | `getLengthRange` with references sized by their captures: fixed ⇒ a per-match constant insertion/deletion count |
| function resolution | known transforms (`Upper`, `Lower`, `Hex`, …) on a capture become a kernel applied to the capture's span |
| `toUTF8` of literals | when the output is UTF-8 |
| cursor / revisit | LDML only: stays in tconv (the rescanning it implies is outside the engine) |

## Design

### 1. A goal for prepared REs

Add a goal to `RE_ModeOptions` (or `PreparedRE`):

```cpp
enum class RE_Goal {MatchEnds, MatchSpans, MatchStarts, SearchReplace};
```

`prepareRE` passes it to `resolveModesAndExternalSymbols` and
`regular_expression_passes`, which choose the passes by the tables above. For
`SearchReplace`, `prepareRE` also takes the replacement RE, so that capture
removal sees it. The existing `matchSearchPipeline`/`matchSpanPipeline`
callers keep the default (`MatchEnds`) and see no change.

The passes become a small table of `{name, function, applicable goals}` in
`regex_passes.cpp`, so that `-ShowREs` can name the goal and the skipped passes.

### 2. MatchStarts mode

Public API, beside the two existing pipelines:

```cpp
void matchStartsPipeline(const PreparedRE & re, kernel::StreamSet * starts);
```

Convention: a mark at the first position of each match, in the code unit space
of the context (the first code unit of its first character in the UTF-8 modes;
for an empty match, the position it is at). The internal `MatchStarts {stream,
offset}` stays for externals; the public result is shifted to offset 0
(`ShiftBack` by `offset - 1`, or the shift folded into the last kernel).

Strategy order, per alternative of the factored RE (reusing `spanFactoring`'s
alternative splitting, without `emptyMatchElimination`):

1. strip a leading lookbehind (filter, compiled everywhere) and fold a
   trailing lookahead into the body;
2. apply `removeNullableSuffix` (valid for starts);
3. fixed length → ends shifted back (exists);
4. unique prefix → `uniquePrefixEndsBack` (exists);
5. **bounded RE → `lengthClasses`**: each class's ends shifted back by its
   length, OR-combined (new; mirrors `lengthClassSpans`);
6. lookahead chain → `chainMatchStarts` (exists; forces Unicode code units,
   which `determineREMode` must know: add `hasLookaheadChain`-style analysis on
   the *top-level* RE for this goal);
7. otherwise: `MatchStartsFromEnds` from spans and ends when ends are unique
   (moved from tconv; the analysis is the `endsUnique` test of
   `lib/ldml/transform_plan.cpp:471`, to be generalised with
   `CC_Sequence_Search`), else a fatal "unsupported" error as today.

Tests (sub-second ctests, one test binary, inputs in `QA/`): each strategy,
UTF-8 and Unicode code units, empty matches, matches touching region follows,
anchored REs, and agreement with a brute-force reference implementation on
small random texts.

### 3. SearchAndReplace mode

Semantics to fix first (options on the plan object): global vs first-only;
**leftmost-longest** (POSIX, and what tconv's string rules need given rule
priority) vs **leftmost-first** (PCRE alternation order, which constrains the
simplification passes, see above); empty matches (allowed, not adjacent to the
previous match, as Perl/ICU).

Pipeline stages, each a reusable piece:

1. **Candidates**: MatchStarts of the search pattern.
2. **Ends per start**: the longest end of a match from each start. Exact by
   construction for the fixed-length, length-class and unique-prefix
   (`LongestSpan`) strategies, and for chains (`ChainCoverage` gives the
   reach); in general by compiling the RE from the start markers
   (`RE_Compiler::compileRE(re, initialMarkers)` already exists) — exact when
   no match can begin within another.
3. **Selection**: if the overlap analysis (`CC_Sequence_Search`, tconv's
   `RuleOverlapAnalysis::mayMatchWithin`) shows no match can begin strictly
   within another, every candidate is selected — fully bit-parallel. Otherwise
   a sequential leftmost scan: generalise `LeftmostMatches` (fixed lengths and
   runs today) to take ends.
4. **Capture boundaries** for referenced captures: for `A (B) C`, the start of
   `B` is the end of `A` from the selected starts, and its end the start of `C`
   back from the selected ends; exact when `A` is fixed length or `B` is
   possessive (tconv's `CaptureWalk` handles fixed texts between captures).
   Unsupported capture shapes are reported, as tconv does now.
5. **Replacement assembly**: deletion mask of the matched text not kept,
   insertion counts (`InsertionCounts`), spreading (`SpreadByMask`), then the
   literal bytes/codepoints and the copied captures written into the inserted
   positions (generalising `InsertedTexts` / `FillInsertedPositions`).
   Fixed-length replacements use one constant count per selected match; capture
   copies use the capture span lengths.

API sketch:

```cpp
struct SearchReplaceSpec {
    re::RE * search;
    re::RE * replacement;      // literals, References, function Names
    bool global = true;
    enum class Semantics {LeftmostLongest, LeftmostFirst} semantics;
};
PreparedSearchReplace prepareSearchReplace(const SearchReplaceSpec &, const RE_ModeOptions &);
void searchReplacePipeline(const PreparedSearchReplace &, kernel::StreamSet * output);
```

The kernels that tconv already uses for this move from `tools/ldml/tconv.cpp`
to `lib/kernel/re/` (selection, capture walk) and `lib/kernel/streamutils/`
(insertion counts, filling), with tconv calling them from there.

## Milestones

1. **Goal plumbing**: `RE_Goal`, pass table, goal-aware
   `removeUnneededCaptures`; no behaviour change for Ends/Span (check icgrep
   ctests and `-ShowREs` output unchanged).
2. **MatchStarts public mode** with strategies 1–4 and 7; move
   `MatchStartsFromEnds`; tests.
3. **Length-class starts** (strategy 5) and `removeNullableSuffix`; extend
   `determineREMode` for chains as the top-level query.
4. **tconv on MatchStarts**: replace `RulePipelineBuilder::matchStarts` (the
   `(?<=B)(?=T A)` trick) and the starts half of `matchPattern`; compare outputs
   on the CLDR test cases (`tconv_coverage.py`) — no change expected.
5. **Replacement patterns**: parser for `$n`/`${name}`/`\n`, analyses
   (validation, length, literal folding); the LDML result side mapped to the
   same representation.
6. **S&R, non-overlapping fragment**: literal replacements, all candidates
   selected, fixed or length-class matches; a small `resub`-style tool (or an
   icgrep `--replace` option) to exercise it.
7. **S&R, captures and leftmost selection**: capture boundaries, generalised
   `LeftmostMatches`, empty-match rules.
8. **tconv on S&R**: string rules and MultipleCaptureMultipleReplacement sets
   through the engine; tconv keeps rule ordering, filters, cursors and deferred
   rules.
9. Optional: `removeNullablePrefix` for the Ends mode (a search speedup, separate
   from the new modes).

## Open questions

- Should the public starts convention be offset 0 at the first code unit, or
  the existing `StartIndexed` convention (`s + offset - 1`) to avoid a shift?
- Leftmost-first semantics: is PCRE compatibility needed, or is
  leftmost-longest (bit-parallel friendly) enough for the target clients?
- For UTF-8-indexed mode, do starts need `u8index`-relative marks (first byte)
  or is it acceptable to force full Unicode indexing whenever starts are
  variable length (as chains already do)?
- How far should general (non-unique-ends, unbounded, non-chain) REs be
  supported in MatchStarts before falling back to an error?
