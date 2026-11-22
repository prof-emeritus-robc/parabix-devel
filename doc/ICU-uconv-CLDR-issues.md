# ICU, uconv and CLDR issues

Known cases where ICU's output for a transform is not what the transform's
rules specify (UTS #35 Part 2), where the output of ICU's `uconv` command
differs from what ICU itself produces through its API on the whole text, and
where CLDR's transform test data disagree with the transforms' rules. These
matter wherever ICU, `uconv` or the CLDR test data are the reference for
xlit's output.

## ICU

Observed with ICU 78.3 (`Transliterator::createFromRules`, as `xlit
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
  rules                                      input   ICU   per the filter (xlit)
  :: [a-m] ; x > Y ;                         x       Y     x
  :: [a-m] ; a > Y ; x > Z ;                 ax      YZ    Yx
  :: [a-m] ; ax > Y ;                        ax      Y     ax
  :: [a-m] ; xa > Y ; a > A ;                xa      Y     xA
  :: [a-m] ; x > Y ; :: Null ;               x       x     x
  :: [a-m] ; :: Null ; x > Y ; :: Null ;     x       x     x
  ```

  With any other `::` transform (`:: Null ;` suffices) the filter applies.
  xlit follows the rules: it applies the filter in both cases. Its tests
  compare such rules with ICU through an added `:: Null ;` step
  (`QA/xlit_divcode_test.py`, case `filter`).

  **CLDR.** Three CLDR transforms have this form: `am_Ethi-am_Ethi/Geminate`
  and `sgw_Ethi-Ethi/Gurage_2013` (`:: [:Ethiopic:] ;`) and `mn-mn_Latn/MNS`
  (`::[[:Cyrl:]];`). None of their rules' texts has a character outside its
  filter, so ICU's output is the same with and without it: xlit and ICU
  agree on their CLDR test data and on random text mixing their characters
  with punctuation, Latin letters and combining marks (2026-11-17).

  **The inverse filter.** The same holds in the backward direction, for the
  inverse filter `:: ( [set] ) ;` at the end of rules that are otherwise a
  single rule set. `Latn-Ethi/SERA` (the backward direction of
  `und-Ethi-t-und-latn-m0-sera.xml`) has the inverse filter
  ``:: ([-!#',.:<>?`0-9A-Za-z]) ;``, without `;`, and the rule
  `፤ ↔ \; ;`. Read backward, `;` is outside the filter and stays as it is
  (xlit), but ICU converts every `;` to `፤` (`a;` gives `አ፤`, xlit `አ;`).
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
  rules (forward)                                              backward on   ICU    per UTS #35 (xlit)
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
    the result keeps numeric tones: `ㄅㄚˇ` gives `ba3` (xlit `bǎ`), `ㄓㄨㄥ`
    `zhong1` (xlit `zhōng`). The file itself notes it: "currently in reverse it
    only goes back to Latin with numeric tones (not sure why); ok but would
    rather have marks."
  - `byn_Latn-byn_Ethi/Tekie_Alibekit` (forward `:: NFD (NFC) ;` then the
    rules): backward, ICU applies NFC before the rules instead of after them.
    On decomposed input this changes the result: `s a U+0308 l` gives `ሰል`
    (composed to `ä` first, then `sä → ሰ`), and xlit `ሳ̈ል` (`sa → ሳ`, the
    mark left). On precomposed input both give `ሰል`. 65 of 99 short words of
    decomposed Latin differ.
  - `Spacedhan-Han` (forward `:: [set] nfkc ; :: fullwidth-halfwidth ;`, then
    rules inserting a space between Han and other letters, whose inverses
    delete it): backward, ICU (its built-in transliterator, elements
    `halfwidth-fullwidth;[set] NFKD` then `%Pass`) applies halfwidth-fullwidth
    first, which makes the space U+3000, and the inverse rules, which delete
    only U+0020, leave it: `乏 U+0020 乬` gives `乏 U+3000 乬`, xlit `乏乬`.

  The others agree with ICU on the same decomposed words: their rules give
  the same results whether NFC is applied before or after them.

- **The filter of a `::` transform also applies to the `::` transforms that
  follow it directly.** ICU makes consecutive `::` transforms one element of
  the compound, with the filter of the first written before all of them:
  `Han-Spacedhan` (`:: [set] nfkc ; :: fullwidth-halfwidth ;`, then its rules)
  has the elements `[set] nfkc;fullwidth-halfwidth` and `%Pass`, so
  fullwidth-halfwidth applies only to the characters of the set. U+3000 is not
  in it: `乏 U+3000 乬` and `a U+3000 b` keep U+3000 in ICU, while xlit, which
  applies the filter to `nfkc` only, gives U+0020. (Also noted in
  ldml-xlit-coverage.md, "A filtered first step": `::[x] Null ;
  ::fullwidth-halfwidth ;` leaves `．` U+FF0E unchanged.)

  **Which rules** (minimal rules through `createFromRules`, `xlit --use-ICU`
  with `--rules-file`, ICU 78.3, 2026-11-22; on `a。，b`):

  ```
  rules                                     ICU     per the rules (xlit)
  :: [a] Null ; :: Any-Upper ;              A。，b   A。，B
  :: [a] Null ; :: Null ; :: Any-Upper ;    A。，b   A。，B
  :: [a] Any-Upper ; :: Any-Upper ;         A。，b   A。，B
  x > y ; :: [a] Null ; :: Any-Upper ;      A。，b   A。，B
  :: Null ; :: [a] Null ; :: Any-Upper ;    A。，B   A。，B
  :: [b] ; :: [a] Null ; :: Any-Upper ;     a。，B   a。，B
  :: [a] Null ; b > Z ;                     a。，Z   a。，Z
  :: [。] nfkc ; :: fullwidth-halfwidth ;   a｡，b   a｡,b
  ```

  So the filter carries to the rest of a block of `::` transforms when the
  filtered transform is the first of its block (at the start of the rules or
  after a rule set), and not when it follows another `::` transform of the
  block; a global filter before it prevents it; and the rule sets after the
  block are not affected. `uconv -x '[a] Null; Any-Upper'` gives the same
  `A。，b`: in ICU's compound IDs a filter at the start is the global filter
  of the compound, so the block's ID, written with the first transform's
  filter in front, reads as filtered as a whole (presumably; not checked in
  ICU's source).

  **CLDR.** In both directions, `Han-Spacedhan.xml` is the only CLDR file with
  a block of `::` transforms that begins with a filtered transform. Its
  `fullwidth-halfwidth` then applies only to its NFKC set (CJK punctuation,
  the ideographs and Han), so the fullwidth forms of ASCII stay as they are
  in ICU, while xlit makes them ASCII, as the rules say:

  ```
  input                  ICU (createFromRules, and uconv -x Han-Spacedhan)   xlit
  a，i（u）e！１ U+3000 o：  a， i （u） e！１ U+3000 o：                       a, i (u) e!1 o:
  ```

  `Fullwidth-Halfwidth` alone converts them in ICU too (`uconv -x
  Fullwidth-Halfwidth`). Han-Spacedhan is invoked by Hani-Latn (Han-Latin),
  Hant-Latn, Han-Latin/Names and Jpan-Latn: on the first 2,000 lines of the
  Japanese Wikibooks text of the survey (ldml-xlit-coverage.md, §4.5),
  Jpan-Latn's output differs from ICU's on 405, all at fullwidth punctuation,
  digits or U+3000 (`，（）！：１`). Hira-Latn and Kana-Latn, its other steps,
  agree with ICU on them. No CLDR test file has such characters.

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
  `xlit Any-NFC` differ on 2 of the 19,965 lines (18393 and 18394):

  ```
  input   0061 1D16D 05AE 302B 0300 0062     (and with 05AE and 302B swapped)
  uconv   0061 1D16D 05AE 302B 0300 0062     unchanged
  xlit   00E0 1D16D 05AE 302B 0062          a + U+0300 composed to à
  ```

  The combining classes of the marks are 226, 228, 228 and 230. No mark between
  `a` and U+0300 has class 0 or a class of 230 or more, so U+0300 is not blocked
  and composes with `a`: xlit is correct. ICU agrees when called directly:
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

- **`uconv -x` does not transliterate each line as a text of its own.** It
  transliterates the input a line at a time (as above), but each line's text
  includes its line feed. So the start of a line is the start of a text, but
  its end is not the end of a text. On the three lines `xa`, `b`, `ab` (no
  final line feed):

  ```
  rules                    uconv          each line as a text (xlit --lines, ICU per line)
  [$] { a > Y ;            xa  b  Yb      xa  b  Yb
  [a\u000A] { b > X ;      xa  b  aX      xa  b  aX
  b } [$] > X ;            xa  b  aX      xa  X  aX
  b } \u000A > Z ;         xa  Z  ab      xa  b  ab
  ```

  The first two rows agree (a line's start is a text's start: the `b` after
  the line feed is not changed); in the last two, `uconv` sees the line feed
  after the `b` of the second line, and the end of the text only on the last
  line.

  A rule that looks at the end of a text, or at a line feed after it, applies
  differently from the CLDR test data, which take each line as a text without
  its line feed (as `xlit --lines` does).

  **Example: my-my_FONIPA.** Under `uconv -x my-my_FONIPA`, 57 of the 1,228
  lines of `my-fonipa-t-my.txt` lose a word-final inherent vowel at the end of
  the line: `က` gives `k` (expected `ka̰`), `ကုလသမဂ္ဂ` gives `kṵ.lə.θə.mɛʔ.ɡ`
  (expected `kṵ.lə.θə.mɛʔ.ɡa̰`). The same happens with the checkout's rule text
  given to `-x`, and not on a last line without a line feed (`က` alone gives
  `ka̰`). ICU's API applied to each line without its line feed (`xlit --lines
  --use-ICU`) gives the expected output on all of them.

  **Consequence.** `uconv` cannot serve as the per-line reference for rules
  sensitive to the end of a text; xlit applies ICU to each line in its own
  process (doc/masked-transforms-plan.md, step 11).

## CLDR

Observed with the CLDR checkout at `~/cldr` (`common/transforms` and
`common/testData/transforms`), 2026-11-21. Each test file has one case per
line: the input, a tab, and the expected output, the input taken as a text of
its own.

- **Test files named by IDs their transforms do not declare.** Six test files
  are named by IDs that no transform file declares as an alias, so ICU
  (`uconv -x`: `U_INVALID_ID`) does not find their transforms by the file
  names (nor did xlit before 2026-11-21, see below):

  ```
  test file                                         transform (as declared)
  byn-Latn-t-byn-ethi-m0-tekie-alibekit             byn-Latn-t-byn-ethi-m0-tekieali, Ethiopic-Latin/Tekie_Alibekit
  d0-morse-t-am-Ethi                                am-Ethi-t-d0-morse, am_Ethi-d0_Morse
  und-Latn-t-und-ethi-m0-beta-metsehaf              und-Latn-t-und-ethi-m0-betamets, Ethiopic-Latin/Beta_Metsehaf
  und-Latn-t-und-ethi-m0-beta-metsehaf-geminate     und-Latn-t-und-ethi-m0-betamets-geminate, Ethiopic-Latin/BetaMetsehaf_Geminate
  und-Latn-t-und-ethi-m0-ies-jes-1964               und-Latn-t-und-ethi-m0-iesjes-1964, Ethiopic-Latin/IES_JES_1964
  und-Latn-t-und-ethi-m0-ies-jes-1964-geminate      und-Latn-t-und-ethi-m0-iesjes-1964-geminate, Ethiopic-Latin/IES_JES_1964_Geminate
  ```

  Applied by their declared names, all six give the expected output on every
  case, in `uconv`, ICU through xlit and xlit. (The upstream repository has
  the test files under the same names: checked for the byn and Morse files.)

  **The names as BCP 47 transform IDs.** A BCP 47 transform ID (RFC 6497, as
  UTS #35 Part 2 uses it) is the tag of the result, `-t-`, the tag of the
  source, then fields: `m0` the mechanism (a variant), `d0` and `s0` a
  destination or source that is not a language, in which case the main tag is
  the language of the text itself (`und-t-d0-publish` for Any-Publishing).
  Script codes are lowercase in the extension. CLDR registers the field values
  (`common/bcp47/transform.xml` for `m0`, `transform-destination.xml` for `d0`
  and `s0`; not in every checkout), some with aliases.

  - The four Ethiopic `m0` names spell a registered value by its registered
    alias, where the transform files declare the registered name: `tekieali`
    (alias `tekie-alibekit`), `betamets` (alias `beta-metsehaf`), `iesjes`
    (alias `ies-jes`). They are valid but not canonical. ICU does not map the
    aliases to the names. The subtags `geminate` and `1964` are not registered
    at all, in the test file names or in the transforms' declared IDs (nor in
    `am-Ethi-t-am-ethi-m0-geminate`).
  - `d0-morse-t-am-Ethi` is not well formed: a tag begins with a language
    subtag, and the `d0` field belongs after `-t-`; the halves seem swapped by
    analogy with "result-t-source". It also has `Ethi` in title case within
    the extension. `morse` is registered for `d0` and `s0`. The transform
    file's IDs are right: `am-Ethi-t-d0-morse` (Amharic text to Morse, forward)
    and `am-Ethi-t-s0-morse` (Amharic text from Morse, backward).

  ICU does not find `ka-Latn-t-ka-m0-bgn-2009` and `my-fonipa-t-my` either,
  although their transform files declare them as aliases and both are well
  formed (`bgn` is a registered `m0` value, `2009` is not; `fonipa` is a
  registered variant of the main tag): ICU apparently does not register every
  BCP 47 alias of a transform file (not checked in ICU's source).

  **xlit** (2026-11-21) finds a BCP 47 transform ID not found as it is with
  its field values canonical, the registered aliases replaced by their names
  (`ldml::TransformRegistry::lookup`), and reads a name with a `d0` or `s0`
  field before `-t-` as the tag with the field after it. So all eight test
  files are found by their names.

- **`ka-Latn-t-ka-m0-bgn-2009.txt` expects U+02BC where the rules write
  U+2019.** 215 of its 682 cases differ from the output of
  `Georgian-Latin-BGN.xml`, all by one character: the test data have the
  modifier letter apostrophe `ʼ` (U+02BC) for the ejective consonants, and the
  rules write the right single quotation mark `’` (U+2019; the file has no
  U+02BC). For example `ყოველ`: expected `qʼovel`, `uconv`, ICU through xlit
  and xlit all give `q’ovel`. The test data or the rules need changing.
  *Fixed locally* (2026-11-21, `6868d41bf`): the test file in
  `third_party/cldr` now expects U+2019, as the rules, ICU and xlit give
  (`git diff cldr-upstream -- third_party/cldr`).

- **`my-fonipa-t-my.txt` has two expected outputs with a trailing space.**
  `နန်းယွန်းမြို့` expects `náɴ.jʊ́ɴ.mjo̰ ` and `မင်းဘူးမြို့` expects
  `mɪ́ɴ.bú.mjo̰ `; `my-my_FONIPA.xml` gives `náɴ.jʊ́ɴ.mjo̰` and `mɪ́ɴ.bú.mjo̰`
  (ICU through xlit and xlit agree; `uconv` too, apart from the issue above).
  The space is presumably an artifact of the test file.
  *Fixed locally* (2026-11-21, `2cc3c72b9`): the two expected results in
  `third_party/cldr` no longer end with a space.
