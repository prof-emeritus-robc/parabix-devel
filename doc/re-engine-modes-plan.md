# Regular expression engine modes: MatchStarts and SearchAndReplace (plan)

Written after the engine refactoring that made RE preparation its first step
(7961b9fdd), match spans as the union of match extents (1c78923b0), lookahead
chains computed in `matchStartPipeline` (3137e5e04) and exact spans by length
classes (5a2cc5a8f). Code references are to `Rework_RE` at 4f721235f.

Revision 2 (after review of f10feda56):

- captures and search and replace are limited to **unambiguous** REs, whose
  results do not depend on matching order (lazy, greedy, leftmost-longest,
  leftmost-first); other semantics are later options;
- the new modes use byte mode or **full Unicode indexing**, never the UTF-8
  indexed mode, which is a later optimization;
- each mode has **preconditions**, checked before any kernel is built: an RE
  outside the engine's current capabilities is reported as an error, and later
  phases relax the preconditions;
- **LDML conventions** (functions, `[$]` and the special treatment of `$`,
  cursors, possessive contexts) are not Parabix RE semantics: tconv translates
  them;
- a section on **general lookahead by reversing bit stream segments within
  region boundaries**.

## Goal

Give the regular expression engine (`RE_PipelineBuilder`,
`lib/kernel/re/regexp_engine.cpp`) two new public modes beside the existing two:

| Mode | Output | Today |
|---|---|---|
| **Search** (match ends) | a mark at (or `grepOffset` after) the last code unit of every match | `matchSearchPipeline` |
| **Span** | the union of the positions of all nonempty matches | `matchSpanPipeline` |
| **MatchStarts** *(new)* | a mark at the first position of every match | internal only: `matchStartPipeline`, used for lookahead externals |
| **SearchAndReplace** *(new)* | the text with the matches of an unambiguous search pattern replaced by a replacement pattern (literals and capture references) | hand-built in tconv |

and classify every RE analysis and transformation by the mode(s), and the role
(search pattern / replacement pattern), for which it is valid and useful, so that
each mode runs exactly the passes that preserve what it computes, after checking
the preconditions that make the RE implementable in that mode.

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

Unsupported constructs are found late today: `compileExternal` and
`matchStartPipeline` call `report_fatal_error` while the pipeline is being
built (e.g. "Unsupported lookahead assertion").

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
offset (`s + offset - 1`) along the index stream, the convention of
`StartIndexed` externals.

The key duality: **the starts of the matches of R are the positions where
`(?=R)` holds**, just as the ends are where `(?<=R)` holds. Everything that
makes lookbehinds easy for the search mode makes lookaheads easy for the
starts mode, and vice versa. The section on reversal below exploits this
directly.

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

## Principles for the new modes

### Unambiguous search and replace

Regex engines differ in which matches a search-and-replace selects and what
its captures bind: greedy vs lazy repetition, leftmost-longest (POSIX) vs
leftmost-first (PCRE alternation order), the treatment of empty matches, and
the binding of captures that repeat or are not taken. The first phases support
only REs for which **all of these agree**. Matches are always selected from the
left, without overlap (every convention does this); what must not vary is
everything else:

- **U1, unique end**: from each start there is at most one match end, i.e. no
  match (in its context) is a proper prefix of another match from the same
  position. `a+` fails (`aaa` is one match greedily, three lazily);
  `a+(?!a)`, `a++`, `[a-z]+@` and fixed-length REs satisfy it.
- **U2, nonempty**: the RE is not nullable, so there are no empty matches,
  whose treatment varies (after a nonempty match, at the end of the text).
- **U3, unique captures**: in every match, each referenced capture has one
  binding: its boundaries are determined by the match. Captures under a
  repetition with an upper bound above 1 (which iteration?) or under an
  optional or alternative path (unset group: empty or error?) are excluded.
  `(a*)(a*)` fails; `([a-z]+)@([a-z]+)\.` satisfies it.

Under U1–U3 every semantics gives the same result, so the engine is free to
use any language-preserving transformation outside captures (no constraint
from alternation order), and selection is "every match whose start is not
within an earlier selected match".

Sufficient, checkable tests (precondition analyses, below):

