# Revisit rules in the CLDR transforms: what the revisit triggers

*October 28, 2026. CLDR transforms from `~/cldr/common/transforms` (with the
local W7 fixes, including the Greek_Latin_UNGEGN capture fix), tconv built
from `c2853ea32` plus the uncommitted `--revisit-report` instrumentation.*

A revisit rule is a conversion rule whose result places the cursor before
part of its output (`X → A | B ;`): ICU then continues matching at `B`, so the
rules of the group may apply again to `B`, and through `B` to the text that
follows. This report classifies every such rule by the rules its revisit
triggers: how many, and of what kind. Part 1 covers the rules tconv already
implements, and how; Part 2 the rules it does not implement yet.

## Method

`tconv --revisit-report <transform>` runs the usual analysis and, for each
rule with text to revisit in the transform's own rules (in the direction of
the transform, after function calls on captures are expanded), records what
`ComposeRevisits` (`lib/ldml/transform_rules_revisit.cpp`) found:

- the outcome: rewritten as a suffix revisit, composed, or not composed (with
  the reason);
- each rule that applies to the text to revisit along any path of the
  composition, with flags: *conditional* (on characters beyond the text to
  revisit or contexts not implied by the known text), *extends* (matches
  characters beyond the text to revisit), *revisits* (its own result has text
  to revisit), *blocking* (the rule composition was abandoned on);
- the composed rules replacing it, the number of complete paths, and the
  most rule applications on a path.

The composer explores the rules of the group in order at the cursor, with the
first rule whose conditions are implied by the known text ending the
alternatives, so a rule is counted as triggered only if it can actually apply
there. For rules not composed, the triggered rules are those found before the
composition was abandoned.

All 422 rule-defined transforms were run (Hani-Latn, Hant-Latn and Jpan-Latn
skipped for memory; their own files have no revisit rules, and the transforms
they invoke were run separately). Each transform's records are reproducible
with `tconv --revisit-report NAME` (one JSON object per line).

## Summary

**932 revisit rules in 46 transforms: 498 implemented, 434 not yet.**

| | Class | What the revisit triggers | Rules | Transforms | Composed rules per revisit rule (median / max) |
|---|---|---|---:|---:|---|
| **Implemented** | **S** suffix | nothing: the text to revisit is the end of the text matched | 125 | 13 | 1 / 1 |
| | **A** inert | no rule of the group | 50 | 7 | 1 / 1 |
| | **B** determined | a fixed sequence of unconditional rules, decided by the known text (and enumerated captures) | 118 | 19 | 1 / 45 |
| | **C** context-conditional | several rules, chosen by contexts on the known text | 3 | 2 | 3 / 9 |
| | **D** lookahead-conditional | several rules, chosen by 1–2 characters that follow the revisited text | 160 | 16 | 18 / 43 |
| | **E** chained | rules that themselves revisit, ending within a few applications | 42 | 6 | 3 / 25 |
| **Not yet** | **R** runs | the rule itself (or its family) again, on its own output and the next input, without bound | 272 | 15 | — |
| | **L** long chains | different rules, consuming further input in turn | 3 | 2 | — |
| | **V** variable-length trigger | a rule whose text has a repetition, or an insertion rule | 104 | 8 | — |
| | **N** revisit rule not enumerable | not explored: its referenced capture is not one character, or has more than 64 | 52 | 6 | — |
| | **K** contexts not combinable | contexts with repetitions or optional items that must be combined | 3 | 2 | — |

Two structural facts stand out:

- **Where tconv implements the revisit, the trigger is shallow.** At most 5
  rule applications on a path (B, E), at most 2 characters of lookahead beyond
  the text to revisit (D), and at most 45 composed rules for one revisit rule,
  mostly from enumerating a captured character.
- **Almost everything not implemented is a run (R, 272 of 434).** The revisit
  rule's text to revisit is a *prefix* of a rule's own text (`rh → | r`,
  `([ሀ-ሇ]) $ጥበቅ → $ሀ | $1`, `â្ក → k | â`), so the rule applies again to its
  own output plus the next input character, as often as the input repeats:
  the mirror image of the suffix revisits (S).

Notation in the examples: `→ d1 [flags] rule` is a rule triggered at
application depth 1 (on the text to revisit), with flags c = conditional,
x = extends beyond the text to revisit, r = revisits in turn, B = blocking.
`⇒` introduces the composed rules that replace a revisit rule, as
`--revisit-report` prints them.

