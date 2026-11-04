# tconv: status of the LDML / CLDR transforms

*Status of `tools/ldml/tconv` on branch LDML at `239c4cfe4`: with rules with text
to revisit composed at compile time, and prefix runs (`(P) S → A | $1`) applied
by a kernel of their own (W6, §5), overlapping string matches selected
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
`7cdeb9e` (2026-10-09), with the local fixes of W7 (`~/cldr`). October 29, 2026.*

*This is a full run of `tconv_coverage.py` (all 455 names planned, kernels
counted and CLDR test files run, `--jobs 3`, on a 15 GB machine). Unlike the
runs at `323305975` and `585ccdeba`, it uses the CLDR transforms with the local
fixes of W7, so Latin-ug and ug-Latin are implementable, and Grek-Latn/UNGEGN
and Latn-Grek/UNGEGN report their W3, W4 and W6 problems rather than the parse
problem. From the run at `585ccdeba` (371 implementable), 15 more transforms
are implementable (386): the ten Ethi-Latn romanizations (five of them
`_Geminate`), ro-ja and ro_FONIPA-ja by prefix runs, Jpan-Latn, whose analysis
no longer exceeds the memory (`308aad759`), and Latin-ug and ug-Latin by the
local fixes. The generated tables and blocks of this report are marked
`<!-- coverage:… -->`.*

*Update (October 29, 2026): prefix runs with texts S of several characters,
in families (`247f3112e`), and the triggers that produce their P (`03c89572a`), planned
again by an incremental update (`--update --only Khmr-Latn/UNGEGN`; the
families changed the plan of no transform): Khmr-Latn/UNGEGN is
implementable (387). Then prefix runs with A empty that the split does not
allow, as run rules (`96ff91dd7`, `--update --only xh-xh_FONIPA`): xh-xh_FONIPA and the
four `xh-*` transforms through it are implementable (392). Then contexts
with repetitions or optional items combined (`--update --only
Latn-Grek/UNGEGN Latn-Kana`): Latn-Grek/UNGEGN, Latn-Kana and Latn-Hira are
implementable (395). Then Latn-Grek's iota subscript rules as chain revisits,
in a group of their own (`ef4e202f9`, `--update --only Latn-Grek`): Latn-Grek remains
blocked (W3, W6), with fewer problems. Then mark run edits (`1317f8cfa`, `--update --only
Latn-Arab Latin-ThaiLogical Latn-Thai`): Latn-Arab is implementable (396).
Then MultipleCaptureMultipleReplacement matches that may end within another,
selected by the lengths of their possessive walks, with the sets of these
rules within the filter (`75e82e277`, `00bea45f0`, `--update --only byn_Latn-byn_Ethi/Xaleget my-Zawgyi
Latn-Grek/UNGEGN Latn-Hebr si-si_Latn`): byn_Latn-byn_Ethi/Xaleget and
my-Zawgyi are implementable (398). Then captures of fixed strings replaced by
the strings, and captures of sets with strings selected by their walks
(`11eb18fd0`, `--update --only Latin-NumericPinyin Latn-Bopo my-Zawgyi`): Latin-NumericPinyin
and Latn-Bopo are implementable (400). Then rules keeping the characters of
their captures as string rules, with their before contexts matched against
the converted text (`a9e44f8d8`, `--update --only blt-blt_FONIPA am-am_FONIPA my-Zawgyi`):
blt-blt_FONIPA is implementable (401).*

