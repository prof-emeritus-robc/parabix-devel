# ICU, uconv and CLDR issues

Known cases where ICU's output for a transform is not what the transform's
rules specify (UTS #35 Part 2), where the output of ICU's `uconv` command
differs from what ICU itself produces through its API on the whole text, and
where CLDR's transform test data disagree with the transforms' rules. These
matter wherever ICU, `uconv` or the CLDR test data are the reference for
xlit's output. The last section lists flaws in the documentation itself:
what UTS #35 leaves unspecified that CLDR relies on, and where ICU's
documentation and UTS #35 use the same words differently.

In this document, the *global filter* of a set of rules is UTS #35's filter
rule (`:: [set] ;` before the other rules), as in ICU's Transform Rule
Tutorial; see "Flaws in documentation" for ICU's other use of the term.

## ICU

Observed with ICU 78.3 (`Transliterator::createFromRules`, as `xlit
--use-ICU` calls it), with ICU4J 78.3 (`Transliterator.createFromRules` with
the same rules, through `tools/ldml/ICU4JRun.java`), and with `uconv v2.1`,
ICU 77.1. The ICU source references are to the ICU4C and ICU4J sources
fetched on 2026-11-22.

**ICU4C, ICU4J and xlit compared** (`tools/ldml/icu4j_compare.py`,
2026-11-22: all 453 transforms of the registry, each on up to 500 lines of
its CLDR test data and of natural text in its source script; the outputs are
in `tools/ldml/xlit-coverage/icu4j/`). ICU4J and ICU4C agree on every
transform both have, except Latn-Ethi/SERA (the global filter of a single
rule set) and Thai-Latn (Thai word segmentation), below. xlit (`af15fa6d2`)
agrees with both on all the transforms it implements, except for these issues:

```
transform                                         lines differing   issue
Han-Latin/Names, Hani-Latn, Hant-Latn, Han-Spacedhan   297 each     the filter carried through a block of :: rules (*)
Jpan-Latn                                               86          the same (through Han-Spacedhan)
Spacedhan-Han                                          339          backward order of the :: transforms
Bopo-Latn                                              137          backward order of the :: transforms
Latn-Ethi/SERA                                           2          the global (inverse) filter of a single rule set: ICU4C only
Any-Title, nl-Title                                  11, 16         Any-Title not at word boundaries
```

(*) Each line differs where the input has a character that fullwidth-halfwidth
converts and that is outside Han-Spacedhan's NFKC set: fullwidth ASCII and
U+3000 on all but three lines, and on those, arrows and shapes that it makes
halfwidth (`→` U+2192 to `￫` U+FFEB, `○` U+25CB to `￮` U+FFEE, `←` U+2190 to
`￩` U+FFE9), which ICU leaves as they are.

Latn-Kana and Latn-Hira (8 lines each) and the ten Latin to Indic transforms
(3 to 15 lines) also differed when the comparison was first made; those were
xlit's (a variable of one character within a set, `16be23054`; before
contexts matched against the unconverted text in groups over
`--whole-group-rules`, `af15fa6d2`) and now agree. ICU4C has no Any-CaseFold
(`U_INVALID_ID`); ICU4J's agrees with xlit. xlit does not implement Any-Name,
Name-Any, Any-BreakInternal, and so Thai-Latn, nor Latn-Grek (87 rules not
implementable).