# Part 1. Rules implemented by tconv (498 rules)

## How tconv implements revisits

tconv implements no revisit at run time. `ComposeRevisits`, early in the
analysis of a transform (after function calls on captures are expanded, before
capture simplification, finite expansion, order disambiguation and planning),
replaces each revisit rule by ordinary rules without revisits, at the revisit
rule's position in its group. The rules replacing it then take the ordinary
path: single-character rules become character-map subgroups, longer ones
string rules (with the overlaps resolved as for any other string rules), and
their contexts are implemented as for any other rule. Two mechanisms are used.

**Suffix rewrite** (`Composer::suffixRevisit`, class S). If the text to revisit
is the text matched by the end of the rule's text,

```
before { P S } after → A | S ;   ⇒   before { P } S after → A ;
```

leaves the same text with the cursor in the same place, so the revisit is just
an after context. These rules are rewritten first, and the rewritten rules are
the ones the compositions below apply. Conditions: `S` is one or more single
characters (or captures) equal to the last items of the text; if the result
has references to captures, the completed result ends with one (ICU then
places the cursor after it), and refers only to captures of `P`.

**Composition** (`Composer::composeRule`, classes A–E). The rest are composed
with the rules of their group by a symbolic run of the transliterator over the
text to revisit:

1. *Enumeration.* Each capture referenced by the result must be a single
   character from a set of at most 64; the rule is composed separately for
   each character (the capture replaced by the character in the text), so the
   result is a fixed string: a completed part, already output, and the text to
   revisit, pending.
2. *Exploration* (`explore`). At the start of the pending text the rules of
   the group are tried in order. A rule applies if its text matches the pending
   characters; text beyond the pending text matches characters *beyond*, kept
   as sets (at most 4). Its before context is matched against the output so
   far, then conjoined with the revisit rule's own before context; its after
   context against the rest of the pending text, then the characters beyond.
   Applying a rule appends its completed result to the output and puts its own
   text to revisit, if any, back in front of the pending text (so chains are
   followed, up to 8 applications). A rule whose match is implied by the known
   text alone ends the alternatives (ICU would take it); otherwise it is one
   alternative, and later rules, and finally passing the first pending
   character through unchanged, are the others.
3. *Emission* (`finish`). When nothing is pending, the path gives one rule:
   the revisit rule's text followed by the characters beyond, with the
   conjoined before and after contexts, and the output as a fixed result.
   Paths are emitted in the order explored (most specific first, the
   pass-through path last), and a rule whose condition repeats an earlier one
   is dropped, since it could never apply.

The composition is abandoned (and the rule left unchanged, so the transform is
not implementable) when a rule that may apply is not a fixed-length sequence
of sets, the characters beyond exceed 4, the applications exceed 8, more than
1024 rules result, a captured or referenced set has more than 64 characters,
contexts other than sequences of sets would have to be combined, or a cursor
is placed beyond a result. Part 2 is the rules for which this happens.

The classes below differ in which of these steps do the work.

## S: suffix revisits (125 rules, 13 transforms)

*Triggers:* nothing; the text to revisit is the end of the matched text.

*Implementation:* the suffix rewrite. The repeated text becomes an after
context, and is then converted by whatever rule applies to it in the input;
runs need nothing special (`kk → x | k` on `kkkk` is `xxk`, since
`k } k → x` applies at each `k` but the last).

- it-am `cc → | c ;` ⇒ `c } c → ;`, es_FONIPA-ja `bb → ッ | b ;` ⇒
  `b } b → ッ ;` (gemination: the second consonant is converted normally);
- es-es_FONIPA `$bow { ct → | t ;` ⇒ `$bow { c } t → ;`, cs_FONIPA-ja
  `d͡ʒ → | ʒ ;`.

Mostly the FONIPA→ja/am transforms, it-am, it-ja and ja_Latn-ko.

## A: inert revisits (50 rules, 7 transforms)

*Triggers:* no rule of the group applies to the text to revisit.

*Implementation:* composition with a single path that passes every pending
character through: one rule with the revisit dropped, `X → A | B` ⇒ `X → AB`.

