# tconv: UTF-8 mode

tconv builds its rule pipelines in one of two modes:

- **Unicode mode:** the text is decoded from UTF-8 to the 21 basis bits of
  its codepoints (one position per character), the steps are applied there,
  and the result is encoded to UTF-8 again.
- **UTF-8 mode:** the steps are applied to the 8 basis bits of the UTF-8 code
  units (one position per byte), without decoding.

Each transform is classified (`TransformAnalyzer::mode` in
`tools/ldml/tconv.cpp`). A transform is in UTF-8 mode when every step of its
plan can be applied to UTF-8 code units, including those of the transforms
it invokes, transitively. If any step, or any transform it invokes, needs
Unicode mode, the whole transform is in Unicode mode. `tconv --plan` reports
the mode as its last line:

```
  mode: UTF-8
  mode: Unicode: invokes Any-NFD (the built-in Any-NFD)
```

By default tconv uses UTF-8 mode wherever a transform is in it;
`--utf8-mode=0` always uses Unicode mode. `--kernel-counts` counts the
kernels of the pipeline used (`mode: UTF-8`).

## What UTF-8 mode supports

- **Character maps.** A conversion group whose rules are all
  single-character rules (`utf8CharMap`): each character of a set is replaced
  by a string of any length, possibly empty.
- **Contexts on those maps.** The rules may have contexts, provided two
  conditions hold:
  - the engine can match the rule with its contexts on UTF-8 code units, and
  - the group's own conversions cannot change what its before contexts see,
    since ICU matches them against the converted text (see
    `convertedContextMatters`).

  The positions where the contexts hold come from the engine (see
  "Contexts" below).
- **Transform rules** invoking transforms in UTF-8 mode.
- **Built-ins:** `Any-Null`, the normalizations `Any-NFD`, `Any-NFKD`,
  `Any-NFC` and `Any-NFKC`, and the case mappings `Any-Lower` and
  `Any-Upper` (see "Normalizations and case mappings").
- **Filters,** global or on transform rules (see "Filters").

Anything else puts a transform in Unicode mode:
- string, capture, insertion or deferred rules;
- contexts that are not sequences of sets, that see the converted text, or
  that the engine cannot match;
- other built-ins (`Any-FCD`, `Any-FCC`, `Any-Title`, the hex transforms,
  `Any-Remove`);
- segments applied by shape groups;
- `--utf8-errors=abort-on-error`.

38 CLDR-defined transforms are in UTF-8 mode:
- **Character maps:** Publishing-Any, Geor-Latn, Syrc-Latn, Cans-Latn,
  Ethi-Latn/Williamson, Fullwidth-Halfwidth, ThaiLogical-Latin,
  ThaiSemi-Thai, ASCII-Latin, Pinyin-NumericPinyin, NumericPinyin-Pinyin,
  Ethi-sgw_Ethi/Gurage_2013, and the Devanagari, Gujarati and Malayalam to
  InterIndic and InterIndic to Bengali, Gujarati and Oriya maps.
- **Contexts:** Thai-ThaiSemi.
- **Normalizations and case mappings:** Accents-Any, az-Upper, tr-Upper,
  lt-Lower, kk-kk_FONIPA.
- **Filters** (most also normalizing within them): Deva-Beng, Deva-Gujr,
  Deva-Orya, Gujr-Beng, Gujr-Orya, Mlym-Beng, Mlym-Gujr, Mlym-Orya,
  Hebr-Latn, am_Ethi-am_Brai, ka-ka_Latn/BGN, ka-ka_Latn/BGN_1981,
  sgw_Ethi-Ethi/Gurage_2013, yo-yo_BJ.

## The UTF-8 character map

`kernel::UTF8_CharMapPipeline` (`include/kernel/unicode/utf8_charmap.h`)
applies a map from sets of characters to strings directly to UTF-8 basis
bits. Each mapped character keeps, in place, as many of its bytes as its
replacement has: all of them, for a replacement of the same encoded length.
Those bytes are changed by XOR.

1. **Classes.** One `CharClassesKernel`, compiled by lookahead, marks each
   mapped character at its first byte. It computes these classes, per
   codepoint, as a set's characters may differ in length:
   - the characters mapped;
   - for byte j and bit b, the characters whose replacement's byte j differs
     from their own in bit b;
   - the number of bytes to delete;
   - the characters deleted entirely (empty replacements);
   - the number of the tail to insert, where a tail is the bytes of a
     replacement beyond those of its character.
