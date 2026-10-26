# tconv: status of the LDML / CLDR transforms

*Status of `tools/ldml/tconv` on branch LDML at `323305975`: with rules with text
to revisit composed at compile time (W6), overlapping string matches selected
sequentially (W2b2, §4.2), variable-length texts expanded, as runs, or as
MultipleCaptureMultipleReplacement rules (W3, §5; with exact starts, selected
together with string rules, and with captures of finitely many lengths
expanded), insertion rules (W4, §4.2),
large mappings multiplexed (§4.3), captures referred to out of order (SwapBack,
MoveRegions, §5), function calls on captures of single characters (W5, §5),
captures of before contexts copied (W5, §5), unreferenced captures removed
(§5), and the built-ins Hex-Any (all variants), Any-CaseFold, Any-FCD, Any-FCC,
Any-Hex/XML10 and Any-Remove (§2.1),
against CLDR `common/transforms` and `common/testData/transforms` at commit
`7cdeb9e` (2026-10-09). October 17, 2026.*

*This run used the CLDR transforms without the local fixes of W7 (§5), from a
copy (`~/transforms`, 380 files rather than 381), so Latin-ug and ug-Latin,
implementable with them, are counted as blocked by W7 here, and
Grek-Latn/UNGEGN and Latn-Grek/UNGEGN report the parse problem rather than the
W3, W4 and W6 problems they have with the fixes. Any-FCD, Any-FCC,
Any-Hex/XML10 and Any-Remove, and the transforms that the capture changes affect
(Kana-Latn, Hira-Latn, Jpan-Latn, Zawgyi-my, my-Zawgyi, my-my_FONIPA,
si-si_FONIPA, fa-fa_FONIPA) and the W3 changes affect (NumericPinyin-Latin,
am_FONIPA-am, am-am_FONIPA, byn Xaleget, cy-cy_FONIPA, and the transforms
through them), were added
to the results by an incremental update (`tconv_coverage.py --update`, see
[Reproducing](#reproducing)); the generated tables and blocks of this report are
marked `<!-- coverage:… -->`.*

*Update at `585ccdeba` (October 18, 2026): a full run of `tconv_coverage.py`
at `07794b04b` (`d92fd824b` with `tconv --kernel-counts`), with the same CLDR
data, without the local fixes of W7. It adds the kernel counts of each
transform to the per-transform table (§4.4). Its generated blocks replace
those of the run at `323305975`; the rest of the report still describes that
run. The test results are the same (253 files, 246,751 cases, the same 217
failures). 371 transforms are implementable rather than 372: on a 15 GB
machine, the analysis of Jpan-Latn exceeds the memory available (killed at
13.3 GB after 112 s, also when run alone), so it is counted as "analysis
failed" (W9) and has no kernel counts. Hani-Latn and Hant-Latn, whose analysis
needs several GB each, failed when run two at a time and were run again one
at a time, with Any-CaseFold (whose kernels were counted in its direct
pipeline).*

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

<!-- coverage:summary -->
| | names | implementable |
|---|---:|---:|
| Built-in transforms (UTS #35 / ICU) | 30 | 25 |
| Transforms defined by CLDR files (380 files) | 425 | 371 (87%) |
| …of which public (not `visibility="internal"`) | 393 | 340 (87%) |
<!-- /coverage:summary -->

- **Tests.** Of the 284 CLDR test files, 253 belong to implementable transforms.
  Their 246,751 cases fail only in two files, where the test data disagrees with
  the rules and with ICU (§3): ka-ka_Latn/BGN (215 cases) and my-my_FONIPA (2).
  The coverage script now runs each case as a text of its own (`tconv --lines`
  for the cases that fail in the text of all the cases), which removed 10,675
  spurious failures. Of the transforms made implementable since `02e77ee59`, only
  the `*-am` and `am-*` transforms have CLDR test data (23 files, 6,083 cases, all
  passing); all were checked against ICU 78.3 (§2.3).
- **More MultipleCaptureMultipleReplacement rules** (W3, §5): exact starts by
  the regular expression engine where sets of such rules overlap, string rules
  and such rules selected together, and captures of finitely many lengths
  expanded into fixed-length variants. am_FONIPA-am and the 18 `*-am`
  transforms through it, am-am_FONIPA and the three `am-*` transforms through
  it, NumericPinyin-Latin and Bopo-Latn are implementable.
- **Captures of before contexts copied** (W5, §5), `t (y* $vowel) { ヽ $voice → d $1`:
  the rules copying a capture at the end of their before context, and the
  other rules of their group with the same first characters, are applied after
  the rest of the group to the converted text, in rounds. Kana-Latn, Hira-Latn
  and Jpan-Latn are implementable; Kana-Latn is identical to ICU 78.3's
  `createFromRules` on 20,000 random lines.
- **Function calls on captures** (W5, §5), `&Any-Lower($1)`: the rule is
  replaced by rules for the characters of the capture, grouped by the value of
  the call. az-, el-, lt- and tr-Title, Latn-Ethi/ALALOC and Latn-Ethi/SERA are
  implementable.
- **Captures referred to out of order** (§5): two captures of fixed lengths in
  reverse order (`($a) ($b) → $2 $1`) by SwapBack, and captures of variable
  lengths moved after a later one by MoveRegions. Thai-ThaiLogical and
  ThaiLogical-Thai are implementable, and Thai-Latn is left blocked only by
  `Any-BreakInternal` (W8).
- **Built-ins** (§2.1): Hex-Any and its variants (Java, C, Perl, Unicode, XML,
  XML10), with the bare Hex-Any decoding all the forms, Any-CaseFold, the
  normalizations Any-FCD and Any-FCC of ICU, identical to ICU 78.3's
  Normalizer2 on `QA/Normalization` and on random text, Any-Hex/XML10
  (decimal), exact on every codepoint, and Any-Remove.
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
  - W3 variable-length texts: 7 transforms (Latn-Arab, blt-blt_FONIPA,
    Latin-NumericPinyin and Latn-Bopo through it, byn Xaleget, my-Zawgyi and
    Zawgyi-my);
  - W6 revisits not composed (unbounded runs, variable-length texts): 23;
  - W7 parser semantics: 4 in this run (2 with the local CLDR fixes);
  - W5 back-references: none alone (fa-fa_FONIPA, sk-ja and sk_FONIPA-ja also
    need W6).

  Each of these is concentrated in a few rule files (§5).

<!-- coverage:chart -->
```
cumulative implementable CLDR-defined transforms (of 425), greedy order
now                          371  ███████████████████████████████████
+ W6 revisit                 395  █████████████████████████████████████
+ W3 variable-length text    414  ███████████████████████████████████████
+ W7 parser semantics        418  ███████████████████████████████████████
+ W5 captures/functions      421  ████████████████████████████████████████
+ W10 engine lookaheads      422  ████████████████████████████████████████
+ W4 insertion               423  ████████████████████████████████████████
+ W8 built-ins               424  ████████████████████████████████████████
+ W9 analysis scale          425  ████████████████████████████████████████
```
<!-- /coverage:chart -->

These counts are lower bounds on the remaining work. `planTransform` reports one
problem per rule and skips the rules it cannot implement, so some problems
surface only after others are fixed.

## 2. What works

### 2.1 Built-in transforms (25 of 30)

| implemented | how |
|---|---|
| Any-NFD, Any-NFKD | `NFD_PipelineBuilder` (as the `nfd`/`nfkd` tools) |
| Any-NFC, Any-NFKC | the focused NFC pipeline of the `nfc` tool (NFKC = NFKD then NFC) |
| Any-FCD | NFD of the regions that fail the FCD test, the rest left as it is (`FCD_RegionMask`, `e2554e7f1`) |
| Any-FCC | NFD, then NFC in passes on the starters and the characters just after them (`42df6910a`) |
| Any-Lower, Any-Upper | string override properties `lc`/`uc`, with the Final_Sigma context for Lower |
| Any-CaseFold | string override property `cf` (`89e922e4f`) |
| Any-Title | Unicode toTitlecase (word boundaries + `tc`/`lc`), issue #16 |
| Any-Null | identity |
| Any-Remove | the characters within the filter deleted (`FilterByMask`); without a filter, an empty text (`CreateEmptyStreamSet`, `33e9cb1e1`, `335e35694`) |
| Any-Hex/Unicode | `UnicodeHexPipeline`: `U+` and 4–6 hex digits |
| Any-Hex/Java, Any-Hex | `JavaHexPipeline`: UTF-16 code units, each `\u` and 4 hex digits |
| Any-Hex/C | `\u` and 4 hex digits, or `\U` and 8 for characters beyond U+FFFF (`6b6b08cb6`) |
| Any-Hex/XML | `&#x`, 1–6 hex digits and `;` (`0686a09d7`) |
| Any-Hex/Perl | `\x{`, 1–6 hex digits and `}` (`c2bd705f6`) |
| Any-Hex/XML10 | `&#`, 1–7 decimal digits and `;` (`XMLDecimalPipeline`, `96fa4766f`) |
| Hex-Any/Java, /C, /Perl, /Unicode, /XML, /XML10, Hex-Any | `HexAnyPipeline` (`41df85c9e`): decodes the escapes of the variant; Hex-Any decodes all of them |

`Any-Hex/Unicode`, `Any-Hex/Java` and `Any-Hex` are byte-identical to ICU 78.3 on:

- edge values from U+0000 to U+10FFFF;
- 300,000 random characters;
- an empty input;
- segment sizes 1 to 7, 1 to 4 threads, and `-EnableAsserts -VerifyIR`.

Within a filter, they convert only the filtered characters (the masked fast
path of `localCharacterMapping`). Any-Hex/XML and Any-Hex/Perl are identical to
ICU 78.3 on 3,000 random characters and the edge values, unfiltered and within a
filter (the masked path had 8 instead of 9 positions for five hex digits).

**Any-Hex/XML10** follows `XMLHexPipeline` with decimal digits. `U21ToBCD`
converts each codepoint to 7 decimal digits of 4 bits by BixNum division by 10
(each quotient truncated to the width its value needs: 17, 14, 11, 7, 4 and 1
bits); `BixSplit` splits them into 8 positions of 4 bits, as the 8 hex digits of
a codepoint; every codepoint is spread to 10 positions (`&#`, 7 digits, `;`);
and `XMLEscapedDecimal` writes the ASCII digits, `&#` and `;` and deletes the
leading zeroes but the last. (A BixNum division of a dividend narrower than the
divisor returned the divisor as the remainder, `c3e3fe5b1`.) On every codepoint
but the surrogates (1,112,064) the output is exactly `&#N;`, also with segment
sizes 1 and 3, 3 threads, and `-EnableAsserts -VerifyIR`; Hex-Any/XML10 restores
the codepoints; it is identical to ICU 78.3 on 5,000 random lines of all planes,
and with the filter `[\u0080-\U0010FFFF]` to `createFromRules`.

**Any-Remove** deletes the characters of the filter in effect, keeping the
masks of the enclosing filters in step with the text, as Hex-Any does for the
escapes it shortens; with no filter, its result is an empty stream set
(`PipelineBuilder::CreateEmptyStreamSet`, a kernel that terminates without
producing items). No CLDR transform invokes it. It is identical to ICU 78.3
(`uconv -x` with the rules) on 20,000 random lines (Latin, combining marks,
Greek, CJK, emoji and all planes), a short accented text and an empty input,
for `:: [:Mn:] Remove ;`, `:: NFD ; :: [:Mn:] Remove ; :: NFC ;` (also within
the global filter `[:Greek:]`), the global filters `:: [a-z] ; :: Remove ;` and
`:: [\u0000-\u024F] ; :: [:L:] Remove ;`, conversion rules before and after it
(`a > ae ; :: [ae] Remove ; e > E ;`, `:: [a-m] ; :: [aeiou] Remove ; [a-z] > X ;`)
and an unfiltered Remove before NFC and after conversion rules; Any-Remove on
its own gives empty output. Also with segment sizes 1 and 3, 3 threads, and
`--EnableAsserts --VerifyIR` (one run aborted in the JIT, §3.2). The one
difference is U+1D25C, which is `Mn` in the Unicode data of Parabix and
unassigned in ICU 78.3; the comparisons are on the lines without the characters
ICU treats as unassigned.

**Hex-Any.** The escapes are those of ICU 78: `\uXXXX` (Java), also
`\UXXXXXXXX` (C), `\x{X..}` with 1 to 6 digits (Perl), `U+XXXX` with 4 to 6
(Unicode), `&#xX..;` with 1 to 6 (XML) and `&#D..;` with 1 to 7 decimal digits
(XML10). `HexEscapes` recognizes them (within a filter, entirely in the mask),
`HexDecode` and `DecimalDecode` compute their values, the rest of each escape
(and escapes of values beyond U+10FFFF) is deleted, and `CombineSurrogates`
makes a pair of surrogates decoded one after the other a single codepoint (a
lone surrogate becomes U+FFFD). Identical to ICU 78.3 on random text with
escapes of all the forms (8 × 2,000 lines for each variant, and within
filters), also with segment sizes 1 and 2, 3 threads, and `-EnableAsserts
-VerifyIR`; Any-Hex/X followed by Hex-Any/X is the identity for Java, C, Perl,
Unicode and XML. ICU's skipping of the character after a decoded pair or a
deleted escape is not reproduced (§3.3).

**Any-FCD** (ICU's FCD normalizer). A violation is a character whose leading
canonical combining class (lccc, the class of the first character of its NFD) is
not 0 and is below the trailing class (tccc, of the last character of its NFD)
of the character before it. Its region begins at the last character before it
with lccc = 0, or after the last one with tccc ≤ 1 if that is later (as ICU's
FCD normalizer backs up), and ends before the next character with lccc = 0; the
regions are decomposed (NFD), and the rest of the text is left as it is, so that
a Hangul syllable or À before a violation stays composed. `FCD_Violations`
finds the violations and the possible region starts, `IndexedShiftBack` moves the
first violation of each span back to its start, and `FCD_Regions` extends the
starts over the following nonstarters; the regions are then decomposed by NFD
with the other characters as barriers (U+FDD0), as for a filter. Within a
filter, the regions are found with the characters outside it replaced by
barriers, so that each run of filtered characters is taken by itself, as in
ICU.

**Any-FCC** (ICU's FCC: NFD, then composition of contiguous characters only).
It differs from NFC where NFC composes a mark with a starter across another
mark of a lower class: `1E0A 031B` (Ḋ and the horn) is NFC, and FCC
`0044 031B 0307`. tconv applies NFD, then NFC in passes, each on the starters
and the characters just after them, the other characters replaced by barriers:
NFC then composes only adjacent characters. A character that composes with the
starter before it makes the next one adjacent to the composite, which the next
pass tries; one that does not stays next to the starter, and nothing after it
composes. The passes are as many as the nonstarters of a primary composite (3,
as in U+1F82 = α 0313 0300 0345). The cost is that of an NFD and three NFCs.

**Verification.** `QA/Normalization/FCD` and `QA/Normalization/FCC` are the FCD
and FCC of `NF-source` by ICU 78.3's Normalizer2 (the API: `uconv -x Any-NFD`
and `-x Any-FCD` misorder combining marks in 73 lines of it, as noted in §3.3);
tconv's results are identical to them. Against ICU 78.3:

- Any-FCD: 20,000 random strings (Hangul, singletons, ccc-1 marks, Tibetan and
  Kannada vowel parts, misordered marks) identical, also with segment sizes 1 and
  3, 3 threads, and `--EnableAsserts --VerifyIR`; with the filters `[^x]` and
  `[^\u0316]`, 5,000 strings identical to ICU's `createFromRules`;
- Any-FCC: the same 20,000 strings and 30,000 strings of composition chains
  (Greek and Vietnamese multiple accents, ǭ, the Sinhala and Kannada two-part
  vowels, Hangul jamo; 2,257 of them differ from NFC) identical, also with
  segment sizes 1 and 3 and 3 threads; with the filter `[^\u0302]`, identical to
  `createFromRules`. With `--EnableAsserts --VerifyIR` the output is identical;
  some runs that compile the pipeline afresh abort in the JIT ("Failed to
  materialize symbols", §3.2), which runs from the object cache do not.

<!-- coverage:builtins-missing -->
Not implemented: `Any-Name`, `Name-Any`, `Any-Hex/Plain`, `Hex-Any/Plain` and `Any-BreakInternal`.
<!-- /coverage:builtins-missing -->

The registry (`lib/ldml/transform_registry.cpp`) lists `Any-Hex/Plain` and
`Hex-Any/Plain`, which ICU 78.3 does not have. `Hex-Any` is no longer an alias
of `Hex-Any/Java`: as in ICU, it accepts every escape form.

### 2.2 Transforms defined by rules (372 of 425)

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
  de-ASCII, hy-hy_Latn/BGN, Latn-Hebr, tr-Lower and az-Lower (and ug-Latin and
  Latin-ug, with the local fixes of W7);
- with MultipleCaptureMultipleReplacement rules: si-si_FONIPA, si-si_Latn,
  ta-ta_FONIPA, si-ar, si-fa, si-chr; with identity rules, vec-vec_FONIPA;
- with insertion rules: ConjoiningJamo-Latin, Hang-Latn, Jamo-Latn,
  my-my_FONIPA, my-ar, my-fa, my-chr, Han-Spacedhan, Hani-Latn, Hant-Latn and
  Han-Latin/Names;
- with captures in reverse order: Thai-ThaiLogical and ThaiLogical-Thai;
- with function calls on captures: az-, el-, lt- and tr-Title,
  Latn-Ethi/ALALOC and Latn-Ethi/SERA;
- with captures of before contexts copied: Kana-Latn, Hira-Latn and Jpan-Latn;
- with MultipleCaptureMultipleReplacement rules matched by exact starts, selected
  with string rules, or expanded: am_FONIPA-am and the `*-am` transforms through
  it, am-am_FONIPA, am-ar, am-chr, am-fa, NumericPinyin-Latin and Bopo-Latn.

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

**Captures out of order.** Against ICU's `createFromRules`, all identical:

- 156 random rule sets of two captures of fixed lengths in reverse order, with
  other rules, contexts and filters (SwapBack);
- 47 random rule sets with two or three captures of variable lengths moved, with
  texts, replacements, contexts, other rules and filters (MoveRegions).

Thai-ThaiLogical and ThaiLogical-Thai are identical to ICU 78.3 on 2,000 random
lines. The `SwapBack` kernel has tests of its own in `test_bit_movement`
(regions crossing 128-, 256- and 512-bit blocks, segment sizes 1 and 3).

**Function calls (W5).** az-, el-, lt- and tr-Title, Latn-Ethi/ALALOC and
Latn-Ethi/SERA are identical to ICU 78.3 on 3,000 random lines each, and random
rule sets calling the case mappings on captures are identical to ICU's
`createFromRules`.

**Captures of before contexts copied (W5).** Kana-Latn has no CLDR test data,
and ICU's own Katakana-Latin differs from the CLDR rules (it gives `ka` for
`カヽ`). It was compared with ICU 78.3's `createFromRules` on the rules of
`Latin-Katakana.xml` (reverse): identical on 20,000 random lines of Katakana,
iteration marks (ヽ, ヾ), voicing marks, small kana, halfwidth forms and Latin,
also with segment sizes 1 and 3, 3 threads and `--EnableAsserts --VerifyIR`, and
on chains of up to four iteration marks (`カヽヽヽヽ`; six with
`--deferred-rule-rounds=6`). Eight rule sets of copying rules (contexts with
repetitions, triggers within the captured sets, replacements before the copy,
texts of two characters, conversions in the same group) are identical to ICU's
`createFromRules` on 5,000 random lines each. Hira-Latn was checked on a few
lines only (through ICU it would use ICU's own Katakana-Latin), and Jpan-Latn
was not run.

**Unreferenced captures removed.** Removing the captures that the results
never refer to (`9043a6b5b`) changes the plans of four transforms and the
status of none: my-my_FONIPA, si-si_FONIPA and the six implementable transforms
through them give the same output as before on their CLDR test sources and on
20,000 random lines each.

**MultipleCaptureMultipleReplacement rules (W3, `1230cb8e4`, `03e837f5f`,
`323305975`).** Against ICU 78.3's `createFromRules`:

- NumericPinyin-Latin (`Latin-NumericPinyin.xml`, reverse): identical on 20,000
  random lines of numeric pinyin (initials, finals with `ü` precomposed and
  decomposed, tone digits, noise); 15,722 are changed by the rules.
- am_FONIPA-am (`am-am_FONIPA.xml`, reverse, with `::null;` written
  `::Null (Null);`, as ICU 78.3 cannot invert a bare Null): identical on 20,000
  lines of IPA made by ICU from random Ethiopic text and on 10,000 random
  strings of its characters. am-am_FONIPA (forward): identical on 20,000 random
  Ethiopic lines.
- The CLDR test files of am_FONIPA-am, am-am_FONIPA and the 21 transforms
  through them (6,083 cases) all pass.
- Rule sets with overlapping and nested matches (sets with strings sharing an
  end), string rules overlapping such rules both ways, and insertions and
  replacements between captures of finitely many lengths are identical to ICU
  on 5,000 to 8,000 random lines each, of which 951 to 4,672 are changed.

The fix to sets with strings in differences (below) changes no transform that
was implementable before: instrumented, it applies in none of the 342.

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
- **NFC memory bounds** (fixed). With `--EnableAsserts`, the direct pipeline of
  Any-NFC failed a memory bounds assertion in `NFC_CandidateClass`: the
  provisional stride of a kernel with lookahead read a block past the end of
  its input buffer when the producer, not itself a provisional stride kernel,
  had filled it (`81816aedb`). A second assertion, in the `elemMerge` of the
  output, came from the count of the partial sums of a terminated popcount
  kernel (`61225fd63`). Both are fixed on master, merged here.
- **JIT.** Some runs that compile a pipeline afresh (not from the object cache)
  abort with "LLVM ERROR: Failed to materialize symbols" (seen with Any-FCC and
  `--EnableAsserts`, about 1 run in 3), or with a JIT session error, a
  relocation out of range of a `Page21` fixup in `SingletonCanonicalization`
  (seen once with `:: NFD ; :: [:Mn:] Remove ; :: NFC ;` and `--EnableAsserts`);
  not investigated.
- **JIT failures of fresh compilations** are more frequent with the larger
  pipelines: NumericPinyin-Latin's first compilation aborts in 2 of 3 runs with
  the `Page21` relocation error; runs from the object cache do not.
- **Jpan-Latn compiles slowly.** Its pipeline, with the deferred rules of
  Kana-Latn twice (also through Hira-Latn), compiles only since the positions
  of the deferred rules are inserted once (`edbf0b88f`) and its planning no
  longer takes gigabytes (`2f882cba6`): with the default of 4 rounds, on an
  8 GB machine, the first compilation takes 228 s (300 s of CPU) and 1.5 GB,
  and runs from the object cache 3 s and 274 MB. It is identical to ICU 78.3's
  `createFromRules` (the rules of Japn-Latn.xml, with its `::` steps) on 507
  lines: 7 of Japanese text and runs of marks, and 500 random lines of up to 20 characters mixing
  the Han characters of its rules, kana, iteration marks, punctuation and ASCII.
  Runs that switch between plain and voiced iteration marks more often than
  the rounds allow would differ, as in Kana-Latn (see below). Before, it compiled only with `--deferred-rule-rounds` of 1 or 2
  ("failed to allocate virtual address space", or a Z3 error at the default
  of 4), and its analysis exceeded 13 GB.
- **Rounds of the deferred rules.** The copying rules are applied in rounds
  (`--deferred-rule-rounds`, default 4): a chain of more triggers whose before
  contexts reach back to each other leaves the last ones unchanged, unless it is
  a run of a self-reproducing rule (`カヽヽヽヽヽ`, `ナヾヾヾヾヾ`, any length,
  see `DeferredRuns`). Within a run, each switch between plain and voiced marks
  (`カヽヾヽヾ`) still takes a round, as the rule applying after the switch
  depends on the block before it. The positions the rules may write are inserted once, before
  the rounds (as many after each trigger as the longest R and copy), and the
  rounds and `DeferredRuns` work within them, so that the bounds of the
  pipeline's buffers no longer grow with the number of rounds. (Inserting
  positions in each round had them grow to the power of the rounds: on an 8 GB
  machine, Kana-Latn itself failed to compile with the default of 4, "failed to
  allocate virtual address space".) The runs of the repeated set of a capture
  copied are counted up to 16 characters, which bounds the positions reserved;
  a rule copying a longer run does not apply.

### 3.3 Intended differences from ICU

- **Any-Title** implements Unicode toTitlecase with word boundaries (issue #16).
  ICU's titlecase transliterator instead uses its own cased/case-ignorable
  context test. This should be confirmed. The locale Title transforms (az-, tr-,
  lt-, el-Title) do not use Any-Title: their rules call Any-Lower on captures,
  and they are identical to ICU 78.3 (§2.3).
- **Hex-Any.** After a decoded surrogate pair or a deleted escape, ICU 78.3 skips
  the next character, leaving it undecoded; tconv decodes it.
- **Global filters.** ICU 74 ignores the global filter of a transform made of a
  single rule set, with no `::` steps. tconv applies it, as UTS #35 specifies.
- **Normalization.** On synthetic runs of many combining marks, ICU's `uconv`
  output was not canonically ordered. tconv agrees with Python's `unicodedata`
  on those inputs, and so does ICU itself through its API: `Normalizer2`, and
  the transliterators created by `Transliterator::createInstance`, give exactly
  `QA/Normalization/NFD` and `NFC`, while `uconv -x Any-NFD` (and `-x Any-FCD`)
  leaves the first mark of some runs in place (73 lines of `NF-source`, such as
  `a 302E 059A 0316`, and `212B 0323`, given as `0041 030A 0323`). The difference
  is in `uconv`, not in ICU's normalization: compare with ICU through its API.
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

**Kernels per transform.** The per-transform table gives, for each
transform (from `tconv --kernel-counts`, which builds its pipeline without
compiling it):

- **groups, rules:** the conversion groups of its own rules (not of the
  transforms it uses) and their rules of all kinds, as planned (without the
  rules reported as problems);
- **group kernels:** the kernels these groups add;
- **kernels:** all the kernels of the transform, including those of its
  filters and of the transforms it uses, transitively (but not the kernels
  decoding the input to U21 and encoding the result). Any-CaseFold, which
  only the direct pipeline implements, is counted there, decoding and
  encoding included.

In the run at `07794b04b`, the 371 implementable CLDR-defined transforms have
a median of 227 kernels. The largest are am_FONIPA-am (2,605; its own groups have
602 rules in 2,457 kernels) and the `*-am` transforms through it (up to 3,070,
my-am).
Other large groups are NumericPinyin-Latin (75 rules, 1,946 kernels) and
Latin-ASCII (847 rules, 688 kernels). Over all the transforms, 319
conversion groups have 69,373 rules.

## 5. What is not covered

Table 1: work items, by the number of transforms that need them (own problems and
those of invoked transforms).

<!-- coverage:work-items -->
| work item | needed by | the only blocker of | effort |
|---|---:|---:|---|
| W3 variable-length texts | 22 | 7 | M–L (the rest) |
| W6 text to revisit, not composed | 40 | 24 | L (with W3) |
| W5 back-references | 3 | 0 | M |
| W7 parser semantics (#5, #6) | 4 | 4 | S |
| W10 engine lookaheads | 1 | 0 | M |
| W4 insertion rules (with string rules) | 1 | 0 | M |
| W8 built-ins (Any-BreakInternal) | 1 | 1 | L |
| W9 Jpan-Latn (Hira-Latn, Kana-Latn) | 1 | 1 | L |
<!-- /coverage:work-items -->

(W7 counts the run without the local CLDR fixes; with them, it blocks nothing,
Latin-ug and ug-Latin are implementable, and the two Greek UNGEGN transforms
need W3, W4 and W6.)

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
  Han-Latin (`02e77ee59`);
- two captures of fixed lengths in reverse order (`7e927d2d7`, SwapBack), and
  captures of variable lengths moved out of order (`a090553b4`, MoveRegions);
- W5 function calls on captures of single characters (`687470e89`);
- W5 captures of before contexts copied (`258f7c284`), and unreferenced
  captures removed (`9043a6b5b`);
- W3 MultipleCaptureMultipleReplacement rules with exact starts (`1230cb8e4`),
  selected with string rules (`03e837f5f`), and with captures of finitely many
  lengths expanded (`323305975`);
- the built-ins Hex-Any (`41df85c9e`), Any-CaseFold (`89e922e4f`), Any-FCD
  (`e2554e7f1`), Any-FCC (`42df6910a`), Any-Hex/XML10 (`96fa4766f`) and
  Any-Remove (`335e35694`).

W7 blocks nothing where the proposed CLDR fixes are applied locally (see W7
below); this run did not apply them.

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
from the ends of the matches where no match may end within another: where Tk
may not occur ending within a match, overlapping itself or in the part before
it (`1230cb8e4`; `($L) ːwaː` of am_FONIPA-am).

- **Exact starts** (`1230cb8e4`). Where the matches of sets of such rules may
  overlap, and are selected from the left (`LeftmostMatches`), a set whose
  starts are not known by its first characters gets every start from the
  regular expression engine: the pattern and its after context as a
  lookahead, after its before context as a lookbehind. These include the
  matches beginning within another of the set with the same end (through a set
  with strings, `{t͡ʃ}` and `ʃ`), which apply where that one does not. The engine
  does not support the possessive repetition of a set with strings in a
  lookahead (W10); the greedy form gives the same matches where what follows
  each repetition cannot begin with a character it repeats
  (`([Oo]) ([$vowel-[AEae]]* …) $digit`).
- **Selected with string rules** (`03e837f5f`). Where string rules may overlap
  such rules (one matching within the other), the sets join the string rules'
  classes in `LeftmostMatches`, each as its starts followed by a run of the rest
  of its span, and both apply only where selected. A string rule and such a
  rule that may match at the same position are still rejected.
- **Captures of finitely many lengths expanded** (`323305975`). Where a
  replaced or inserted text lies between captures of variable length, the
  rule becomes a rule for each combination of the alternatives of the items of
  its captures (an optional item present or absent, each string of a set with
  strings, then its characters), in ICU's possessive order, where that is
  equivalent: `($IPA_CONSONANT) ([jw]? $IPA_VOWEL) ፝ → $1 ː $2 ː` is 16 rules.
- **Sets with strings.** `expandedContext` dropped the strings of differences
  and intersections of sets (`[$vowel-[aeAE]]` lost `{ü}`), and the engine
  resolves a possessive repetition as PCRE does, taking the first alternative:
  the alternatives are now ordered longest first, as ICU matches the longest
  string of a set.
- The pipeline compiler enumerated every combination of the roots of its
  partitions (exponential in their number): NumericPinyin-Latin's pipeline
  spent over 10 minutes there. Beyond 4,096 combinations it now chooses each
  root in turn (`d2411817d`).

**Captures out of order.** A rule `T0 (B1) T1 (B2) T2 → R0 $2 R1 $1 R2`, with
B1 and B2 of fixed lengths, is planned as the rule with the captures in order,
`→ R0 R1 $1 $2 R2`, and the region `R1 B1 B2` of each match is then made
`B2 R1 B1` by `SwapBack` (`7e927d2d7`). The first character of B1 marks the
region, and the marks are kept in step with the text as positions are inserted
and deleted. Where a before context of the group may tell the order of the
characters of the regions apart (ICU matching it against the converted text),
the rules are not implemented. More generally (`a090553b4`), where one capture
j of variable length is referred to before captures i to j − 1, the rule is
planned in order, the captures are found by a possessive walk from the starts of
the matches (`CaptureWalk`), and the region from T(i+1) to capture j is moved
after capture j: its length is carried to the end of capture j (`MoveRegions`,
`MoveCounts`), and its characters are taken out (`FilterByMask`) and put into as
many positions inserted there (`InsertionSpreadMask`, `MergeByMask`). The
starts of the matches are also found from their ends where Tk is empty and the
last capture ends with a set not occurring before it in a match. Such moves are
planned in Latin-NumericPinyin, blt-blt_FONIPA, my-Zawgyi and Zawgyi-my, which
have other problems.

W3 alone now blocks 7 transforms:

- Latn-Arab (`t ($notAbove+) ̈ → ة | $1`, with text to revisit);
- blt-blt_FONIPA and Latin-NumericPinyin (before contexts that the conversions
  change), and Latn-Bopo through Latin-NumericPinyin;
- byn Xaleget and my-Zawgyi (rules whose starts cannot be told apart);
- Zawgyi-my (runs followed by after contexts, the text boundary in the text,
  and starts that cannot be told apart).

The W3 rules that remain mostly also have other problems:

- back-references (W5);
- runs followed by an after context (Grek-Latn/UNGEGN, cy-cy_FONIPA, Zawgyi);
- runs of sets with strings (`[{i̯}ij]+`);
- text boundaries in the text (`\. ([$])`);
- MultipleCaptureMultipleReplacement rules whose starts cannot be told apart
  (byn Xaleget, my-Zawgyi, Zawgyi-my).

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

**W5.** Function calls on captures of single characters are implemented
(`687470e89`): `ldml::ExpandFunctionCalls` replaces a rule whose result calls a
transform on such a capture, `&T($k)`, by rules for the characters of the
capture grouped by the value of T on them: the capture is restricted to the
group (or replaced by its set, if no longer referred to), and the call by the
value, or by `$k` for the characters T leaves unchanged. It runs before revisits
are composed. tconv evaluates Any-Lower, Any-Upper and Any-Title by the full case
mappings, and other transforms through their plans, on single characters. This
made az-, el-, lt- and tr-Title and Latn-Ethi/ALALOC and /SERA implementable, and
removed W5 from the Pinyin transforms and my-Zawgyi.

**Unreferenced captures** are removed before the other capture simplifications
(`ldml::RemoveUnreferencedCaptures`, `9043a6b5b`, with `re::removeUnneededCaptures`):
a capture that the result never refers to, such as the grouping in
`($wspace)+ ([…]) → $2`, becomes its pattern, and the other captures are
renumbered. Seven rules of the CLDR transforms have such captures, in
my-my_FONIPA, my-t-my-d0-zawgyi, my-t-my-s0-zawgyi and si-si_FONIPA. The three
capture simplifications (with trivial and nullable capture elimination) are on
by default; `--disable-capture-simplification` turns them off (in tconv and
`ldml_trules`, whose `--eliminate-trivial-captures` and
`--eliminate-nullable-captures` it replaces). Zawgyi-my no longer needs W5.

**Captures of before contexts copied** (`258f7c284`). A rule
`P (C) { T } A → R $1` copies the text that C, at the end of its before
context, matches: as ICU matches before contexts against the text it has
converted, the copy is of converted text, and a rule may see the copy of
another (with `(.) { b → $1`, ICU turns `xbb` into `xxx`). Kana-Latn repeats a
syllable this way at the iteration mark ヽ, with 18 such rules and a rule
deleting the mark otherwise. The planner defers these rules, with the other
rules of their group whose texts begin with the same characters (the
triggers), and applies them after the rest of the group, to the converted text,
in rounds: at each trigger whose before context may not reach back to a trigger
not yet replaced, the first deferred rule matching applies (matched by the
regular expression engine), R and the copy replace T, and the text written is
not matched again. The capture is counted right to left from the trigger,
possessively, as ICU matches it: it must be a sequence of sets with at most one
unbounded repetition of a set, whose run `RunIndex` counts. The other rules of
the group may not match the triggers or the characters after them in the texts
of the deferred rules, nor see them in their before contexts, and the after
contexts of the deferred rules (seeing the converted text) must be sets that
the other rules' changes cannot tell apart. Kana-Latn, Hira-Latn and Jpan-Latn
are implementable.

W5 now needs only back-references, in 3 transforms, none of which it alone
blocks: fa-fa_FONIPA (`([$IPA_CONSONANT$VOWEL\|]) { ّ → $1`, a copy of a set
with strings, also W3 and W6), sk_FONIPA-ja and sk-ja (also W6).

**W4 insertion rules** (§4.2) are implemented. They remain only in Grek-Latn and
Grek-Latn/UNGEGN (`[Pp] { } [ΣςσϷϸϺϻ] → \'`), in groups with string rules, and
with rules whose after contexts have repetitions, which the correction kernel
does not match. Both transforms also need W3 and W6 (in this run, Grek-Latn/UNGEGN
reports only its W7 parse problem).

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

Both have proposed CLDR fixes, applied locally in `~/cldr`. This run used
transforms without them (`~/transforms`), so these four transforms are reported
as blocked by W7.

**W8 and W9.** Thai-Latn now needs only `Any-BreakInternal`, which uses a
dictionary word break (its Thai-ThaiLogical step is implemented). Jpan-Latn,
which needed Hira-Latn and Kana-Latn, is implementable, and W9 blocks nothing:
the Han transforms compile (§4.3). (`tconv_coverage.py` counted Jpan-Latn's
`::Hira-Latin ;` as an unknown transform; it now resolves names that mix the
forms of the scripts of a transform's names, as tconv does.)

The per-transform table lists the problem classes of every transform.

## 6. Recommended order

1. **Correctness first.**
   - In `tconv_coverage.py`, record exit status, carry warnings over to invoking
     transforms, and report "implementable but failing its tests" separately.
   - Report the ka-ka_Latn/BGN test data (§3) to CLDR, and the backward step
     order of ICU (§3.3) to ICU.
   - Replace the rounds of the converted-text correction by a Pablo `While`
     fixpoint (issue #21), exact for chains of any length.
2. **W3 and W6.** Before contexts that the conversions change in
   MultipleCaptureMultipleReplacement rules (blt-blt_FONIPA,
   Latin-NumericPinyin), captures revisited (Latn-Arab), and the revisits not
   composed (24 transforms, mostly unbounded runs, which the run rules of W3 now
   make approachable).
3. **Converted-text contexts in large groups** (§5): the correction for groups
   of more than 256 rules, which affects Latin-InterIndic.
4. **W7.** Small; it depends on whether to adopt the proposed CLDR fixes. Run
   the coverage with them (`~/cldr`), as the earlier reports did.
5. **W5 back-references.** The copy of a set with strings (fa-fa_FONIPA), with
   W6 in fa-fa_FONIPA, sk-ja and sk_FONIPA-ja; and the deferred rules exact for
   chains of any length (a fixpoint rather than rounds, with buffer bounds that
   do not compound).
6. **Built-ins and pipeline size.**
   - Built-ins: `Any-Name` and `Name-Any`, and
     `Any-BreakInternal` for Thai-Latn.
   - The JIT failures of fresh compilations (§3.2).
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
| `d424a1029`, `7e927d2d7` | SwapBack: two captures in reverse order (Thai-ThaiLogical, ThaiLogical-Thai) |
| `730101c2e`, `41df85c9e` | HexDecode kernel; Hex-Any and its variants |
| `687470e89` | W5 function calls on captures of single characters (az-, el-, lt-, tr-Title, Latn-Ethi/ALALOC, /SERA) |
| `a090553b4` | captures of variable lengths referred to out of order (MoveRegions) |
| `89e922e4f` | Any-CaseFold: 344 (without the local W7 fixes) |
| `e2554e7f1` | Any-FCD (`FCD_RegionMask`); `QA/Normalization/FCD` |
| `86f6ad53d` | `tconv_coverage.py --update`: incremental coverage updates |
| `42df6910a` | Any-FCC (NFC in passes on adjacent characters); `QA/Normalization/FCC`: 23 of 30 built-ins |
| `81816aedb`, `61225fd63` | (master) room for a consumer's provisional stride; the partial sums of a terminated popcount kernel: no assertion failures in NFC |
| `c3e3fe5b1`, `96fa4766f` | BixNum division of narrow dividends; Any-Hex/XML10 (`XMLDecimalPipeline`): 24 of 30 built-ins |
| `33e9cb1e1`, `335e35694` | `PipelineBuilder::CreateEmptyStreamSet`; Any-Remove: 25 of 30 built-ins |
| `56473be9a`, `9043a6b5b` | unreferenced captures removed; `--disable-capture-simplification` |
| `258f7c284` | captures of before contexts copied (deferred rules): Kana-Latn, Hira-Latn, Jpan-Latn: 347 |
| `d2411817d`, `1230cb8e4` | partition roots no longer all enumerated; exact starts of MultipleCaptureMultipleReplacement sets, sets with strings kept: NumericPinyin-Latin, Bopo-Latn |
| `03e837f5f` | string rules and MultipleCaptureMultipleReplacement rules selected together: am_FONIPA-am and 18 `*-am` |
| `323305975` | captures of finitely many lengths expanded: am-am_FONIPA, am-ar, am-chr, am-fa: 372 |
| `07794b04b`, `585ccdeba` | `tconv --kernel-counts`; kernel columns in the per-transform table: 371 on 15 GB (Jpan-Latn's analysis exceeds the memory) |

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
- Analyzing the Han transforms took 8–14 GB each before `planTransform` kept
  the characters of each subgroup as it adds rules (and the multiplexed sets
  were built from their intervals); it now takes about 120 MB (Jpan-Latn,
  Hani-Latn and Hant-Latn, `--kernel-counts`). (Runs have used `--jobs 4` on
  64 GB; a full run takes about 35 minutes, mostly rerunning failing cases per
  line.)
- When comparing tconv versions, give each run its own `-object-cache-salt`.
- **Kernel counts.** Each transform's `tconv --kernel-counts` output is kept in
  `kernels/`, and gives the groups, rules, group kernels and kernels columns of
  the per-transform table. With `--update`, the kernels of the transforms
  planned again are counted again, with those of all the transforms using
  them, directly or not; `--no-kernel-counts` keeps the previous counts.
- The run at `89e922e4f` used `--transforms-dir ~/transforms`, without the local
  W7 fixes; use a checkout with them to count Latin-ug and ug-Latin.
- **Incremental updates.** After a change to tconv, update the results instead of
  a full run: `--update` keeps the previous results in `--out` and plans (and
  tests) only the selected transforms (`--only NAME …`, `--builtins`,
  `--blocked`), then the transforms using one whose status changed. It refuses
  other `--transforms-dir` or `--test-data` than the previous results'
  (`coverage-meta.json` records them, and the tconv commit), and writes
  `changes.md`, the differences from the previous results. Then
  `--table-doc doc/ldml-tconv-coverage-table.md` regenerates the per-transform
  table (with `--note` and `--cldr-note` for its header), and
  `--report-doc doc/ldml-tconv-coverage.md` the blocks of this report between
  `<!-- coverage:NAME -->` markers (summary, chart, builtins-missing,
  work-items); the rest of the report is edited by hand, from `changes.md`. For
  Any-FCD and Any-FCC:

  ```
  python3 tools/ldml/tconv_coverage.py --tconv build18/bin/tconv \
      --transforms-dir ~/transforms --test-data ~/testData/transforms \
      --out tools/ldml/tconv-coverage --update --only Any-FCD Any-FCC \
      --table-doc doc/ldml-tconv-coverage-table.md --report-doc doc/ldml-tconv-coverage.md \
      --note '…' --cldr-note 'without the local fixes of W7'
  ```

  `tools/ldml/tconv-coverage/` is ignored by git (`tools/ldml/.gitignore`).
