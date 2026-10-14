# tconv: status of the LDML / CLDR transforms

*Status of `tools/ldml/tconv` on branch LDML at `02e77ee59`: with rules with text
to revisit composed at compile time (W6), overlapping string matches selected
sequentially (W2b2, §4.2), variable-length texts expanded, as runs, or as
MultipleCaptureMultipleReplacement rules (W3, §5), insertion rules (W4, §4.2),
and large mappings multiplexed (§4.3), against CLDR `common/transforms` and
`common/testData/transforms` at commit `7cdeb9e` (2026-10-09). October 14,
2026.*

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
| Built-in transforms (UTS #35 / ICU) | 29 | 13 |
| Transforms defined by CLDR files (381 files) | 425 | 338 (80%) |
| …of which public (not `visibility="internal"`) | 393 | 309 (79%) |

- **Tests.** Of the 284 CLDR test files, 230 belong to implementable transforms.
  Their 240,668 cases fail only in two files, where the test data disagrees with
  the rules and with ICU (§3): ka-ka_Latn/BGN (215 cases) and my-my_FONIPA (2).
  The coverage script now runs each case as a text of its own (`tconv --lines`
  for the cases that fail in the text of all the cases), which removed 10,675
  spurious failures.
- **Insertion rules** (W4, §4.2), such as `a { } [$E $EO $EU] → $sep`: in groups
  of their own, and in groups with single-character rules, whose before contexts
  then see the text inserted, as in ICU. With them, ConjoiningJamo-Latin,
  Hang-Latn and Jamo-Latn, my-my_FONIPA (and my-ar, -chr, -fa), and Han-Spacedhan
  (and Hani-Latn, Hant-Latn, Han-Latin/Names) are implementable.
- **MultipleCaptureMultipleReplacement rules** (§5, W3), generalizing
  SingleCaptureDoubleReplacement: `T0 (B1) T1 … (Bk) Tk → R0 $1 R1 … $k Rk`, with
  after contexts and empty T0. si-si_FONIPA, si-si_Latn, ta-ta_FONIPA and the
  `si-*` transforms through them are implementable. Identity rules (`(S) → $1`)
  are single-character rules that keep their characters (vec-vec_FONIPA).
- **Large mappings are multiplexed** (§4.3). Han-Latin maps 44,666 characters to
  1,395 readings; its kernel was still compiling after 46 minutes, and now
  compiles in about a minute.
- **Variable-length texts** (W3, §5): finite texts are expanded into fixed-length
  alternatives, runs (`h h+ → h`) are deleted after their text, and
  SingleCaptureDoubleReplacement rules (`A (B) C → D $1 E`) are matched by the
  regular expression engine, with B kept in place. 76 more transforms are
  implementable, including the `*-fa`, `*-ar` and `*-chr` families, the Indic→Arabic
  transforms, Latin-ASCII, Latn-Hebr, tr-Lower and az-Lower.
- **Overlapping string matches are selected sequentially** (W2b2, §4.2), from
  the left as ICU applies them, by a scalar kernel. 39 more transforms are
  implementable, including every Latin→Indic transform, Latin→Hangul,
  Hans-Hant and Hant-Hans.
- **Revisits are composed** (W6, §5): rules with text after the cursor are
  replaced at compile time by rules without it. Ten more transforms are
  implementable; most of the others W6 blocked now need W2b2 or W3.
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
  - W3 variable-length texts: 26 transforms (am_FONIPA-am and the 18 `*-am`
    transforms through it, am-am_FONIPA and the three `am-*` transforms through
    it, Latn-Arab, blt-blt_FONIPA, byn Xaleget);
  - W6 revisits not composed (unbounded runs, variable-length texts): 25;
  - W5 back-references and function calls: 8.

  Each of these is concentrated in a few rule files (§5).

```
cumulative implementable CLDR-defined transforms (of 425), greedy order
now                          338  ████████████████████████████████
+ W3 (rest)                  364  ██████████████████████████████████
+ W6 revisit (rest)          402  ██████████████████████████████████████
+ W5 captures/functions      420  ███████████████████████████████████████
+ W4, W10                    423  ████████████████████████████████████████
+ W8, W9                     425  ████████████████████████████████████████
```

These counts are lower bounds on the remaining work. `planTransform` reports one
problem per rule and skips the rules it cannot implement, so some problems
surface only after others are fixed.

## 2. What works

### 2.1 Built-in transforms (13 of 29)

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
| Any-Hex/XML | `&#x`, 1–6 hex digits and `;` (`0686a09d7`) |
| Any-Hex/Perl | `\x{`, 1–6 hex digits and `}` (`c2bd705f6`) |

`Any-Hex/Unicode`, `Any-Hex/Java` and `Any-Hex` are byte-identical to ICU 78.3 on:

- edge values from U+0000 to U+10FFFF;
- 300,000 random characters;
- an empty input;
- segment sizes 1 to 7, 1 to 4 threads, and `-EnableAsserts -VerifyIR`.

Within a filter, they convert only the filtered characters (the masked fast
path of `localCharacterMapping`). Any-Hex/XML and Any-Hex/Perl are identical to
ICU 78.3 on 3,000 random characters and the edge values, unfiltered and within a
filter (the masked path had 8 instead of 9 positions for five hex digits).

Not implemented:

- `Any-Hex/XML10`;
- all `Hex-Any/*`;
- `Any-CaseFold`, `Any-Remove`, `Any-FCD`, `Any-FCC`, `Any-Name`, `Name-Any` and
  `Any-BreakInternal`.

Two notes on the registry (`lib/ldml/transform_registry.cpp`):

- It lists `Any-Hex/Plain` and `Hex-Any/Plain`, which ICU 78.3 does not have.
- It makes `Hex-Any` an alias of `Hex-Any/Java`. In ICU, the bare `Hex-Any`
  accepts every escape form.

### 2.2 Transforms defined by rules (338 of 425)

Implementable now:

- all script-to-script transforms among Devanagari, Bengali, Gujarati,
  Gurmukhi, Kannada, Malayalam, Oriya, Tamil and Telugu, their Urdu conversions,
  and their romanizations;
- the Armenian, Georgian, Syriac, Canadian syllabics, Ethiopic (Williamson,
  SERA, Gurage) and Greek (BGN) romanizations, and many `*-BGN` transforms;
- the case mappings with locale rules, Fullwidth/Halfwidth, Accents and
  Publishing;
- many FONIPA transforms, and ja_Hrkt-ja_Latn/BGN;
- with composed revisits: Hebr-Latn, Latn-Cyrl, ru-zh, es-am, es-es_FONIPA,
  Ethi-Latn/ES3842, Sinh-Latn/UNGEGN, gz_Ethi-Sarb, am_Ethi-am_Brai;
- with sequential selection: Latin-InterIndic and all the Latin→Indic
  transforms, Latin-ConjoiningJamo and Latn-Hang, Hans-Hant and Hant-Hans,
  am_Ethi-am_Ethi/Geminate, the Ethiopic Latin→Ethi transforms, Sarb-gz_Ethi,
  am_Brai-am_Ethi, d0_Morse-am_Ethi, Any-Publishing, and the Spanish and Czech
  conversions to Chinese and Korean;
- with variable-length texts: und_FONIPA-fa, -ar and -chr and the transforms to
  Persian, Arabic and Cherokee through them (about 50), InterIndic-Arabic and the
  nine Indic→Arabic transforms, ro, rm, chr and es_419 FONIPA, Latin-ASCII,
  de-ASCII, ug-Latin, hy-hy_Latn/BGN, Latn-Hebr, tr-Lower and az-Lower;
- with MultipleCaptureMultipleReplacement rules: si-si_FONIPA, si-si_Latn,
  ta-ta_FONIPA, si-ar, si-fa, si-chr; with identity rules, vec-vec_FONIPA;
- with insertion rules: ConjoiningJamo-Latin, Hang-Latn, Jamo-Latn,
  my-my_FONIPA, my-ar, my-fa, my-chr, Han-Spacedhan, Hani-Latn, Hant-Latn and
  Han-Latin/Names.

The per-transform status is in the table.

### 2.3 Verification

**CLDR test data.** Every test file in `common/testData/transforms` is mapped to
its transform by alias, and those of implementable transforms are run:

- 230 files, 240,668 cases;
- all but two files pass entirely (§3).

The failures are in §3.

**ICU.** When 34 transforms were implementable, all of them were compared with
`uconv -x` (ICU 74) on a 295,204-line multi-script corpus. All matched, except
for newer case mappings, ICU artifacts on long runs of combining marks, and the
word-boundary semantics of `Any-Title` (§3.3). That comparison has not been
repeated for the 195. The work since then was checked against ICU 78.3 on
specific inputs: the Hex transforms, ja_Hrkt-ja_Latn/BGN, and the tier-B context
rules of §4.3.

**Sequential selection (W2b2).** Against ICU's `createFromRules`, on about 280
random rule sets of overlapping string rules (two to four characters, with and
without contexts), each on 3,000 to 9,000 random characters, including segment
sizes 1 and 2 and 3 threads: all identical. Of the new transforms without test
data, Latn-Deva (300 lines), Latn-Beng (200) and Latn-Hang (104) match ICU 78.3
run per line, and Hans-Hant and Hant-Hans match on 400 lines of random
concatenations of the words of their rules (73 KB each, densely overlapping).

**Variable-length texts (W3).** Against ICU's `createFromRules`, all identical:

- about 350 random rule sets with optional items, sets with strings and runs,
  mixed with other rules and contexts, each on 200 to 3,000 random characters
  (including segment size 1 and 3 threads);
- 204 random sets of SingleCaptureDoubleReplacement rules with a shared B, mixed
  with single-character rules.

tr-Lower, az-Lower and Latn-Hebr, which have no CLDR test data, match ICU 78.3 on
300 random lines each of letters with combining marks, per line and as whole
files. MultipleCaptureMultipleReplacement rules (with after contexts, empty T0,
several captures, starts by the first set of B1) and identity rules were fuzzed
the same way, with no differences; vec-vec_FONIPA matches ICU 78.3 on 3,000
random lines.

**Insertion rules (W4).** Against ICU's `createFromRules`:

- groups of insertion rules alone, with filters and groups around them: 87 rule
  sets identical;
- insertion rules with single-character rules before and after them (whose
  before contexts see the text inserted), deletions, longer replacements, and
  before contexts of fixed items or with repetitions: 416 rule sets identical.
  Three more differed only by the number of rounds (§3.1): they are identical
  with 32 rounds instead of the default 8.

Against ICU 78.3 on generated text, as whole files: my-my_FONIPA (3,000 lines of
syllables), Hang-Latn (3,000 lines of random syllables, with 1,397 separators
inserted) and Jamo-Latn (the same, decomposed) are identical, as is Han-Spacedhan
(3,000 lines, 26,109 spaces inserted) on text without fullwidth forms (§3.3).

**Multiplexed mappings.** A mapping of 3,000 random Han characters to random
strings (with deletions and some contextual rules) is identical to ICU's
`createFromRules`, with and without a filter. On 2,000 random lines of CJK
characters, Hani-Latn differs from ICU 78.3 in 95 lines: in the 92 readings
checked, tconv gives the reading of the CLDR rules (`緕` → `zī`, ICU `qi`), whose
data is newer than ICU's; one is a fullwidth comma (§3.3).

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

Two test files fail, where the test data disagrees with the rules and with ICU
78.3:

- ka-ka_Latn/BGN (215 of 682 cases): the test data expects `ʼ` (U+02BC) where the
  rules give `’` (U+2019);
- my-my_FONIPA (2 of 1,228): the expected results end with a space.

Each line of the test data is a separate text. `tconv_coverage.py` runs all the
cases of a file as one text, one per line, and then runs the cases that fail
again with `tconv --lines`, which applies the pipeline to each line as a text of
its own (running all the cases that way would take hours, at about 30 ms per
line). In one text, rules that depend on the start or end of the text (`[$]`)
see the line breaks instead: the `*-chr` rules, which delete every character
that is not Cherokee, merge all the lines, and my-my_FONIPA's
`[^.$] { } $independent … → .` inserts a `.` at the start of each line. These
accounted for 10,675 reported failures in 43 files.

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
A before context matched against the converted text may also let a string rule
match within a match that applies, where ICU applies no rule (`c { c → A ; cc → Y`
on `ccc`, where `DisambiguateOrder` gives the string rule the context `[^c]`,
which the `Y` then satisfies). Each round drops the matches starting within the
matches applied in the previous round.

Two more cases were found by the W3 fuzzing and fixed:

- A string rule whose before context ends with a repetition (`a m* { b c → X`)
  never applied when the context was matched against the converted text, because
  its after context was not computed (`fusedMatch` short-circuited).
- In a group whose single-character rules have before contexts matched against
  the converted text, such a rule could apply within an applied string match: the
  converted text before the match is seen through its deleted characters
  (`a a c b → ; b { b → Z` on `baacb`). The single-character rules of such groups
  now always apply only outside the string matches.

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
- **ICU's cursor in results.** ICU's `StringReplacer` starts each rule as
  "complex" and builds the result piece by piece, recording the cursor before
  adding the literal text accumulated since the last reference to a capture. So:
  - for a result with references (or function calls), the cursor is placed after
    the last reference before `|`, or at the start of the result, and everything
    after it is revisited (`X → A | $1` revisits `A $1`). tconv follows this
    (§5, W6);
  - for a literal result, this happens only on the rule's first application in
    the life of the transliterator: `ca → PQR | X` first revisits `PQRX`, and
    afterwards only `X`. tconv always places the cursor at `|`. When comparing
    with ICU on revisit rules, warm the transliterator up (apply it twice and
    take the second result).
- **Insertion rules and filters.** Within a filter, ICU applies a compound
  transform to each run of the filtered characters, so an insertion rule inserts
  only before a filtered character, though its contexts see the whole text; a
  transform of a single group ignores the global filter (above), and inserts
  anywhere. tconv does the same. Where an insertion would let an insertion rule
  match again at the same position, ICU inserts repeatedly (dozens of times on
  `[^.$] { } X → Y`); tconv rejects such rules.
- **A filtered first step.** In some cases ICU 78.3 applies the filter of a
  first step `::[set] X ;` to the following steps too: `::[x] Null ;
  ::fullwidth-halfwidth ;` leaves `．` (U+FF0E) unchanged, but with `[ax]` it
  converts it. Han-Spacedhan begins with `::[…] nfkc ;`, so ICU leaves fullwidth
  punctuation that tconv converts. The rule has not been found.
- **The order of the steps of a backward transform.** For Spacedhan-Han (the
  backward direction of Han-Spacedhan), tconv reverses the steps, as UTS #35
  specifies: the conversion rules, then `halfwidth-fullwidth`, then NFKD. ICU 78.3
  applies the conversion rules last, after `halfwidth-fullwidth` has made the space
  U+3000, so that its rules deleting spaces between letters and ideographs never
  apply. On random mixed-script text, 392 of 20,000 lines differ for this reason.

## 4. Implementation

### 4.1 Overview

tconv parses the rules (`lib/ldml`). It then restates them as forward rules,
composes the rules with text to revisit (W6, §5), removes trivial and nullable
captures, expands finite texts (W3), and disambiguates the order of rules
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
   - self-overlapping two-character rules by parity;
   - otherwise (two-way overlaps, cycles, longer self-overlaps: W2b2),
     sequentially from the left. The matches of the rules are combined by the
     lengths of their texts (in the fused kernels), and `LeftmostMatches`, a
     scalar kernel, scans them 64 bits at a time: the leftmost match applies,
     the matches beginning within it are skipped (by counting trailing zeroes),
     and the positions it still covers are carried to the next word. The fused
     kernels then apply the rules where selected. As no two string rules of a
     group match at the same position (`DisambiguateOrder`), this is exactly
     the order in which ICU applies them. In a group whose before contexts are
     matched against the converted text (§3.1), each round of that matching is
     a kernel of its own, and the matches of each round are selected this way.

   Single-character rules apply only outside the applied matches of string rules
   (W2a), and within the filter mask.
2. **Changes.** These are computed at the positions of the characters, for each
   position i of the replacements:
   - for i = 0, the XOR bits that change the character;
   - for i > 0, the bits written to the i-th position inserted after it.

   The number of positions to insert after each character is computed from the
   rules that apply (W2c).

   **Insertion rules** (`A { } C → S`, W4) insert S after the character before
   the position, where the before context matches the converted text up to and
   including that character, and the after context the text from the next
   position. At each position the first insertion rule applies, and only if no
   rule before it in the group may apply there (checked by the planner). A group
   of insertion rules alone, with before contexts of one character, is matched by
   the regular expression engine. Otherwise the insertions are made in the
   correction kernel (§3.1): in each round, the text inserted is added after the
   converted text of its character (or replaces the deletion mark of a deleted
   one), and the before contexts of the rules of the group see it. An insertion's
   own before context is matched without its own text, and may have repetitions.
   The planner rejects insertion rules in groups with string rules, and those
   whose text would let an insertion rule match again.
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

**Multiplexed mappings.** A subgroup's small mappings (at most 64 characters
each) change characters by per-bit sets: for each position of the replacements
and each of the 21 bits, the characters whose bits change. Where a subgroup maps
more than 2,048 characters in all, each such set holds about half of them,
scattered: Han-Latin, mapping 44,666 characters to 1,395 readings of up to six
letters, needed 244 such classes, a class kernel of about 2 million Pablo
statements, whose simplification had not finished after 46 minutes. Such a
subgroup is now multiplexed: the mappings are numbered, in the order of their
replacements, and the number of the mapping of each character is given by 11
classes of its bits, the only classes of scattered characters; the changes and
insertions are sets of numbers, compiled over these bits in the fused kernel.
The classes of the bits are computed in 3 kernels of their own
(`--mux-class-kernels`), as the cost of a kernel of scattered classes grows
faster than their number: Hani-Latn compiles in 140 s with them in one kernel
with the other classes, 33 s in 3, 38 s in 11 (each still about 1 to 3 minutes
of CPU).

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
| W3 variable-length texts | 50 | 26 | M–L (the rest) |
| W6 text to revisit, not composed | 44 | 25 | L (with W3) |
| W5 back-references and function calls | 19 | 8 | M–L |
| W4 insertion rules (with string rules) | 2 | 0 | M |
| W10 engine lookaheads | 1 | 0 | M |
| W7 parser semantics (#5, #6) | 0 | 0 | S |
| W8 built-ins (Any-BreakInternal) | 1 | 0 | L |
| W9 Jpan-Latn (Hira-Latn, Kana-Latn) | 1 | 1 | L |

Done since the first assessment:

- W1 filters (`9dea741`);
- W2a single-character rules within string rules (`9119ae0d`);
- W2b1 one-way string rule overlaps (`1ed0394e`);
- W2c expanded non-final characters (`029371343`);
- W6 revisits, where composition is finite (`eea89e00d`);
- W2b2 two-way, cyclic and self-overlapping string rules, by sequential
  selection (`c230d569a`);
- W3 finite texts, runs and SingleCaptureDoubleReplacement rules (`379de8796`);
- W3 MultipleCaptureMultipleReplacement rules (`1bc9ef253`), identity rules and
  starts by the first set of B1 (`08c303c30`);
- W4 insertion rules (`3b6ee29fa`, `4f26e7d02`, `a3a64eda2`), and the scale of
  Han-Latin (`02e77ee59`).

W7 no longer blocks anything because the proposed CLDR fixes are applied in the
local `~/cldr` checkout (see W7 below).

**W6 revisit** (`x } [^x] → | ks`, `t } [^hṭ̱] → $ta | $virama`). The text after
`|` is processed again by the same group. Most such rules are now replaced at
compile time by rules without a cursor (`ldml::ComposeRevisits`,
`lib/ldml/transform_rules_revisit.cpp`), run after the forward rules are
extracted:

- **Composition.** The rules of the group are applied symbolically to the text
  to revisit, in order, as ICU tries them at the cursor. A rule applies where
  the characters to revisit are in the sets of its text, and its contexts may
  match the known text around the cursor. Its remaining contexts become
  conditions on the text before the rule's text, or after it. A rule may match
  beyond the text to revisit; its remaining items are appended to the text of
  the composed rule. Each complete sequence of applications gives one rule:
  `k → $ka | $virama` with `$virama ā → $aa` gives `k ā → $ka $aa`, with
  `$virama } [$z] → $virama` gives `k } [$z] → $ka $virama`, and so on, ending
  with `k → $ka $virama`. The composed rules are ordered depth first, so the
  earliest one that matches gives ICU's result. Captures referenced by the
  result are enumerated (up to 64 characters each).
- **Suffixes.** A rule whose text to revisit is the end of its own text,
  `P S → A | S`, is rewritten as `P } S → A`, which leaves the same text with
  the cursor at the same place. This handles runs such as ja_Latn-ko's
  `kk → ᆺ | k` on `kkkk`, which no finite composition covers.
- **ICU's cursor.** For a result with references to captures, ICU places the
  cursor after the last reference before `|`, or at the start of the result
  (§3.3). The composition follows ICU here: `([ሀ-ሇ]) $ጥበቅ → $ሀ | $1` revisits
  `$ሀ` too.
- **Tested against ICU** (`createFromRules`): about 250 random rule sets with
  revisits over a four-letter alphabet, 5 strings each, are identical to ICU.
  The fuzzing also found two older bugs, fixed here: single-character rules
  whose text is a capture (`([ab]) → B`) applied to every character, and a
  string rule could apply within an applied match in the correction kernel
  (§3.1).

Of the 43 transforms that W6 alone blocked, 9 are now implementable (Hebr-Latn,
Latn-Cyrl, ru-zh, es-am, es-es_FONIPA, es_FONIPA-am, Ethi-Latn/ES3842,
Sinh-Latn/UNGEGN, gz_Ethi-Sarb), as is am_Ethi-am_Brai (whose `InsertionCounts`
kernel name was too long for the object cache, now hashed). The rest are now
blocked by other items:

- **W2b2.** Composed rules are string rules, and some overlap: Latin-InterIndic's
  62 revisit rules compose into about 2,000 string rules with two two-way
  overlaps (`$consonants { lh → $lh` and `hlh → …`, from `h → $ha | $virama`)
  and two self-overlapping three-character rules; Latin-ConjoiningJamo is left
  with `eo`/`oe`. Both are now implementable by sequential selection.
- **Not composed** (`tconv --plan` gives the reason):
  - unbounded runs, where a rule revisits text that it matches again further
    on: Khmr-Latn/UNGEGN's `ô្គ → k | ô`, it-am's `($consonant) ' → | $1`, the
    Ethiopic gemination rules (`$ጥበቅ` repeated);
  - rules with variable-length texts that may apply to the text to revisit:
    Grek-Latn's `($lcgvowel+ $ddotmac?) $rough → h | $1`, am_Ethi-d0_Morse's
    `([:lo:]) ([:zs:]+) ([:lo:]) → | $1 ⁄⁂⁄ $2 $3`.

  Both are W3 cases.

**W3 variable-length texts.** Three mechanisms now cover most of it:

- **Finite expansion** (`ldml::ExpandFiniteTexts`,
  `lib/ldml/transform_rules_expand.cpp`). A text with optional items, bounded
  repetitions or sets with strings (`[{t͡ʃ}ʧ]`) is replaced by one rule for each
  way it matches. ICU matches possessively and takes the longest string of a set,
  without backtracking, so each alternative excludes the others:
  - an absent `x?` requires that the next character is not in `x`;
  - a shorter string requires that no longer one continues there.

  What may not follow the text becomes the first item of its after context
  (`[^x]`, including the end of the text). A string that may not follow becomes
  an alternation (`[^͡] | ͡ [^χ] | …`).
- **Runs** (`StringRule::run`). A text ending with an unbounded repetition of a
  set, such as `h h+ → h`, `[sθ]+ → s`, `وو و+ → وو`,
  `[[:latin:]0-9] { [:mn:]+ → ;` or `(S T*)+`, matches its fixed items and then
  the longest run of the set. The run is deleted (`MatchStar` within the mask)
  and covered. Groups with runs are selected sequentially: `LeftmostMatches`
  follows the run of the selected match by counting trailing ones, across words
  and strides. Only runs without an after context are supported.
- **SingleCaptureDoubleReplacement rules**, `before { A (B) C } after → D $1 E`
  (`TransformStep::singleCaptureDoubleReplacements`). Here A and C have fixed
  lengths, B is any pattern, and D and E are fixed strings. B stays in place, so
  only A is replaced by D (at the start of each match) and C by E (at its end),
  and the match is covered.
  - The regular expression engine finds the spans and ends, with B possessive.
  - A's first characters may not occur elsewhere in a match, so the starts are
    the span positions holding them.
  - Rules with the same B, contexts, C and E are matched together, by one engine
    call. D is chosen from the character at the start. The information flows
    forward only, so D cannot depend on C: Latn-Hebr's six rules take three
    engine calls, one per C.
  - These rules may not overlap the string rules of their group.

  This covers Latn-Hebr, tr-Lower and az-Lower, and rules with fixed-length B.

Also: trivial capture elimination now accepts `(X) Y → $1 …` (as `X { Y → …`)
where the rule cannot match within itself, other than as `(X) X+`. This
reduces InterIndic-Arabic's `($alif) $alif+ → $1` to `$alif { $alif+ → ;`, a run
rule (issue #17).

**MultipleCaptureMultipleReplacement rules** (`1bc9ef253`) generalize these:
several captures, after contexts, and an empty T0, with the inserted texts kept
around a character of a capture. The starts of the matches are found by the
first characters of T0, or of B1 where it begins with a set (`08c303c30`), or
from the ends of the matches where no match may end within another.

W3 alone now blocks 26 transforms:

- am_FONIPA-am and the 18 `*-am` transforms through it: three
  MultipleCaptureMultipleReplacement rules whose matches may overlap other such
  rules, or string rules;
- am-am_FONIPA and the three `am-*` transforms through it: an insertion that is
  not next to a capture (`($IPA_CONSONANT) ([jw]? $IPA_VOWEL) ፝ → $1 ː $2 ː`);
- Latn-Arab (`t ($notAbove+) ̈ → ة | $1`, with text to revisit);
- blt-blt_FONIPA (before contexts that the conversions change) and byn Xaleget. The W3 rules that remain mostly also
have other problems:

- results that reorder or repeat captures (W5);
- runs followed by an after context (Grek-Latn/UNGEGN, cy-cy_FONIPA, Zawgyi);
- runs of sets with strings (`[{i̯}ij]+`);
- text boundaries in the text (`\. ([$])`);
- SingleCaptureDoubleReplacement rules that string rules of their group may
  overlap (byn Xaleget, Zawgyi).

**The MultipleCaptureMultipleReplacement pattern** generalizes the above:
`T0 (B1) T1 … (Bk) Tk → R0 $1 R1 … $k Rk`, with fixed-length Ti, fixed texts Ri
and the references in the order of the captures. `ldml_trules --classify-rules`
counts it:

- 458 rules (forward and backward);
- 414 with one capture (SingleCaptureDoubleReplacement): 287 of these have text
  to revisit, 83 have a fixed-length B and no cursor, and 45 a variable B and no
  cursor;
- 44 with two to four captures, mostly in my-Zawgyi.

**Converted-text contexts in large groups.** A group of more than 256 string
rules or subgroups keeps its before contexts matched against the input text,
where the conversions may change them (§3.1): `tconv --plan` warns about these.
Composition made some groups that large: 8 implementable transforms have the
warning: Latin-InterIndic (31 rules, and so every Latin→Indic transform),
Latn-Cyrl, Hans-Hant, und_FONIPA-ar, es-ar, es-fa, es_419-ar and es_419-fa. They matched ICU on the inputs
tried (§2.3), but are not known to be exact. Applying the correction to the rules
with such contexts only, rather than to the whole group, would remove the limit.

**W5.**

- az-, tr-, lt- and el-Title are blocked only by `&Any-Lower($1)`, a masked
  application of Lower. That is close to the existing masked machinery, but
  depends on the Title-semantics decision (§3.3).
- Thai-ThaiLogical and its inverse need back-references.

**W4 insertion rules** (§4.2) are implemented. They remain only in Grek-Latn and
Grek-Latn/UNGEGN (`[Pp] { } [ΣςσϷϸϺϻ] → \'`), in groups with string rules, and
with rules whose after contexts have repetitions, which the correction kernel
does not match. Both transforms also need W3 and W6.

**W10 engine lookaheads.** cy-cy_FONIPA's after contexts `$cons* $end` repeat
`$cons`, a set with strings (`{m̥}{d͡ʒ}`). ICU matches it possessively, taking
the longest string at each step; the regular expression engine supports the
greedy form but not a possessive repetition of alternatives inside a lookahead
(the possessive form adds negative lookaheads within it), and the greedy form
differs (`yw } $cons* [^bdm$]` on `ywm̥`). cy-cy_FONIPA also needs W3.

**W7.**

- Grek-Latn/UNGEGN and Latn-Grek/UNGEGN capture a segment within a repetition,
  which `5efe07bd2` rejects as a ruleset bug.
- Latin-ug and ug-Latin have `[:separator:]*` matching the empty text.

Both have proposed CLDR fixes, applied locally in `~/cldr`.

**W8 and W9.** Thai-Latin needs `Any-BreakInternal`, which uses a dictionary
word break. Jpan-Latn needs Hira-Latn and Kana-Latn (W5) and the scale work of
W9; the Han transforms now compile (§4.3).

The per-transform table lists the problem classes of every transform.

## 6. Recommended order

1. **Correctness first.**
   - In `tconv_coverage.py`, record exit status, carry warnings over to invoking
     transforms, and report "implementable but failing its tests" separately.
   - Report the ka-ka_Latn/BGN test data (§3) to CLDR, and the backward step
     order of ICU (§3.3) to ICU.
   - Replace the rounds of the converted-text correction by a Pablo `While`
     fixpoint (issue #21), exact for chains of any length.
2. **W3 and W6.** The overlapping MultipleCaptureMultipleReplacement rules of
   am_FONIPA-am (26 transforms), and the revisits not composed (25 transforms,
   mostly unbounded runs, which the run rules of W3 now make approachable).
3. **Converted-text contexts in large groups** (§5): the correction for groups
   of more than 256 rules, which affects Latin-InterIndic.
4. **W5 Title and W7.** Small. They depend on decisions: Title semantics, and
   whether to adopt the proposed CLDR fixes.
5. **W9.** Jpan-Latn.
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
| `4c2e4609a` | converted-text contexts for selected and covered string rules, and with repetitions: no transform keeps the warning |
| `0686a09d7`, `c2bd705f6` | Any-Hex/XML, Any-Hex/Perl |
| `ac5a5d095` | Any-Hex/XML and Any-Hex/Perl lengths for five hex digits |
| `eea89e00d` | W6 composition of revisits: 205; capture texts of single-character rules; string matches within applied matches in the correction kernel |
| `c230d569a` | W2b2 sequential selection of string matches (`LeftmostMatches`): 244 |
| `379de8796` | W3 finite expansion, runs, SingleCaptureDoubleReplacement rules; MultipleCaptureMultipleReplacement classification: 320 |
| `1bc9ef253` | MultipleCaptureMultipleReplacement rules; negated properties hold at the text boundary: 326 |
| `2aa931ad1`, `7223645cd` | regular expression engine: `grepOffset` as the compiler places the match marker; UTF-8 characters factored by `toUTF8` in indexed mode |
| `08c303c30` | identity rules; starts by the first set of B1: 327 |
| `3b6ee29fa` | W4 insertion rules in groups of their own; `tconv --lines`, failing test cases rerun per line (10,892 → 217): 331 |
| `4f26e7d02` | W4 insertion rules with single-character rules: 334 |
| `a3a64eda2` | repetitions in the before contexts of insertion rules: 338 |
| `02e77ee59` | multiplexed mappings: Han-Latin compiles (46+ min → about a minute) |

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
- Use `--jobs 1` or 2: analyzing the Han transforms takes 8–14 GB each. (Runs
  have used `--jobs 4` on 64 GB; a full run takes about 35 minutes, mostly
  rerunning failing cases per line.)
- When comparing tconv versions, give each run its own `-object-cache-salt`.