*Update (November 1, 2026): the engine's possessive repetitions of sets with
strings, taking the longest string (`2276cddd3`); after contexts repeating them
checked against the engine's preconditions (`0e561c416`); the sets with strings
of contexts taking their longest strings, backward in before contexts
(`22d03949b`); and the test cases whose results are ICU's, where the test data
disagrees, counted as discrepancies rather than failures (§3, `345b69bf1`), by an incremental
update (`--update --only ka-ka_Latn/BGN my-my_FONIPA cy-cy_FONIPA`). No status
changes (401); cy-cy_FONIPA's W10 problems go from 4 to 2 (it also needs W3).
The 217 failing cases of ka-ka_Latn/BGN and my-my_FONIPA are discrepancies, and
no test case fails. The other test files were not run again: their results
are those of the earlier runs, with no failures (and so no discrepancies).
Then the anchors of texts moved to their contexts (`85277dbd7`, `--update
--only ja_Latn-ko pl_FONIPA-ja fa-fa_FONIPA Zawgyi-my`): W3 is needed by 13
transforms (17 before), and W6 alone blocks 7 (4): ja_Latn-ko, pl_FONIPA-ja and
pl-ja need only W6. Then the last captures of texts of sets including the text
boundary (`9a4a0c6a6`, `--update --only` the ten transforms whose plans
changed): rules that a variable-length rule kept from being composed now are,
and W6 is needed by 11 (17); W3 alone blocks 9 (3): am_Ethi-d0_Morse (its
`([.-])' '[$]` in a filtered run), cs-ja, cs_FONIPA-ja, es-ja, es_419-ja and
es_FONIPA-ja (runs of sets with strings), besides Latin-ThaiLogical, Latn-Thai
and Zawgyi-my. Then runs of sets with strings (`b45845a25`, `--update --only
cs-ja cs_FONIPA-ja es-ja es_419-ja es_FONIPA-ja`): the five are implementable
(406), with no failing or discrepant test cases, and W3 is needed by 8. Then
insertion revisits beyond the first rule of a group (`920d7551f`, `--update
--only my-my_Latn`): my-my_Latn is implementable (407), and W6 alone blocks 6.
Then prefix runs with exceptions (`19d0a02a2`, `07b1a4c13`, `--update --only
pl_FONIPA-ja pl-ja am_FONIPA-am und_FONIPA-chr`, the last two for their plans
as printed): pl_FONIPA-ja and pl-ja are implementable (409), and W6 alone
blocks 4. Then prefix runs that only delete, revisited P and characters of S
in after contexts (`9238f5c0c`: ja_Latn-ko from 16 problems to 6), and repeated
captures of characters enumerated (`5c1ed53ee`, `--update --only sk_FONIPA-ja
sk-ja ja_Latn-ko`): sk_FONIPA-ja and sk-ja are implementable (411), and W5 is
needed by fa-fa_FONIPA alone. Then the ends of the runs of a global filter in after
contexts (`58ec782da`): am_Ethi-d0_Morse is implementable (412), and W3 alone
blocks 3. Then ICU stages that see the text around their runs, and Any-Name,
Name-Any and Any-BreakInternal applied by ICU whenever they are called for
(`0550a1483`), with Any-Hex/Plain and Hex-Any/Plain dropped from the registry
(`2770c3519`; `--update --only am_Ethi-d0_Morse Thai-Latn Any-Name Name-Any
Any-BreakInternal`): Thai-Latn is implementable (413), W8 blocks nothing, and
all 28 built-ins are implemented. The CLDR test file of am_Ethi-d0_Morse,
`d0-morse-t-am-Ethi.txt`, is not matched to the transform by its name, so
`tconv_coverage.py` does not run it; run by hand, its 613 cases pass, as one
text and each line a text, and tconv agrees with ICU on 3,000 random lines of
Ethiopic text with spaces and dashes. Thai-Latn has no CLDR test file: it
agrees with ICU on 3,507 lines of Thai text (with Latin letters, digits and
punctuation between runs). Then blocking marks for prefix runs that only
delete, revisits composed up to a return to the text, and rules left to
priority (`f2cb92480`, `--update --only it-am it-ja ja_Latn-ko ja_Latn-ru
byn_Latn-byn_Ethi/Xaleget`, the last for its plan as printed, changed by
`58ec782da`): it-am, it-ja and ja_Latn-ko are implementable (416), with no
failing test cases; W6 is needed by 5 and alone blocks ja_Latn-ru (from 8
problems to 2). On 6,000 random lines dense with apostrophes, it-am and it-ja
differ from ICU on 6 and 19, all at rules whose before contexts the plan warns
may match differently in the converted text (an apostrophe after a vowel,
deleted in the group, before `[:^Letter:] { so`; and the ~ written by a
composed rule ending at a return to the text, before `[:^Letter:] { zi`);
ja_Latn-ko agrees with ICU on 5,000 random lines (length marks, ~ chains,
geminates), and byn_Latn-byn_Ethi/Xaleget on 5,000. Then prefix runs whose P
has several characters (`0d35780df`: ja_Latn-ru's `tsu~ → | ts`, from 8
problems to 1), and revisits composed one step into prefix-run form, split with
a marker (`587215146`, `--update --only ja_Latn-ru`): ja_Latn-ru is
implementable (417), with no failing test cases, and agrees with ICU on 6,000
random lines (with `yi`, `ī`, `ji`, `dji~`, `tsu~` and the `~` runs); W6 alone
blocks no transform, and is needed by 4 (Grek-Latn, Latn-Grek,
Grek-Latn/UNGEGN and fa-fa_FONIPA, with W3, W4 or W5). Then mark run edits
left in their group with a lookahead and a mark (`2bc2e73a1`, `--update --only
Latin-ThaiLogical Latn-Thai`): Latin-ThaiLogical and Latn-Thai are
implementable (419); neither has CLDR test data, and both agree with ICU on
20,000 random lines (dense with `h`, `k`, `s`, `z`, `u` and the marks below
and above); W3 is needed by 5 and alone blocks Zawgyi-my. Then insertion rules
folded into the rules before them, and the before contexts of capture rules
translated from the converted text to the text itself (`8f13868d4`, `--update
--only Grek-Latn/UNGEGN Latn-Grek/UNGEGN Grek-Latn`): Grek-Latn/UNGEGN is
implementable (420), and W4 is needed by none. It has no CLDR test data; on
30,000 random lines it differs from ICU on 15, each a `ͺ` (deleted) before σ,
whose before context `$afterLetter`, with a repetition, is matched against the
text itself (a warning of its plan). Latn-Grek/UNGEGN is identical to ICU on
ICU's 30,000 Grek-Latn/UNGEGN results. Then lookahead chains over possessive
repetitions of sets with strings (`86cf1f797`, W10: guarded strings,
`m(?!̥) | m̥`) and cy-cy_FONIPA's stress placement (`06ce8fcce`, `--update
--only cy-cy_FONIPA`): cy-cy_FONIPA is implementable (421), its 613 CLDR test
cases pass, identical to ICU, as are 5,000 random lines; W10 is needed by none,
and W3 by 3. Two engine bugs of search and replace found on the way are fixed:
a NUL after each text (a position after the text kept), and the end of an
empty text unmarked. Then Zawgyi-my (`3d94638d0`, `5032fb2ca`, `b374fdbca`,
`f4ca44892`, `--update --only Zawgyi-my`): rules whose texts begin with a run
split (each character of the run deleted where the rest follows, a block of
rules beginning with the same run sharing one deletion), run rules with after
contexts, capture rules selected by walks of several runs, the regions of
several sets moved together, and a run within a text split with a marker:
Zawgyi-my is implementable (422), its 93 CLDR test cases pass, and it is
identical to ICU on 20,000 random lines of its range; W3 is needed by 2
(Grek-Latn, Latn-Grek, with W6) and alone blocks none. Replanning all 453
transforms changes only Zawgyi-my. Then fa-fa_FONIPA's before context
capturing a set with strings, expanded one rule per string, longest first
(`3987326eb`): W5 is needed by none, and W6 alone blocks fa-fa_FONIPA. Then
Greek-Latin's rough breathing as front-letter revisits (`3471879aa`), and
before contexts with repetitions matched against the converted text when their
after contexts, with repetitions too, are matched by the regular expression
engine (`65350b3ce`; `--update --only` the 19 transforms concerned):
Grek-Latn is implementable (423). It has no CLDR test data; on 50,000 random
lines it differs from ICU on 2, an iota subscript before a macron, out of
canonical order, at the end of a text (ICU's own normalization). The CLDR test
results of the 18 transforms with such contexts are unchanged, and
Grek-Latn/UNGEGN is identical to ICU on its 30,000 random lines (from 15
differences, its `ͺ` before σ). Then fa-fa_FONIPA's shadda rule as run moves
with marks, and deferred deletions through runs of triggers in one round
(`53121b04a`, `--update --only fa-fa_FONIPA Kana-Latn Hira-Latn Latn-Hira
ja_Hrkt-ja_Latn/BGN`): fa-fa_FONIPA is implementable (424), passes its 2,547
CLDR test cases, and is identical to ICU on 20,000 random lines and 20,000
lines with shadda runs of up to 11; Kana-Latn, whose deferred deletions now
also run in one round, is identical to ICU on 5,000 random lines with runs of
iteration marks (its kernels 1,053 → 1,149). W6 is needed by Latn-Grek alone
(with W3).*

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
| Built-in transforms (UTS #35 / ICU) | 28 | 28 |
| Transforms defined by CLDR files (380 files) | 425 | 424 (100%) |
| …of which public (not `visibility="internal"`) | 393 | 392 (100%) |
<!-- /coverage:summary -->

- **Tests.** Of the 284 CLDR test files, 261 belong to implementable transforms.
  Their 263,287 cases fail only in two files, where the test data disagrees with
  the rules and with ICU (§3): ka-ka_Latn/BGN (215 cases) and my-my_FONIPA (2).
  The coverage script runs each case as a text of its own (`tconv --lines`
  for the cases that fail in the text of all the cases), which removed 10,675
  spurious failures. The 8 files of the transforms made implementable by prefix
  runs (16,536 cases, two `_Geminate` files of 6,888 each among them) all pass,
  as does am_Ethi-am_Ethi/Geminate (6,888), whose first compilation no longer
  overflows the JIT thread's stack (`fa7352b10`).
- **Prefix runs** (W6, §5), `([ሀ-ሇ…]) $ጥበቅ → $ሀ | $1`: a rule that revisits its
  prefix P after a text S of one character turns each run P S…S into A…A P.
  These rules, where no other rule of the group interferes, are split into a
  group of their own and applied by one kernel: the positions to replace are
  found by `MatchStar`, P's characters are carried to the end of the run, and
  replacements A of other than one character are written by insertion and
  multiplexed selectors. The ten Ethi-Latn romanizations, ro-ja and
  ro_FONIPA-ja are implementable. Texts S of several characters are matched
  in families (the rules with the same P, whose texts may follow each other
  in a run), and the rules that produce P, as Khmer's `ក → k | â`, are
  applied first, in a group of their own, except where a covering rule
  (`្ក → kâ`) consumes their character: Khmr-Latn/UNGEGN is implementable.
  Where the split is not allowed, a prefix run with A empty becomes run rules
  if the other rules match P only as single characters: xh-xh_FONIPA's
  `rh → | r` (kept from the split by `gr → ɣ`) is `r h+ → r`, and the five xh
  transforms are implementable.
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
  ThaiLogical-Thai are implementable, and Thai-Latn was left blocked only by
  `Any-BreakInternal` (W8), now applied by ICU (§2.1).
- **Built-ins** (§2.1): Hex-Any and its variants (Java, C, Perl, Unicode, XML,
  XML10), with the bare Hex-Any decoding all the forms, Any-CaseFold, the
  normalizations Any-FCD and Any-FCC of ICU, identical to ICU 78.3's
  Normalizer2 on `QA/Normalization` and on random text, Any-Hex/XML10
  (decimal), exact on every codepoint, and Any-Remove; Any-Name, Name-Any and
  Any-BreakInternal are applied by ICU wherever they are called for.
- **Insertion rules** (W4, §4.2), such as `a { } [$E $EO $EU] → $sep`: in groups
  of their own, and in groups with single-character rules, whose before contexts
  then see the text inserted, as in ICU, and in groups with string rules,
  folded into the rules whose results end with a character of their before
  contexts (§5). With them, ConjoiningJamo-Latin, Hang-Latn and Jamo-Latn,
  my-my_FONIPA (and my-ar, -chr, -fa), Han-Spacedhan (and Hani-Latn,
  Hant-Latn, Han-Latin/Names) and Grek-Latn/UNGEGN are implementable.
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
  - W3 variable-length texts: none alone (Latin-ThaiLogical and Latn-Thai are
    implementable with mark run edits left in their group, cy-cy_FONIPA with
    capture rules over sets with strings, Zawgyi-my with runs split, and
    Grek-Latn with front-letter revisits, §5; Latn-Grek also needs W6);
  - W6 revisits neither composed nor prefix runs: none alone (Latn-Grek's
    mirror of Greek-Latin's rough breathing, with W3; fa-fa_FONIPA's
    `([َ ِ ُ])ّ → ّ | $1` is applied as run moves with marks, and it-am, it-ja,
    ja_Latn-ko and ja_Latn-ru are implementable, §5);
  - W5 back-references: none (fa-fa_FONIPA's copy of a set with strings is
    expanded, §5);
  - W8 built-ins: none (Thai-Latn's `Any-BreakInternal` is applied by ICU);
  - W7 parser semantics: none, with the local CLDR fixes.

  Each of these is concentrated in a few rule files (§5).

<!-- coverage:chart -->
```
cumulative implementable CLDR-defined transforms (of 425), greedy order
now                          424  ████████████████████████████████████████
+ W6 revisit                 425  ████████████████████████████████████████
```
<!-- /coverage:chart -->

These counts are lower bounds on the remaining work. `planTransform` reports one
problem per rule and skips the rules it cannot implement, so some problems
surface only after others are fixed.

## 2. What works

### 2.1 Built-in transforms (28 of 28)

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
`--EnableAsserts --VerifyIR`. The one
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
  `createFromRules`. With `--EnableAsserts --VerifyIR` the output is identical.

<!-- coverage:builtins-missing -->
Not implemented: none.
<!-- /coverage:builtins-missing -->

`Any-Name`, `Name-Any` and `Any-BreakInternal` are applied by ICU (if tconv is
built with it, and unless `--use-ICU=0`) wherever they are called for: by name,
or as stages of the transforms whose rules invoke them, at any depth (and of
rules files), between the pipelines of tconv's own steps (`0550a1483`). They
are planned as implementable (`--plan` marks them "applied by ICU"), and
`--kernel-counts` leaves them out. Within a global filter, an ICU stage
transforms each run of the filter's characters with the text around the run
as its context (`finishTransliteration`), as ICU does: Any-BreakInternal's
dictionary word break then sees the characters before the run, and finds no
break after a Thai mark that begins a run after a character outside the
filter (`aีพ → aīph`, not `aī ph`). `Any-Hex/Plain` and `Hex-Any/Plain` are no
longer in the registry (`2770c3519`): ICU4C has neither, Any-Hex/Plain is
ICU4J's alone (the Unicode form without its `U+` prefix, which cannot be
reversed), and no ICU has Hex-Any/Plain. `Hex-Any` is no longer an alias of
`Hex-Any/Java`: as in ICU, it accepts every escape form.

### 2.2 Transforms defined by rules (395 of 425)

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
  de-ASCII, hy-hy_Latn/BGN, Latn-Hebr, tr-Lower and az-Lower, and ug-Latin and
  Latin-ug (with the local fixes of W7);
- with MultipleCaptureMultipleReplacement rules: si-si_FONIPA, si-si_Latn,
  ta-ta_FONIPA, si-ar, si-fa, si-chr; with identity rules, vec-vec_FONIPA;
- with insertion rules: ConjoiningJamo-Latin, Hang-Latn, Jamo-Latn,
  my-my_FONIPA, my-ar, my-fa, my-chr, Han-Spacedhan, Hani-Latn, Hant-Latn and
  Han-Latin/Names;
- with captures in reverse order: Thai-ThaiLogical and ThaiLogical-Thai;
- with function calls on captures: az-, el-, lt- and tr-Title,
  Latn-Ethi/ALALOC and Latn-Ethi/SERA;
- with captures of before contexts copied: Kana-Latn, Hira-Latn and Jpan-Latn;
- with prefix runs: Ethi-Latn and its romanizations Aethiopi, ALALOC,
  Beta_Metsehaf and IES_JES_1964 (each also `_Geminate`) and Lambdin, ro-ja and
  ro_FONIPA-ja, and with their triggers, Khmr-Latn/UNGEGN; as run rules,
  xh-xh_FONIPA, xh-am, xh-ar, xh-chr and xh-fa;
- with contexts with repetitions or optional items combined: Latn-Grek/UNGEGN,
  Latn-Kana and Latn-Hira;
- with MultipleCaptureMultipleReplacement rules matched by exact starts, selected
  with string rules, or expanded: am_FONIPA-am and the `*-am` transforms through
  it, am-am_FONIPA, am-ar, am-chr, am-fa, NumericPinyin-Latin and Bopo-Latn;
- with MultipleCaptureMultipleReplacement rules whose matches may end within
  another, selected by the lengths of their possessive walks:
  byn_Latn-byn_Ethi/Xaleget and my-Zawgyi (identical to ICU 78.3's
  `createFromRules` on their files' rules on 40,000 and 2,000 random lines;
  neither has CLDR test data);
- with captures of fixed strings replaced by the strings, and sets with strings
  selected by their walks: Latin-NumericPinyin and Latn-Bopo (identical to
  ICU on 10,000 random pinyin lines each; no CLDR test data);
- with the ends of the runs of a global filter in after contexts:
  am_Ethi-d0_Morse;
- with a built-in applied by ICU as a stage: Thai-Latn (`Any-BreakInternal`);
- with mark run edits in a group of their own, and left in their group with a
  lookahead and a mark: Latin-ThaiLogical and Latn-Thai through it;
- with blocking marks after prefix runs that only delete, and rules left to
  priority: it-am and it-ja; with revisits composed up to a return to the
  text: ja_Latn-ko; with prefix runs of P of several characters, and a revisit
  composed one step and split with a marker: ja_Latn-ru;
- with insertion rules folded into the rules before them, and the before
  contexts of capture rules translated to the text itself: Grek-Latn/UNGEGN;
- with lookahead chains over possessive repetitions of sets with strings, and
  capture rules whose starts the engine finds with possessive captures:
  cy-cy_FONIPA;
- with leading runs split (also in blocks, and before a rule matching at their
  last character), run rules with after contexts, capture rules selected by
  walks of several runs, regions of several sets moved together, and runs
  within texts split with a marker: Zawgyi-my;
- with front-letter revisits (the rough breathing's letters written before the
  runs of vowels), and before contexts with repetitions matched against the
  converted text with their after contexts matched by the engine: Grek-Latn;
- with run moves (the shadda after a vowel, written before the vowel's
  conversion) by marks, and deferred deletions through runs of triggers in
  one round: fa-fa_FONIPA.

The per-transform status is in the table.

### 2.3 Verification

**CLDR test data.** Every test file in `common/testData/transforms` is mapped to
its transform by alias, and those of implementable transforms are run:

- 261 files, 263,287 cases;
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

**Prefix runs (W6, `71749b4e9`, `239c4cfe4`).** `QA/tconv_prefix_runs_test.py`
(ctest `tconv_prefix_runs_test`) checks four rule sets against ICU's results:
captured and literal prefixes, replacements of zero to three characters, a
filter, and a run of 600 (longer than a block). Six transforms with prefix runs
are identical to ICU 78.3 on 408 random texts each, and a 15 MB file gives the
same output with 1 and 3 threads. The CLDR test files of the transforms made
implementable (8 files, 16,536 cases) all pass. Of all the transforms, only the
12 made implementable change status.

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

## 3. Correctness: known failures and discrepancies

The test files with failing cases (tconv's result differs from the expected one
and from ICU 78.3's):

<!-- coverage:failing-tests -->
No test file fails.
<!-- /coverage:failing-tests -->

**Discrepancies.** A case whose expected result differs from tconv's, where
tconv's is ICU's (the transform ICU 78.3 makes from the same rules, `tconv
--use-ICU`, each case a text of its own), is a discrepancy, not a failure: the
test data disagrees with the rules, tconv and ICU.

<!-- coverage:discrepant-tests -->
- ka-ka_Latn/BGN (ka-Latn-t-ka-m0-bgn-2009.txt: 215 of 682 cases)
- my-my_FONIPA (my-fonipa-t-my.txt: 2 of 1228 cases)
<!-- /coverage:discrepant-tests -->

- ka-ka_Latn/BGN: the test data expects `ʼ` (U+02BC) where the rules give `’`
  (U+2019);
- my-my_FONIPA: the expected results end with a space.

(Both were reported as failures before the discrepancies were told apart, at
`a9e44f8d8`.)

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

A rule whose before context can be matched against the converted text but
whose after context has a repetition too (`$ignore [:Uppercase_Letter:] {
$rough } $ignore [^…]`, from `DisambiguateOrder`) was matched by the regular
expression engine, both contexts in the text itself (`65350b3ce`); its after
context is now matched by the engine (the positions of its characters where it
holds, given to the correction kernel), and its before context against the
converted text, as for insertion rules. Matching only some of them against the
converted text is not safe: rules whose contexts `DisambiguateOrder` made
complementary must be matched against the same text, or neither may apply
(ky-ky_FONIPA left `к` unconverted in words like `бисидк` until its
repetitions were matched too; Greek-Latin left a rough breathing after a
deleted smooth breathing, `Υ̓̔`, until these after contexts were). Ten
implementable transforms keep the warning of `tconv --plan`, for before
contexts that are not fused (more than one repetition, as lt-Title's
`[:softdotted:] […]* […]*`, or several items beyond a repetition): Hans-Hant,
Latin-InterIndic, Latn-Ethi/SERA, Latn-Grek/UNGEGN, Zawgyi-my, it-am, it-ja,
und_FONIPA-ar, lt-Title and nl-Title.

**Tested against ICU** (rules applied by `createFromRules`): 27 rule sets with
deletions, chains, expansions, fallback rules, selected and covered string rules,
and repetitions, each on 5,000 random strings, are identical to ICU. Without the
correction, between 108 and 1,853 of each 5,000 differ.

### 3.2 Robustness

The limitations of tconv itself (not of the regular expression engine or the
Parabix pipeline framework on which it runs):

- **Jpan-Latn compiles slowly.** Its pipeline, with the deferred rules of
  Kana-Latn twice (also through Hira-Latn), compiles only since the positions
  of the deferred rules are inserted once (`edbf0b88f`) and its planning no
  longer takes gigabytes (`2f882cba6`): with the default of 4 rounds, on an
  8 GB machine, the first compilation takes 228 s (300 s of CPU) and 1.5 GB,
  and runs from the object cache 3 s and 274 MB. It is identical to ICU 78.3's
  `createFromRules` (the rules of Japn-Latn.xml, with its `::` steps) on 509
  lines: 9 of Japanese text and runs of marks, and 500 random lines of up to 20 characters mixing
  the Han characters of its rules, kana, iteration marks, punctuation and ASCII.
  Consecutive runs of iteration marks that reach back to each other may need
  more rounds than the default, as in Kana-Latn (see below). Before, it compiled only with `--deferred-rule-rounds` of 1 or 2
  ("failed to allocate virtual address space", or a Z3 error at the default
  of 4), and its analysis exceeded 13 GB.
- **Rounds of the deferred rules.** The copying rules are applied in rounds
  (`--deferred-rule-rounds`, default 4): a chain of more triggers whose before
  contexts reach back to each other leaves the last ones unchanged, unless it is
  a run of a self-reproducing rule (`カヽヽヽヽヽ`, `ナヾヾヾヾヾ`, any length,
  see `DeferredRuns`), and of rules known to reproduce the same block: a
  self-reproducing rule that wrote the block just after the same block (in any
  round) continues the run too, so that a run switching between plain and
  voiced marks (`ハヾヾヽヾヽ`) takes at most three rounds (a rule that is not
  self-reproducing, as `k … → g`, then the first mark of each kind). A run whose
  marks' before contexts reach back to an earlier run (`…ヂヽヾサヾヽヾ`) waits
  for it: on 4,000 random lines of one to four such runs, 143 differ from ICU
  with 4 rounds, 12 with 6, none with 8 (single runs: none with 4). Triggers
  outside the filter (the ヽ of Hira-Latin, which filters ヾ only) are
  characters like any other: before, they kept the triggers after them from
  applying. The positions the rules may write are inserted once, before
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

In the run at `239c4cfe4`, the 386 implementable CLDR-defined transforms have
a median of 228 kernels. The largest are am_FONIPA-am (2,614; its own groups have
602 rules in 2,465 kernels), the `*-am` transforms through it (up to 3,093,
my-am) and Jpan-Latn (3,027). Other large groups are NumericPinyin-Latin (75
rules, 2,206 kernels) and Latin-ASCII (847 rules, 650 kernels). Over all the
transforms, 331 conversion groups have 74,326 rules. A group of prefix runs
takes 5 kernels (ro_FONIPA-ja) to 12 (the Ethiopic romanizations, whose
replacements have one to three characters).

## 5. What is not covered

Table 1: work items, by the number of transforms that need them (own problems and
those of invoked transforms).

<!-- coverage:work-items -->
| work item | needed by | the only blocker of | effort |
|---|---:|---:|---|
| W3 variable-length texts | 1 | 0 | M–L (the rest) |
| W6 text to revisit, not composed | 1 | 0 | L (with W3) |
| W5 back-references | 0 | 0 | M |
| W7 parser semantics (#5, #6) | 0 | 0 | S |
| W10 engine lookaheads | 0 | 0 | M |
| W4 insertion rules (with string rules) | 0 | 0 | M |
| W8 built-ins (Any-BreakInternal) | 0 | 0 | L |
| W9 Jpan-Latn (Hira-Latn, Kana-Latn) | 0 | 0 | L |
<!-- /coverage:work-items -->

(This run uses the local CLDR fixes of W7: W7 blocks nothing, Latin-ug and
ug-Latin are implementable, and the two Greek UNGEGN transforms are
implementable.)

Done since the first assessment:

- W1 filters (`9dea741`);
- W2a single-character rules within string rules (`9119ae0d`);
- W2b1 one-way string rule overlaps (`1ed0394e`);
- W2c expanded non-final characters (`029371343`);
- W6 revisits, where composition is finite (`eea89e00d`), and prefix runs
  (`71749b4e9`, `239c4cfe4`);
- W2b2 two-way, cyclic and self-overlapping string rules, by sequential
  selection (`c230d569a`);
- W3 finite texts, runs and SingleCaptureDoubleReplacement rules (`379de8796`);
- W3 MultipleCaptureMultipleReplacement rules (`1bc9ef253`), identity rules and
  starts by the first set of B1 (`08c303c30`);
- W4 insertion rules (`3b6ee29fa`, `4f26e7d02`, `a3a64eda2`; in groups with
  string rules, folded, `8f13868d4`), and the scale of Han-Latin (`02e77ee59`);
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
below), as in this run.

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
- **Prefix runs** (`71749b4e9`, `239c4cfe4`). A rule `(P) S → A | $1` (or
  `p S → A | p`), with S a set of one character, revisits its prefix, which
  matches again: ICU turns each run P S…S into A…A P. Where no other rule of
  the group can match within such runs, sees them in its contexts, or matches
  the replacements (`prefixRunConflict` checks this), these rules are split
  into a group of their own, before the rest (`:: Null ;` between them), and
  applied by the `PrefixRuns` kernel: `laS = Lookahead(S, 1)`, the run
  `MatchStar(Advance(P & laS), S) & S`, P's bits carried by `MatchStar` to the
  last S, and A written at P and at the other S of the run. Replacements of one
  character are written in place; otherwise the kernel gives a mask of the
  positions of each A (multiplexed into ⌈log₂ k⌉ selector streams for k
  replacements), the positions are spread by `InsertionSpreadMask`, the
  `PrefixRunFill` kernel writes each A from the selectors, and the positions of
  empty replacements are deleted. The Ethiopic gemination rules
  (`([ሀ-ሇ…]) $ጥበቅ → $ሀ | $1`) and ro_FONIPA-ja's `t͡ → | t` (A empty: the tie
  bars of a run are deleted) are prefix runs;
  `tconv --revisit-report` lists every revisit rule with its outcome, and
  `doc/ldml-revisit-classification.md` classifies them. Where the prefix runs
  only delete (A empty, no triggers), the last item of an after context may
  see their P, which stays in place (`19d0a02a2`). And a rule before such a
  prefix run that applies at its P S, `P S X } C → R` (pl_FONIPA-ja's
  `t͡ʂa → ツァ`, `t͡ʂ } $word_boundary → チ`, before `t͡ → | t`), is an exception
  of the prefix run rather than a conflict (`07b1a4c13`): ICU deletes S until
  `P S X` remains, to which it applies the rule, so the prefix run is applied as
  `P S* { S } (?!X C) → ;` in a group of its own, keeping the last S where an
  exception follows, for the rule to apply in the rest of the group.
- **Insertion passes** (`27321f4e9`, `920d7551f`). A rule inserting a fixed
  string between its captures and revisiting all, `(T1) (T2) → | $1 M $2`, is
  the insertion rule `T1 { } T2 → M` in a group of its own before its group,
  where the rest of the group cannot see the difference (`insertionRevisit`).
  It need not be the first rule of its group: where T1 and T2 are sets, no rule
  before it may apply where it matches (begin with a character of T1 followed
  by one of T2, or with one of T1 alone), and a character of T1 within the text
  of another rule, which ICU does not reach as a position for the insertion,
  is allowed where it is followed by a character not in T2: my-my_Latn's
  `($consonants) ($consonants) → | $1 a $2`, rule 412 of about 560, despite
  `($consonants) ြောင့် → | $1 yount` (whose `င` is followed by `်`).
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
- **Blocking marks** (it-am, it-ja; `f2cb92480`). Their group begins with
  `($consonant) \' → | $1 ; \' → ;`, so every apostrophe is deleted, and its
  only effect is on what matches (with ICU's `createFromRules`): a run of
  apostrophes after a consonant at which the cursor is, the first character of
  a match or a revisited one, is deleted before the rules are tried there
  (`c'ha` as `cha`, `K`; `b'sa` as `bsa`), but one after a consonant within a
  match blocks it (`ch'a` gives `Ca`, not `K`; `c'h'a`, `ch''a` likewise; the
  after context of `($consonant) s } $vowel` sees it in `bs'a`), and one after
  the last character of a text without an after context changes nothing
  (`ch'`). Applied before the group, the prefix run makes the blocked matches
  match: 27 rules of each file have a consonant within their texts (`cha` …
  `cho`, `ghi`, `gli`, `gna` …, `thi` …, `sci`, `cqu`, `tzu`, and the two
  `$1 \~s`, `$1 \~z` rules), and the 14 revisit rules not composed fail on
  chains through the apostrophe rule. Which consonants are cursor positions
  depends on the matches, so no change of the deletions alone is exact. The
  design: the prefix run step also gives a stream D of the positions that
  follow a deleted run; each rule of the rest of the group has the offsets at
  which a deleted run is transparent (1, after its first character; for a
  composed rule, also those after the points where its chain revisits); and a
  match with D at another offset within its text or after context does not
  apply. The condition is local to each match (it does not depend on the
  selection), and is checked wherever the group finds matches: the fused
  kernels, the engine's context matches, and the MultipleCaptureMultipleReplacement
  rules. With it, two conflicts of prefixRunConflict are relaxed where the
  prefix runs only delete: a revisited P that is the original character at its
  position (`tz → | zz`), and triggers whose texts are within P (it-ja's
  `k → | c`, `q → | c`, whose runs are already deleted). It would make it-am
  and it-ja implementable.

  As implemented (`f2cb92480`), D is the stream of the positions after the
  runs that prefix runs deleted (where only deleting, as the kernel leaves P
  at the end of its run: the kept positions after a moved P), and the rules
  of the group after them have *blocking marks* (`ldml::DeletedRunBlockMark`,
  U+FDD4, a noncharacter in their sets): a marked set matches its characters
  only where D does not hold. The marks are where a match may follow a deleted
  run that ICU would still have found: after a character of P other than the
  first of the text (`BlockMarker`), and, for a composed rule, at the
  boundaries its path crosses, other than where ICU revisited a character of
  P just before the boundary (and so deleted the run there first). With them,
  `prefixRunConflict` allows after contexts that see the texts of the prefix
  runs and, where the rest of the group deletes S wherever ICU reaches it
  (`' → ;`), P within texts and revisited at the end of a text to revisit.
  The pipeline gives D to the class kernels (`NotAfterDeletedRuns`) and to the
  engine (an external, intersected: `re::resolveDiffs` would fold a
  difference with a name defined as a CC). The composition of the revisits
  then failed on unbounded chains such as `ss → | \~s` on `sss…`; a
  composition that fails is retried ending its paths where the text to
  revisit returns to the text of the composed rule, as the suffix revisits do
  (`s } s → ~`), which also made ja_Latn-ko implementable. DisambiguateOrder
  does not see the marks as characters; where it cannot keep a later rule
  apart from a marked earlier rule (the composed alternatives `ss [a﷔] → ሣ`
  and `ssa → ~ሣ`, the second for where a run was deleted), it leaves the pair
  to priority: the later string rule, or subgroup of single-character rules,
  applies only where the earlier rule does not match at the same position
  (`starts & ~earlier`, as the deferred rules' kernel takes the first rule
  matching; `TransformStep::stringPriority`, `subgroupPriority`, printed by
  `--plan`). The remaining differences from ICU are at rules whose before
  contexts the plan warns may match differently in the converted text.

- **Prefix runs of P of several characters** (`0d35780df`). ja_Latn-ru's
  `tsu~ → | ts` deletes the runs of `u~` after `ts` (`tsu~u~a → ца`). A rule
  `p1 … pk S → | p1 … pk` (literal characters, only deleting) is a prefix run
  with P its last character, preceded by the others (`PrefixRun::prefix`), in
  a group of its own after the other prefix runs: `te~ → | t` brings `t` next
  to `s` in `te~su~a`, which ICU then deletes too (`ца`). The pipeline gives
  P's class only where the characters before it precede it (`PrecededBy`),
  and carries the positions after the runs deleted before through these
  deletions. An earlier rule matching P's sequence is a conflict, P's first
  character may not be matched after the first character of a text, and the
  run after such a P at the start of a text is transparent. The after
  context of `\~ } tsu → ;` sees the run's `u` (`~tsu~a → ца`, but
  `~tsa → ~ца`): its last item, after `s`, also matches after a deleted run,
  as the engine matches `t (s followed by a deleted run) | t s (u not after
  one)`, at the end of the text too.