- **ICU4C ignores a global filter when the rules make a single rule set.** A
  global filter (`:: [set] ;` before the rules) restricts the whole transform
  to the characters of the set: the rules apply to each run of those
  characters, and no rule's text can match a character outside them. ICU4C
  applies it only when the rules have at least one `::` transform besides the
  filter. When they make a single rule set (only the filter and conversion
  rules), the rules apply to every character of the text, as if there were no
  filter. In ICU4C's `Transliterator::createFromRules` (`translit.cpp`,
  line 1071), the case of a single rule set and no `::` block makes a
  `RuleBasedTransliterator` from the rule data alone: the parser's
  `compoundFilter`, the global filter, is not passed. ICU4J's
  `Transliterator.createFromRules` (`Transliterator.java`, line 1677) passes it
  (`new RuleBasedTransliterator(ID, parser.dataVector.get(0),
  parser.compoundFilter)`), and ICU4J follows the filter, as xlit does:

  ```
  rules                                      input   ICU4C   per the filter (xlit, ICU4J)
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
  ICU4J keeps `;` (`አ;`), as xlit does: on the comparison, Latn-Ethi/SERA
  differs from ICU4C on 2 lines and agrees with ICU4J.
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
  transliterators.) In ICU4C's rule parser (`rbt_pars.cpp`), each rule set and
  each block of `::` transforms is added at the end of its list in the forward
  direction and inserted at the front in the backward direction
  (`dataVector.insertElementAt(curData, 0, …)`, line 985;
  `idBlockVector.insertElementAt(…, 0, …)`, line 1044), and the two lists are
  then interleaved from their starts, so the pairing is kept and only the order
  of the pairs is reversed. ICU4J gives the same output as ICU4C on Bopo-Latn
  and Spacedhan-Han (137 and 339 lines of the comparison differ from xlit in
  both).

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
  `A。，b`: in ICU's compound IDs a filter at the start, followed by `;`, is
  the filter of the whole compound (what ICU calls its global filter), so the
  block's ID, written with the first transform's filter in front, reads as
  filtered as a whole. In ICU4C's `TransliteratorIDParser::parseCompoundID`
  (`tridpars.cpp`, lines 373 to 387), a filter at the start of the ID is parsed
  as the compound's filter; when no `;` follows it (`[a] Null;Any-Upper`: the
  filter of the first element, not a filter of its own), the parser backs up
  to read it again as that element's filter, but keeps it as the compound's
  filter too: the assignment `globalFilter = filter` is not in an `else` of
  that test. ICU4J's
  `TransliteratorIDParser.parseCompoundID` (lines 355 to 366) has the same
  code, and ICU4J gives the same output as ICU4C on Han-Spacedhan and the
  transforms invoking it (297 lines of the comparison differ from xlit for
  each of Han-Latin/Names, Hani-Latn, Hant-Latn and Han-Spacedhan, 86 for
  Jpan-Latn, in both).

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

- **Any-Title is not applied at word boundaries.** UTS #35 defines Any-Title
  as one of the "full case transformations, defined by [Unicode] Chapter 3",
  and Chapter 3 (Unicode 18.0, §3.13.2, R3) defines `toTitlecase(X)` by the
  word boundaries of UAX #29: "Find the word boundaries in X according to
  Unicode Standard Annex #29 … For each word boundary, find the first cased
  character F following the word boundary. If F exists, map F to
  Titlecase_Mapping(F); then map all characters C between F and the following
  word boundary to Lowercase_Mapping(C)." xlit does that. ICU (ICU4C and ICU4J, with the
  same output) decides differently around characters that are not letters: a
  letter after a case-ignorable character (an apostrophe, U+FEFF) is
  lowercased, as continuing a word, even where UAX #29 has a word boundary
  before it (an apostrophe after a space, U+FEFF at the start of the text);
  and a letter after a digit or `_` begins a new word, where UAX #29 has no
  boundary (both are word characters). Where both have the word continue, as
  in `a.b` and `c:d`, they agree (`A.b C:d`):

  ```
  input                          ICU                            xlit
  FROM THE 'LONDON TIMES'        From The 'london Times'        From The 'London Times'
  U+FEFF The Project             U+FEFF the Project             U+FEFF The Project
  A._Einstein:_Kommentare_und    A._Einstein:_Kommentare_Und    A._Einstein:_Kommentare_und
  word2word a4paper x_n          Word2Word A4Paper X_N          Word2word A4paper X_n
  ```

  On the comparison, Any-Title differs on 11 lines and nl-Title (Dutch
  titlecasing: Any-Title, then a fix-up of `Ij` to `IJ`) on 16; on every one
  of them the outputs differ only in the case of letters, and on those
  examined, as above. `nl-Title.xml` assumes ICU's notion of a word: its rule fixes up "Ij
  at the beginning of a 'word' (per Any-Title, not UAX #29)". Whether Any-Title
  is meant to follow UAX #29, as UTS #35's reference to Chapter 3 says, or
  ICU's behavior, is a question for CLDR (see also "Flaws in documentation").

- **ICU4J and ICU4C segment Thai text differently.** Thai-Latn invokes
  Any-BreakInternal, which inserts a space at the word boundaries of ICU's
  dictionary-based break iterator. ICU4J's iterator finds more boundaries
  than ICU4C's within some words, so on 4 lines of the comparison ICU4J's
  output has an extra space: `ฟราย` gives `f rāy` (ICU4C `frāy`),
  `ออฟฟิศ.อ็อก` gives `…ṣ̄.x̆ xk` (ICU4C `…ṣ̄.x̆xk`). xlit does not implement
  Any-BreakInternal.

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

- **Greek-Latin's breathing rules (Latin→Greek) revisit captures of
  variable length; a restatement without them (a possible submission,
  2026-11-24).** The backward direction (Latn-Grek) writes the rough breathing
  with 24 rules of the form `| $1 $rough ← h ($evowel $macron? $ddot? …)` (and
  `H`, and titlecase forms `| A $1 $rough ← H a (…)`), and the smooth
  breathing with 3 of the form `| $1 $smooth ← $notLetter { ($evowel $macron?
  $evowel2 $macron?) } [^$smooth$rough]`. Each revisits its capture, a vowel
  cluster of one to five characters, so that the rest of the rules then convert
  it. What they do is a reordering: the rough breathing that `$rough ↔ h`
  writes in place of the `h` is moved after the cluster, and the smooth
  breathing is written after a cluster at the start of a word. Revisits of
  variable-length captures are hard for implementations other than ICU's
  (xlit's planner could not implement Latn-Grek because of exactly these 27
  rules), and they make the order of the rules subtle: an `h` taken by a
  digraph (`th`, `ph`, `rh`, `ch`) is never reached; `j`, `v`, `w` revisit to
  vowels that the smooth breathing and the iota subscript rules then match
  (`w̄i → ῡͅ`); a smooth breathing is written within a rough cluster with a
  diaeresis at the start of a word (`hë → ἐ̈̔`) but not after a letter
  (`ahë → ἀε̈̔`).

  The same results come from rules without revisits, in groups of their own
  before the conversions (Latin→Greek order, as `ldml_trules --backward`
  prints the backward rules; xlit's `tools/ldml/prototypes/Latn-Grek-breathing.txt`
  is the whole transform so restated):

  ```
  :: NFD ;
  ([$evowel jJvwVW] $macron $accentMinus*) i → | $1 $iotasub ;     # (and I)
  :: Null ;
  ph → ph ;  th → th ;  rh → rh ;  ch → ch ;  …                     # the digraphs taking an h
  h ($evowel $macron $ddot? $evowel2_i $macron?) → \uE000 $1 $rough ;
  h ($evowel $ddot? $evowel2 $macron?) → \uE000 $1 $rough ;
  h ($evowel $macron? $ddot?) → \uE000 $1 $rough ;
  …                                                                # H, and the titlecase forms → \uE001 A $1 $rough
  :: Null ;
  $nlm = [^[:l:][:m:]\uE000\uE001] ;
  $nlm [\uE000\uE001]? { ([Rr]) } [^$smooth$rough Hh\uE000\uE001] → $1 $smooth ;
  $nlm [\uE000\uE001]? [$evowel jJ] $macron? $evowel2 { $macron } [^$smooth$rough] → $macron $smooth ;
  $nlm [\uE000\uE001]? [$evowel jJ] $macron? { ($evowel2) } [^$macron$smooth$rough] → $1 $smooth ;
  $nlm [\uE000\uE001]? [$evowel jJ] { $macron } [^$evowel2$smooth$rough] → $macron $smooth ;
  $nlm [\uE000\uE001]? { ([$evowel jJ]) } [^$macron$evowel2$smooth$rough] → $1 $smooth ;
  …                                                                # v, w before a macron or a non-vowel
  :: Null ;
  \uE000 → ;  \uE001 → ;                                           # then the rest of the rules
  ```

  The markers U+E000 and U+E001 stand where the `h` was, so that the rules
  applied before it still see a letter there (`$beforeLetter` and the global
  filter include them); the smooth rules look through them. This restatement
  equals ICU4C's and ICU4J's Latn-Grek on 60,060 lines (random words, the first
  2,000 lines of Twain, and random text dense in `h`/`H`, vowels, macrons,
  diaereses, `j v w`, digraphs and punctuation). As a submission it would need
  writing as backward-only rules of Greek-Latin.xml, placed so that the
  forward direction keeps its groups, with markers that cannot occur in text
  (noncharacters), and checking against CLDR's test data, which has no
  Latin→Greek file. xlit's planner now derives this restatement from the
  family's form (see doc/ldml-xlit-coverage.md).

## Flaws in documentation

Read on 2026-11-22: UTS #35 Part 2, section Transforms (revision 79, CLDR 49,
2026-10-21: https://www.unicode.org/reports/tr35/tr35-79/tr35-general.html);
the ICU User Guide pages Transforms/General, the Transform Rule Tutorial and
Case Mappings (https://unicode-org.github.io/icu/userguide/transforms/);
and the Unicode Standard 18.0, Chapter 3. Quotations are verbatim. The
counts of CLDR files are of `third_party/cldr/common/transforms`.

### UTS #35 (LDML)

- **Filtered transform rules are not specified.** "Transform Rules" defines
  a transform rule as "two colons followed by a transform name, which is of the
  form source-target", with an optional inverse in parentheses (`:: lower () ;`,
  `:: (lower) ;`, `:: lower ;`). A filter before the name, `:: [set] X ;` (or
  `:: ([set] Y) ;` for the inverse), appears nowhere in the syntax or the
  text, yet CLDR uses it: nine rules in eight files (`Han-Spacedhan`,
  `Katakana-Latin-BGN`, `Latin-Jamo`, `Latin-Katakana` (twice), `it-am`,
  `it-ja`, `ja_Latn-ko`, `ja_Latn-ru`). Its meaning is left to ICU (UTS #35 is
  silent on all of these):
  - whether the transform applies to each run of the set's characters by
    itself;
  - whether a match, or a context, may see or span characters outside the set;
  - whether the filter also applies to the `::` rules that follow. ICU's
    carrying it through a block of `::` rules (above) is exactly what xlit and
    ICU disagree on for Han-Spacedhan and the five transforms invoking it.

- **The filter rule's effect is stated in one sentence.** "Filter Rules": "A
  filter rule consists of two colons followed by a UnicodeSet. This filter is
  global in that only the characters matching the filter will be affected by
  any transform rules or conversion rules." The inverse filter rule "is also
  global for the inverse transform". Nothing says:
  - how the text is divided (the word "run" does not occur);
  - whether a conversion rule's text may match across, or its contexts see,
    characters outside the filter;
  - what `$` in a UnicodeSet ("matches the start or end of the source text"),
    and the negation of a set, which "will normally also match before or after
    the end of a string", match at the edge of a run;
  - how the filter rule combines with the filters of transform rules.

  Implementations may differ on all of these without contradicting the text.
  ICU4C's dropping the filter of a single rule set (above) is a plain
  contradiction of it.

- **Function calls are shown, not specified.** The only mention is a row of
  the table "Transform Syntax Characters": "`&` | Invoke a function in the
  replacement_text | `([a-b]) > &hex($1);`". The text does not say which
  transforms may be called, what the argument is (a capture, a string with
  references), how the call relates to the cursor and to filters, or what
  `hex` is: it is not among the transforms the spec defines. CLDR uses calls
  in six files: `&Any-Lower(` and `&Any-Upper(` (`tr-Title`, `lt-Title`),
  `&Any-Title(` and `&Any-Lower(` (`el-Title`), `&lower(` (the SERA and
  ALALOC Ethiopic files), and `&Pinyin-NumericPinyin(`,
  `&NumericPinyin-Pinyin(` (`Latin-NumericPinyin`).

- **Built-in transforms that CLDR uses are not defined.** "Transform Rules"
  defines only "Any-NFC, Any-NFD, Any-NFKD, Any-NFKC - the normalization forms
  defined by [UAX15]", "Any-Lower, Any-Upper, Any-Title - full case
  transformations, defined by [Unicode] Chapter 3", "Any-Null" and
  "Any-Remove". `Thai-Latin.xml` invokes `Any-BreakInternal`, which neither
  UTS #35 nor ICU's user guide defines. Its result depends on the word
  segmentation of the implementation, and ICU4J's and ICU4C's differ (above).
  Any-Hex appears only as an example ID ("Any-Hex/xml"); Any-Name, Any-FCD,
  Any-FCC and Any-CaseFold are not mentioned (ICU has the first three; ICU4J
  has Any-CaseFold and ICU4C does not).

- **Any-Title's words.** UTS #35 refers Any-Title to Chapter 3, whose
  `toTitlecase` uses the word boundaries of UAX #29 (quoted above), and adds
  nothing; Chapter 3 allows tailoring ("The default case conversion operations
  may be tailored for specific requirements") without saying anything about
  word boundaries. ICU's Any-Title decides words differently (above), and a
  CLDR file (`nl-Title.xml`) is written for ICU's behavior ("per Any-Title,
  not UAX #29"). Either the spec should say that Any-Title's words are ICU's,
  or nl-Title should be written for UAX #29.

- **Variables in UnicodeSets.** "Variables are only replaced within other
  variable definition rules and within conversion rules", and "The contents
  of a variable definition is any sequence of Unicode sets and characters or
  characters." The spec does not say what a variable holding one character
  is within a UnicodeSet: a character, which may then end a range (`[$a-$b]`),
  as ICU reads it, or a set. `Latin-InterIndic.xml` and `Thai-ThaiLogical.xml`
  have such ranges; xlit read them as sets (an error) until `16be23054`, which
  made the ten Latin-to-Indic transforms differ from ICU.

- **Minor.** "Any-Remove - maps each character to the empty string; this,
  removes each character." (for "thus").

### ICU documentation

- **"Global filter" means two things, neither of them UTS #35's.**
  - In "Compound IDs" (Transforms/General), the global filter is a filter at
    the start of a compound ID: "This is called a **global filter** to
    distinguish it from filters on the individual transforms within the
    compound", for `[:Latin:]; NFKD; Lower; Latin-Katakana;`. The grammar,
    "`Compound_ID := ( filter ';' )? ( Single_ID ';' )+ ( '(' filter ');' )?`",
    marks it by the `;` that follows it.
  - The Transform Rule Tutorial uses the term for the filter rule `:: [set] ;`:
    "A global filter can also be used with the transform rules", and "The
    global filter will cause any other characters to be unaffected."
  - UTS #35 never uses the term. It says the filter rule "is global in that
    only the characters matching the filter will be affected", a statement
    about its scope over the rules, not a name.

  The two ICU uses differ where it matters:
  - A compound ID's global filter covers all the elements of the compound.
    A rule file's global filter covers its rules.
  - A filter on the first element of a compound ID covers that element only,
    by the grammar. But ICU builds a block of `::` rules into such an ID, and
    its parser keeps the first element's filter as the global filter even
    without the `;` the grammar requires (`tridpars.cpp`, above). So a filter
    on the first `::` rule of a block reads as a global filter of the block.
    That is the carried filter, which ICU's documentation does not describe.
  - In ICU4C, a rule file's global filter is lost when the rules make a single
    rule set (`translit.cpp`, above), which the tutorial's "The global filter
    will cause any other characters to be unaffected" does not allow for.

  A reader comparing ICU with UTS #35 has to keep the three apart. This
  document uses the term for the filter rule only.

- **Filters on single transforms.** "Filtered IDs": "The filter is a valid
  UnicodeSet pattern prefixed to the basic ID", and "Only characters within the
  set will be modified by the transform", for `[aeiou] Upper`. Nothing says
  that, in rules, the filter of the first `::` rule of a block also applies to
  the rules after it, which is what ICU does.

- **How filtering works is not described.** No page says that a filtered
  transform applies to runs of the set's characters, or whether rules see the
  characters outside the filter as context. The only related text, in the
  tutorial's "Æther", is about context limits: "Characters that are outside
  the context limits – contextStart to contextEnd – are also treated as
  æther."

- **Inverses: the documentation follows UTS #35, the implementation does
  not.** "Compound IDs": "The inverse of such a transform is formed by
  reversing the list and inverting each element." The tutorial: "The inverse
  uses the transform rules in reverse order, so the (NFD) goes at the bottom
  and (NFC) at the top." For rules that end with a rule set after a `::` rule,
  ICU's backward transform does not do this (above, Bopo-Latn,
  byn_Latn-byn_Ethi/Tekie_Alibekit, Spacedhan-Han). The documented examples
  are symmetric, so they do not show the difference.

- **Any-Title.** The table of transliterators says "Any-Lower, Any-Upper,
  Any-Title | Converts to the specified case. See Case Mappings for more
  information." The Case Mappings page says what title case is ("used to
  capitalize the first character of a word") but not what a word is for
  Any-Title. ICU's actual treatment of apostrophes, U+FEFF, digits and `_`
  (above) is not documented.

- **Built-ins not listed.** The table of ICU transliterators lists Any-Null,
  Any-Remove, the case and normalization forms (with Any-FCD and Any-FCC),
  Any-Name and the Any-Hex forms. It does not list Any-BreakInternal, which
  CLDR's Thai-Latin uses, or Any-CaseFold, which ICU4J has and ICU4C does not.

- **Minor.** The inverse of the compound-ID example is spelled
  "Katkana-Latin; Upper; NFKC; ([:Latin:]);".
