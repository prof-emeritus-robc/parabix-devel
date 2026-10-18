# uconv errata

Known cases where the output of ICU's `uconv` command differs from what ICU itself
produces for the same transform, through its API, on the whole text. These matter
when `uconv` is used as the reference for tconv's output. Observed with
`uconv v2.1`, ICU 78.3.

- **`uconv -x` transliterates the text in pieces, cut after many characters that
  are not line ends.** `uconv` does not apply the transliterator to the input as
  one text. It cuts the text into pieces at what it takes to be line ends, and
  transliterates each piece separately, so no rule can match, and no context can
  see, across a cut. The test for a line end is too loose: besides the line
  terminators U+0085, U+2028 and U+2029, the cut also follows any of

  - U+3028–302F, 3038–303F, 3068–306F, 3078–307F, 30A8–30AF, 30B8–30BF, 30E8–30EF
    and 30F8–30FF: the ideographic tone marks and many common hiragana and
    katakana (と, な, エ, カ, ラ, ー, ヽ, …);
  - U+2028–202F, 2038–203F and 2068–206F: the bidirectional formatting controls
    and some punctuation.

  This pattern comes from a bit mask, not from a Unicode property. A rule anchored
  at the start of each piece shows the cuts: with
  `uconv -x ":: [\u0000-\U0010FFFF] ; ^ (.) → '<' \$1 ;"`, the text
  `x a U+302B U+0300 b` becomes `<x a U+302B <U+0300 b`.

  **Example: Any-NFC on `QA/Normalization/FCC`.** `uconv -x Any-NFC` and
  `tconv Any-NFC` differ on 2 of the 19,965 lines (18393 and 18394):

  ```
  input   0061 1D16D 05AE 302B 0300 0062     (and with 05AE and 302B swapped)
  uconv   0061 1D16D 05AE 302B 0300 0062     unchanged
  tconv   00E0 1D16D 05AE 302B 0062          a + U+0300 composed to à
  ```

  The combining classes of the marks are 226, 228, 228 and 230. No mark between
  `a` and U+0300 has class 0 or a class of 230 or more, so U+0300 is not blocked
  and composes with `a`: tconv is correct. ICU agrees when called directly:
  `Normalizer2::getNFCInstance()->normalize()`, and the Any-NFC transliterator
  applied to the whole text (also incrementally, then finished), give
  `00E0 1D16D 05AE 302B 0062`. Only `uconv` does not compose, because it cuts
  the text after U+302B and normalizes `a U+302B` and `U+0300 b` separately.

  **Consequences.**

  - Expected output made with `uconv` for text containing these characters may
    be wrong wherever a rule's match or context, or a normalization, spans them.
  - For text with kana, or with these punctuation and formatting characters,
    compare against ICU's API on the whole text (for example
    `Transliterator::createInstance` or `createFromRules`, then `transliterate`),
    not `uconv`.
