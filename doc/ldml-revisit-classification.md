# Revisit rules in the CLDR transforms: what the revisit triggers

*October 28, 2026; updated October 29 for the prefix runs (class P), with S
of one or more characters, and their triggers. CLDR
transforms from `~/cldr/common/transforms` (with the local W7 fixes, including
the Greek_Latin_UNGEGN capture fix), tconv with `--revisit-report`.*

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

**932 revisit rules in 46 transforms: 736 implemented, 196 not yet.**

| | Class | What the revisit triggers | Rules | Transforms | Composed rules per revisit rule (median / max) |
|---|---|---|---:|---:|---|
| **Implemented** | **S** suffix | nothing: the text to revisit is the end of the text matched | 125 | 13 | 1 / 1 |
| | **A** inert | no rule of the group | 50 | 7 | 1 / 1 |
| | **B** determined | a fixed sequence of unconditional rules, decided by the known text (and enumerated captures) | 118 | 19 | 1 / 45 |
| | **C** context-conditional | several rules, chosen by contexts on the known text | 3 | 2 | 3 / 9 |
| | **D** lookahead-conditional | several rules, chosen by 1–2 characters that follow the revisited text | 160 | 16 | 18 / 43 |
| | **E** chained | rules that themselves revisit, ending within a few applications | 42 | 6 | 3 / 25 |
| | **P** prefix runs | the rule itself, on P S^n, leaving A^n P; applied first, as a group of their own (after their triggers, which produce P) | 238 | 8 | — |
| **Not yet** | **R** runs | the rule itself (or its family) again, on its own output and the next input, without bound | 34 | 9 | — |
| | **L** long chains | different rules, consuming further input in turn | 3 | 2 | — |
| | **V** variable-length trigger | a rule whose text has a repetition, or an insertion rule | 104 | 8 | — |
| | **N** revisit rule not enumerable | not explored: its referenced capture is not one character, or has more than 64 | 52 | 6 | — |
| | **K** contexts not combinable | contexts with repetitions or optional items that must be combined | 3 | 2 | — |

Two structural facts stand out:

- **Where tconv implements the revisit, the trigger is shallow.** At most 5
  rule applications on a path (B, E), at most 2 characters of lookahead beyond
  the text to revisit (D), and at most 45 composed rules for one revisit rule,
  mostly from enumerating a captured character.
- **Runs were most of what was not implemented (272 of 434).** The revisit
  rule's text to revisit is a *prefix* of a rule's own text (`rh → | r`,
  `([ሀ-ሇ]) $ጥበቅ → $ሀ | $1`, `â្ក → k | â`), so the rule applies again to its
  own output plus the next input character, as often as the input repeats:
  the mirror image of the suffix revisits (S). The 238 whose groups allow
  it (with the rules producing their P, in Khmr-Latn/UNGEGN) are now
  implemented (P); 34 remain (R).

Notation in the examples: `→ d1 [flags] rule` is a rule triggered at
application depth 1 (on the text to revisit), with flags c = conditional,
x = extends beyond the text to revisit, r = revisits in turn, B = blocking.
`⇒` introduces the composed rules that replace a revisit rule, as
`--revisit-report` prints them.

# Part 1. Rules implemented by tconv (736 rules)

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

**Prefix runs** (class P) are not composed: they are moved out of their group
and applied by kernels of their own (see P below).

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

## P: prefix runs (238 rules, 8 transforms)

*Triggers:* the rule itself. A prefix run is a rule `(P) S → A | $1 ;` or
`p S → A | p ;`: its text is a character of a set P followed by a text S of
characters of sets S1 … Sm (m ≥ 1), it has no contexts, and its text to
revisit is its character of P. On `P S^n x`, ICU applies it n times and leaves
`A^n P`, with `P` then converted by the rest of the group (with the after
context `x`). (In the first form ICU also revisits `A`, which then must not
match.) The prefix runs with the same set P form a family, whose texts may
follow each other in a run: with `te\~ → | t ;` and `to\~ → | t ;`, ICU turns
`te~o~e~x` into `tx`.

- Ethi-Latn/ALALOC, Aethiopi, Beta_Metsehaf, IES_JES_1964, Lambdin (167
  rules): `([ሀ-ሇ]) $ጥበቅ → $ሀ | $1 ;`, one per consonant series: on
  `ሀ፟፟፟ል`, `hhh` then `ሀ` (`ha`), then `ል` (`l`): `hhhhal`;
- ro_FONIPA-ja `e̯ → | e ;`, `o̯ → | o ;`, `t͡ → | t ;` and sk_FONIPA-ja
  `t͡ → | t ;`, `r̩ → | r ;`, `u̯ → | u ;` (A empty: the run of marks is
  deleted);
