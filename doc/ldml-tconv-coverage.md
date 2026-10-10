# tconv: coverage of the LDML / CLDR transforms

*Assessment of `tools/ldml/tconv` at commit `89e8d21` (branch LDML), against
CLDR `common/transforms` at commit `7cdeb9e` (2026-10-09). October 2026.*

This report:

- says which transforms tconv implements today, and how that was checked;
- classifies the transforms it does not implement by the work needed to
  implement them in the Parabix framework;
- proposes an order for that work.

The numbers can be regenerated with `tools/ldml/tconv_coverage.py` (see
[Reproducing](#reproducing)). The per-transform table is in
[ldml-tconv-coverage-table.md](ldml-tconv-coverage-table.md).

## Update: W1 (filters) implemented

Commit `9dea741` implements filters (§4.2). A filter is computed once, as a
mask of positions on U21, and every step of the filtered transform changes
characters only there, while contexts see the whole text. Normalization
handles each run of filtered characters separately, using a barrier
codepoint.

With the same CLDR data:

- **Coverage:** 60 of 425 CLDR-defined transforms are implementable (was
  34). The 26 transforms blocked only by W1 now work, and none of the 34 was
  lost. W2 is now the only blocker of 154 transforms.
- **Tests:** 20 CLDR test files now run, 34,744 cases. 16 files pass
  entirely. The 4 that fail are not caused by filters:
  - **Deva-Guru, Gujr-Guru, Mlym-Guru (333 cases):**
    InterIndic-Gurmukhi's rule `$consonant { \uE002 → ੰ` has a before
    context that only the *converted* text can satisfy. ICU matches before
    contexts against converted text, tconv does not (§4.1, the warning
    printed by `--plan`).
  - **ka-ka_Latn/BGN (215 cases):** the test file expects `ʼ` (U+02BC)
    where the current rules, and ICU 74, give `’` (U+2019). tconv agrees
    with ICU on all 682 cases.
- **Remaining differences from ICU:**
  - ICU 74 ignores the global filter of a transform made of a single
    rule set, with no `::` steps; tconv applies it, as UTS #35 specifies.
  - A context added by `DisambiguateOrder` for a string rule whose text
    extends outside the filter still sees the character outside the
    filter. This does not arise when a filter contains the characters of
    its rules, as CLDR's do.

The per-transform table now reflects this state. The rest of this report
describes the state before W1.

## 1. Summary

| | names | implementable today |
|---|---:|---:|
| Built-in transforms (UTS #35 / ICU) | 29 | 8 |
| Transforms defined by CLDR files (381 files) | 425 | 34 |
| …of which public (not `visibility="internal"`) | 393 | 23 |

- **About 8% of the CLDR-defined transforms are implementable now.**
  Everything implementable gives correct results on every check run:
  - all 9,736 cases of the 5 CLDR test files that belong to implementable
    transforms pass;
  - outputs agree with ICU 74 on a 295k-line multi-script corpus. The only
    differences are newer Unicode case mappings, ICU artifacts on long
    combining-mark runs, and the intended word-boundary semantics of
    `Any-Title` (§3).
- **Two kinds of work block almost everything else:**
  - **W2, parallel-application conflicts:** a rule may match inside the text of
    a longer rule, which ICU's left-to-right scan never allows. This blocks
    329 of 425 transforms.
  - **W1, filters:** the standard CLDR template is `::[filter]; ::NFD; …rules…; ::NFC;`,
    and tconv cannot yet run a transform inside a filtered transform. This
    blocks 195.
  - Together, W1 and W2 take coverage from 34 to **214 of 425 (50%)**.
- After those, the order by transforms unlocked is:
  - W3 variable-length texts: to 283;
  - W6 cursor/revisit: to 356;
  - W4 insertion rules: to 377;
  - W5 back-references and function calls: to 419.
  - The rest is 4 parser-semantics cases (issues #5, #6), 1 built-in
    (`Any-BreakInternal`, used by Thai-Latin) and Jpan-Latn, whose analysis
    runs out of memory. Analyzing the Han transforms takes 8–14 GB (W9).

```
cumulative implementable CLDR-defined transforms (of 425)
now                      34  ███
+ W2 parallel conflicts  85  ████████
+ W1 filters            214  ████████████████████
+ W3 variable length    283  ███████████████████████████
+ W6 revisit            356  ██████████████████████████████████
+ W4 insertion          377  ███████████████████████████████████
+ W5 captures/functions 419  ███████████████████████████████████████
+ W7, W8, W9            425  ████████████████████████████████████████
```

These counts are lower bounds on the remaining work, not exact
predictions. `planTransform` reports one problem per rule and skips rules it
cannot implement, so some problems surface only after others are fixed.
For example, a variable-length rule supported by W3 may then turn out to
match within the text of a string rule (W2).

## 2. Method

1. **Registry.** `tconv --list-transforms` reads the 381 CLDR transform files
   and lists 454 names:
   - 29 built-in;
   - 425 defined by files: forward and backward directions, 32 of them
     internal.
2. **Analysis.** `tconv --plan <name>` runs the same analysis tconv uses before
   building a pipeline: trivial and nullable capture elimination,
   `DisambiguateOrder`, then `planTransform`. It lists every rule that cannot
   be implemented, and why. A transform is implementable when it has no
   problems and every transform it invokes is implementable.
3. **Classification.** Each reported problem is assigned to a class (Table 2).
   Classes are propagated through `::X` transform rules, so a transform is
   charged with the work needed by the transforms it uses.
4. **Verification of the implementable transforms:**
   - **CLDR test data:** every `common/testData/transforms` file (284 files)
     was mapped to its transform by alias; implementable ones were run and
     compared with the expected results.
   - **ICU:** the 42 implementable names were compared with
     `uconv -x <name>` (ICU 74.2) on a 295,204-line corpus. The corpus is
     every source and target string of the CLDR test data, plus all assigned
     BMP characters in lines of 40.

## 3. What works

### Built-in transforms (8 of 29)

| implemented | how |
|---|---|
| Any-NFD, Any-NFKD | `NFD_PipelineBuilder` (as the `nfd`/`nfkd` tools) |
| Any-NFC, Any-NFKC | the focused NFC pipeline of the `nfc` tool (NFKC = NFKD then NFC) |
| Any-Lower, Any-Upper | string override properties `lc`/`uc`, with the Final_Sigma context for Lower |
| Any-Title | Unicode toTitlecase (word boundaries + `tc`/`lc`), issue #16 |
| Any-Null | identity |

Not implemented: `Any-CaseFold`, `Any-Remove`, `Any-FCD`, `Any-FCC`,
`Any-Name`, `Name-Any`, `Any-Hex/*` and `Hex-Any/*` (7 variants each), and
`Any-BreakInternal` (W8, §4.8).

### Transforms defined by rules (34)

Accents-Any, Any-Accents, ASCII-Latin, Cans-Latn, Devanagari-InterIndic,
Ethi-Latn/Williamson, Ethi-sgw_Ethi/Gurage_2013, Fullwidth-Halfwidth,
Geor-Latn, Gujarati-InterIndic, Gurmukhi-InterIndic, InterIndic-Bengali,
InterIndic-Gujarati, InterIndic-Gurmukhi, InterIndic-Oriya,
Malayalam-InterIndic, NumericPinyin-Pinyin, Pinyin-NumericPinyin,
Publishing-Any, Syrc-Latn, Thai-ThaiSemi, ThaiSemi-Thai, ThaiLogical-Latin,
az-Upper, el-Lower, el-Upper, kk-kk_FONIPA, lt-Lower, lt-Upper,
mn-mn_Latn/MNS, nl-Title, sgw_Ethi-Ethi/Gurage_2013, tr-Upper,
und_FONIPA-und_FONXSAMP.

Eleven of these are internal: the `*-InterIndic` / `InterIndic-*` pieces of
the Indic script-to-script transforms, plus the `Thai*` pieces.

### Verification

**CLDR test data.** 284 test files map to transforms. Only 5 belong to
implementable transforms, and all of their cases pass:

| test file | transform | cases | failed |
|---|---|---:|---:|
| kk-fonipa-t-kk | kk-kk_FONIPA | 1,009 | 0 |
| mn-Latn-t-mn-m0-mns | mn-mn_Latn/MNS | 39 | 0 |
| und-Ethi-t-sgw-ethi-m0-gurage-2013 | sgw_Ethi-Ethi/Gurage_2013 | 8,532 | 0 |
| und-fonxsamp-t-und-fonipa | und_FONIPA-und_FONXSAMP | 110 | 0 |
| und-t-s0-publish | Publishing-Any | 46 | 0 |

The other 279 test files are ready-made acceptance tests for the remaining
work.

**ICU 74 comparison** (295,204 lines). 25 of the 42 implementable names are
identical to ICU on every line. The other 17 differ only on these lines:

- **Upper and its locale variants** (Any-Upper, az-, tr-, el-, lt-Upper: 7
  lines): case mappings newer than ICU 74's Unicode 15.1 data, such as
  ɤ → U+A7CB and ƛ → U+A7DC. Parabix's UCD tables are Unicode 18.0.
- **Normalization** (NFC, NFD, NFKC, NFKD, and the transforms that apply
  them, i.e. Any-Accents, Accents-Any, el-/lt-Lower, el-/lt-Upper,
  kk-kk_FONIPA, und_FONIPA-und_FONXSAMP: 3–10 lines). These are synthetic
  lines with long runs of combining marks (U+20D6…U+20F0, U+302A…U+302F,
  U+FE20…U+FE2F), plus halfwidth Kana and Hangul under NFKC.
  - On every such line tconv agrees with Python's `unicodedata`, which uses
    Unicode 15.1, the same version as ICU 74.
  - ICU's output is not even canonically ordered: its "NFD" has U+20EA
    (ccc 1) after U+20E7/U+20E9 (ccc 230).
  - So these look like artifacts of uconv's buffered (incremental)
    transliteration, not tconv errors.
- **Any-Title and nl-Title** (2,743 lines): tconv implements Unicode
  toTitlecase with word boundaries (issue #16). ICU's titlecase
  transliterator does not use word boundaries but its own
  cased/case-ignorable context test. Observed with `uconv -x Any-Title`:
  - `ʔə.nɨ` becomes `ʔƏ.nɨ` in ICU but stays `ʔə.nɨ` in tconv;
  - `‘alama` becomes `‘Alama` in tconv but stays `‘alama` in ICU, even at
    the start of a line;
  - `hello world` becomes `Hello World` in both.

  This choice should be confirmed. ICU is the reference implementation for
  CLDR transforms, so the locale Title transforms (az-, tr-, lt-, el-Title)
  will in practice be judged against ICU's behavior.

**Speed.** On a 64 MB Devanagari text, `tconv Devanagari-InterIndic` takes
0.3–0.4 s with a warm JIT cache, against 3.9 s for `uconv`. The output is
byte-identical.

## 4. What is not covered, and what it would take

Table 1: work items, ordered by transforms unlocked.

| Work item | needed by (transitively) | only blocker of | effort |
|---|---:|---:|---|
| W2 parallel-application conflicts | 329 | 51 | M–L |
| W1 filters around transforms | 195 | 26 | S–M |
| W3 variable-length texts | 139 | 3 | M (finite) / L (unbounded) |
| W6 cursor / text to revisit | 88 | 1 | L–XL |
| W4 insertion rules | 43 | 1 | M |
| W5 back-references and function calls | 43 | 6 | M–L |
| W7 parser semantics (#5, #6) | 4 | 4 | S |
| W8 built-ins | 1 | 0 | S each, L for Name-Any / BreakInternal |
| W9 analysis scale (Han, Japanese) | 1+ | 1 | L |

Most transforms need several items. The commonest combinations:

| combination | transforms |
|---|---:|
| W1 + W2 | 103 |
| W2 + W3 | 52 |
| W2 alone | 51 |
| W1 + W2 + W6 | 27 |
| W1 alone | 26 |
| W2 + W3 + W6 | 19 |
| W2 + W3 + W4 + W5 | 18 |

Table 2: problem classes. "Own" counts the transforms with such a rule in their
own file; "transitive" also counts those that invoke one.

| class | item | own | transitive | rules |
|---|---|---:|---:|---:|
| rule may match within the text of a string rule | W2 | 130 | 327 | 19,914 |
| non-final character of a text expanded for a longer replacement | W2 | 70 | 174 | 10,545 |
| `::X` inside a filtered transform | W1 | 177 | 186 | 564 |
| `::[set] X` (filtered transform rule) | W1 | 10 | 15 | 10 |
| repetition / optional in the text | W3 | 27 | 127 | 292 |
| set with strings in the text (`[{t͡ʃ}ʧ]`) | W3 | 11 | 97 | 92 |
| variable of varying length in the text | W3 | 11 | 38 | 34 |
| text boundary within the text | W3 | 4 | 4 | 7 |
| empty text (insertion) | W4 | 10 | 43 | 163 |
| back-reference in the result | W5 | 12 | 35 | 574 |
| function call `&X($n)` in the result | W5 | 6 | 8 | 20 |
| other non-fixed result (truncated in report; Title variants, Tai Viet) | W5 | 5 | 5 | 13 |
| cursor with text to revisit | W6 | 43 | 88 | 1,099 |
| rules rejected by the parser | W7 | 4 | 4 | 4 |
| analysis ran out of memory (Jpan-Latn) | W9 | 1 | 1 | – |

The sections below describe each item:

- what the rules look like;
- why the current design cannot handle them;
- how they could be done in Parabix.

The current design is the following:

- Text is decoded to a 21-bit basis (U21), one position per character.
- Each conversion group is applied to all positions at once:
  - each subgroup of single-character rules (same contexts) computes, per
    bit, the bits to change at its matching positions;
  - fixed-length string rules do the same at the positions of their
    texts;
  - contexts are lookbehind/lookahead assertions of the regex engine;
  - all changes are ORed and applied by XOR.
- Longer replacements first insert filler positions (marked U+D800).
- Shorter ones mark positions for deletion.

This is fast and correct exactly when, at every position, at most one rule
applies and the applying rules do not interact. Most of the remaining work
consists of relaxing those two conditions.

### 4.1 W2: parallel-application conflicts (M–L; 329 transforms)

ICU scans left to right. At the cursor it applies the first rule (in rule
order) that matches, then moves past the replaced text. The rules after the
replaced text never see the positions inside it. tconv applies every rule
everywhere at once. `DisambiguateOrder` removes rule-order dependencies, but
that does not stop a rule from matching *inside* text that another rule
replaces.

| subclass | transforms with it | examples from `--plan` |
|---|---:|---|
| W2a single-character rule inside a string rule's text | 120 | `a → ア` within `ba → バ` (Latn-Kana); `飞 → 飛` within `于飞 → 于飛` (Hans-Hant) |
| W2b string rule inside or overlapping another string rule's text | 85 | `su → ス` within `tsu → ツ`; `ya → ヤ` overlapping `cy → セィ` on "cya" (Latn-Kana) |
| W2c a non-final character of a text needs inserted positions | 70 | `j $caron → ј` while `j } [^$caron] → й` expands `j` to two characters, и + U+0306 (Latn-Cyrl) |

Implementation in Parabix:

- **W2a: covered positions.**
  - For each string rule, mark the end positions of its matches.
  - Spread each end back over the text, which is a fixed-length shift
    (`IndexedAdvance` / `Advance` by `-k`).
  - Remove the covered positions, other than the starts, from the
    positions where single-character rules apply.
  - This is cheap and needs no new kernels. It is complete only when the
    string-rule matches themselves cannot overlap.
- **W2b: leftmost selection among overlapping matches.** Selected matches
  must not overlap, scanning from the left and taking rule priority at each
  start. That is a sequential dependency.
  - Priority at a single start position can be resolved in parallel: rule i
    applies at p if it matches at p and no earlier rule does.
  - The leftmost-non-overlapping chain is the same problem as the classic
    odd/even run computation for backslash escapes (length 2). In general
    it is an iterative fixpoint, i.e. a Pablo `While` loop: drop a candidate
    covered by a selected earlier candidate; iterate until stable.
  - The number of iterations is the length of the longest overlap chain,
    which is small in real text.
  - This is also the step that makes dictionary transforms (Han-Latin,
    Simplified-Traditional) possible, together with W9.
- **W2c.** Insert the filler positions after the *last* character of each
  text rather than after each character that some rule expands. The
  expansions can then be keyed by the match that applies (from W2a/W2b)
  rather than by character class.
- **Related correctness risk (not counted as a blocker).** ICU matches
  *before* contexts against already-converted text. `--plan` prints a
  warning when a context may see the text differently; 68 transforms have
  at least one.
  - Of the implementable ones, InterIndic-Gurmukhi has such a warning but
    agrees with ICU on the whole corpus.
  - A systematic treatment is to match before contexts on the output of
    the group, iterating when the two differ. Alternatively, rewrite the
    contexts over converted characters, which is what the
    `expandedContext` machinery already does for inserted positions.

### 4.2 W1: filters around transforms (S–M; 195 transforms)

185 CLDR transforms have a global filter. Most use the pattern

```
:: [filter] ;  :: NFD ;  … rules … ;  :: NFC ;
```

Only 2 are implementable. tconv already applies a filter to conversion groups,
by restricting the characters a group may change. It does not yet apply one to
a transform rule within a filtered transform. 564 such `::X` rules occur:

- 146 `::NFC` and 145 `::NFD`;
- 16 `::Lower`, 13 `::Null`;
- 11 each of the `*-InterIndic` transforms.

A further 10 `::[set] X` rules filter a single step:

- Fullwidth-Halfwidth / Halfwidth-Fullwidth;
- `[:latin:] Lower`;
- NFC / NFKC / NFKD restricted to Han.

Implementation in Parabix:

- **Character-local transforms** (NFD, NFKD, Lower/Upper/CaseFold, Null,
  Remove, Fullwidth-Halfwidth):
  - compute the filter mask as a character class on U21;
  - restrict the mapping to it, i.e. string overrides only at masked
    positions. `maskedStringOverrides` already does this for Title.
- **NFC/NFKC:** composition may only join adjacent filtered characters.
  Mark unfiltered characters as composition barriers, or filter the work
  spans, as the focused NFC pipeline already does.
- **Nested rule-defined transforms:** pass the combined filter down, as
  `RulePipelineBuilder::transform` already does for conversion groups.
- **Semantics to settle.** ICU applies a filtered transliterator to each
  maximal run of filtered characters, and context matching treats run
  boundaries as text boundaries. tconv's current conversion-group
  filtering lets contexts see unfiltered characters. The two can differ
  for transforms with a global filter and contexts, so this needs a
  decision and a test.

W1 alone unlocks 26 transforms. Combined with W2, it unlocks 103 more,
including almost every `*-Latin` romanization of a non-Latin script and the
`und_FONIPA-*` transforms.

### 4.3 W3: variable-length texts (M/L; 139 transforms)

These are texts whose length is not fixed:

- optional items: `ø̞ ː? → او`;
- repetition: `t t+ → tː`, `(ေ ေ*) } … → $1`;
- sets with strings: `[{n̼}{n̊}{n̥}nᵑⁿ] → n`, `[{t͡ʃ}ʧ] → تْش`;
- variables whose values differ in length;
- `$` (text boundary) inside the text: Amharic punctuation, Myanmar digits.

Issue #17 (`($alif) $alif+ → $1`) is an instance.

Implementation in Parabix:

- **Finite cases (M).** Rewrite at compile time into fixed-length rules,
  in priority order:
  - longest alternative first, as ICU's matcher would choose;
  - sets with strings split into their strings plus the single
    characters;
  - `x?` expanded into the two lengths;
  - bounded variable alternatives enumerated.

  This needs no new runtime machinery. About 30% of the repetition rules
  use only `?`, and all set-with-strings rules are finite. Generated
  string rules then fall under W2.
- **Unbounded repetition (L).** Needs *match spans* from the regex
  engine, i.e. both ends of each match:
  - the regex engine marks match ends; starts can be found by matching the
    reversed text pattern backwards from the ends, or by the
    possessive-match machinery already used for contexts (see #15, #17);
  - given spans, replacement is mechanical: delete the span except its
    last position (deletion mask, `FilterByMask`), write or insert the
    fixed replacement at the end (existing filler insertion);
  - captured segments within the span stay in place (W5).
- **`$` within the text (S):** treat the boundary item as an end-of-text
  assertion on the last item. Its contexts are handled already.

### 4.4 W4: insertion rules (M; 43 transforms)

These are rules with an empty text, such as `[Pp] { } [ΣςσϷϸϺϻ] → \'` (Grek-Latn),
`$latinMedialEnd lg { } $Gi → $sep` (ConjoiningJamo-Latin) and the spacing
rules of Han-Spacedhan. They insert text *between* two characters.

- **Implementation:**
  - the before context matches ending at character i and the after context
    matches starting at i+1;
  - insert k filler positions after character i, which the existing
    `Insertions` mechanism does for characters, here keyed by the context
    match rather than by a character class;
  - write the replacement into the fillers.
- `ldml_trules --classify-rules` already identifies "simple insertion
  rules" (commit 4cde878), the capture-based form `(x) → $1 y`.
- **Priority interaction:** in ICU, an insertion rule at a position
  competes with the other rules there. That needs the W2 priority
  resolution at that position.

### 4.5 W5: back-references and function calls (M–L; 43 transforms)

**Back-references** that `TrivialCaptureElimination` does not remove:

- keep or reorder captured segments: `($consonant) ်ႌ → $ukinzi $1 ျီ`,
  `($medialraZ) ($wideconsonant) … → $1 $2 $3 $4 ဳ` (Zawgyi-my);
- re-emit them after inserted text: `(sʼ) ː (j [aeiou…]) → $1 ፟ $2`.

Captured segments that stay in order need no data movement: replace the
non-captured parts around them. This is the span model of W3, with several
replaced sub-spans per match. Reordering fixed-length captures can be done
with bounded shifts. Reordering variable-length captures (rare) would need a
general permutation, and is best left for last.

**Function calls** `&X($n)` apply another transform to a captured segment:

- `&Any-Lower($1)` in az-, tr-, lt- and el-Title;
- `&NumericPinyin-Pinyin($2)` in Latin-NumericPinyin.

They are a masked application of X to the capture span, which is the same
machinery as W1 with a position mask in place of a character-class filter.
The 4 locale Title transforms then also depend on the Title semantics
decision (§3).

### 4.6 W6: cursor and text to revisit (L–XL; 88 transforms)

Examples:

- `x } [^x] → | ks` (Latn-Kana);
- `ϐ → | β` (Grek-Latn);
- `t } [^hṭ̱] → $ta | $virama` (Latin-InterIndic);
- `$jamoInitial { y } [^aeou] → | yu` (Latin-ConjoiningJamo).

The text after `|` is put back in front of the cursor and processed again
by the same rule group. So the output of a rule is input to other rules of
the same pass. That is fundamentally sequential, and a parallel pipeline has
to handle it in one of two ways:

- **Compile-time composition (preferred where it terminates):**
  - for each revisit rule `X → A | B`, find the rules that can match at B
    (with what follows), and generate composed rules `X Y → A f(B Y)`;
  - repeat until no revisit remains, or a depth bound is exceeded;
  - the composed rules are ordinary rules that W2/W3 handle;
  - the analysis needs `RuleOverlapAnalysis`-style reasoning about what
    can follow B, and termination checks for revisit cycles.
- **Bounded iteration:** if analysis bounds the revisit chain length at
  d, instantiate the group d+1 times in the pipeline, each pass
  restricted to positions produced by revisits of the previous one.
  This needs the "which positions are still in play" bookkeeping, which
  costs extra streams per pass but keeps the pipeline static.

All Latin-to-Indic, Latin-to-Kana, Latin-to-Greek and Latin-to-Hangul
transforms depend on this item, and many of the remaining Latin-* ones. It
should come after W2, W3 and W4, since composed rules land in those classes.

### 4.7 W7: parser semantics (S; 4 transforms)

- `Latin-ug` / `ug-Latin`: `[:separator:]* →  ` matches the empty text
  (issue #6). The parser rejects the rule. Accepting it requires choosing
  the semantics, e.g. treating it as `[:separator:]+`, which is presumably
  what ICU's behavior amounts to, since an empty match makes no progress.
  This should be checked against ICU on the ug test data.
- `Grek-Latn/UNGEGN`, `Latn-Grek/UNGEGN`: `( $shiftForwardVowels )*` captures
  only the last repetition (issue #5). The rule is rejected. Accepting it
  needs a decision on the intended semantics (all repetitions vs. the
  last), and then the span machinery of W3/W5.

### 4.8 W8: built-in transforms (29 names, 21 missing)

| built-in | used by CLDR files | approach | effort |
|---|---|---|---|
| Any-CaseFold | – | string override property `cf`, as `lc` | S |
| Any-Remove | – | deletion of (filtered) positions, `FilterByMask` | S (with W1) |
| Any-FCD, Any-FCC | – | partial NFD / NFC, using the NFD/NFC pipelines' work-span logic | M |
| Any-Hex/* (7) | – | each character becomes a prefix and 4–6 hex digits: bounded insertion; digits computed from U21 bits (BixNum); length from the codepoint range | M |
| Hex-Any/* (7) | – | recognize escapes by regex (W3 spans); compute the codepoint from the hex digits (BixNum); delete the escape except one position | M–L |
| Any-Name | – | the string property `na` exists in the UCD tables; string override like `lc` plus `\N{…}` wrapping | M |
| Name-Any | – | dictionary match of about 40k names (as W9) | L |
| Any-BreakInternal | Thai-Latin | ICU inserts word breaks for Thai with a dictionary break iterator | L |

Only `Any-BreakInternal` blocks a CLDR transform (Thai-Latin). The others are
user-facing names in UTS #35 and cheap except Name-Any.

### 4.9 W9: analysis scale (Han and Japanese)

| transform | forward rules | --plan time | peak memory |
|---|---|---:|---:|
| Hans-Hant (Simplified-Traditional) | 3,845: 2,883 single characters, 957 strings of 2+ | 77 s | 8.1 GB |
| Hani-Latn / Han-Latin, Han-Latin/Names, Hant-Latn | 1,398, mostly 1,281 large sets of characters by reading | 108–116 s | 10.6 GB |
| Jpan-Latn (Hira + Kana + Han) | – | killed after 155 s | > 14 GB |

`DisambiguateOrder` takes 4 s and 39 MB on Simplified-Traditional, so the
cost is in planning (`planTransform` and its pairwise `mayMatchWithin`
check, quadratic in rules × string rules), or in the `--plan` analysis around
it. For the Han-Latin family it presumably comes from the size of the sets
(tens of thousands of codepoints). The memory use should be profiled and
fixed before these transforms can be used. Even pipeline generation for them
runs the same analysis.

The two families differ in what else they need:

- **Hani-Latn (Han-Latin):** its own rules are implementable. It is blocked
  only by `::Han-Spacedhan`, which needs filtered NFKC/NFKD (W1) and
  insertion rules (W4). Its large sets also make the per-bit XOR
  computation of a subgroup expensive (21 sets per mapping, over about 1,300
  mappings), so a table lookup (codepoint → replacement index) may be the
  better kernel here.
- **Hans-Hant (Simplified-Traditional)** is a dictionary: 957 multi-character
  keys taking priority over 2,883 single-character mappings, with 772 W2
  conflicts. It needs:
  - the W2a/W2b leftmost/priority selection;
  - for scale, a many-fixed-strings matcher (e.g. hashing on the U21
    stream as in the Parabix phrase-dictionary (ztf) work, or a match of
    keys grouped by length) in place of one regex per string rule.

## 5. Recommended order

1. **W2a + W2b (match selection), then W1 (filters).**
   - Takes coverage from 34 to 214, about half of CLDR.
   - Makes 100 transforms implementable that currently fail only through
     the transforms they invoke.
   - W1 is the smaller job and could go first. Done first, it unlocks only
     26 transforms on its own, but it is a prerequisite for W5 function
     calls and the filtered built-ins.
2. **W3 finite cases** (compile-time expansion), then **W2c**.
3. **W4 insertion** and **W5 in-order back-references.** These share the
   span model with W3 unbounded repetition, which comes next.
4. **W6 revisit**, by composition, with bounded iteration as a fallback.
5. **W7** (small, can be done any time), the cheap built-ins of **W8**
   (CaseFold, Remove, Hex, Name), and **W9** as a separate
   dictionary-matching project.

At each step, the 279 CLDR test files that belong to not-yet-implementable
transforms are ready-made acceptance tests. Run them with `--test-data`
below.

## Reproducing

```
# CLDR data (transforms and their test data)
git clone --depth 1 --filter=blob:none --sparse https://github.com/unicode-org/cldr.git
(cd cldr && git sparse-checkout set common/transforms common/testData/transforms)

# tconv
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make tconv && cd ..

python3 tools/ldml/tconv_coverage.py --tconv build/bin/tconv \
    --transforms-dir cldr/common/transforms \
    --test-data cldr/common/testData/transforms --jobs 1 --out tconv-coverage
```

`tconv-coverage/coverage.md` has the summary tables and per-transform table,
and `coverage.json` the same data for scripting. Use `--jobs 1` or 2: the Han
transforms need 8–14 GB each to analyze.
