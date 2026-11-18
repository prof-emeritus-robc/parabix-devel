# ICU and uconv issues

Known cases where ICU's output for a transform is not what the transform's
rules specify (UTS #35 Part 2), or where the output of ICU's `uconv` command
differs from what ICU itself produces through its API on the whole text. These
matter wherever ICU or `uconv` is the reference for tconv's output.

## ICU

Observed with ICU 78.3 (`Transliterator::createFromRules`, as `tconv
--use-ICU` calls it) and with `uconv v2.1`, ICU 77.1.

- **A global filter is ignored when the rules make a single rule set.** A
  global filter (`:: [set] ;` before the rules) restricts the whole transform
  to the characters of the set: the rules apply to each run of those
  characters, and no rule's text can match a character outside them. ICU
  applies it only when the rules have at least one `::` transform besides the
  filter. When they make a single rule set (only the filter and conversion
  rules), the rules apply to every character of the text, as if there were no
  filter (presumably because ICU then makes a plain `RuleBasedTransliterator`,
  without the filter it parsed; not checked in ICU's source):

  ```
  rules                                      input   ICU   per the filter (tconv)
  :: [a-m] ; x > Y ;                         x       Y     x
  :: [a-m] ; a > Y ; x > Z ;                 ax      YZ    Yx
  :: [a-m] ; ax > Y ;                        ax      Y     ax
  :: [a-m] ; xa > Y ; a > A ;                xa      Y     xA
  :: [a-m] ; x > Y ; :: Null ;               x       x     x
  :: [a-m] ; :: Null ; x > Y ; :: Null ;     x       x     x
  ```

  With any other `::` transform (`:: Null ;` suffices) the filter applies.
  tconv follows the rules: it applies the filter in both cases. Its tests
  compare such rules with ICU through an added `:: Null ;` step
  (`QA/tconv_divcode_test.py`, case `filter`).

  **CLDR.** Three CLDR transforms have this form: `am_Ethi-am_Ethi/Geminate`
  and `sgw_Ethi-Ethi/Gurage_2013` (`:: [:Ethiopic:] ;`) and `mn-mn_Latn/MNS`
  (`::[[:Cyrl:]];`). None of their rules' texts has a character outside its
  filter, so ICU's output is the same with and without it: tconv and ICU
  agree on their CLDR test data and on random text mixing their characters
  with punctuation, Latin letters and combining marks (2026-11-17).

  **The inverse filter.** The same holds in the backward direction, for the
  inverse filter `:: ( [set] ) ;` at the end of rules that are otherwise a
  single rule set. `Latn-Ethi/SERA` (the backward direction of
  `und-Ethi-t-und-latn-m0-sera.xml`) has the inverse filter
  ``:: ([-!#',.:<>?`0-9A-Za-z]) ;``, without `;`, and the rule
  `፤ ↔ \; ;`. Read backward, `;` is outside the filter and stays as it is
  (tconv), but ICU converts every `;` to `፤` (`a;` gives `አ፤`, tconv `አ;`).
  The same rules written forward show it is this issue: with the filter
  ``:: [\-!#',.:<>?`0-9A-Za-z] ;`` and the rules `\; → ፤ ; a → አ ;`, ICU
  gives `አ፤` on `a;`, and with `:: Null ;` added, `አ;`. Of the CLDR files
  with an inverse filter and no other `::` transform, the other two do not
  show it: `am-Ethi-t-am-brai.xml` (no backward rule has text outside
  `[:Braille:]`; Braille-Ethiopic/Amharic agrees with ICU on random Braille
  mixed with ASCII punctuation) and `und-Ethi-t-und-latn-m0-es3842.xml`
  (forward only).

- **In the backward direction, ICU puts a `::` transform before the rule set
  it follows.** UTS #35 inverts a compound transform by inverting each step
  and reversing their order: `p ↔ q ; :: X (Y) ; R` read backward is `R⁻¹`,
  then `Y`, then `p ← q`. ICU (`createFromRules(…, UTRANS_REVERSE)`, and so
  the CLDR backward transforms) builds the steps in another order: the
  elements of the compound are `Y`, `R⁻¹`, `p ← q`, so `Y` applies to the text
  before the rules that should feed it.

  ```
  rules (forward)                                              backward on   ICU    per UTS #35 (tconv)
  :: Latin-NumericPinyin(NumericPinyin-Latin) ; a1 ↔ X ;       X             a1     ā
  p ↔ q ; :: Latin-NumericPinyin(NumericPinyin-Latin) ;
          a1 ↔ X ;                                             qX            pa1    pā
  ```

  ICU's compound alternates blocks of `::` transforms and rule sets
  (`[ids₀, rules₀, ids₁, rules₁, …]`), and in the backward direction it
  prepends each block to its list as it reads them. So each block of `::`
  transforms comes out paired with, and placed before, the inverse of the rule
  set that followed it, not the one that preceded it. The order is right when
  the `::` transforms are symmetric around the rules (`:: NFD (NFC) ; rules ;
  :: NFC (NFD) ;`, as in most CLDR files) and wrong when the rules end with a
  rule set after a `::` transform. (Observed through `getElement` on the
  transliterators; not checked in ICU's source.)

  **CLDR.** Among the CLDR files read in both directions, those that end with
  a rule set after a `::` transform are `Latin-Bopomofo.xml`,
  `byn-Ethi-t-byn-latn-m0-tekie-alibekit.xml`, `und-Ethi-t-und-latn.xml` and
  the `und-Ethi-t-und-latn-m0-*` (Aethiopica, ALALOC, Beta_Metsehaf,
  IES_JES_1964, Lambdin), `und-Ethi-t-und-cyrl-m0-gutgarts.xml` and
  `Han-Spacedhan.xml`. It shows in three:

  - `Bopo-Latn` (Bopomofo-Latin): the forward rules are a rule set (`[ln] { v
    → ü`), `:: Latin-NumericPinyin(NumericPinyin-Latin) ;`, and the rules
    between pinyin with numeric tones and Bopomofo. Backward, ICU applies
    NumericPinyin-Latin first, to the Bopomofo text, where it does nothing, so
    the result keeps numeric tones: `ㄅㄚˇ` gives `ba3` (tconv `bǎ`), `ㄓㄨㄥ`
    `zhong1` (tconv `zhōng`). The file itself notes it: "currently in reverse it
    only goes back to Latin with numeric tones (not sure why); ok but would
    rather have marks."
  - `byn_Latn-byn_Ethi/Tekie_Alibekit` (forward `:: NFD (NFC) ;` then the
    rules): backward, ICU applies NFC before the rules instead of after them.
    On decomposed input this changes the result: `s a U+0308 l` gives `ሰል`
    (composed to `ä` first, then `sä → ሰ`), and tconv `ሳ̈ል` (`sa → ሳ`, the
    mark left). On precomposed input both give `ሰል`. 65 of 99 short words of
    decomposed Latin differ.
  - `Spacedhan-Han` (forward `:: [set] nfkc ; :: fullwidth-halfwidth ;`, then
    rules inserting a space between Han and other letters, whose inverses
    delete it): backward, ICU (its built-in transliterator, elements
    `halfwidth-fullwidth;[set] NFKD` then `%Pass`) applies halfwidth-fullwidth
    first, which makes the space U+3000, and the inverse rules, which delete
    only U+0020, leave it: `乏 U+0020 乬` gives `乏 U+3000 乬`, tconv `乏乬`.

  The others agree with ICU on the same decomposed words: their rules give
  the same results whether NFC is applied before or after them.

## uconv

Observed with `uconv v2.1`, ICU 78.3.

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
