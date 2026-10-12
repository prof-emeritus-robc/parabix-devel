# tconv: status of the LDML / CLDR transforms

*Status of `tools/ldml/tconv` on branch LDML, after `6b6b08cb6` with before contexts
matched against the converted text (§3.1), against CLDR
`common/transforms` and `common/testData/transforms` at commit `7cdeb9e`
(2026-10-09). October 12, 2026.*

This report covers:

- which transforms tconv implements, and how correct they are against the CLDR
  test data;
- how tconv implements them, including the fused rule kernels;
- pipeline size and speed;
- the work needed for the transforms it does not implement, and a proposed order
  for it.

The numbers can be regenerated with `tools/ldml/tconv_coverage.py` (see
[Reproducing](#reproducing)). The per-transform table is in
[ldml-tconv-coverage-table.md](ldml-tconv-coverage-table.md).

## 1. Summary

| | names | implementable |
|---|---:|---:|
| Built-in transforms (UTS #35 / ICU) | 29 | 11 |
| Transforms defined by CLDR files (381 files) | 425 | 195 (46%) |
| …of which public (not `visibility="internal"`) | 393 | 171 (44%) |

- **Tests.** Of the 284 CLDR test files, 130 belong to implementable transforms.
  Their 166,382 cases fail only in ka-ka_Latn/BGN (215), where the test data
  disagrees with the rules (§3). The coverage script still reports 378 failures,
  because it runs all the cases of a file as one text; run separately, as the
  test data intends, the other 163 pass.
- **Before contexts are matched against the converted text** (§3.1), as ICU does.
  That fixed 14,133 failing cases, including the nine Indic→Latin transforms
  (about 95% of their cases failed) and the eight conversions to Gurmukhi.
- **Pipeline scale.** Conversion groups are now compiled into a few fused kernels
  instead of about 13 kernels per rule (§4.3).
  - InterIndic-Latin's pipeline went from 1,676 kernels to 63.
  - ja_Hrkt-ja_Latn/BGN no longer crashes, and passes all 186 of its CLDR test
    cases.
  - Cold start, including JIT compilation, of Deva-Latn went from 66 s to 4.5 s.
- **Remaining work.** By the number of transforms each item alone unblocks:
  - W6 revisit: 43 transforms;
  - W3 variable-length texts: 32;
  - W2b2 two-way and self-overlapping string rules: 19.

  Each of these is concentrated in a few rule files (§5).

```
cumulative implementable CLDR-defined transforms (of 425), greedy order
now                          195  ██████████████████
+ W6 revisit                 238  ██████████████████████
+ W3 variable-length text    285  ███████████████████████████
+ W2b2 two-way overlaps      356  █████████████████████████████████
+ W4 insertion               377  ███████████████████████████████████
+ W5 captures/functions      419  ███████████████████████████████████████
+ W7, W8, W9                 425  ████████████████████████████████████████
```

These counts are lower bounds on the remaining work. `planTransform` reports one
problem per rule and skips the rules it cannot implement, so some problems
surface only after others are fixed.

## 2. What works

### 2.1 Built-in transforms (11 of 29)

| implemented | how |
|---|---|
| Any-NFD, Any-NFKD | `NFD_PipelineBuilder` (as the `nfd`/`nfkd` tools) |
| Any-NFC, Any-NFKC | the focused NFC pipeline of the `nfc` tool (NFKC = NFKD then NFC) |
| Any-Lower, Any-Upper | string override properties `lc`/`uc`, with the Final_Sigma context for Lower |
| Any-Title | Unicode toTitlecase (word boundaries + `tc`/`lc`), issue #16 |
| Any-Null | identity |
| Any-Hex/Unicode | `UnicodeHexPipeline`: `U+` and 4–6 hex digits |
| Any-Hex/Java, Any-Hex | `JavaHexPipeline`: UTF-16 code units, each `\u` and 4 hex digits |
| Any-Hex/C | `\u` and 4 hex digits, or `\U` and 8 for characters beyond U+FFFF (`6b6b08cb6`) |

`Any-Hex/Unicode`, `Any-Hex/Java` and `Any-Hex` are byte-identical to ICU 78.3 on:

- edge values from U+0000 to U+10FFFF;
- 300,000 random characters;
- an empty input;
- segment sizes 1 to 7, 1 to 4 threads, and `-EnableAsserts -VerifyIR`.

Within a filter, they convert only the filtered characters (the masked fast
path of `localCharacterMapping`).

Not implemented:

- `Any-Hex/Perl`, `/XML`, `/XML10`;
- all `Hex-Any/*`;
- `Any-CaseFold`, `Any-Remove`, `Any-FCD`, `Any-FCC`, `Any-Name`, `Name-Any` and
  `Any-BreakInternal`.

Two notes on the registry (`lib/ldml/transform_registry.cpp`):

- It lists `Any-Hex/Plain` and `Hex-Any/Plain`, which ICU 78.3 does not have.
- It makes `Hex-Any` an alias of `Hex-Any/Java`. In ICU, the bare `Hex-Any`
  accepts every escape form.

### 2.2 Transforms defined by rules (195 of 425)

Implementable now:

- all script-to-script transforms among Devanagari, Bengali, Gujarati,
  Gurmukhi, Kannada, Malayalam, Oriya, Tamil and Telugu, their Urdu conversions,
  and their romanizations;
- the Armenian, Georgian, Syriac, Canadian syllabics, Ethiopic (Williamson,
  SERA, Gurage) and Greek (BGN) romanizations, and many `*-BGN` transforms;
- the case mappings with locale rules, Fullwidth/Halfwidth, Accents and
  Publishing;
- many FONIPA transforms, and ja_Hrkt-ja_Latn/BGN.

The per-transform status is in the table.

### 2.3 Verification

**CLDR test data.** Every test file in `common/testData/transforms` is mapped to
its transform by alias, and those of implementable transforms are run:

- 130 files, 166,382 cases;
- 124 files pass entirely.

The failures are in §3.

**ICU.** When 34 transforms were implementable, all of them were compared with
`uconv -x` (ICU 74) on a 295,204-line multi-script corpus. All matched, except
for newer case mappings, ICU artifacts on long runs of combining marks, and the
word-boundary semantics of `Any-Title` (§3.3). That comparison has not been
repeated for the 195. The work since then was checked against ICU 78.3 on
specific inputs: the Hex transforms, ja_Hrkt-ja_Latn/BGN, and the tier-B context
rules of §4.3.

**Regression of the fused kernels.** The fused implementation was compared with
the previous tconv on every implementable transform (205 names). Each ran on:

- its CLDR test input;
- a 2,000-line mixed-script corpus;
- eight one-word inputs without a final newline.

The output was identical on all 1,965 runs, other than ja_Hrkt-ja_Latn/BGN, where
the previous tconv crashed. On 16 transforms the output was also identical with
segment sizes 1 to 3, 1 to 3 threads, and `-EnableAsserts -VerifyIR`.

**Regression of the converted-text contexts.** On the same 1,975 runs:

- with `--converted-context-rounds=0` (the correction off), the output is
  identical to the tconv before fusion, other than ja_Hrkt-ja_Latn/BGN;
- with the correction, 117 outputs change, in 25 transforms, and every one of
  them is identical to ICU 78.3.

On 8 corrected transforms, the output is also identical with segment sizes 1 to
3, 1 to 3 threads, and `-EnableAsserts -VerifyIR`.

## 3. Correctness: known failures

The only failing test file is ka-ka_Latn/BGN (215 of 682 cases): the test data
expects `ʼ` (U+02BC) where the rules, and ICU 78.3, give `’` (U+2019).

`tconv_coverage.py` runs all the cases of a test file as one text, one case per
line, but each line of the test data is a separate text. Rules that depend on the
end or start of the text (`[$]`) then see the line breaks instead. This accounts
for all the other failures it reports: ky-ky_FONIPA (65), pl-pl_FONIPA (78),
sk-sk_FONIPA (11), ru-ja (8) and ia-ia_FONIPA (1). Run once per case, tconv
passes all 3,311 of them, as ICU does when applied to each line.

### 3.1 Before contexts on converted text

ICU scans left to right and matches a before context against the text to the
left of the cursor, which it has already converted. tconv applies a conversion
group to all positions at once, so it originally matched before contexts against
the input. The two differ wherever the conversion changes whether a context
matches.

InterIndic-Latin is an example:

```
$forceIndependentMatra = [^[[:L:][̀-͌]]] ;
$forceIndependentMatra { $aa → ̔ā ;
```

- The context means "the preceding character is not a letter or combining mark".
- In ICU, the preceding consonant has already become a Latin letter such as `n`,
  so the rule does not apply.
- Matched against the input, the preceding character is still a private-use
  InterIndic consonant, which is not `[:L:]`, so the rule applied. `अनिया` gave
  `an̔iy̔ā` where `aniyā` is expected.

ICU also:

- looks through deleted characters (`x → ; [^a] { b → B` turns `axb` into `ab`);
- matches against the last characters of longer replacements;
- sees the results of earlier rules of the same kind, so chains propagate
  (`a { b → a ; a { c → C` turns `abbbc` into `aaaaC`).

**Which rules are affected.** A rule's before context is matched against the
converted text if some conversion of its group:

- changes whether a character is in the set of a context item;
- deletes a character;
- or, for items beyond the one adjacent to the rule, changes the length of the
  text.

This is `convertedContextMatters` in `tools/ldml/tconv.cpp`. `tconv --plan` lists
these contexts ("before context matched against the converted text").

**How.** These rules are applied by a correction kernel (`FusedRules` with
rounds), after all the other rules of the group, given their results:

1. Round 0 matches the before contexts against the input.
2. Each further round rebuilds the converted text at each position from all the
   results so far: its length (0 if deleted, or 1 plus the positions inserted
   after it) and its characters.
3. From that, it computes whether the j-th last converted character before each
   position is in a set. This uses `Advance`, looks through deleted positions
   (`MatchStar`), and handles the start of the text. Only the set's intersection
   with the group's replacement characters is compiled on computed bits.
4. It then matches the before contexts again.

A rule is right after as many rounds as rules of this kind precede it in a chain.
`--converted-context-rounds` sets the number of rounds (default 8; 0 turns the
correction off). Pablo `While` loops cannot be used for this fixpoint, because an
`Advance` inside a loop takes the previous block's carry only in the first
iteration.

**Cost.** It is small:

- 2 kernels per group (the correction kernel and its classes);
- cold starts within noise (Deva-Latn 3.1–3.4 s at 1 to 8 rounds, 3.6 s at 16);
- no measurable run-time cost.

**Whole groups.** Where such string rules must be selected (their matches
overlap), or other rules need their coverage, the correction kernel takes all
the rules of the group: in each round it also selects the string-rule matches
that apply (as `SelectStringMatches`) and computes where the single-character
rules may apply. (Groups of more than 256 string rules or subgroups keep their
before contexts matched against the text.) For example, pl-pl_FONIPA's
`$voiceless { rz → ʂ` sees `W` converted to the voiceless `f` (`Wrz` gives `fʂ`).

**Repetitions.** A before context may also hold a possessive repetition of a
set (`[^$cs y] $cs* $cs {` in ky-ky_FONIPA, `[:letter:] [:mark:]* {` in
Spacedhan-Han), with fixed items between it and the rule and at most one item
beyond it. Going back from its depth, the run of converted characters in the set
is skipped (within a position's converted text, and across positions whose
converted text is all in the set, or deleted, by `MatchStar`), and the first
other character must be in the item beyond it.

All the before contexts of the implementable transforms that their conversions
may change are now matched against the converted text: no transform has the
warning of `tconv --plan`. Matching only some of them against the converted text
is not safe: rules whose contexts `DisambiguateOrder` made complementary must be
matched against the same text, or neither may apply (ky-ky_FONIPA left `к`
unconverted in words like `бисидк` until its repetitions were matched too).

**Tested against ICU** (rules applied by `createFromRules`): 27 rule sets with
deletions, chains, expansions, fallback rules, selected and covered string rules,
and repetitions, each on 5,000 random strings, are identical to ICU. Without the
correction, between 108 and 1,853 of each 5,000 differ.

### 3.2 Robustness

- **Unexplained crash.** In one of the full regression runs, tconv crashed once
  (exit 139) on Spacedhan-Han with the mixed corpus, while four transforms ran
  in parallel. It was not reproduced in about 230 further runs:
  - sequential runs;
  - concurrent cold starts sharing a cache salt;
  - runs under compile load.
- **Reading past the end of the text.** The fused kernels assume that a
  `Lookahead` past the end of the text reads zeros. They read the character
  classes and a stream of the text positions (`TextPositions`) with lookahead.
  Existing Pablo kernels such as `Misorder_Check` make the same assumption, and
  no test showed a difference at text endings. However, the pipeline's guarantee
  of this has not been confirmed.

### 3.3 Intended differences from ICU

- **Any-Title** implements Unicode toTitlecase with word boundaries (issue #16).
  ICU's titlecase transliterator instead uses its own cased/case-ignorable
  context test. This should be confirmed, since the locale Title transforms
  (az-, tr-, lt-, el-Title) will be judged against ICU.
- **Global filters.** ICU 74 ignores the global filter of a transform made of a
  single rule set, with no `::` steps. tconv applies it, as UTS #35 specifies.
- **Normalization.** On synthetic runs of many combining marks, ICU's `uconv`
  output was not canonically ordered. tconv agrees with Python's `unicodedata`
  on those inputs.
- **The order of the steps of a backward transform.** For Spacedhan-Han (the
  backward direction of Han-Spacedhan), tconv reverses the steps, as UTS #35
  specifies: the conversion rules, then `halfwidth-fullwidth`, then NFKD. ICU 78.3
  applies the conversion rules last, after `halfwidth-fullwidth` has made the space
  U+3000, so that its rules deleting spaces between letters and ideographs never
  apply. On random mixed-script text, 392 of 20,000 lines differ for this reason.

## 4. Implementation

### 4.1 Overview

tconv parses the rules (`lib/ldml`). It then restates them as forward rules,
removes trivial and nullable captures, and disambiguates the order of rules
(`DisambiguateOrder`). Finally `ldml::planTransform` plans the transform as a
sequence of steps:

- **Transform steps** (`::X`, optionally filtered). These are built-in transforms
  (§2.1) or other rule-defined transforms, applied recursively.
- **Conversion groups.** These are the consecutive conversion rules between `::`
  steps. Each consists of:
  - *subgroups* of single-character rules with the same contexts, mapping
    characters to strings;
  - *string rules*, which replace a fixed-length text of two or more characters.

Text is decoded to a 21-bit basis (U21), with one position per character.

**Filters** (global, and `::[set] X`) are computed once, as a mask of positions,
and every step of the filtered transform changes characters only there.
Normalization handles each run of filtered characters separately, using a
barrier codepoint. A context-free, single-character mapping is applied only to
the filtered characters (`localCharacterMapping`).

### 4.2 A conversion group

1. **Matching.** The rules are matched on the text as it is. Where string rules
   may match within the texts of others, the matches that apply are selected
   (`SelectStringMatches`):
   - one-way overlaps in a topological order (W2b1);
   - self-overlapping two-character rules by parity.

   Single-character rules apply only outside the applied matches of string rules
   (W2a), and within the filter mask.
2. **Changes.** These are computed at the positions of the characters, for each
   position i of the replacements:
   - for i = 0, the XOR bits that change the character;
   - for i > 0, the bits written to the i-th position inserted after it.

   The number of positions to insert after each character is computed from the
   rules that apply (W2c).
3. **Applying the changes.**
   - Positions holding a deletion mark (U+D800, which cannot occur in decoded
     UTF-8) are inserted (`InsertionSpreadMask`).
   - The changes are spread with the text, shifted to their positions, and
     applied by XOR.
   - The deletion marks are then deleted.

### 4.3 Fused rule kernels

Phases 1 and 2 are computed by a few fused Pablo kernels (`FusedRules`, in
`0e51d5a69` and `ed416e768`), each for a chunk of the string rules or of the
subgroups. Previously each string rule needed about 13 kernels: two regular
expression kernels (one only for its spans), a span kernel, shifts, a
`TargetBits` per replacement character, and `OrCombine` chains.

**Matching in the kernel.** Matches are anchored at the first character of the
text, so spans, coverage and changes all follow by `Advance`. A rule is matched
as the AND of character classes:

- its text, and the items of its after context, by `Lookahead` of the classes;
- the items of its before context, by `Advance`.

Negated sets also hold beyond the text. The classes come from a
`CharClassesKernel` per chunk (`UTF_Compiler`); compiling large sets such as
`[^[:nd:]]` inside the fused kernel produced code that was far too large.

**Tiers of contexts.** Measured over the 23,668 string rules and 30,769
single-character rules that the planner accepts:

| tier | contexts | share of string rules | share of single-character rules | how |
|---|---|---:|---:|---|
| A | fixed sequences of sets (including negated sets and boundaries) | 99.8% | 98.5% | matched in the kernel |
| B | optional items, bounded repetitions, alternatives of different-length sequences | 11 rules | 42 rules | expanded into fixed alternatives (`ContextExpansion`) |
| C | unbounded repetitions, and the rest | 29 rules | 423 rules | regular expression engine; the matches are input to the fused kernel |

Tier B expansion:

- **Possessive.** It follows ICU's possessive matching outward from the text.
  An optional item is taken where it matches and skipped only where it does not;
  `x{lb,ub}` becomes `lb` copies and `ub − lb` optional ones.
- **Limits.** A context with more than 32 alternatives stays on the engine path,
  as does an optional item with alternatives of different lengths (such as
  `[b{cd}]?`), where ICU's choice depends on their order.
- **Tested against ICU.** On 16 rule sets, each on 3,000 random strings, the
  output is identical to ICU's `createFromRules`. The former engine path differed
  from ICU on `x } b b? b? c → X` (55 of 3,000 lines).

**Chunks.**

- String rules are grouped by (text length n, replacement length m).
- Buckets of fewer than 16 rules join a neighbour with the same n.
- At most 256 rules per chunk; subgroups at most 64 per chunk.
- Each kernel ORs its results into those of the kernels before it, so no
  `OrCombine` chains are needed.
- Where string rules overlap, the fused kernels output each rule's matches for
  `SelectStringMatches`, and compute the changes from the matches it selects.

Over all of CLDR, pure (n, m) buckets, before small buckets are merged, number
1,092 in 278 groups (median 3 per group, at most 19). The largest groups would need:

- am_Ethi-am_Ethi/Geminate (7,718 string rules): 18 buckets, split into chunks of
  at most 256 rules;
- Hans-Hant and Hant-Hans: 5 buckets each.

### 4.4 Pipeline size and speed

Cold runs (including JIT compilation) on a one-line input, before the fused
kernels and with them:

| transform | kernels before | kernels after | cold start before | cold start after |
|---|---:|---:|---:|---:|
| Deva-Latn | 2,934 | 398 | 66 s | 4.5 s |
| Latn-Cans | 1,654 | 45 | 30 s | 0.8 s |
| el-el_Latn/BGN | 1,135 | 166 | 16 s | 1.7 s |
| ru_Latn-ru/BGN | 740 | 238 | 9.3 s | 5.8 s |
| Arab-Latn | 519 | 414 | 14 s | 10 s |

(The kernel counts are before the change of the sort default below; Deva-Latn now
has 290.)

**Normalization.** The largest remaining part of most pipelines is
normalization. Canonical reordering sorts runs of combining marks with
`SortRuns`: an odd-even merge network for short runs, completed by a sequential
sort.

- **The cost.** With the former default of 6 parallel steps, the network had 21
  compare stages of three kernels each (one compare, two swaps), for every `::NFD`
  or `::NFC`. That is 126 of Deva-Latn's 398 kernels.
- **The new default.** `481afafb5` sets the default to 2 (runs of up to 4 marks).
  Measured on 8 MB inputs:
  - kernels: `nfd` 146 → 92, `nfc` 167 → 113, Deva-Latn 398 → 290;
  - pointed Hebrew: equal or faster (`nfd` 0.09 → 0.07 s; 0 steps would be
    2.5 times slower);
  - Vietnamese and English: equal or faster;
  - only runs of 5 to 12 scrambled marks are slower (`nfd` 0.10 → 0.20 s).
- **Why the network cannot simply be fused.** Each swap stage moves data
  backward, which in Pablo needs `Lookahead`, and that is allowed only on a
  kernel's inputs. Fusing it would need a non-Pablo kernel, or sharing
  normalizations between steps.

## 5. What is not covered

Table 1: work items, by the number of transforms that need them (own problems and
those of invoked transforms).

| work item | needed by | the only blocker of | effort |
|---|---:|---:|---|
| W6 cursor / text to revisit | 88 | 43 | L–XL |
| W3 variable-length texts | 140 | 32 | M (finite) / L (unbounded) |
| W2b2 two-way and self-overlapping string rules | 104 | 19 | L |
| W5 back-references and function calls | 43 | 6 | M–L |
| W4 insertion rules | 44 | 5 | M |
| W7 parser semantics (#5, #6) | 4 | 4 | S |
| W8 built-ins (Any-BreakInternal) | 1 | 0 | L |
| W9 analysis scale (Han, Japanese) | 1 | 0 | L |

Done since the first assessment:

- W1 filters (`9dea741`);
- W2a single-character rules within string rules (`9119ae0d`);
- W2b1 one-way string rule overlaps (`1ed0394e`);
- W2c expanded non-final characters (`029371343`).

**W6 revisit** (`x } [^x] → | ks`, `t } [^hṭ̱] → $ta | $virama`). The text after
`|` is processed again by the same group.

- Latin-InterIndic causes 10 of the 43 blocked transforms, and is behind every
  Latin→Indic transform. Latin-ConjoiningJamo causes 5.
- Latin-InterIndic's 62 revisit rules are almost all of the form
  `… → $consonant | $virama`.
- Proposal: compose rules at compile time, first for this one shape, where the
  revisited virama is consumed by the following vowel rules after one level.

**W3 variable-length texts.** Of the 32 transforms it alone blocks, two rule
files cause almost all:

- **und_FONIPA-fa** (26 transforms, the `*-fa` family). Its problems are almost
  all finite: 14 rules use `?` (`[$] { [iɪ] ː? → ای`), and 23 use sets with
  strings (`[{t͡ʃ}ʧ] → چ`).
- **InterIndic-Arabic** (10 `*-Arab` transforms). Its single problem rule is
  `($alif) $alif+ → $1` (issue #17).

The unbounded cases that remain are mostly runs collapsed or deleted:

- `($alif) $alif+ → $1`;
- `وو و+ → وو`;
- `[[:latin:]0-9] { [:mn:]+ →` (Latin-ASCII).

A deletion mask (`MatchStar`) and `FilterByMask` handle these without the full
span model.

**W2b2** covers two-way and self-overlapping string rules:

- dictionaries: Hans-Hant and Hant-Hans (with W9), am_Ethi-am_Ethi/Geminate;
- number and punctuation sequences: am_Brai-am_Ethi, Sarb-gz_Ethi,
  d0_Morse-am_Ethi, Any-Publishing;
- vowel sequences in either order: ro-ro_FONIPA, zh_Latn_PINYIN-ru.

It needs the leftmost selection among overlapping matches: a fixpoint in a Pablo
`While` loop, or a dictionary matcher for the Han transforms.

**W5.**

- az-, tr-, lt- and el-Title are blocked only by `&Any-Lower($1)`, a masked
  application of Lower. That is close to the existing masked machinery, but
  depends on the Title-semantics decision (§3.3).
- Thai-ThaiLogical and its inverse need back-references.

**W4 insertion rules.** Rules with an empty text, such as ConjoiningJamo-Latin's
`$latinMedialEnd lg { } $Gi → $sep`. `ldml_trules --classify-rules` already
identifies the simple insertion rules.

**W7.**

- Grek-Latn/UNGEGN and Latn-Grek/UNGEGN capture a segment within a repetition,
  which `5efe07bd2` rejects as a ruleset bug.
- Latin-ug and ug-Latin have `[:separator:]*` matching the empty text.

Both have proposed CLDR fixes, applied locally in `~/cldr`.

**W8 and W9.** Thai-Latin needs `Any-BreakInternal`, which uses a dictionary
word break. Jpan-Latn and the Han transforms need the scale work of W9.

The per-transform table lists the problem classes of every transform.

## 6. Recommended order

1. **Correctness first.**
   - Run the CLDR test cases as separate texts: a tconv option treating each line
     as a text (boundaries at line ends), used by `tconv_coverage.py`.
   - In `tconv_coverage.py`, record exit status, carry warnings over to invoking
     transforms, and report "implementable but failing its tests" separately.
   - Report the ka-ka_Latn/BGN test data (§3) to CLDR, and the backward step
     order of ICU (§3.3) to ICU.
   - Replace the rounds of the converted-text correction by a Pablo `While`
     fixpoint (issue #21), exact for chains of any length.
2. **W3, finite expansion plus run collapse.** About 30 transforms, mostly through
   und_FONIPA-fa and InterIndic-Arabic.
3. **W6 composition for Latin-InterIndic.** Up to 43 transforms, with Latin→Kana
   and Latin→Hangul next.
4. **W5 Title and W7.** Small. They depend on decisions: Title semantics, and
   whether to adopt the proposed CLDR fixes.
5. **W2b2 and W9.** Dictionary-style selection and matching.
6. **Built-ins and pipeline size.**
   - Built-ins: the remaining `Any-Hex/*` variants, `Hex-Any/*`,
     `Any-CaseFold`, `Any-Remove`.
   - Pipeline size: a single-kernel sort network, or sharing normalizations
     between steps.

At each step, run the CLDR test files of the newly implementable transforms. For
any change to the conversion-group pipeline, also compare against the previous
tconv on every implementable transform.

## 7. History

| commit | change |
|---|---|
| `12455459b` | first coverage assessment: 34 of 425 implementable |
| `9dea741` | W1 filters: 60 |
| `9119ae0d` | W2a single-character rules within string rules: 145 |
| `b5501bda`, `17fe8701`, `eac773f4` | classification of string rule overlaps |
| `d11b949e`, `92ca8af9`, `335b2b2a`, `1ed0394e` | W2b1 one-way overlaps, selection in one kernel: 151 |
| `029371343` | W2c positions inserted where rules apply: 195 |
| `bb8af3816` | pipeline compiler: partition entry PHIs no longer quadratic (InterIndic-Latin's pipeline 27 GB → 1 GB) |
| `dd89d33ec` | BlockKernelCompiler: no stride-loop branch to the final block (debug-build verifier failure) |
| `0eda90a48` | RunIndex: six index streams (debug-build failures of nfd, nfc, test_sorting) |
| `ed99eed6f` | ldml_trules: classifyRevisit with single-item result parts |
| `d4d33d3b5`, `1d86a7557` | Any-Hex/Unicode, Any-Hex/Java and Any-Hex |
| `1adbb7f43` | SpreadByMask with a repeating mask |
| `0e51d5a69` | fused rule kernels (tier A) |
| `ed416e768` | tier-B contexts in the fused kernels |
| `481afafb5` | SortRuns default of 2 parallel steps |
| `6b6b08cb6` | Any-Hex/C |
| `bdbf47ef5` | before contexts matched against the converted text: failing CLDR test cases 14,511 → 378 |
| (next) | converted-text contexts for selected and covered string rules, and with repetitions: no transform keeps the warning |

## Reproducing

```
# CLDR data (transforms and their test data)
git clone --filter=blob:none --sparse https://github.com/unicode-org/cldr.git
(cd cldr && git sparse-checkout set common/transforms common/testData/transforms && git checkout 7cdeb9e)

# tconv
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make tconv && cd ..

python3 tools/ldml/tconv_coverage.py --tconv build/bin/tconv \
    --transforms-dir cldr/common/transforms \
    --test-data cldr/common/testData/transforms --jobs 1 --out tconv-coverage
```

- `tconv-coverage/coverage.md` has the summary tables and the per-transform
  table; `coverage.json` has the same data for scripting.
- Use `--jobs 1` or 2: analyzing the Han transforms takes 8–14 GB each.
- When comparing tconv versions, give each run its own `-object-cache-salt`.