2. **Apply** (`UTF8_CharMapApply`):
   - It changes the bytes kept in place by XOR (`basis XOR Advance(I & X[j][b], j)`).
   - It computes the positions kept: the surplus last bytes of a character,
     given its length by its lead byte, are deleted, and all the bytes of a
     character with an empty replacement.
   - It moves the lengths and numbers of the tails to the character's last
     byte.
3. **Deletion and insertion,** only where bytes are deleted or inserted:
   - `FilterByMask` on the basis and the tail streams;
   - `InsertionSpreadMask` after the last bytes;
   - `SpreadByMask` on the basis and the tail numbers.

   The change classes never pass through deletion or insertion.
4. **Tails** (`UTF8_CharMapTails`). The bytes of each tail are written at the
   positions inserted, decoded from the tail's number.

A map whose characters all keep their lengths, applied to well-formed text,
takes two kernels: the classes and the in-place changes.

**Contexts.** For each subgroup with contexts, tconv matches the pattern
`(?<=before) [chars] (?=after)` with the regular expression engine on the
UTF-8 basis bits (`RE_context{&cc::UTF8, Basis}`, `utf8Applies`). The engine
chooses its mode: byte mode, UTF-8 indexed, or Unicode indexed. Unicode
indexing is always used with `drop-bad-characters`, so that the characters
around dropped input become adjacent. The marks of all the subgroups are
ORed and given to `UTF8_CharMapPipeline`, which maps a contextual character
only where its last byte is marked.

To place the marks, tconv queries the engine's plan, `planRE(prepared,
&cc::UTF8)` (`RE_Plan` in `include/kernel/re/regexp_engine.h`), which the
engine computes without building a pipeline. The plan gives:
- the mode as compiled (`modeName()`);
- the offset of the match marks (`matchOffset`), in code units, or in
  characters with Unicode indexing, when the marks are mapped to last bytes
  through `getU8Index`;
- whether the starts of matches are found by reversal;
- the precondition failures.

tconv uses the plan to decide whether a rule with contexts can be in UTF-8
mode, and `--plan` reports the engine's mode for each context:

```
  mode: UTF-8
    contexts [$thai { } $thai] matched by the engine in byte mode