- Khmr-Latn/UNGEGN (65 rules): the prefix runs `â្ក → k | â ;`,
  `ô្គ → k | ô ;`, … (S of two characters, a coeng and a consonant; two
  families, `â` and `ô`), and their 33 triggers, `ក → k | â ;`, … A trigger
  `C → B | p ;` produces the P of a prefix run (the inherent vowel of the
  consonant's series), which ICU then revisits: on `ក្ខ្តា`, `k` and `â`,
  then `kh` and `t` by the prefix runs, then `âា → a`: `kkhta` (on
  `ក្ខ្ញា`, the run stops at `្ញ`, of the other series: `kkhânhôa`).

*Implementation:*

1. *Split* (`ComposeRevisits`, `prefixRunConflict`). The prefix runs of a group
   are moved to a group of their own, placed before the rest of the group and
   separated from it by `:: Null ;`, if the rest of the group cannot tell the
   difference between ICU's order (the runs applied as the cursor reaches
   them) and applying all the runs first:
   - no rule before a prefix run may match its P followed by S (in its text
     or after context), and no rule inserts;
   - no rule matches P after the first character of its text (ICU would not
     reach P), nor A (which ICU has converted), unless ICU revisits A and the
     rule matches A alone;
   - each set of the after contexts contains all of the characters of P, S
     and A or none (unassigned code points aside), and if all, A has as many
     characters as S: an after context sees `A^n P` where ICU saw `P S^n`;
   - no rule revisits P followed by S, or P at the end of its text to
     revisit;
   - the sets P of two prefix runs are equal (a family, whose texts S then
     have the same length) or disjoint, and S and A contain no character of
     P (`prefixRunFamilies`);
   - no two matches of the texts S of a family may overlap, other than a
     match with itself: for each two texts (or one with itself) and each
     shift d < m (0 < d for one text), some position of the overlap has
     disjoint sets. A run is then a sequence of matches, each beginning where
     the one before ends, as ICU applies them.

   The before contexts see `A^n P` as in ICU. Otherwise the rules stay in the
   group, and are reported with the reason (`a prefix run, but …`).

   *Triggers.* The rules `C → B | p ;` (C a set, no contexts, B a fixed
   string, p a character of a P) are applied first, as `C → B p ;`, in a
   group of their own before the prefix runs, except where ICU does not reach
   C: after a character k of a covering rule `k c → … ;` (no contexts, c in
   C), which consumes c with k (Khmer's `្ក → kâ ;`, for a coeng not after
   P). That group is `k { c → c ;` for the covered pairs, then the triggers.
   The conditions, beyond those above (`prefixRunConflict`):
   - no rule before a trigger may match its C;
   - no rule matches a character of C after the first character of its text,
     other than the covering rules, and the texts S, where each such character
     must be covered after the character before it (in Khmer, every consonant
     of a run has a covering rule);
   - ICU applies a covering rule wherever its k c occurs: no other rule that
     may match k followed by c comes before it, and no rule matches k after
     the first character of its text (the coeng's own `្ → ;` comes last);
   - no rule revisits a character of C; B contains no character of C, P, S or
     k, and the other rules may not match it (as A).
2. *Plan* (`planTransform`). A group of prefix runs alone becomes a step with
   `TransformStep::prefixRuns` (P, S, A for each rule).
3. *Kernel* (`PrefixRuns` in tconv). By family, the families with the same
   texts S sharing a run class, with the character classes of the sets P and
   of the sets of S, and a lookahead of the longest m:
   - for each text t, `Full_t = Lookahead(S1, 1) & … & Lookahead(Sm, m)`
     (each also within the filter), and `Full` their union for the class;
   - `Fill = Advance(Full, 1) | … | Advance(Full, m)`, the positions within
     matches, and `Ends = Advance(Full, m)`, those that end one;
   - `PS = P & Full`, then, within an `If` on the union of the PS,
     `run = MatchStar(Advance(PS, 1), Fill) & Fill`. As matches do not
     overlap, the run is a sequence of matches, each beginning after the one
     before.

   The anchors, `PS | (run & Ends & Full)` (P and the ends followed by another
   match), get the A of the text that follows (`Full_t`). The last end,
   `run & Ends & ~Full`, gets the character of P, carried along the run bit
   by bit: for each of the 21 basis bits b, `MatchStar(Advance(PS & b, 1),
   Fill) & last` (once for all the families of a run class). The other
   positions of the run (`run & ~Ends`, when m > 1) are deleted. The anchors
   get 0, and the kernel writes, as bit streams:
   - the mask of the anchors, with the index of their A among the k
     distinct strings A multiplexed on ⌈log₂ k⌉ selector streams;
   - the number of positions to insert after each, |A| − 1 (a BixNum), if some
     A has more than one character;
   - the positions to delete, those of empty strings A and within the texts,
     if any.

   With m = 1, `Fill` and `Ends` are S, and this is the kernel of the single
   characters S. The run stops at characters outside the filter.
4. *Insertion and fill.* The positions are inserted (`InsertionSpreadMask`,
   then `SpreadByMask` of the text, the mask and selectors, the deletions and
   the filter masks, whose inserted positions inherit them). The mask and
   selectors, shifted forward j positions (`ShiftForward`, for j < max |A|),
   mark the positions of the j-th characters of each A, whose bits are then
   set (`PrefixRunFill`). Finally, the positions of empty strings A, and
   those within the texts, are deleted (`FilterByMask`, with the filter
   masks).

On Ethi-Latn/ALALOC (33 prefix runs, many distinct A of one to three
characters, within the filter `[:Ethiopic:]`) the step has 12 kernels; on
ro_FONIPA-ja (3 prefix runs, A empty) 5.

*Verification.* For the 7 transforms (and the 5 using them: Ethi-Latn, ro-ja,
and the 4 `_Geminate` variants), the output matches ICU (`uconv -x` on the
same CLDR rule text): 408 random texts each, rich in runs, including runs of
40 marks crossing block boundaries, and a 15 MB text with runs of up to 700
marks, with 1 and 3 threads. The CLDR test data of these transforms (which
have no runs) passes. `QA/tconv_prefix_runs_test.py` (`make check`) covers a
captured P with A of two characters (and a run of 600), a literal P with an
empty A, A of one character, and A of three, zero and one characters in one
group within a filter, and texts S of two and three characters (families of
two texts mixed in runs, of 300 matches), against ICU's results. Prefix runs
with S of one to three characters were fuzzed against ICU's `createFromRules`
(`uconv -x`): 381 random rule sets (families of one to three texts, captured
and literal P, sets within the texts, A of zero to three characters, other
rules, filters), 5 random lines each, some with runs of up to 400 matches,
also with 3 threads and segment size 1, all identical; 69 more were refused
as overlapping. The extension changes the plan of no CLDR transform.
With the triggers, Khmr-Latn/UNGEGN (which has no CLDR test data) is
identical to ICU 78.3 (`uconv -x` on its rule text, and through the registry
name) on 9,000 random lines of Khmer syllables with stacks of up to six
subscript consonants, vowels, signs, independent vowels, stray coengs and
`â`/`ô` (also with 3 threads and segment size 1). Random groups of triggers,
prefix runs of two characters, covering rules, rules on P and a final rule
on k were fuzzed against ICU: 244 implementable rule sets, 20 lines each,
identical; 128 were refused (a run character without a covering rule, or a
rule on k before the covering rules). Of the CLDR transforms, only
Khmr-Latn/UNGEGN changes plan. The CLDR test data of the 4 `_Geminate`
variants (6,888 cases each) passes: their first step, am_Ethi-am_Ethi/Geminate,
inserts the gemination marks that the prefix runs then apply to. (It crashed on
macOS until the JIT compile threads were given 8 MB stacks: LLVM's
ScalarEvolution overflowed the 512 KB stack of a std::thread compiling its
7,718 string rules.)

# Part 2. Rules not yet implemented (196 rules)

tconv leaves these rules unchanged, and reports their transforms as not
implementable ("the result has text to revisit", with the reason the
composition was abandoned).

## R: runs (34 rules, 9 transforms)

*Triggers:* the revisit rule itself, or a rule of the same family. The
revisited text is the *start* of the rule's text, so each application leaves
text that the rule matches again together with the next input. The composer
gives up when the match extends more than 4 characters beyond the text (or
after 8 applications). These are the "unbounded runs" of the coverage report
(§5, W6).

The 238 prefix runs and triggers of class P were in this class. What remains:

- ja_Latn-ru and ja_Latn-ko `te\~ → | t ;`, `to\~ → | t ;` (prefix runs with
  S of two characters, A empty) and `tsu\~ → | ts ;` (P of two characters).
  In ja_Latn-ru, the after context of `\~ } tsu → ;` would see `te~su`
  become `tsu`; in ja_Latn-ko, that of `t } t → ᆺ` sees the `t` of a run
  (its single set sees only P, which a run with A empty leaves in place, so
  this conflict could be refined). Both have other revisits;
- prefix runs whose groups do not allow the split: xh-xh_FONIPA `rh → | r ;`
  (`gr → ɣ` would match its `r` within its text), it-am and it-ja
  `($consonant) \' → | $1 ;` (`cqu`), fa-fa_FONIPA `([َ-ِ]) ّ → ّ | $1 ;`
  (`یّ → jj` matches its replacement, a shadda, with another character),
  pl_FONIPA-ja `t͡ → | t ;` (the after context of `^ t } t → ト` sees its `t`);
- rules of other shapes in the same transforms: it-am and it-ja `tz → | zz ;`,
  `ss → | \~s ;`, `($consonant) s } $vowel → | $1 \~s ;`; ja_Latn-ru
  `i $lengthMarker → | ii ;`; ja_Latn-ko `e $lengthMarker → | ei ;`.

*Possible implementation:* prefix runs with S of several characters, in
families, and their triggers are implemented (above). The conflicts of the
split are specific to each group; some (xh, it) are real: ICU would not reach
P.

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