- gz_Ethi-Sarb `፲፻ → | 𐩿𐩱𐩿 ;` ⇒ `፲፻ → 𐩿𐩱𐩿 ;` (21 such numeral rules): the
  result is already in the target script;
- pl_FONIPA-ja `ç → | h ;`, es_FONIPA-zh `ð → | d ;`: the replacement letter
  is not converted in that group;
- Latn-Ethi/ALALOC `J → | j ;`: ALALOC spells this sound `ǧ`, so `j` has no
  rule.

Several of these are probably authoring slips (the author expected a rule to
apply to the revisited letter).

## B: determined (118 rules, 19 transforms)

*Triggers:* one rule (B1, 45 rules), or a fixed sequence of rules over the
text to revisit (B2, 73 rules), each implied by the known text.

*Implementation:* composition with one path per enumerated capture value: the
rules applied are folded into the result. A rule with a captured character
gives one rule per character of the capture's set.

- Latn-Grek `c → | k ;` → `d1 k → κ` ⇒ `c → κ ;` (normalizing an alternative
  spelling to the one the group converts: 10 such rules in Latn-Grek);
- Hebr-Latn `ױ → | וי ;` → `d1 ו → w`, `d2 י → y` ⇒ `ױ → wy ;`;
- Latn-Ethi/SERA `U → | $glottal u ;` → `d1 $glottal $ኡ → ኡ`;
- si-si_FONIPA `ඃ ([ක-ෆ]) → | $1 ් $1 ;` ⇒ 45 rules, one per consonant,
  `ඃක → kk ;`, `ඃග → ɡɡ ;`, `ඃඟ → ᵑɡᵑɡ ;`, … (each applies the consonant's
  rule, then `් → ;`, then the consonant's rule again);
- my-my_Latn `($consonants) ြောင့် → | $1 yount ;` ⇒ 34 rules, one per
  consonant (`က → k`, `ခ → hk`, …).

## C: conditional on contexts within the known text (3 rules, 2 transforms)

*Triggers:* several rules, which apply or not depending on the contexts of
the revisit rule's match.

*Implementation:* composition, with the before and after contexts of each
triggered rule carried into its composed rule (conjoined with the revisit
rule's own contexts), and the unconditional rule last.

- Grek-Latn/UNGEGN `ϒ → | Υ ;` → `d1 [c] $fmaker { Υ } $softener → V $under`,
  `d1 [c] $fmaker { Υ → U $under`, `d1 Υ → Y` ⇒
  `$fmaker { ϒ } $softener → V̱ ;`, `$fmaker { ϒ → U̱ ;`, `ϒ → Y ;`;
- Ethi-Latn/ES3842 `([ሀሐኀአኣዐዓ]) ይ → | $1 i ;` ⇒ 9 rules, `ሀይ → hai ;`, …,
  `$wordBoundary { አይ → ai ;`, `አይ → ai ;`, … (the word-boundary condition of
  the rule for `አ` carried into the composed rule).

## D: conditional on the text that follows (160 rules, 16 transforms)

*Triggers:* several rules that begin with the text to revisit and continue with
1–2 characters of the following input, or have after contexts on it.

*Implementation:* composition, with the characters beyond appended to the
composed rule's text (one rule per continuation, enumerated where the result
references them) and the after contexts on the input carried over; the path
passing the text through gives the last rule, without the extension.

- Latin-InterIndic `q → $uka | $virama ;` → `d1 [cx] $virama ā → $aa`,
  `d1 [cx] $virama ai → $ai`, … ⇒ 43 rules: `qā → $uka $aa ;`,
  `qai → $uka $ai ;`, …, `q } $endThing → $uka ;`, and finally
  `q → $uka $virama ;` (the virama is cancelled by a following vowel, or
  dropped at the end; 55 such rules in Latin-InterIndic);
- Sinh-Latn/UNGEGN `ක → k | a ;` ⇒ 18 rules: `ක් → k ;`, `කා → kā ;`,
  `කැ → kæ ;`, …, `ක → ka ;` (an inherent vowel replaced by a following vowel
  sign or removed by a virama; 41 rules);
- Latn-Ethi/SERA `M → | m ;` → `$መ $ግዕዝ → መ`, `$መ $ካዕብ → ሙ`, … (15 rules):
  the lowercased consonant combines with the vowel that follows;
- Latn-Kana `by } $vowel → ビ | \~y ;` → `\~ya → ャ`, `\~yi → ィ`, …;
- Latin-ThaiLogical `g → | k ;` → `k̄h → ข`, `kh } $notHAccent1 → ค`, ….

This is the largest implemented class. Each composed rule is a string rule of
length at most |text| + 2.

## E: chained (42 rules, 6 transforms)

*Triggers:* rules whose own results have text to revisit, ending within at
most 5 applications.

*Implementation:* composition, following each triggered rule's own revisit by
putting its text to revisit back in front of the pending text; the composed
rules fold the whole chain, with characters beyond and contexts as in D.

- zh_Latn_PINYIN-ru `Ju → Цз | ü ;` → `d1 [r] ü → ю | v`, then
  `d2 [cx] ve → э`, `d2 [c] v } [an] → `, or `d2 v → й` ⇒
  `Jue → Цзюэ ;`, `Ju } [an] → Цзю ;`, `Ju → Цзюй ;`;
- ja_Latn-ko `fu → | hu ;` → `d1 h → ᄒ`, `d2 [cxr] u\~a → | wa`,
  `d3 wa → ᅪ`, … ⇒ `fu\~a → 화 ;`, `fu\~i → 휘 ;`, `fu\~e → 훼 ;`,
  `fu\~o → 훠 ;`, `fu → 후 ;` (the results are conjoining jamo, `ᄒ ᅪ` for
  `화`, composed by the transform's final `::NFC`);
- Latin-ConjoiningJamo `([bdghj-npst]) → | $1 eu ;`: 38 triggered rules over
  the consonant, the inserted `eu` and the following Latin medial, 24 composed
  rules;
- ro_FONIPA-ja `ŋ → | nɡ ;` → `d1 [r] ɡ → | g` (a normalization chain).

# Part 2. Rules not yet implemented (434 rules)

tconv leaves these rules unchanged, and reports their transforms as not
implementable ("the result has text to revisit", with the reason the
composition was abandoned).

## R: runs (272 rules, 15 transforms)

*Triggers:* the revisit rule itself, or a rule of the same family. The
revisited text is the *start* of the rule's text, so each application leaves
text that the rule matches again together with the next input. The composer
gives up when the match extends more than 4 characters beyond the text (or
after 8 applications). These are the "unbounded runs" of the coverage report
(§5, W6).

- Ethi-Latn/* (ALALOC, Aethiopi, Beta_Metsehaf, IES_JES_1964, Lambdin: 167
  rules) `([ሀ-ሇ]) $ጥበቅ → $ሀ | $1 ;`: a gemination mark after a syllable
  emits the consonant and revisits the syllable, which a further gemination
  mark matches again (`ሀ ጥበቅ ጥበቅ …`);
- Khmr-Latn/UNGEGN (65 rules) `â្ក → k | â ;`, `ô្គ → k | ô ;`, …: every
  `â្C` rule revisits `â`, which begins all of them, so a run of subscript
  consonants `â ្C1 ្C2 …` is consumed one at a time;
- xh-xh_FONIPA `rh → | r ;` (`rhhh…`), sk_FONIPA-ja and pl_FONIPA-ja
  `t͡ → | t ;`, ro_FONIPA-ja `e̯ → | e ;`, ja_Latn-ru and ja_Latn-ko
  `te\~ → | t ;`, it-am and it-ja `($consonant) \' → | $1 ;` (`C'''`),
  fa-fa_FONIPA `([َ-ِ]) ّ → ّ | $1 ;` (a vowel moved after a run of shaddas),
  ja_Latn-ru `i $lengthMarker → | ii ;`.

*Possible implementation:* a *prefix revisit* rewrite, the counterpart of the
suffix rewrite, turning `P S → A | P` (with `P` the revisit and `S` fixed)
into a run rule `P S+ → A^n P`. In the Ethiopic gemination rules the repeated
output `A` depends only on the captured syllable (8 values per series), so
enumeration gives fixed run rules, which the W3 run mechanism (MatchStar
within a mask) handles. In Khmer each element of the run (`្C`) selects its
own output, so the run needs a per-element mapping followed by `P`. Within a
group this is the `While` fixpoint (issue #21) restricted to one rule family.

## L: long chains (3 rules, 2 transforms)

*Triggers:* different rules, alternately consuming further input.

- ja_Latn-ko `[^$consonant] { ([aeiouwy]) → ᄋ | $1 ;` and `i\~e → | ie ;`:
  `e ̄ → | ei`, `e → ᅦ`, then the null-initial rule on `i`, then
  `i\~e → | ie`, …: it ends, but not within 8 applications;
- my-my_Latn `($consonants) ($consonants) → | $1 a $2 ;`: the second
  consonant can begin `($consonants) ြောင့် → | $1 yount`, which needs 6 more
  characters, beyond the limit of 4.

*Possible implementation:* raising the limits for these rules (the my-my_Latn
case is finite), or the fixpoint of R for ja_Latn-ko.

## V: a variable-length rule may be triggered (104 rules, 8 transforms)

*Triggers:* a rule whose text is not a fixed-length sequence of characters.
The composition stops at the first such rule. Almost all come from 7 rules:

| Blocking rule | Transform | Revisit rules blocked |
|---|---|---:|
| `([:lo:]) ([:zs:]+) ([:lo:]) → \| $1 ⁄⁂⁄ $2 $3 ;` | am_Ethi-d0_Morse | 52 |
| `($lcgvowel+ $ddotmac?) $rough → h \| $1 ;` | Grek-Latn | 17 |
| `($shiftForwardVowels*) v $under → υ $1 ;` | Latn-Grek/UNGEGN | 12 |
| `($evowel $macron $accentMinus*) i → \| $1 $iotasub ;` | Latn-Grek | 6 |
| `[{i̯}iɪ]+ → i ;` / `[{i̯}ij]+ → i ;` | cs_FONIPA-ja / es_FONIPA-ja | 6 + 6 |
| `[Pp] { } [ΣςσϷϸϺϻ] → \' ;` (insertion) | Grek-Latn/UNGEGN | 3 |

For example, am_Ethi-d0_Morse `ፙ → | ምያ ;` revisits `ምያ`, whose `ም` (or `ያ`)
may begin the word-separator rule `([:lo:]) ([:zs:]+) ([:lo:])`. In most V
cases the blocking rule matches only a first character of the text to revisit
(a letter, a vowel), and the rest of its text is a run in the following input,
so V reduces to "a run follows the revisit".

The 3 Grek-Latn/UNGEGN cases are spurious: `explore` rejects a rule that is not
of fixed length as soon as its first items match, before checking its contexts.
The insertion rule `[Pp] { } [ΣςσϷϸϺϻ]` has no text items, so it "matches"
anywhere, but its after context cannot match `φ` (`ϕ → | φ ;`), `β` or `Υ`.

*Possible implementation:* check a non-fixed rule's contexts before
abandoning (the 3 Grek-Latn/UNGEGN rules); for the rest, let the W3 run rules
be triggered rules, with the run read from the following input (in the 52
am_Ethi-d0_Morse cases only the first item `[:lo:]` of the blocking rule
involves the revisited text).

## N: the revisit rule cannot be enumerated (52 rules, 6 transforms)

*Triggers:* not explored. The rule's own result references a capture that is
not a single character (`Latn-Grek ($evowel $macron $accentMinus*) i → | $1 $iotasub ;`,
28 rules; Latin-ThaiLogical `h ($notAbove*) ̄ → ห | $1 ;`; Latn-Arab
`t ($notAbove+) ̈ → ة | $1 ;`; Zawgyi-my `^ ၄ ($nondigits) → | ၎ $1 ;`), or a
capture of more than 64 characters (Grek-Latn `Ε ($glower) $rough → H | ε $1 ;`).
Most move a run of combining marks (`$1`) past a letter, and revisit the
marks: they are reorderings, and the revisit only lets later rules convert the
moved marks.

*Possible implementation:* revisit captures of runs of marks as a whole (the
reordering is already handled by MoveRegions); then they would fall into B
or D.

## K: contexts that cannot be combined (3 rules, 2 transforms)

Latn-Grek/UNGEGN `W } $vowel → | B ;` → `B } $beforeLower → Μπ ;` (two
contexts with repetitions), and Latn-Kana `þ → | th ;` (a context with an
optional item, `$voweled_basekana [゙゚]?`).

*Possible implementation:* combine contexts as regular expressions rather than
as sequences of sets.

## Other observations

Inert revisits (A, 50) and some determined ones (Latn-Grek `c → | k`) look
like candidates to report to CLDR as simplifications, not as bugs.