```

**Ill-formed input** is handled before the steps, by the UTF-8
well-formedness library (`kernel::WellFormedUTF8`,
`include/kernel/unicode/utf8_wellformed.h`), as the decoding in Unicode mode
handles it (`codegen::UTF8Errors`); its kernels have fast paths for
well-formed text. The details below describe the earlier, built-in handling.
`UTF8_structure` finds the maximal ill-formed subparts. With the default
`use-replacement-char`, the first byte of each subpart is set to EF in place
and the rest of the subpart deleted, and the tail BF BD is inserted, giving
U+FFFD. With `drop-bad-characters`, the whole subpart is deleted. The output
in both modes is then identical byte for byte.

The in-place XOR translation of same-length characters is the technique of
xch's UTF-8 translation (`tools/transcoders/xch.cpp`). This design extends it
to strings and deletions, and uses first-byte marks throughout. In a quick
check, xch's UTF-8 path differed from its `-U21` path on some translations
that lengthen characters (for example `-src=a -tgt=𐍈`); this was not
investigated further.

## Normalizations and case mappings

- **Normalizations.** `Any-NFD`, `Any-NFKD`, `Any-NFC` and `Any-NFKC` are the
  focused UTF-8 normalizations of `kernel/unicode/normalization`, as in the
  nfd, nfkd and nfc tools. They take the basis bits and the bytes (P2S) of
  the text, and give bytes (S2P).
- **Any-Lower.** As in Unicode mode, the Final_Sigma rule comes first: Σ
  becomes ς where the rule of `finalSigmaPlan` applies, its contexts matched
  by the engine. Then comes the full lowercase mapping (`lc`) as a character
  map: each character of Changes_When_Lowercased mapped to its value.
- **Any-Upper.** The `uc` mapping, from Changes_When_Uppercased.

## Filters

A filter is a mask over the bytes of the characters it selects (their first
bytes by lookahead, extended over their lengths, within the enclosing
filter's mask). The masks of the filters in effect are kept in step with the
text: each character map filters them with its deletions and spreads them
with its insertions, and the bytes inserted after a character take its
mask. As in Unicode mode (`RulePipelineBuilder::pushFilter`):
- a character map applies only within the innermost mask;
- contexts, matched by the engine, see the whole text;
- a normalization within a filter normalizes each run of the filter's
  characters by itself.

**Normalization within a filter.** Unicode mode replaces each character
outside the filter by a barrier codepoint, U+FDD0, which is never reordered
or composed. In UTF-8 the replacement must keep the character's length, so
there is a barrier for each length: U+0000, U+0080, U+FDD0 and U+1FFFE, all
starters without decompositions that compose with nothing. The characters
outside the filter, and any barrier in the text, are replaced in place by
their barriers, and the text is normalized. The original bytes are then
restored at the bytes of the barriers, which the normalization keeps in
order. The masks are restored the same way.

## Results

**Correctness.** On 400 random lines per transform, all 38 transforms give
output identical to Unicode mode. The lines mix the characters of each
transform's rule file, ASCII, 2-, 3- and 4-byte characters, combining marks
and ill-formed sequences. They were checked with both treatments of
ill-formed input, and the output is identical to ICU's on the same text.
The 14 CLDR test files of the transforms in UTF-8 mode give the same results
as Unicode mode and ICU. All match their expected results except
ka-ka_Latn/BGN's 215 discrepant cases, where the test data disagrees with
the rules and ICU. Normalization within filters was stressed on 600 lines
per transform for the 11 transforms that do it (and for Accents-Any and
lt-Lower). The text was dense with combining marks and nuktas next to
filtered characters, with decomposed and precomposed letters and the barrier
characters themselves, and the output was identical to Unicode mode and ICU. `QA/tconv_utf8_mode_test.py`
(ctest `tconv_utf8_mode_test`) checks nine of the transforms (Thai-ThaiSemi,
Accents-Any, lt-Lower and Deva-Beng among them) in both modes
and both error treatments.

**Time.** Measured on 128 MB of well-formed text (the characters of each
rule file and ASCII), `-thread-num=4`, median of 5 warm runs (the first
rows before the well-formedness library, the others with it):

| Transform | UTF-8 mode | Unicode mode | change |
|---|---:|---:|---:|
| Publishing-Any | 0.173 s | 0.438 s | −61% |
| Geor-Latn | 0.121 s | 0.233 s | −48% |
| Syrc-Latn | 0.142 s | 0.234 s | −39% |
| Fullwidth-Halfwidth | 0.174 s | 0.225 s | −23% |
| InterIndic-Bengali | 0.188 s | 0.246 s | −23% |
| Devanagari-InterIndic | 0.153 s | 0.189 s | −19% |
| Thai-ThaiSemi (contexts) | 0.145 s | 0.237 s | −39% |
| yo-yo_BJ (filter, NFC) | 0.288 s | 0.726 s | −60% |
| Accents-Any (NFD, map, NFC) | 0.647 s | 0.893 s | −28% |
| lt-Lower (Any-Lower) | 0.975 s | 1.338 s | −27% |
| Deva-Beng (filter, NFD, NFC) | 0.914 s | 1.125 s | −19% |
| Hebr-Latn (filter) | 1.396 s | 1.568 s | −11% |

Each of these maps counts 13 kernels in UTF-8 mode, most of them for the
handling of ill-formed input, which needs deletion and insertion. Without
that handling (a later map in a chain, on text already well-formed),
Devanagari-InterIndic's same-length map takes 2 kernels. In an earlier
version, the change classes passed through deletion and insertion; UTF-8 mode
was then 14% slower than Unicode mode for Devanagari-InterIndic and 82%
slower for Fullwidth-Halfwidth.

## Next steps

- **Ill-formed input.** The handling of ill-formed input costs most of the
  kernels of a same-length map. A check that the text is well-formed, with
  the general path taken only where it is not, could leave such maps their
  two kernels.
- **Engine plans.** `RE_Plan` is a first step towards plans of the engine
  that clients query before building: for example, the strategies for the
  starts of matches and the walks of captures, and the kernels a pattern
  would take. tconv could then choose between implementations by their
  costs, as it does now by counting kernels.
- **More patterns:** string rules (85 of the transforms invoking a
  normalization have them), and contexts that see the converted text (51).
  These are the main blockers left.
- **NFD on dense text.** The focused UTF-8 NFD is slower than the codepoint
  NFD where most characters change (0.42 s against 0.29 s on 128 MB of
  accented text); not yet investigated.