- U1: fixed length; length classes in which no string of a shorter class is a
  prefix of a string of a longer one (a CC-sequence test between classes);
  chains (`parseLookaheadChain` already requires every star run to be maximal)
  ending with a fixed segment, `End` or a negative one-character lookahead
  covering the last star's class; a unique prefix followed by a suffix that
  satisfies U1; possessive repetitions (resolved to `X*(?!X)`).
- U2: `!isNullable`.
- U3: each referenced capture lies on every path of the RE (not under `Alt` or
  `Rep` with `lb = 0` or `ub > 1`), and each of its two boundaries is fixed by
  one of: a fixed-length distance from the match start or end; a maximal run
  (the item before the boundary is a star of class X and the item after it
  begins with characters outside X, as in chains).

Phase 1 also requires **no overlap** (no match may begin strictly within
another, `CC_Sequence_Search` / tconv's `RuleOverlapAnalysis::mayMatchWithin`),
so that every candidate is selected bit-parallel. Overlap is not an ambiguity
(all conventions select from the left), only a capability: it is relaxed later
by a sequential leftmost selection kernel.

Options for later phases: leftmost-longest and leftmost-first selection for
REs failing U1, lazy repetition, empty matches with a chosen convention,
last-iteration capture binding.

### Code units: byte mode or full Unicode

The new modes never use the UTF-8 indexed mode (Unicode lengths over UTF-8
code units): starts, shifts by a length, capture boundaries and replacement
counts would all have to be measured along `u8index`. They use:

- **byte mode** when the RE is fixed-UTF-8 (`validateFixedUTF8`, as today) and
  every strategy it needs works on code units (fixed length, length classes,
  unique prefix);
- **full Unicode indexing** otherwise (chains, reversal, captures of variable
  length, any RE that would otherwise be UTF-8 indexed).

This is the existing `unicodeBasisOverride` (as grep's coloring sets it) plus
forcing Unicode where a strategy needs it. In Unicode mode the S&R result is a
U21 stream, converted to UTF-8 by the client (tconv already works in U21).
Supporting the UTF-8 indexed mode is a later optimization.

### Preconditions per mode

Each mode has a precondition check, run in `prepareRE` (or
`prepareSearchReplace`) on the prepared RE, before any kernel is built:

```cpp
struct PreconditionFailure {
    RE_Goal goal;
    re::RE * construct;       // the offending subexpression
    std::string reason;       // e.g. "lookahead body not a chain, unique prefix or fixed length"
};
std::vector<PreconditionFailure> checkPreconditions(const PreparedRE &);
```

The pipeline methods treat any failure (printed with `Printer_RE`) as fatal;
clients such as tconv call `checkPreconditions` themselves to choose another
implementation or report the rule. The `report_fatal_error` calls of
`compileExternal` and `matchStartPipeline` become assertions, unreachable once
the preconditions hold. Each later phase that adds a capability relaxes the
corresponding precondition, with tests for the cases newly accepted and for
those still rejected.

Phase 1 preconditions:

| Mode | Precondition |
|---|---|
| Ends | as compiled today: every lookahead longer than one character is fixed length, has a unique prefix or is a chain; references are fixed-length property references (`FixedReferenceTransformer`) |
| Span | Ends, plus, when exact spans are requested, each factored alternative has an exact strategy (named fixed span, chain, unique prefix, length classes) instead of the minimum-length approximation |
| Starts | Ends, plus each alternative of the factored RE (after strategies 1–2 below) is fixed length, has a unique prefix, has length classes or is a chain, or has unique ends for `MatchStartsFromEnds` |
| S&R search | Starts, plus U1, U2, U3 for the referenced captures, no overlap, no back-references |
| S&R replacement | only literal characters and references to captures of the search pattern (by number or name); no functions, case escapes or cursor |

### Standard Parabix RE semantics only

The engine implements standard Parabix RE semantics. LDML transform
conventions are translated by tconv into standard REs and standard engine
calls, and are not engine features:

| LDML convention | Translation in tconv |
|---|---|
| contexts `before { text } after` | lookbehind / lookahead assertions (as `matchPattern` does now) |
| possessive context matching (ICU) | standard possessive quantifiers (`ldml::possessiveContext`) |
| `[$]` text boundary, `^` and `$` in contexts (`makeTextBoundary`, `resolveTextBoundary`) | standard `Start` / `End`, with the regions tconv supplies (whole text, or lines with `-lines`) |
| functions `&Any-Hex($1)` | the engine delivers the capture's span; tconv applies the transform kernel to it |
| cursor and revisit (`|`, `@`) | stays in tconv (it implies rescanning) |
| insertion rules (`A { } C → S`, empty text) | stay in tconv: an empty match fails U2 |
| deletion mark, filters, rule order | stay in tconv |

For functions, the S&R mode offers the streams of a capture's span (start,
coverage, end) as an output, so that a client can transform captured text with
its own kernels and merge the result.

## Classification of analyses and transformations

Columns:

- **Ends**: search mode (match ends; grep's matched lines are a coarser use of it).
- **Span**: span mode (coverage of nonempty matches).
- **Starts**: MatchStarts mode.
- **S&R‑S**: the search pattern of SearchAndReplace (needs starts, the matches'
  ends and the boundaries of referenced captures; satisfies U1–U3).
- **S&R‑R**: the replacement pattern (literals and capture references).

✔ valid and used · ✔\* valid with the stated restriction · ✘ invalid (changes
the answer) · — not applicable · ★ new use proposed by this plan · **P** used as
a precondition analysis.

### Front end: modes and external symbols

| Pass | Ends | Span | Starts | S&R‑S | S&R‑R | Notes |
|---|---|---|---|---|---|---|
| `resolveEscapeNames` | ✔ | ✔ | ✔ | ✔ | ✔★ | replacement literals may use `\n`, `\x{..}`, `\N{..}` |
| `resolveGraphemeMode` | ✔ | ✔ | ✔ | ✔ | — | |
| `UCD::linkAndResolve`, `inlineSimpleProperties`, `PropertyExternalizer` | ✔ | ✔ | ✔ | ✔ | — | |
| `validateNamesDefined`, `resolveRanges`, `resolveDiffs`, `expandPermutes` | ✔ | ✔ | ✔ | ✔ | ✔ (names) | |
| `removeUnneededCaptures(r)` | ✔ | ✔ | ✔ | ✘ | — | drops captures the replacement references |
| `removeUnneededCaptures(r, repl)` | — | — | — | ✔★ | — | keep captures referenced by back-references *or* the replacement |
| U2: `toNFD`, `addClusterMatches`, `addEquivalentCodepoints` | ✔ | ✔ | ✔ | ✔\* | — | captures must survive the rewriting (verify); a referenced capture copies the text's form |
| `resolveCaseInsensitiveMode` | ✔ | ✔ | ✔ | ✔ | ✘ | `-i` never folds the replacement |
| `resolvePossessiveQuantifiers` (`X*+` → `X*(?!X)`) | ✔ | ✔ | ✔ | ✔ | — | produces lookaheads: cheap in Starts, external in Ends; makes runs maximal (U1) |

### Simplification (`regular_expression_passes`)

| Pass | Ends | Span | Starts | S&R‑S | Notes |
|---|---|---|---|---|---|
| `standardizeAssertions` | ✔ | ✔ | ✔ | ✔ | |
| `convertToStarNormalForm` | ✔ | ✔ | ✔ | ✔\* | language-preserving; in S&R only on capture-free subtrees (it changes what a capture under a star binds; such captures fail U3 anyway) |
| `minimizeRE` / `simplifyRE` | ✔ | ✔ | ✔ | ✔\* | captures are barriers: factoring must not move capture boundaries. Alternation order is free under U1 |
| `resolveDiffs` | ✔ | ✔ | ✔ | ✔ | |

Because S&R is limited to unambiguous REs, no simplification is restricted by
a matching-order convention; the only restriction is keeping referenced
captures intact.

### Mode-specific reductions (not used today)

| Pass | Ends | Span | Starts | S&R‑S | Notes |
|---|---|---|---|---|---|
| `removeNullablePrefix` (`a*b` → `b`) | ✔★ | ✘ | ✘ | ✘ | the set of match ends is unchanged |
| `removeNullableSuffix` (`ab*` → `a`) | ✘ (line-match ✔) | ✘ | ✔★ | ✘ | dual: the set of match starts is unchanged; for grep's matched lines it is valid too |
| `zeroBoundElimination` | ✔ | ✔ | ✔ | ✔\* | `x{0}` matters only for captures |
| `emptyMatchElimination` | ✘ | ✔ (spans) | ✘ | — | S&R REs are not nullable (U2) |
| leading lookbehind → filter | native | ✔ (`chainSpans`) | ✔★ | ✔★ | `(?<=B)R`: compile `B` everywhere, AND with starts of `R` |
| trailing lookahead → body | external | ✔ | ✔★ (native) | ✔★ | `R(?=C)`: starts of `RC` |

### Mode determination and code units

| Analysis | Ends | Span | Starts | S&R‑S | S&R‑R |
|---|---|---|---|---|---|
| `determineREMode` (byte / UTF-8 indexed / Unicode) | ✔ | ✔ (coloring sets `unicodeBasisOverride`) | ✔\* byte or full Unicode only | ✔\* byte or full Unicode only | output: U21 (Unicode) or UTF-8 (byte) |
| `getLengthRange`, `grepOffset` | ✔ | ✔ | ✔ (offset convention differs) | ✔ | ✔★ replacement length, with capture lengths |
| `hasEndAnchorAnywhere` → one region (`ExtendByOne`, …) | ✔ | ✔ | ✔ | ✔ | — |
| `validateFixedUTF8`, `ByteModeLookaheads`, `byteTestsWithinLimit`, `maxLookaheadLength`, `hasReference`, `hasGraphemeClusterBoundary` | ✔ | ✔ | ✔ | ✔ | — |
| `VariableLengthCCNamer` + `toUTF8` | ✔ | ✔ | ✔ | ✔ | ✔★ literals encoded to the output code units (byte mode) |
| `StringClassRepNamer` | ✔ | ✔ (substituted back) | ✔ (chain string-class stars) | ✔ | — |
| `LookAheadNamer` | ✔ | ✔ | ✔\* the top-level lookahead *is* the query | ✔ | — |
| `processReferences` / `FixedReferenceTransformer` | ✔ | ✔ | ✔\* via ends (references point backward) | ✘ phase 1 (no back-references) | — |
| `collectCCs` / multiplexing (disabled) | ✔ | ✔ | ✔ | ✔ | — |

### Span and start machinery

| Analysis / transform | Ends | Span | Starts | S&R‑S | Notes |
|---|---|---|---|---|---|
| `spanFactoring`: `BoundedRepExpander`, `variableAltPromotion`, `FixedSpanNamer` | — | ✔ | ✔★ | ✔★ | split an RE into alternatives each with a starts strategy |
| `ParseUniquePrefix` / `UniquePrefixNamer` / `uniquePrefixEndsBack` / `LongestSpan` | — | ✔ | ✔ **P** | ✔ **P** (end per start) | |
| `parseLookaheadChain` / `chainMatchStarts` / `ChainCoverage` | **P** | ✔ | ✔ **P** | ✔ **P** (U1: maximal runs) | |
| `lengthClasses` | — | ✔ | ✔★ **P** | ✔★ **P** (U1: prefix test between classes) | each class is fixed length: starts = ends − (k − 1) |
| `isNullable` | — | — | — | **P** (U2) | |
| capture placement (new) | — | — | — | **P** (U3) | on every path; boundaries fixed by distance or maximal run |
| `parseStringClass`, `isRepeatableStringClass`, `firstCharClass`, `CC_Sequence_Search` | ✔ | ✔ | ✔ | ✔ **P** (no overlap; U1 between length classes) | |
| `reverse` (`re_reverse.cpp`, unused) | — | — | ✔★ (later) | ✔★ (later) | see "General lookahead by region reversal" |

### Replacement pattern (S&R‑R only)

| Analysis / transform | Purpose |
|---|---|
| parse (new): `$n`, `${n}`, `${name}`, `\n` | an RE of literal CCs and `Reference`s to captures of the search pattern |
| reference validation **P** | every reference names a capture of the search pattern; feeds `removeUnneededCaptures(search, repl)` |
| literal folding | adjacent single-codepoint CCs → one string; a reference to a capture of one fixed string → that literal |
| length analysis | `getLengthRange` with references sized by their captures: fixed ⇒ a per-match constant insertion/deletion count |
| `toUTF8` of literals | in byte mode (UTF-8 output) |

(Functions and cursors are LDML conventions, translated by tconv; see above.)

## Design

### 1. A goal for prepared REs

Add a goal to `RE_ModeOptions` (or `PreparedRE`):

```cpp
enum class RE_Goal {MatchEnds, MatchSpans, MatchStarts, SearchReplace};
```

`prepareRE` passes it to `resolveModesAndExternalSymbols` and
`regular_expression_passes`, which choose the passes by the tables above, to
`determineREMode` (no UTF-8 indexed mode for `MatchStarts` and
`SearchReplace`), and to `checkPreconditions`. For `SearchReplace`,
`prepareSearchReplace` also takes the replacement RE, so that capture removal
and reference validation see it. The existing `matchSearchPipeline` /
`matchSpanPipeline` callers keep the default (`MatchEnds`) and see no change,
except that their unsupported REs are reported by the precondition check
instead of a fatal error during pipeline construction.

The passes become a small table of `{name, function, applicable goals}` in
`regex_passes.cpp`, so that `-ShowREs` can name the goal and the skipped passes.

### 2. MatchStarts mode

Public API, beside the two existing pipelines:

```cpp
void matchStartsPipeline(const PreparedRE & re, kernel::StreamSet * starts);
```

Convention (decided): offset 0, a mark at the first code unit of each match, in
the code unit space of the mode (the first byte of its first character in byte mode, its first
character in Unicode mode; for an empty match, the position it is at). The
internal `MatchStarts {stream, offset}` (the `StartIndexed` convention) stays for
externals and internal match spans, as an optimization; the public result is
shifted to offset 0 (`ShiftBack` by `offset - 1`, or the shift folded into
the last kernel). With no UTF-8 indexed mode, offsets are always in code units.

Strategy order, per alternative of the factored RE (reusing `spanFactoring`'s
alternative splitting, without `emptyMatchElimination`):

1. strip a leading lookbehind (filter, compiled everywhere) and fold a
   trailing lookahead into the body;
2. apply `removeNullableSuffix` (valid for starts);
3. fixed length → ends shifted back (exists);
4. unique prefix → `uniquePrefixEndsBack` (exists);
5. **bounded RE → `lengthClasses`**: each class's ends shifted back by its
   length, OR-combined (new; mirrors `lengthClassSpans`);
6. lookahead chain → `chainMatchStarts` (exists; forces full Unicode indexing,
   which `determineREMode` must know: a chain analysis of the *top-level* RE
   for this goal);
7. ends unique → `MatchStartsFromEnds` from spans and ends (moved from tconv;
   the analysis is the `endsUnique` test of `lib/ldml/transform_plan.cpp:471`,
   to be generalised with `CC_Sequence_Search`).

The precondition is that every alternative reaches one of 3–7; otherwise the
RE is reported (later: region reversal, below).

Tests (sub-second ctests, one test binary, inputs in `QA/`): each strategy, in
byte and Unicode modes, matches touching region follows, anchored REs,
agreement with a brute-force reference implementation on small random texts,
and the precondition failures.

### 3. SearchAndReplace mode

For an unambiguous search pattern (U1–U3, no overlap in phase 1), all
semantics agree and the pipeline is:

1. **Starts**: MatchStarts of the search pattern; with no overlap, all are
   selected.
2. **Ends**: the unique end of the match from each start (U1). By
   construction for fixed length, length classes, unique prefix (`LongestSpan`
   gives the one end) and chains (`ChainCoverage`); otherwise by compiling the
   RE from the start markers (`RE_Compiler::compileRE(re, initialMarkers)`
   already exists), exact under U1 and no overlap.
3. **Capture boundaries** for referenced captures (U3): for `A (B) C`, the
   start of `B` is `A`'s fixed distance from the start or the end of `A`'s
   maximal run; its end likewise from `C`'s side, back from the match end
   (tconv's `CaptureWalk` handles fixed texts between captures). Output: the
   start, coverage and end streams of each referenced capture, also offered to
   clients (for LDML functions).
4. **Replacement assembly**: deletion mask of the matched text not kept,
   insertion counts (`InsertionCounts`), spreading (`SpreadByMask`), then the
   literal code units and the copied captures written into the inserted
   positions (generalising `InsertedTexts` / `FillInsertedPositions`). A
   fixed-length replacement gives one constant count per match; capture copies
   use the capture span lengths.

API sketch:

```cpp
struct SearchReplaceSpec {
    re::RE * search;
    re::RE * replacement;      // literal CCs and References only
    bool global = true;
};
PreparedSearchReplace prepareSearchReplace(const SearchReplaceSpec &, const RE_ModeOptions &);
void searchReplacePipeline(const PreparedSearchReplace &, kernel::StreamSet * output);
// For clients that transform captured text themselves (e.g. LDML functions).
void captureSpans(const PreparedSearchReplace &, unsigned capture, kernel::StreamSet * startCoverEnd);
```

The kernels that tconv already uses for this move from `tools/ldml/tconv.cpp`
to `lib/kernel/re/` (selection, capture walk) and `lib/kernel/streamutils/`
(insertion counts, filling), with tconv calling them from there.

## General lookahead by region reversal

### The capability

A kernel that **reverses bit stream segments made of whole match regions**:
given a segment of streams `[0, N)` that begins at a region start and ends just
after a region follow, it produces the streams in reverse position order,
`R(X)[i] = X[N - 1 - i]` (bit-reverse each 64-bit word with
`simd_bitreverse`, reverse the order of the words, and shift by the unused bits
of the last word). Because matches never cross region boundaries, reversing the
whole segment reverses every region in place, and the order of the regions,
which does not matter. For a region `[s, f)` (follow `f`), the reversed
regions are:

- starts′ = `Advance(R(follows), 1)` (the region's last character, at `N - f`);
- follows′ = `Advance(R(starts), 1)` (at `N - s`, which holds the follow of
  the preceding region in the original order, or `N` for the first).

The segment boundary rule is what is new to the pipeline: the kernel may only
consume up to the last region follow available, so it needs a variable-rate
input with a "complete regions" consumption bound (like grep's line-oriented
kernels), a buffer large enough for the longest region, and a flush at the end
of the text. Positions are characters (full Unicode indexing): reversing UTF-8
code units would reverse the bytes of each character. Reversal is applied to
the few streams the reversed RE needs (its CC streams and externals, computed
forward) rather than the 21-bit basis where possible.

### Using it

The starts of the matches of R are the ends of the matches of `reverse(R)`
(`re_reverse.cpp`, unused today: it reverses sequences, swaps `Start` and
`End` and lookahead and lookbehind, and renumbers captures) in the reversed
text, reversed back:

```
starts(R) = R⁻¹( ends_{reversed regions}( reverse(R) ) )
```

with the `grepOffset` of `reverse(R)` handled as for ends. This gives:

- **MatchStarts for any RE that the Ends mode supports in reverse**, strategy
  8, after all the cheaper strategies;
- **general lookahead** `(?=R)` / `(?!R)` in the Ends mode, as a
  `StartIndexed` external computed by reversal when R is not fixed length, a
  unique prefix or a chain;
- in reverse, every lookahead of R is a lookbehind of `reverse(R)` (cheap),
  and every lookbehind becomes a lookahead: on its own, reversal moves the
  difficulty rather than removing it. Combining it with forward matching
  (next section) removes it: the lookbehinds are matched forward first;
- **capture boundaries** in S&R, backward from the match ends: for `A (B) C`
  with `C` of variable length, the end of `B` is the end of `reverse(C)` run
  from the reversed match ends, which relaxes U3's boundary test to "fixed by
  a unique parse from either side";
- **exact spans** for any RE, as positions reachable forward from match starts
  and backward from match ends, where the RE is unambiguous enough that this
  intersection is exact (U1-like conditions; in general it over-approximates).

### Combining forward and backward matching

The reversal kernel reverses a stream set, so any streams computed by forward
matching in a region before its reversal travel with it, and any streams
computed by backward matching in the reversed region come back with the reversal
back. The backward RE kernels take the reversed forward results as externals,
and the forward kernels after the reversal back take the backward results as
externals. A pipeline is then

```
forward kernels ─▶ R ─▶ backward kernels (reversed regions) ─▶ R⁻¹ ─▶ forward kernels
```

with one reversal of all the streams needed in each direction (a single
complete-region segmentation for both), rather than a reversal pair per use.

What each direction contributes:

- **Assertions in their cheap direction.** For an R with both lookbehinds and
  general lookaheads, the lookbehinds are compiled forward (everywhere, as
  today) and reversed as zero-width externals of `reverse(R)`, in which they
  sit at the right positions; the lookaheads are lookbehinds of `reverse(R)`,
  compiled backward. Neither needs a lookahead strategy, so the "lookbehinds
  become lookaheads" limit of reversal alone disappears. Nesting (a lookahead
  inside a lookbehind) alternates directions once per level.
- **Meeting in the middle.** For R = P S, forward matching gives the ends of P
  (from the region starts, or from given starts) and backward matching gives
  where S can begin (the ends of `reverse(S)`, from the region follows or from
  given ends). Their meeting points are the positions m with a match of P
  ending at m − 1 and a match of S beginning at m. ANDed with either side's
  marks, they give:
  - the **starts of R**: P's starts whose matches reach a meeting point
    (forward from all positions, backward along P from the meeting points);
  - **capture boundaries**: for `A (B) C`, with the selected starts and ends of
    a search and replace, the start of `B` is where forward `A` from the starts
    meets backward `(B C)` from the ends, and the end of `B` where forward
    `A B` meets backward `C`. Under U1 and no overlap there is one meeting
    point per match, so U3's boundary test relaxes to "the capture boundary is
    unique", whichever side fixes it.
- **Pairing starts and ends.** A forward pass from the starts gives the ends;
  a backward pass from those ends gives the starts that reach them. Their
  agreement gives the exact matches of an S&R (and, later, the
  candidates for a sequential leftmost selection of overlapping matches).
- **Exact coverage.** The positions reached forward from match starts and
  backward from match ends: the exact span of the matches when each position's
  reach is kept per state of the RE (for example per segment of a chain, as
  `ChainCoverage` does forward), an over-approximation when only positions are
  kept.

The choice of split for meeting in the middle (which items are matched
forward, which backward) follows the same cost reasoning as the choice of
strategy: put the items with hard lookaheads, ambiguous runs or capture
boundaries on the side where they are cheap or unique.

### Cases best handled by reversal

| Case | Example | Today | Why reversal |
|---|---|---|---|
| lookahead body of variable, unbounded length without a unique prefix, not a chain | `(?=(ab|c)+d)`, `(?=\w+ing\b)` | fatal error | no forward strategy; the reversed body is an ordinary lookbehind search |
| lookahead with nested or alternated stars | `(?=(a*b)*c)`, `(?=x+y|z+w)` | fatal error | chains require one class per star segment, disjoint from the rest |
| password-style conjunctions of lookaheads | `^(?=.*\d)(?=.*[a-z]).{8,}` | partly (chains with string-class stars) | each `.*X` lookahead is one backward reachability pass per region |
| negative lookahead with variable body | `foo(?!\w*bar)` | fatal error | the complement of the reversed matches |
| MatchStarts of an unbounded repetition of a non-string-class RE | starts of `(<[^>]*>)+\w` | fatal unless ends unique | starts directly, no end uniqueness needed |
| S&R capture whose end is fixed only from the right | `([a-z0-9]*[a-z])\d+;` | U3 fails on the forward test (the run of `[a-z0-9]` is not maximal) | backward from the match end, the run of `\d` is maximal, so the capture end is unique |

Cases **not** to handle by reversal: fixed length, length classes, unique
prefix and chains (cheaper forward strategies, no segment constraint); short
bounded lookaheads (the `LookAhead` attribute of kernel inputs); REs with
assertions nested deeply enough in alternating directions that each level
needs one more reversal pair; texts with very
long regions, e.g. one region for the whole text (tconv without `-lines`, or a
file without line breaks): reversal then needs the whole text buffered, which
gives up streaming — a precondition on the maximum region length, or an
explicit whole-text option for memory-mapped inputs.

Costs to measure before adopting it: two reversal kernels per use, the
complete-region segmentation (smaller effective segments, a phase boundary for
the buffering), and one more RE kernel.

## Milestones

1. **Goal plumbing and preconditions**: `RE_Goal`, pass table, goal-aware
   `removeUnneededCaptures`, `checkPreconditions` for Ends and Span reproducing
   today's fatal errors as reported failures; no other change for Ends/Span
   (icgrep ctests and `-ShowREs` output unchanged).
2. **MatchStarts public mode** with strategies 1–4 and 7, byte or full
   Unicode mode, its preconditions; move `MatchStartsFromEnds`; tests.
3. **Length-class starts** (strategy 5) and `removeNullableSuffix`; chains as
   the top-level query (strategy 6, forcing Unicode).
4. **tconv on MatchStarts**: replace `RulePipelineBuilder::matchStarts` (the
   `(?<=B)(?=T A)` trick) and the starts half of `matchPattern`; tconv
   translates its contexts and boundaries to standard REs; compare outputs
   on the CLDR test cases (`tconv_coverage.py`) — no change expected.
5. **Replacement patterns**: parser for `$n`/`${n}`/`${name}`/`\n`,
   validation, length analysis, literal folding.
6. **S&R, literal replacements**: U1, U2, no overlap; precondition analyses
   for them; a small `resub`-style tool (or an icgrep `--replace` option) to
   exercise it.
7. **S&R, captures**: U3 analysis, capture boundaries, `captureSpans`.
8. **tconv on S&R**: string rules and MultipleCaptureMultipleReplacement sets
   through the engine, with functions applied by tconv to `captureSpans`;
   tconv keeps rule ordering, filters, insertions, cursors and deferred rules.
9. **Region reversal**: the reversal kernel and complete-region segmentation;
   MatchStarts strategy 8 and general lookahead externals, with forward-matched
   lookbehinds as externals of the backward pass; then meeting in the middle
   for capture boundaries; relax the lookahead and U3 preconditions
   accordingly.
10. Later options: overlapping matches (sequential leftmost selection,
    generalising `LeftmostMatches`); semantics for REs failing U1–U3
    (leftmost-longest, leftmost-first, lazy, empty matches); the UTF-8 indexed
    mode for the new modes; `removeNullablePrefix` for the Ends mode.

## Open questions

- For region reversal: what maximum region length is acceptable for streaming,
  and should whole-text reversal be offered for memory-mapped inputs?

Decided: the public starts convention is offset 0 at the first code unit (the
`StartIndexed` convention remains internal); `checkPreconditions` is available
to clients, and the pipeline methods treat precondition failures as fatal.

## Milestone 1 status

- `re::RE_Goal` (`include/re/unicode/regex_passes.h`), carried by
  `RE_ModeOptions::goal` and used by `prepareRE` and the mode passes.
- The front-end and simplification passes are a table of
  `{name, goals, pass}` (`lib/re/unicode/regex_passes.cpp`); passes skipped
  for a goal other than `MatchEnds` are named under `-ShowREs`/`-ShowAllREs`
  (the output for `MatchEnds` is unchanged).
- `SearchReplace` keeps the captures referenced by the search pattern or the
  replacement (`removeUnneededCaptures(r, {r, replacement})`); star normal
  form and minimization are not yet applied for it.
- `checkPreconditions` / `preconditionReport` (`regexp_engine.h`): the mode
  passes are one transformation (`ModeTransformation`) shared by the check and
  `applyModePasses`, which reports failures as fatal before building any
  kernel. `LookAheadNamer` records unsupported lookaheads (`mUnsupported`)
  instead of aborting. Covered: lookaheads (no strategy, a unique prefix of
  variable length, chains without Unicode code units) and back references;
  `MatchStarts` and `SearchReplace` report "not yet implemented".
- Still found while building the pipeline: errors of the RE compiler itself
  (a variable-length external not in initial position) and of lookaheads
  within the definitions of names, which `LookAheadNamer` does not visit, and
  a lookahead ending with the end of the text without match regions (a
  property of the context, not the RE).
- Test: `re_preconditions_test` (`tests/re_preconditions.cpp`), no pipeline
  compiled.