- **One step of composition, split with a marker** (`587215146`).
  `i $lengthMarker → | i i` revisits `ii`, of which the first `i` becomes `и`
  unconditionally (`ī̄ → иии`): `Composer::prefixRunStep` composes that step,
  `i $lengthMarker → и | i`, a prefix run. Applied before the group, it would
  not be exact: `yi → и` consumes the `i` (`yī → и`, the mark then deleted),
  as does `dji~`, and `j → | zy` makes `y` before `i` as it goes. So the rule
  is split with a marker M (U+FDE0, a noncharacter) that keeps ICU's cursor:
  `i } $lengthMarker → M` where the rule was, `M { $lengthMarker → | i` first
  in the group (whose composition is finite, `M { lM } lM → M`), and `M → и`
  in a group after it (where no rule's before context could see `и`). The
  blocking marks also allow prefix runs that replace their texts with as many
  characters beside those that delete.
- **Mark run edits left in their group, with a lookahead and a mark**
  (`2bc2e73a1`). Latin-ThaiLogical's `h ($notAbove*) ̄ → ห | $1` cannot be
  applied before its group: ICU does not reach the `h` of `k̄h̄` (`k̄h → ข`
  takes it), nor, by parity, the last `h` of an even run (`h → | k` makes
  `kh } $notHAccent0 → ค` of each pair: `hh̖̄ → ค̖̄`, `hhh̖̄ → คห̖`). Such an
  edit `P R* T → C | $1` stays in the group as `P } R* T → C M`, M a
  noncharacter (`RunEditMark`, U+FDD8 on, one for each R and T), so that the
  group's own selection decides where ICU reaches P, and a group after it
  deletes M with T, `M (R*) T → | $1`, a mark run edit. ICU revisits R* with
  T deleted, the group sees M and T, which it cannot tell when no rule of the
  group begins with a character of R or T and none has a before context. An
  edit is also no longer moved before its group when another rule revisits
  text that may begin its P: `z → | s` makes the `s` of `z̄`, which ICU then
  edits (`z̄ → ส`). Latin-ThaiLogical's `f`, `a`, `i` and `ụ` edits are still
  applied before the group, its `h`, `u`, `s` and `ṣ` edits with lookaheads.
- **Revisits that feed a folded insertion** (`8f13868d4`). Grek-Latn/UNGEGN's
  `ϲ → | σ` and `Ϲ → | Σ` revisit a character after which the insertion
  `[Pp] { } [ΣςσϷϸϺϻ] → \'` may apply: folding the insertion (below) with
  `ϲ`, `Ϲ` added to its after context composes them.

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
  - unbounded runs that are not prefix runs, because other rules interfere
    (it-am's `($consonant) ' → | $1`, fa-fa_FONIPA's `یّ`,
    pl's after contexts, ja_Latn-ru's `\~ } tsu → ;`) or P has two
    characters (ja_Latn-ru's `tsu\~ → | ts`);
  - rules with variable-length texts that may apply to the text to revisit:
    Grek-Latn's `($lcgvowel+ $ddotmac?) $rough → h | $1`, am_Ethi-d0_Morse's
    `([:lo:]) ([:zs:]+) ([:lo:]) → | $1 ⁄⁂⁄ $2 $3`.

  Both are W3 cases.

**Front-letter revisits** (`3471879aa`, `ldml::FrontLetterRevisits`). Greek-
Latin's 16 rough breathing rules write H or h before a run of vowels and delete
the rough breathing after it, revisiting the run (`($gvowel+ $ddotmac?) $rough
→ H | $1`, `Α ($glower $macron?) $rough → H | α $1`): ICU applies them again at
the same position while they match, each pass taking the next rough breathing,
so that all the letters precede the run (`αωωἑἁν → hhaōōean`), the letter of
each pass chosen anew (`ἱὝ → hHiÚ`). The family is recognised by its form and
set aside before revisits are composed (the iota subscript rules around it then
compose), and applied before the rest of its group, all at once: the passes of a
start take the first rough breathing after it and every one up to the first
blocker after the start (a character other than a vowel, a rough breathing or a
diaeresis or macron, or one after a diaeresis or macron), so that the starts are
the positions where a rule of the family applies first in the source, but for
those with the same first rough breathing as one before, or reached by one
before with no blocker between; two further starts arise from the rough
breathings the passes delete (an uppercase vowel followed by them, a lowercase
consonant and a rough breathing, and `ϵ` after a start by the second form).
Conditions that depend on how a run ends are computed at its end and shifted
back to its start (`IndexedShiftBack`); the letters of each start are counted
among the rough breathings taken. The rest of the group sees the text without
them, the vowels lowercased by the first forms, and before each start its
letters as placeholders (Latin letters of the case of the start's vowel, outside
the filters, so that the contexts of the rules applying before it see what
ICU's see), which become H and h after the group's last step (the planner splits
Greek-Latin's group in three, joined by `:: Null ;`). The decisions were checked
against ICU's passes, rule by rule, on 50,000 random lines. Latn-Grek's mirror
rules (`h ($evowel …) → | $1 $rough`) are not covered.

**Run moves** (`53121b04a`, `ldml::MarkRunMoves`). fa-fa_FONIPA's `([َ ِ ُ])ّ → ّ |
$1` (NFD puts the vowel before the shadda) writes the shadda and revisits the
vowel, which ICU then matches again, so that the whole run of shaddas is
written and the vowel converted with what follows the run (`ِّی → ّej`). The
rule is replaced, before revisits are composed, by a mark for each rule E of
its group beginning with the vowel (text P T, result R), in the group's order:
`P } ّ+ T C → M` where the rule was (C E's after context), so that the group's
own selection decides where ICU reaches the vowel (`اِّ` takes it with the
alef); first in the group, `M ّ+ { T → R`, or for an empty T, `M ّ* { ّ } ّ →
ّ` and `M ّ* { ّ → ّR` (the result at the last shadda: ICU inserts nothing at
the end of a text); and the marks deleted in a group after it. The marks are
supplementary noncharacters (`RunMoveMark`), one for each T and R (10). The
rewritten rules give ICU's results for the original ones on 42,547 lines. It
requires that no rule before the moved one begins with a vowel and no other
rule with the shadda or with text to revisit, that the rules E have fixed
nonempty results and no before contexts, and that no before context of the
group may see a shadda followed by the first character of a result (where it
could reach a mark). An earlier version composed the move into capture rules,
`P (ّ+) T → $run R`: the group then selected its string and capture rules
together, without the correction of its before contexts matched against the
converted text (52 of the 2,547 CLDR cases differed from ICU).

**Deferred deletions through runs** (`53121b04a`). A deferred rule deleting its
trigger alone (no replacement, no copy) leaves the converted text before the
next trigger as it was, so in each round it now also applies through the run
of triggers after one where it applied, as long as its text and after context
match there and those of no rule before it do where they did not at the start
of the run (whose before contexts then failed, and fail again). Runs of more
than `--deferred-rule-rounds` (8) triggers were left partly undeleted, one per
round: fa-fa_FONIPA's last group, `([$IPA_CONSONANT$VOWEL\|]) { ّ → $1` and
`ّ → ;`, kept shaddas after a vowel's run of 9 or more. Kana-Latn's two deletions of
`ヽ` (without a voicing mark after it) take the same path.

Prefix runs then made 12 more transforms implementable (the ten Ethi-Latn
romanizations, ro-ja and ro_FONIPA-ja), and removed the W6 problems of sk-ja
and sk_FONIPA-ja. Prefix runs of several characters, with their triggers
(`ក → k | â`, applied first except where `្ក → kâ` consumes the consonant),
made Khmr-Latn/UNGEGN implementable, and run rules for prefix runs with A
empty (`rh → | r` as `r h+ → r`) the five xh transforms. W6 is now needed by
22 transforms, and alone blocks 7. Contexts with repetitions or optional
items, combined item by item, made Latn-Grek/UNGEGN, Latn-Kana and
Latn-Hira implementable: W6 is needed by 19, and alone blocks 4. Latn-Grek's
iota subscript rules, which revisit their capture extended by U+0345 to the
next `i`, are chain revisits, applied first by a Pablo kernel (a MatchStar
through the run of accents); its rough and smooth breathing rules remain.
Mark run edits, `P ($R*) T → C | $1 ;` applied first by one Pablo kernel per
group (on the starts of `[P1] R* T` from the regular expression engine), made
Latn-Arab implementable and left Latin-ThaiLogical and Latn-Thai blocked by
W3 alone; edits left in their group with a lookahead and a mark (§5, W6) made
them implementable too.

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
  A run of a set with strings, `[{i̯} i ɪ]+ → i` (cs_FONIPA-ja, es_FONIPA-ja;
  `b45845a25`), is the longest run of positions each of a character of one
  character of the set or within an occurrence of a string (found at its first
  position by lookahead, the rest of it by advances), by `MatchStar`: ICU's run,
  taking the longest string at each step, where each string begins with a
  character of one character of the set and no proper suffix of a string is a
  proper prefix of a string (`[{ab} {bc}]+` on `abc` takes `ab` alone, which
  the positions would not tell, and is not planned).
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
- **Anchors** (`ldml::MoveTextAnchors`, `85277dbd7`). ICU matches `^` at the
  start of a text and `$` at its end as anchors of the text being
  transliterated: without a before (after) context they become it (`^ k } g →
  ᆺ`, `n $ → ᆫ`). A set including the text boundary as the last item of a text
  (`[^b]`, `[x $]`, `[$]`) matches a character of the set or, matching nothing,
  the end of the text: such a rule becomes two, the text with the characters
  of the set, and the text without it, with the after context `$` (`a [^b] →
  X` replaces a final `a`). In the regular expression engine, `^` matches at
  the region start and `$` at the region follow, past the last character.
  With a global filter and transform rules, ICU applies each rule set to each
  run of the characters of the filter (of the text given; contexts see beyond
  the run, and the anchors are those of the text), and such a set also matches
  at the end of a run (`:: [a-z] ; :: Null ; b a [$] → X ;` gives `X1` on
  `ba1`). The text of a rule set does not tell where the runs end (the
  characters a step before inserts are within the run of the character they
  follow), but the filter's mask, which the pipeline carries through the
  steps, does: the end of a run is a position whose next is not within the
  mask (or the end of the text). In an after context, such a set's variant
  without a character is then the end of the text or `FilterRunEndMark`
  (U+FDD2, a position outside the filter), and its character variant is
  restricted to the filter's characters; tconv matches the mark from the mask,
  as an external stream of the engine (`RE_context.externals`) or in fused
  classes (`58ec782da`). This made am_Ethi-d0_Morse's `([.-])' '[$] → $1`
  implementable. A capture of such a set, the last item of a
  text, likewise becomes two rules, the second with the capture empty (its
  references removed); revisited unchanged, as Zawgyi-my's `^ ၄ ($nondigits) →
  | ၎ $1`, it is instead the after context (`^ { ၄ } $nondigits → | ၎`), from
  which ICU revisits the text alike (`9a4a0c6a6`).

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
  string of a set.  (Since `2276cddd3`, the engine takes the longest string
  itself, and `0e561c416` removed the ordering.)  Contexts did not take the longest string of a set with
  strings that was not repeated, nor, in before contexts, of a repeated one
  (taken from the right): a string's proper prefix (after) or suffix (before)
  in the set could match instead (`a [{ab} b] { c` matched `abc`). They now do,
  with the set made unambiguous outward from the text, `w (?!t')` or `(?<!t')
  w`, in the engine's patterns and in the fused kernels (`22d03949b`,
  `QA/tconv_context_strings_test.py`).
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
  and starts that cannot be told apart), now implementable (below).

The W3 rules that remain mostly also have other problems:

- back-references (W5);
- runs followed by an after context (Grek-Latn/UNGEGN, cy-cy_FONIPA, Zawgyi);
- runs of sets with strings (`[{i̯}ij]+`);
- text boundaries in the text (`\. ([$])`);
- MultipleCaptureMultipleReplacement rules whose starts cannot be told apart
  and whose walks have more than one run (Zawgyi-my, now selected by such walks).

**Zawgyi-my** (`3d94638d0`, `5032fb2ca`, `b374fdbca`, `f4ca44892`) is
implementable with five mechanisms:

- *Leading runs split* (ldml::SplitLeadingRuns): R{n,} T → C becomes
  R } R{n,} T → ; (each character of the run but the last n deleted where T
  follows) and R{n} T → C in its place, as ICU applies the rule at the first
  character of the run it reaches (`ြ* ႊ → ွှ`). A block of rules beginning
  with the same run (the five `ေ+` rules) shares one deletion,
  R } R{n,} (T1 | T2 | ...) → ; then R{n} Ti → Ci in order. No earlier rule
  of the group may match within such a run; one that may match at its last
  character only (`($wspace) ့` before `($wspace+) (mark) → $2`) is preceded
  by the rules R{k} Ti → Ci (k = max(n, 2)), which take the last k characters
  of the run, those of the shorter runs staying in place.
- *Run rules with after contexts*: a text followed by the longest run of a
  set, deleted, with an after context that holds after the run (S* A): the
  U+200B runs at the start and the end of a text.
- *Walks of several runs*: a possessive walk may have further runs after its
  first, each followed by sets; LeftmostMatches covers a selected match
  through the segments of its runs, and WalkSpans spans it
  (`($umedial*) (ေ+) ($umedial*) → $1 $3 $2`, five variants).
- *Regions moved together*: the regions moved after their captures by several
  sets of a group are moved as one (the matches selected do not overlap);
  moved one set after another, the marks kept in step compounded the stream
  rates until the pipeline's buffers exhausted the virtual address space.
- *Runs within texts split with a marker* (ldml::SplitMiddleRuns):
  X R+ T → C becomes X } R+ T → M (a noncharacter) in its place, M { R → ;
  and M { T → C first in the group, and M → ; in a group after it, in a group
  without before contexts or a global filter (`ေ ့+ ($consonant) → $1 ေ့`).

**Matches that may end within another** (`MultipleCaptureMultipleReplacementSet::
walkStarts`): their starts come from the regular expression engine (MatchStarts),
one Pablo kernel (`WalkBranchStarts`) splits them by the branch of their
possessive walk (an optional item taken or not, a string of two characters of a
set or its first character, tested by lookahead), each branch with a fixed
length, or a fixed length, a run and a fixed suffix, and LeftmostMatches selects
them from the left, alone or with the other sets and the string rules (it now
covers a suffix after a run); `WalkSpans` gives the spans and ends of those
selected. The sets of these rules are within the global filter: a character
outside it ends ICU's text, where a set with the text boundary matches (ICU's
`[^0-9.]`), so such a set at the end of the pattern also matches it, and the
result leaves it. (ICU differs for a single group of rules, where a rule before
whose set has that character stops the matching there; the CLDR transforms are
compound.) `tconv --lines` costs about 8 ms a line for my-Zawgyi, the cost of
running a pipeline of 2,493 kernels, mostly the regular expression engines of its
130 sets (129 of one rule) (0.3 s for 2,000 lines as one text).

**Before contexts on the converted text** (MultipleCaptureMultipleReplacement
rules): ICU matches a before context against the converted text; the
MultipleCaptureMultipleReplacement sets match it against the text itself
(the regular expression engine), so a set whose group may convert what its
before context sees is not implemented (`convertedContextMatters`). Two
cases were not real:

- a capture of a fixed string, as Latin-NumericPinyin's `e { (̆) r → r $1 ;`:
  it is the string itself (`FixedCaptureElimination`, `e { ̆ r → r ̆ ;`), a
  string rule, whose before context FusedRules matches against the converted
  text;
- the same rule's group has `(̄) ([{on}{ng}inou]) → $2 1 ;`, whose set with
  strings ICU matches by the longest string (`̄on → on1`); the ends of the
  engine's matches allow `o1n`, so the sets with strings of two characters
  are now selected by their walks.

blt-blt_FONIPA's 46 rules were real: its tone rules insert a tone after a
syllable's final consonant, which may be the `$LO` that the next syllable's
before context (`$LO $W? $V12 { ($CHK) → $1 ˧˥ ;`) sees. Such rules, whose
captures are sequences of sets and whose results insert only after the last
position, are now planned as string rules keeping the characters of their
captures (`StringRule::kept`: in the replacement, a position kept or deleted,
then the rest), so that the correction kernel of the group matches their
before contexts against the converted text, in rounds, as for the other string
rules; an optional set at the end of the text gives two rules, present, then
absent before another character. (Enumerating the characters instead gave
2,408 string rules, more than a correction kernel takes, 256: the group then
fell back to the text itself, with a warning, and was wrong on 1 line in
3,000; a correction kernel of that size did not compile in 10 minutes.) Three
other problems came out of blt:

- MoveRegions lost the last character of a capture moved at the end of the
  text (`($V1) ($C $W?) → $2 $1 ;` on `ꪹꪂ`): CaptureWalk gave the position after
  the capture, which does not exist there; it now gives its last character;
- NullableCaptureElimination did not match the optional items possessively: of
  `{($LO $W? $V123 $F?)} $NOT_IPA_TONE`, the rule with `$F` absent applied where
  `$F` was there but the after context failed after it; an item absent now
  requires the next character not to be in its set (an item present after it,
  or the first set of the after context, without those characters; or, at the
  end of the text with no after context, a character not in them or the end);
- the regular expression engine does not match a set with strings
  possessively, so a start of a walk set may be of another match; the walk
  branches now test their sets, and their after contexts at their ends.

blt-blt_FONIPA is identical to ICU on 12,000 random lines and passes its 682
CLDR test cases. Implementable transforms with the warning (a group of more than
256 string rules whose before contexts may see conversions) remain:
und_FONIPA-ar (383 rules), Latin-InterIndic (31), Hans-Hant (10), lt-Title (9),
Latn-Ethi/SERA (8), Latn-Cyrl, Latn-Grek/UNGEGN and el-Title (2 each), and
es-ar, es-fa, es_419-ar and es_419-fa (1 each).

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

**Repeated captures** (`5c1ed53ee`). A rule referring more than once to a
capture of a character of a set, as sk_FONIPA-ja's `([b-df-hk-npr-tvzʃʒ]) ː →
$1 $1` (a length mark doubling the consonant), has a result that is not a fixed
string: `ldml::EnumerateRepeatedCaptures` replaces it by a rule for each
character of the set, in order (`bː → bb`, `cː → cc`, …; for each combination
of the characters of such captures, at most 64), where the references are items
of the result (not within function calls). This made sk_FONIPA-ja and sk-ja
implementable; fa-fa_FONIPA needed W5 for the copy of a capture of a set with
strings from its before context, `([$IPA_CONSONANT$VOWEL\|]) { ّ → $1`, now
expanded one rule per string, longest first (`3987326eb`,
`ldml::ExpandContextCaptureStrings`, as ICU takes the longest string ending
at the position); its W6 rule `([َ ِ ُ])ّ → ّ | $1` remains.

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

W5 is now needed by none: the back-references of sk_FONIPA-ja and sk-ja are
enumerated (above), and fa-fa_FONIPA's copy of a set with strings expanded.

**W4 insertion rules** (§4.2) are implemented, and needed by none. In a group
with string rules, an insertion `B { } A → I` is folded into the rules whose
results end with a character of B (`8f13868d4`): each gets a variant
`r } A' → (its result) I` just before it (`π } [Σσ…] → p\'`), and each deletion
without contexts a variant `B { r } A' → I` (`[Pp] { ̓ } [Σσ…] → \'`), A'
extended by the rules `C → | x` (x in A) that feed it (`ϲ → | σ`). This
requires that no rule before the insertion begins with a character of A', that
every character of B within the filter is replaced by a context-free rule, and
that no other rule revisits into A or leaves a character of B before text to
revisit. After the characters of B outside the global filter (Latin `p`), which
ICU's before context sees, the insertion is applied in a group before
(`apΣ → ap'S`). Grek-Latn/UNGEGN is implementable with it, and with the before
contexts of its two υ rules (`$fmaker { υ ($shiftForwardVowels*) } … → $1 f
$under`), which ICU matches against the converted text, translated to the text
itself where the group's conversions allow (each character read back by its
conversion, deleted characters skipped, the strings of several characters
units the context rejects). In a group with capture rules, the string rules
whose before contexts are matched against the converted text are now applied
by the whole-group correction kernel, after the capture rules (they were
matched against the text itself: `ͺμπΕ`), and no single-character rule applies
at the start of a capture rule's match (`υ → y`, whose context excludes the υ
rules' in the converted text). Grek-Latn is now implementable (front-letter
revisits, §5).

**W10 engine lookaheads.** cy-cy_FONIPA's after contexts `$cons* $end` repeat
`$cons`, a set with strings (`{m̥}{d͡ʒ}`). ICU matches it possessively, taking
the longest string at each step, which the greedy form does not
(`yw } $cons* [^bdm$]` on `ywm̥`). The regular expression engine now resolves
such a repetition as ICU does (`2276cddd3`: the strings of the set made
unambiguous, the longest taken), and tconv checks the engine's preconditions
on the lookaheads that a rule's pipeline builds (`0e561c416`): the `yw` string
rules of cy-cy_FONIPA are planned, and QA/tconv_possessive_lookahead_test.py
checks such rules against ICU. The engine does not support these forms in a
lookahead of variable length without a unique prefix: the `y { } $cons* …`
single-character rules (whose after contexts are matched alone, as
lookaheads) and the MultipleCaptureMultipleReplacement rules (whose starts are
found by lookaheads of their captures and after contexts) remained W10, where
the greedy form of the captures is used only if it gives the same matches.

W10 is now needed by none (`86cf1f797`, `06ce8fcce`). The unambiguous
alternatives of such a repetition are guarded strings (`m(?!̥) | m̥`): a
lookahead chain takes them as a string class (re::parseGuardedStringClass),
whose StringClassKernel counts an occurrence of a string only where none of
its guards follows (the guards do not change where occurrences may overlap,
isRepeatableStringClass). The (?!X) after X{lb,}+ is a one-character
lookahead where every first character of X is an alternative of its own, the
assertions before the end of the text that hold there are dropped, and a
final (?=$) is $. A chain's externals are compiled as its kernels are made, so
that names it does not use leave no unread kernel. The capture rules whose
captures repeat such sets have their exact starts found with possessive
captures in a lookahead (engineStarts), which tconv checks the engine
supports. cy-cy_FONIPA's stress rules `$end $cons* { ($vowel+ $cons*) } $end
→ ˈ $1` have before contexts that the insertions of their group cannot
change (every match ends at the end of the text), matched against the text
itself (unchangedBeforeContext). cy-cy_FONIPA is implementable.

Two bugs of the engine's search and replace (by which tconv applies groups of
one rule) were found with it: a position after the text was kept (a NUL after
each text, with Unicode code units and the text not extended), and
EndOfTextMark did not mark the end of an empty text. (getSpan's last resort,
the end of a match less its minimum length, is exact only for fixed lengths;
cy-cy_FONIPA's capture rules no longer reach it, as a final (?=$) is a chain.)

**W7.**

- Grek-Latn/UNGEGN and Latn-Grek/UNGEGN capture a segment within a repetition,
  which `5efe07bd2` rejects as a ruleset bug.
- Latin-ug and ug-Latin have `[:separator:]*` matching the empty text.

Both have proposed CLDR fixes, applied locally in `~/cldr`, which this run
uses: Latin-ug and ug-Latin are implementable, and the two Greek UNGEGN
transforms report their W3, W4 and W6 problems. (The runs at `323305975` and
`585ccdeba` used transforms without them, `~/transforms`, and counted these four
transforms as blocked by W7.)

**W8 and W9.** Thai-Latn needed only `Any-BreakInternal`, which uses a
dictionary word break (its Thai-ThaiLogical step is implemented); ICU now
applies it, as a stage (§2.1), and W8 blocks nothing. Jpan-Latn,
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
   Latin-NumericPinyin), captures revisited (Latn-Arab), and the revisits
   neither composed nor prefix runs (Latn-Grek, with W3, whose rough breathing
   rules mirror Greek-Latin's front-letter revisits; fa-fa_FONIPA's are run
   moves with marks). The whole-group correction of converted before contexts
   where string and capture rules are selected together. Before contexts with more than one repetition matched against
   the converted text (§3.1: ten implementable transforms with the warning).
3. **Converted-text contexts in large groups** (§5): the correction for groups
   of more than 256 rules, which affects Latin-InterIndic.
4. **W7.** Small; it depends on whether to adopt the proposed CLDR fixes. With
   them (`~/cldr`, as in this run), W7 blocks nothing.
5. **Deferred rules.** The deferred rules exact for
   chains of any length (a fixpoint rather than rounds, with buffer bounds that
   do not compound).
6. **Built-ins and pipeline size.**
   - Built-ins: `Any-Name`, `Name-Any` and `Any-BreakInternal` are applied by
     ICU; implementing them in tconv (a names table, a dictionary word break)
     would remove the dependence on ICU.
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
| `0684aaa5b`, `308aad759` | deferred rules' positions inserted once; large mappings planned without quadratic set unions: Jpan-Latn compiles, 374 with the local W7 fixes (`9cebbc6aa`) |
| `b77584f46` | `tconv --revisit-report`; classification of the CLDR revisit rules (`doc/ldml-revisit-classification.md`) |
| `71749b4e9` | W6 prefix runs, `(P) S → A \| $1`, applied by a kernel of their own |
| `fa7352b10` | JIT compile threads with 8 MB stacks: am_Ethi-am_Ethi/Geminate no longer crashes in its first compilation |
| `239c4cfe4` | prefix runs with replacements of other than one character, by mask, insertion and fill: the Ethi-Latn romanizations, ro-ja, ro_FONIPA-ja: 386 |
| `247f3112e` | prefix runs with texts S of several characters, in families |
| `03c89572a` | triggers of prefix runs (`ក → k \| â`), applied first: Khmr-Latn/UNGEGN: 387 |
| `96ff91dd7` | prefix runs with A empty as run rules (`rh → \| r` as `r h+ → r`): the five xh transforms: 392 |
| `27321f4e9` | insert-and-revisit rules as insertion passes (am_Ethi-d0_Morse's separator); V checks by first characters |
| `8a0b58af8` | contexts with repetitions or optional items combined: Latn-Grek/UNGEGN, Latn-Kana, Latn-Hira: 395 |
| `ef4e202f9` | chain revisits (Latn-Grek's iota subscript), in a group of their own: 395 |
| `1317f8cfa` | mark run edits, in a group of their own: Latn-Arab: 396 |
| `75e82e277`, `00bea45f0` | MultipleCaptureMultipleReplacement matches selected by their walks; their sets within the filter: byn_Latn-byn_Ethi/Xaleget, my-Zawgyi: 398 |
| `11eb18fd0` | captures of fixed strings as strings, sets with strings by walks: Latin-NumericPinyin, Latn-Bopo: 400 |
| `a9e44f8d8` | rules keeping the characters of their captures as string rules (converted before contexts); captures moved at the end; possessive optional items: blt-blt_FONIPA: 401 |
| `0e561c416`, `22d03949b`, `345b69bf1` | possessive sets with strings in after contexts checked by the engine; sets with strings in contexts taking their longest strings; test cases with ICU's results counted as discrepancies (217): 401 |
| `85277dbd7` | anchors of texts moved to their contexts: W3 needed by 13, W6 alone blocks 7: 401 |
| `9a4a0c6a6` | last captures of sets including the text boundary: W6 needed by 11, W3 alone blocks 9: 401 |
| `b45845a25` | runs of sets with strings: cs-ja, cs_FONIPA-ja, es-ja, es_419-ja, es_FONIPA-ja: 406 |
| `920d7551f` | insertion revisits beyond the first rule of a group: my-my_Latn: 407 |
| `19d0a02a2`, `07b1a4c13` | prefix runs seen by the last item of an after context, and with exceptions: pl_FONIPA-ja, pl-ja: 409 |
| `9238f5c0c`, `5c1ed53ee` | prefix runs with revisited P and characters of S in after contexts (ja_Latn-ko); repeated captures enumerated: sk_FONIPA-ja, sk-ja: 411 |
| `58ec782da` | ends of the runs of a global filter in after contexts: am_Ethi-d0_Morse: 412 |
| `0550a1483`, `2770c3519` | Any-Name, Name-Any and Any-BreakInternal applied by ICU when called for, ICU stages seeing the text around their runs; Any-Hex/Plain and Hex-Any/Plain dropped: Thai-Latn: 413; built-ins 28 of 28 |
| `f2cb92480` | blocking marks for prefix runs that only delete; revisits composed up to a return to the text; rules left to priority: it-am, it-ja, ja_Latn-ko: 416 |
| `0d35780df`, `587215146` | prefix runs whose P has several characters; revisits composed one step into a prefix run, split with a marker: ja_Latn-ru: 417 |
| `2bc2e73a1` | mark run edits left in their group with a lookahead and a mark: Latin-ThaiLogical, Latn-Thai: 419 |
| `8f13868d4` | insertion rules folded into the rules before them; before contexts of capture rules translated from the converted text; converted contexts by capture rules: Grek-Latn/UNGEGN: 420 |
| `86cf1f797`, `06ce8fcce` | lookahead chains over possessive repetitions of sets with strings (guarded strings, W10); capture rules over such sets, with before contexts unchanged by their insertions; search and replace keeping no position after the text: cy-cy_FONIPA: 421 |
| `3d94638d0`, `5032fb2ca`, `b374fdbca`, `f4ca44892` | leading runs split (in blocks, before a rule matching at their last character); run rules with after contexts; walks of several runs; regions of several sets moved together; runs within texts split with a marker: Zawgyi-my: 422 |
| `3987326eb` | before contexts capturing a set with strings expanded, longest first: fa-fa_FONIPA no longer needs W5 |
| `3471879aa`, `65350b3ce` | front-letter revisits (the rough breathing); before contexts with repetitions matched against the converted text with their after contexts matched by the engine: Grek-Latn: 423 |
| `53121b04a` | run moves with marks (the shadda after a vowel); deferred deletions through runs in one round: fa-fa_FONIPA: 424 |

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
- **Interrupted runs.** The results of each transform (plan, kernel counts,
  test files) are saved as they are computed, in `<out>/progress.jsonl`; running
  the same command again (with the same tconv binary, directories and selection)
  resumes the run, computing only the rest. `--no-resume` starts it afresh.
- **Kernel counts.** Each transform's `tconv --kernel-counts` output is kept in
  `kernels/`, and gives the groups, rules, group kernels and kernels columns of
  the per-transform table. With `--update`, the kernels of the transforms
  planned again are counted again, with those of all the transforms using
  them, directly or not; `--no-kernel-counts` keeps the previous counts.
- The runs at `89e922e4f` to `585ccdeba` used `--transforms-dir ~/transforms`,
  without the local W7 fixes; the run at `239c4cfe4` used `~/cldr`, with them
  (`--transforms-dir ~/cldr/common/transforms --test-data
  ~/cldr/common/testData/transforms --jobs 3`, about two hours on 15 GB).
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
  work-items, failing-tests, discrepant-tests); the rest of the report is edited by hand, from `changes.md`. For
  Any-FCD and Any-FCC:

  ```
  python3 tools/ldml/tconv_coverage.py --tconv build18/bin/tconv \
      --transforms-dir ~/cldr/common/transforms --test-data ~/cldr/common/testData/transforms \
      --out tools/ldml/tconv-coverage --update --only Any-FCD Any-FCC \
      --table-doc doc/ldml-tconv-coverage-table.md --report-doc doc/ldml-tconv-coverage.md \
      --note '…' --cldr-note 'with the local fixes of W7'
  ```

  `tools/ldml/tconv-coverage/` is ignored by git (`tools/ldml/.gitignore`).
