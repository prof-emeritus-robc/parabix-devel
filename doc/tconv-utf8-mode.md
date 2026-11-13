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

- **Context-free character maps.** A conversion group whose rules are all
  single-character rules without contexts: each character of a set is
  replaced by a string of any length, possibly empty (`utf8CharMap`).
- **Transform rules** without filters, invoking `Any-Null` or transforms in
  UTF-8 mode.

Anything else puts a transform in Unicode mode:
- a filter;
- string, capture, insertion or deferred rules;
- contexts;
- other built-ins;
- segments applied by shape groups;
- `--utf8-errors=abort-on-error`.

18 CLDR-defined transforms are in UTF-8 mode: Publishing-Any, Geor-Latn,
Syrc-Latn, Cans-Latn, Ethi-Latn/Williamson, Fullwidth-Halfwidth,
ThaiLogical-Latin, ThaiSemi-Thai, ASCII-Latin, Pinyin-NumericPinyin,
NumericPinyin-Pinyin, Ethi-sgw_Ethi/Gurage_2013, and the Devanagari,
Gujarati and Malayalam to InterIndic and InterIndic to Bengali, Gujarati and
Oriya maps.

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

**Ill-formed input** is handled by the first character map of a pipeline, as
the decoding in Unicode mode handles it (`codegen::UTF8Errors`).
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

## Results

**Correctness.** On 400 random lines per transform, all 18 transforms give
output identical to Unicode mode. The lines mix the characters of each
transform's rule file, ASCII, 2-, 3- and 4-byte characters, combining marks
and ill-formed sequences. They were checked with both treatments of
ill-formed input, and the output is identical to ICU's on the same text.
Publishing-Any passes its 46 CLDR test cases. `QA/tconv_utf8_mode_test.py`
(ctest `tconv_utf8_mode_test`) checks five of the transforms in both modes
and both error treatments.

**Time.** Measured on 128 MB of well-formed text (the characters of each
rule file and ASCII), `-thread-num=4`, median of 5 warm runs:

| Transform | UTF-8 mode | Unicode mode | change |
|---|---:|---:|---:|
| Publishing-Any | 0.173 s | 0.438 s | −61% |
| Geor-Latn | 0.121 s | 0.233 s | −48% |
| Syrc-Latn | 0.142 s | 0.234 s | −39% |
| Fullwidth-Halfwidth | 0.174 s | 0.225 s | −23% |
| InterIndic-Bengali | 0.188 s | 0.246 s | −23% |
| Devanagari-InterIndic | 0.153 s | 0.189 s | −19% |

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
- **More patterns:**
  - string rules, from the string matching of the regular expression
    engine in byte mode;
  - single-character rules with contexts of sets;
  - the normalization built-ins that already have UTF-8 pipelines.
- **Filters,** as masks over code units.
