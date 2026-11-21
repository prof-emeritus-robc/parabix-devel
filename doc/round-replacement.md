# Rounds in xlit: candidates for Pablo While loops

Survey of branch LDML at `d3d0aadc1` (2026-11-17); status re-surveyed at `d7854835d` (2026-11-19), see the next section. DivCode now resolves before contexts matched against the converted text with a monotone Pablo `While` loop (doc/ldml-divcode-plan.md §10–§12). This document lists every other place in `tools/ldml/xlit.cpp` that iterates by a fixed number of rounds, classifies them, gives the transforms whose own rules use each one, and counts the unrolled Pablo copies or duplicated kernel calls.

## Status at `d7854835d` (2026-11-19)

Since the survey: the correction kernel's While loop (section 1, three increments: bbf6441b9, f54649389, be71bdebb), Unicode properties as kernels of their own (ad6249224), DeferredPlan's closure as a While loop (section 4, 5a372fd94), and, from the masked-transforms work (doc/masked-transforms-plan.md), case-map marks (c85065bb7), fused priority (42afb550b), contexts bounded by regions (cf7678185) and masked NFC (a0fbb5024).

**Method of the re-survey.** A build of `d7854835d` with a temporary trace (not committed): for each `FusedRules` correction kernel, whether `resolvableByWhile` holds (`W`) or the rounds remain (`R`), with its string rules and subgroups and how many of each are matched against the converted text; and a line for each staged selection and each deferred-rule group. `xlit --kernel-counts` for all 453 transform names (the pipelines are built, not compiled). A transform is counted if it or a transform it invokes has the kernel.

| Mechanism | At the survey | Now | Remaining |
|---|---|---|---|
| 1. Correction kernel | 34 groups, all on rounds | 21 of those groups on the While loop, 12 on rounds; Latin-NumericPinyin #1 no longer has a correction kernel. 63 transforms have a While correction kernel, 34 one on rounds | converted string rules (all 12 groups). Done by step 3 (2026-11-19): all 34 groups on the loop, no correction kernel on rounds in any transform |
| 2. Staged sequential selection | 13 groups | unchanged: 13 groups, 59 transforms; still 21 kernels more at 8 rounds than at 1 | all |
| 3. Deferred-rule rounds | Kana-Latn, fa-fa_FONIPA | unchanged; Kana-Latn #1 946 kernels at 8 rounds, 162 at 1 (Jpan-Latn 3,084 in all) | all |
| 4. DeferredPlan blocking depth | 9 unrolled steps | While loop, committed (5a372fd94) | checks, see section 4 |
| 5. Any-FCC passes | 422 kernels | 318 kernels (Any-NFC 75): each pass is now a masked NFC on the 21-bit basis | not a candidate |
| 6. Stale text | | still stale (`include/ldml/divcode_plan.h` 63–66, 155–156; doc/ldml-xlit-coverage.md §3.1, §3.2) | edited (2026-11-20) |

**The correction kernels still on rounds.** Every one has string rules whose before contexts are matched against the converted text; none is held back by its contexts' form (no repetition that `ContextExpansion` cannot give as `X T*`). Converted string rules are therefore the one remaining blocker of section 1.

| Transform (and those invoking it) | Class | String rules (converted) | Subgroups (converted) |
|---|---|--:|--:|
| ThaiLogical-Thai (Latn-Thai) | 1E | 1 (1) | 1 (1) |
| ru-zh | 1A | 22 (1) | 2 (0) |
| ia-ia_FONIPA (ia-am, ia-ar, ia-chr, ia-fa) | 1A | 29 (1) | 7 (0) |
| ja_Latn-ru | 1B | 31 (1) | 26 (7) |
| Latn-Kana (Latn-Hira) | 1C | 240 (2) | 60 (5) |
| Latn-Cyrl | 1D | 70 (2) | 24 (4) |
| zu-zu_FONIPA (zu-am, zu-ar, zu-chr, zu-fa) | 1A | 18 (2) | 12 (4) |
| es-es_FONIPA (es-am, es-ar, es-chr, es-fa, es-ja, es-zh and the es_419-* forms: 13) | 1A | 12 (2) | 35 (22) |
| el-el_Latn/BGN | 1A | 73 (18) | 36 (4) |
| hy-hy_Latn/BGN | 1C | 36 (36) | 4 (4) |
| blt-blt_FONIPA | 1C | 48 (48) | 22 (22) |
| ja_Latn-ko | 1B | 80 (60) | 81 (46) |

**The groups of the survey now on the While loop:** 1A Hans-Hant, Latin-InterIndic (and the ten Latn-* Indic transforms), Latn-Ethi/SERA, und_FONIPA-ar (and 21 *-ar transforms), ps-ps_Latn/BGN, Zawgyi-my #9; 1C ConjoiningJamo-Latin (with its 54 insertion rules), uz_Cyrl-uz_Latn, ky-ky_FONIPA, es-ar, es-fa, es_419-ar, es_419-fa; 1D az-, tr-, el-, lt-Title and Grek-Latn #2; 1E Spacedhan-Han, my-my_FONIPA #4, Zawgyi-my #3. Also, not in the survey's tables, the Han-Latin insertion group (Han-Latin/Names, Hani-Latn, Hant-Latn, Jpan-Latn, Han-Spacedhan), Bopo-Latn and NumericPinyin-Latin. For the four 1A groups over `--whole-group-rules` (Hans-Hant, Latin-InterIndic, Latn-Ethi/SERA, und_FONIPA-ar) only the subgroups are in the kernel: their string rules' before contexts are still matched against the text itself (the `--plan` warnings), so these are exact only once converted string rules are resolved too.

**Interactions with the masked-transforms work.** The While loop and the rounds both walk the converted text back within a region when contexts are bounded by `--lines` regions (`FusedRulesConfig::region`, cf7678185). With case-map marks, a marked position's outcome in the loop comes from static classes of the case mapping (`caseMapLengthChars`, `caseMapSlotChars`, `caseMapItemChars`); the Title transforms' class kernels shrink (lt-Title 2,069 Pablo lines against 23,282), but their execution cycles on 10 MB of Wikibooks text rose 1–8% (c85065bb7, to be optimized).

## Method

- `xlit --plan` and `xlit --divcode-report` for all 453 transform names: groups with before contexts matched against the converted text, and their DivCode eligibility and problems.
- `xlit --kernel-counts` per group at `--converted-context-rounds` 8, 1 and 0, and with `--divcode-groups=0`, to tell DivCode groups from regular-plan groups and staged chains from single kernels.
- Eligible groups whose kernel counts did not change with DivCode off were compiled and checked by kernel name in the object cache (el-Upper, lt-Upper, pl_FONIPA-ja #2 and ru-ru_Latn/BGN #2 are DivCode; ja_Latn-ko #2 and ro_FONIPA-ja #2 are not).
- `-ShowPablo` with the object cache off, at 8 rounds and at 1 round, gives the size in lines of the correction kernel. Group numbers are those of `--kernel-counts` and `--divcode-report`.
- Only transforms whose own rules contain the group are listed; Latn-Grek (not implementable) is left out.

## Summary

| Mechanism | Where | Groups (transforms) | Unrolled copies / duplicated kernels at the defaults | Replace by While? |
|---|---|--:|---|---|
| 1. Correction kernel (`FusedRules`, `rounds > 0`) | `FusedRules::generatePabloMethod`, `for (round = 1; round <= mConfig.rounds; ...)` | 34 (33) | 9 copies of the round body (round 0 + 8) in one kernel | Yes: highest value |
| 2. Staged sequential selection | `conversionGroup`, `for (round = 0; round <= ConvertedContextRounds; ...)` | 13 (12) | 9 × 3 = 27 kernel calls (21 more than at 1 round) | Yes: one kernel instead of 27 |
| 3. Deferred-rule rounds (BeforeCaptureCopy) | `for (round = 0; round < DeferredRounds; ...)` | 2 (2) | 8 × a sub-pipeline: 112 kernels per round (Kana-Latn), 42 (fa-fa_FONIPA) | Not directly; partial |
| 4. Deferred blocking depth | `DeferredPlan`, `for (j = 0; j < DeferredBlockingDepth; ...)` | 2 (2) | 9 `MatchStar` steps per kernel × 8 rounds = 72 | Yes: small and exact |
| 5. Any-FCC passes | `RulePipelineBuilder::fcc`, `FCC_Passes()` | built-in | 3 × about 129 kernels | No |
| 6. DivCode residue | `DivCodeOptions::convertedRounds` | n/a | none (a switch only) | Already replaced |

Default `--converted-context-rounds` is 8 and `--deferred-rule-rounds` is 8. Both are inexact for longer chains, while a While loop is exact at any length.

**Measured cost of the rounds** (Latin-ConjoiningJamo, staged, 5 MB Latin text, outputs differ between the two settings):

| | 8 rounds | 1 round |
|---|--:|--:|
| Cold compile | 7.17 s | 2.72 s |
| Warm run, 1 thread | 0.293 s | 0.163 s |

Kana-Latn, 5 MB of random Katakana with iteration marks (runs of 1 to 3 marks), by `--deferred-rule-rounds`:

| | 8 rounds (default) | 4 rounds | 1 round |
|---|--:|--:|--:|
| Cold compile | 734.6 s | 294.1 s | 79.9 s |
| Warm run, 1 thread | 3.85 s | 2.11 s | 0.64 s |

The outputs at 8 and 4 rounds are identical on this text; 1 round differs. Each deferred round adds about 70 to 110 s of cold compilation and 0.45 s of run time on 5 MB. Rounds 5 to 8 change nothing on this text, so the default spends about 440 s of compilation and 1.7 s of run time on them.

## 1. Correction kernel with unrolled rounds

`FusedRules` with `rounds > 0` applies the rules whose before contexts the group's conversions can change. Round 0 matches them against the text; each further round rebuilds the converted text from all results so far (lengths, characters, inserted texts), recomputes the converted characters before each position, matches the contexts again, and drops matches starting inside matches applied in the previous round (`settle`). All 9 copies run in every block.

Why it has not been a While loop: each round recomputes from scratch and drops matches, so it is not monotone, and in a carry-collapsing While an `Advance` reads the previous block's carry only in the first iteration. doc/ldml-xlit-coverage.md §3.1 still says While loops cannot be used. DivCode §10 shows they can, if the state only accumulates (known-in / known-out streams, `K1 |= MatchStar(next(In) ∨ K1, Del)`), with a frontier of newly decided positions. The same reformulation applies here: a before context depends only on positions to its left, so a three-valued state decided left to right converges, with as many iterations as the longest dependency chain, and none in blocks without a frontier.

Expected gains: exact at any chain length (the W4 fuzzing found rule sets needing 32 rounds), one copy of the body instead of nine (compile time and kernel size, see the table), and run time spent only where chains exist (DivCode's InterIndic-Latin went from +62% to −27% when its rounds were replaced).

### 1A. String-rule overlaps, two-way or self (whole-group correction: the selection of the string matches is redone in every round)

The selection is a left-to-right fixpoint of the same kind as the converted contexts: a match applies if its context holds and it does not start within an applied match. Kept as three-valued state (known applied, known rejected), each While iteration decides the matches whose earlier overlapping candidates are decided. Hans-Hant, Latin-InterIndic, Latn-Ethi/SERA and und_FONIPA-ar exceed `--whole-group-rules` (256), so the before contexts of their string rules are matched against the unconverted text (the `--plan` warnings). That limit guards against nine copies of the whole group; with one copy of the body in a loop it could be raised, making these groups exact as well.

| Transform | Group | String rules | Subgroups | Converted contexts | Kernels | Correction kernel, Pablo lines at 8 / 1 rounds | DivCode problems |
|---|--:|--:|--:|--:|--:|--:|---|
| Hans-Hant | 1 | 958 | 258 | 15 (+10 warned) | 47 | 79,735 / 17,764 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| Latin-InterIndic | 1 | 2388 | 51 | 9 (+31 warned) | 88 | 3,076 / 962 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| Latn-Ethi/SERA | 1 | 531 | 23 | 2 (+8 warned) | 40 | 3,007 / 732 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| und_FONIPA-ar | 3 | 440 | 100 | 79 (+383 warned) | 79 | 11,190 / 2,790 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| el-el_Latn/BGN | 1 | 73 | 36 | 22 | 24 | 17,059 / 3,850 | self-overlapping string rules; string rule overlaps, two-way or self |
| es-es_FONIPA | 1 | 12 | 45 | 29 | 19 | 7,779 / 1,717 | self-overlapping string rules; string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| ia-ia_FONIPA | 1 | 29 | 24 | 2 | 23 | 7,216 / 1,581 | self-overlapping string rules; string rule overlaps, two-way or self |
| ps-ps_Latn/BGN | 1 | 58 | 10 | 2 | 50 | 3,357 / 1,313 | string rule overlaps, two-way or self |
| ru-zh | 1 | 22 | 2 | 1 | 8 | 4,152 / 953 | self-overlapping string rules; string rule overlaps, two-way or self |
| zu-zu_FONIPA | 1 | 18 | 19 | 9 | 27 | 7,903 / 1,638 | self-overlapping string rules; string rule overlaps, two-way or self |
| Zawgyi-my | 9 | 18 | 3 | 2 | 103 | not measured (the 1-round compile was stopped after 15 minutes) | rules with captures; string rule overlaps, two-way or self; string rules with runs |

### 1B. Converted-context string rules that DivCode does not apply

ja_Latn-ru #3 has a non-covering family whose match extent depends on the outcome (DivCode plan §11, fallback 3). ja_Latn-ko #2 is DivCode-eligible but follows prefix runs that delete, after which `conversionGroup` keeps the regular plan. For ja_Latn-ko the productive route is to let DivCode follow prefix runs; for ja_Latn-ru a While loop in the regular correction kernel.

| Transform | Group | String rules | Subgroups | Converted contexts | Kernels | Correction kernel, Pablo lines at 8 / 1 rounds | DivCode problems |
|---|--:|--:|--:|--:|--:|--:|---|
| ja_Latn-ru | 3 | 31 | 26 | 8 | 27 | 10,885 / 2,443 | string rules with before contexts matched against the converted text |
| ja_Latn-ko | 2 | 80 | 81 | 106 | 23 | 31,625 / 7,048 | eligible, but follows prefix runs that delete |

### 1C. Before contexts of two or more items matched against the converted text

The correction kernel already computes the j-th last converted character (`memberOf`, `previous(U[j])`). A monotone form keeps known-in/known-out per item and distance, the distance-2 streams derived from the end of the character before. DivCode §12 step 4 judged this not worthwhile inside DivCode for the four es-* groups; in the regular kernel the large groups (ConjoiningJamo-Latin, Latn-Kana) carry the cost, and ConjoiningJamo-Latin also has insertion rules, which the round body already handles.

| Transform | Group | String rules | Subgroups | Converted contexts | Kernels | Correction kernel, Pablo lines at 8 / 1 rounds | DivCode problems |
|---|--:|--:|--:|--:|--:|--:|---|
| ConjoiningJamo-Latin | 1 | 0 | 172 | 170 | 29 | 62,328 / 12,257 | before contexts of two or more items matched against the converted text; insertion rules |
| hy-hy_Latn/BGN | 1 | 36 | 4 | 40 | 22 | 6,913 / 1,467 | before contexts of two or more items matched against the converted text; context sets with strings; string rules with before contexts matched against the converted text |
| uz_Cyrl-uz_Latn | 1 | 13 | 23 | 18 | 27 | 6,910 / 1,422 | before contexts of two or more items matched against the converted text; context sets with strings |
| ky-ky_FONIPA | 1 | 16 | 33 | 17 | 35 | 11,010 / 2,610 | before contexts of two or more items matched against the converted text; contexts that are not fixed sequences of sets; self-overlapping string rules; string rule overlaps, two-way or self |
| blt-blt_FONIPA | 2 | 48 | 22 | 70 | 31 | 13,507 / 2,797 | before contexts of two or more items matched against the converted text; contexts that are not fixed sequences of sets; string rule overlaps, two-way or self; string rules keeping characters of their texts |
| Latn-Kana | 1 | 240 | 60 | 7 | 24 | 54,940 / 12,450 | before contexts of two or more items matched against the converted text; contexts that are not fixed sequences of sets; string rules with before contexts matched against the converted text |
| es-ar | 1 | 0 | 1 | 1 | 11 | 11,190 / 2,790 | (not reported) |
| es-fa | 1 | 0 | 1 | 1 | 11 | 7,779 / 1,717 | (not reported) |
| es_419-ar | 1 | 0 | 1 | 1 | 11 | 11,190 / 2,790 | (not reported) |
| es_419-fa | 1 | 0 | 1 | 1 | 11 | 7,779 / 1,717 | (not reported) |

**Status (2026-11-17): first increment implemented.** A correction kernel whose converted rules are all single-character subgroups with before contexts of fixed items (any number), and which has no insertion rules, resolves them by a While loop instead of rounds (`FusedRulesConfig::resolve`, `resolvableByWhile`; `--converted-context-while`, default on, `=0` for the rounds; kernel signature `W`). The string matches and the coverage of the subgroups do not depend on the converted text there, so they are settled as after the rounds, before the loop.

- **Outcomes.** Each position has a base outcome (the state given, the string rules and the other subgroups; slots compiled as in `matchConverted`), and each converted subgroup that may apply there (its positions without its before context) has its own, from static classes of its mappings: the characters whose replacements have length l, and whose slot i is in an item's set or not (`mappingLengthChars`, `mappingSlotChars`, added to `fusedClassSets`).
- **State**, per item (set, boundary) and distance j = 1 .. depth (`resolvedItems`), all accumulating: `YI`/`YO`, the j-th last converted character up to and including the position known in the set or known not; `K1`/`K0`, the same before the position, `MatchStar(Advance(Y) ∨ start ∨ K, KDel)`; `KDel`, the positions known deleted. A position of length l < j reads `K[j − l]`: the distance-2 streams come from the end of the character before, as planned.
- **The loop.** Each iteration evaluates the subgroups' contexts from `K1`/`K0` (`T` = ∨ alternatives ∧ items `K1`, `F` = ∧ alternatives ∨ items `K0`) and adds to the state the outcome of a subgroup known to apply, or of the base where all are known not to, or of any position whose possible outcomes all agree (consumers need no iteration). It ends when no `K1`/`K0` bit is added. Before the loop, the state of the positions where no converted subgroup may apply.

Results:

- Exact at any length: `QA/xlit_converted_while_test.py` (chains of two and three items longer than 8, a deletion and an expansion seen at distance 2, negated sets at the start of the text; with one thread and with four threads and segments of one stride) matches ICU, where the rounds fail 3 of its lines; also on random texts with chains of up to 3000 across blocks, and with dependent positions at block starts after blocks with none.
- Groups of this table now resolved: uz_Cyrl-uz_Latn #1 and the es-ar, es-fa, es_419-ar, es_419-fa group. Still on rounds: hy-hy_Latn/BGN #1, blt-blt_FONIPA #2 and Latn-Kana (a converted string rule), ky-ky_FONIPA (a repeated set, `S T*`), ConjoiningJamo-Latin (54 insertion rules).
- The mechanism also applies to correction kernels of the other classes with only such subgroups: by default 45 transforms have one (Latin-InterIndic and the Latn-* Indic transforms through it, und_FONIPA-ar and the *-ar transforms, my-my_FONIPA, Hans-Hant, Latn-Ethi/SERA, NumericPinyin-Latin, ps-ps_Latn/BGN, Zawgyi-my, the es-* groups, uz_Cyrl-uz_Latn); 122 with `--divcode-groups=0`. On the CLDR test data where there is some, and on random text of each script, the output is identical to the rounds' for all of them (Zawgyi-my not yet checked: its compilation takes too long), and identical to ICU's except Latin-InterIndic, Latn-Ethi/SERA and Bopo-Latn, which differ from ICU with the rounds as well. Latn-Ethi/SERA and Bopo-Latn are ICU's (doc/ICU-uconv-CLDR-issues.md: the global filter ignored for a single rule set, here the inverse filter; `::` transforms misplaced in the backward direction). Latin-InterIndic is xlit's: its 2,388 string rules exceed `--whole-group-rules`, so the before contexts of its string rules are matched against the text itself (the `--plan` warnings, class 1A), and a vowel after a consonant that a string rule writes (`x → क्स`) or after a deleted inherent `a` is written as an independent vowel (`xi`: `क्सइ`, ICU `क्सि`; `hao`: `हऒ`, ICU `हॊ`), on 41 of 988 random words.
- Cost, uz_Cyrl-uz_Latn: the correction kernel has 2,250 Pablo lines against 6,916 at 8 rounds; execution cycles (4 threads, 5 MB of random Cyrillic) 205,853 against 381,735 (−46%); hy-hy_Latn/BGN's two resolved kernels −55% and −59%. Whole transforms: cold compilation and warm run time unchanged within noise (uz_Cyrl-uz_Latn, es-ar, hy-hy_Latn/BGN), as their class kernels dominate.

**Second increment (2026-11-17): a repeated set in a before context.** An alternative `X T* I₂ … I_{B−1}` (in text order; at most one repetition, with at most one item beyond it, see `ContextExpansion`) is resolved as a run item (`ResolvedItem::run`) at the repetition's distance d: going back from the d-th last converted character over the characters in T, whether the first other one is in X (or, with none, whether X holds before the text). A replacement of length l ≥ j settles it from its characters `l − j … 0`: the first not in T is in X (known in) or not (known out), or all are in T and it is as before the position, `K[1]` (`replacementOutcome`, static classes `mappingItemChars`; for the base outcome the same from its slots, as the rounds' `runIn`). At distance 1, `K` is carried by `MatchStar` through the positions known to be passed over (`Thru`, accumulated as `KDel`), so runs of T take no iterations. T's membership comes from the outcomes, so conversions into and out of T (U+0345 in the Title groups, class 1D) are exact.

- `QA/xlit_converted_while_test.py`: chains through the run, conversions into and out of T and a deletion within it, a negated X at the start of the text, an item between the run and the rule, an expansion ending in T; ICU's results, which the rounds fail on 3 lines.
- Newly resolved by default: ky-ky_FONIPA (and ky-am, ky-ar, ky-chr, ky-fa through it), Grek-Latn, Latn-Grek/UNGEGN, Spacedhan-Han, and the Title groups of class 1D: az-Title, tr-Title, el-Title, lt-Title. All identical to the rounds on random text of their scripts (and ky's CLDR test data), and to ICU except Spacedhan-Han, which differs with the rounds as well: an ICU issue (`::` transforms misplaced in the backward direction, doc/ICU-uconv-CLDR-issues.md).
- Correction kernels: ky-ky_FONIPA 6,167 Pablo lines against 11,016 at 8 rounds; az-Title 1,346 against 18,172.

**Third increment (2026-11-17): insertion rules.** An insertion rule matches its before context against the converted text up to and including its position's character, without the text it inserts there (`FusedInsertion`), and its text follows that character's outcome. The state gains `C`, the same as `Y` for the character's outcome only; the insertion rules are decided on `C` (`T` with the after context and eligibility, `F` without them or with the context known out; a rule applies where it holds and every earlier one is known not to), and `Y` is the inserted text's value (its last characters, then `C` beyond its length; a run item passing over the whole text continues into `C`) where one applies, or `C` where every possible one agrees. A position is deleted (or passed over by a run item) when its character is and no text (or only a text all in the item's set) is inserted. The positions where an insertion rule may apply are dependent.

The loop's body is factored: the subgroups with the same mappings (differing in their contexts) have the same outcomes, so their decisions are combined first (where one of them applies; where each is known not to or is not a candidate), and the outcomes are evaluated once per class of mappings, not per subgroup; likewise the insertion rules by their texts. ConjoiningJamo-Latin's 170 converted subgroups have 17 distinct mappings: its kernel went from 174,798 Pablo lines unfactored to 25,208 (uz_Cyrl-uz_Latn 1,798, ky-ky_FONIPA 4,540).

- `QA/xlit_converted_while_test.py`: chains alternating insertions and conversions, with deletions within them, a repeated set in an insertion's context, two insertion rules in order; ICU's results, which the rounds fail on 4 lines.
- Newly resolved by default: ConjoiningJamo-Latin (and Hang-Latn, Jamo-Latn through it), the Han-Latin groups (Han-Latin/Names, Hani-Latn, Hant-Latn, Jpan-Latn) and Han-Spacedhan. Identical to the rounds and to ICU on random text of their scripts, except Han-Spacedhan, which differs from ICU with the rounds as well (an ICU issue: the filter of its first `::` transform also applies to the next one, doc/ICU-uconv-CLDR-issues.md).
- ConjoiningJamo-Latin on 5 MB of jamo (NFD of random Hangul syllables), 1 thread: cold compilation 3.38 s against 12.87 s at 8 rounds, warm run 0.19 s against 0.35 s (−46%); its correction kernel 25,208 Pablo lines against 62,329.

Next increment for this class: converted string rules (hy-hy_Latn/BGN #1, blt-blt_FONIPA #2, Latn-Kana; with class 1A). At `d7854835d` these are the only reason any correction kernel stays on rounds (see the status section at the top).

**`-CheckWhileProgress` (2026-11-19, `--u21-nfc=0`, see section 5):** it fails on every correction kernel tried (uz_Cyrl-uz_Latn: "lowest bit at 7, then at 0"; ConjoiningJamo-Latin and el-Title: "at 0, then at 0"). This is expected from the loop's form, not a sign of a wrong result: `Progress` starts as `dep` (all dependent positions), and the first iteration adds the `K` bits that the carries from the previous block give at the block's start, so the first comparison is between unrelated streams. The check is useful here only if it skips the first iteration or `Progress` starts as the first iteration's new bits.

### 1D. A repeated set in a before context whose membership the conversions change (S T*)

U+0345 is case-ignorable and cased: whether it is in T in the converted text depends on the converted context itself. In a While loop T becomes an outcome item: the run through T is a `MatchStar` over known-in-T and deleted positions, stopping at positions not yet decided. The Title groups have 1,500 to 3,000 rules each, so nine copies are costly to compile.

| Transform | Group | String rules | Subgroups | Converted contexts | Kernels | Correction kernel, Pablo lines at 8 / 1 rounds | DivCode problems |
|---|--:|--:|--:|--:|--:|--:|---|
| az-Title | 1 | 0 | 2 | 2 | 19 | 18,166 / 3,375 | conversions changing the repeated set of a before context |
| tr-Title | 1 | 0 | 2 | 2 | 19 | 18,166 / 3,375 | conversions changing the repeated set of a before context |
| el-Title | 1 | 0 | 4 | 4 | 37 | 17,157 / 3,227 | conversions changing the repeated set of a before context |
| lt-Title | 1 | 0 | 16 | 14 (+2 warned) | 91 | 18,663 / 3,501 | contexts that are not fixed sequences of sets; conversions changing the repeated set of a before context |
| Grek-Latn | 2 | 4 | 25 | 12 | 155 | 9,300 / 1,803 | contexts that are not fixed sequences of sets; conversions changing the repeated set of a before context; front-letter revisits |
| Latn-Cyrl | 1 | 70 | 24 | 6 | 129 | 12,229 / 2,919 | contexts that are not fixed sequences of sets; conversions changing the repeated set of a before context |

**Status (2026-11-18).** The Title groups and Grek-Latn #2 are resolved by the While loop (the repeated set of 1C's second increment; T's membership comes from the outcomes, so U+0345 is exact); Latn-Cyrl #1 stays on rounds (a converted string rule). Az-, tr-, el- and lt-Title and Grek-Latn match ICU on random text of their scripts with case-ignorable characters, U+0345 and marks; Latn-Cyrl and Any-Title differ from ICU, with the rounds as well (not investigated).

**Unicode properties as kernels of their own** (`--ucd-property-kernels`, default on, Unicode mode). These groups' contexts and texts are Unicode properties (`[:Cased:]`, `[:Case_Ignorable:]`, `[:ccc=Above:]`, `[:Soft_Dotted:]`, `[:Mark:]`, `[:Letter:]`, `[:Uppercase_Letter:]`, ...). The fused kernels' classes were all compiled from their characters in one `CharClassesKernel` per fused kernel. Now the properties of a group's rules with more than 32 ranges (at most 8; `collectProperties`, resolved as `ldml::CharSetAnalysis::propertySet` does) are each computed by a `UnicodePropertyKernelBuilder` kernel (`lib/kernel/unicode/UCD_property_kernel.cpp`, named by the property: shared, and cached, across transforms), once per pipeline. A class set is written over the properties' cells (the characters in exactly the properties of a mask): the union of the cells it mostly covers, without the characters that adds (`out`), with those it misses (`in`), when these have less than a quarter of the set's ranges (`propertyFormula`); such classes are computed from the property streams by a `PropertyClasses` kernel, the others by `CharClassesKernel` as before, merged by `MergeStreamSets`.

- Classes moved to the property streams: az-Title 3 sets of 1,195 ranges (of 6,043), lt-Title 4 of 1,403, Grek-Latn 9 of 5,643 (of 5,726). The rest are mostly the case mappings' changes (`(.) → &Any-Lower($1)`, by bit), not properties.
- Output identical with and without (the 1D transforms, Latn-Cyrl, Any-Title); all xlit tests pass.
- Cold compilation, each in an empty cache: az-Title 1.79 s against 3.39 s; el-Title 6.09 against 6.68; Grek-Latn 10.55 against 10.96; lt-Title 7.76 against 7.65 (their time is elsewhere). With the property kernels cached by another transform: el-Title after az-Title 5.94 s against 6.52, lt-Title after both 6.09 against 6.52.

### 1E. Other forms: captures, runs, contexts that are not sequences of sets

Small correction kernels (one to six converted contexts). A While loop would make them exact but gains little in size; lowest priority.

| Transform | Group | String rules | Subgroups | Converted contexts | Kernels | Correction kernel, Pablo lines at 8 / 1 rounds | DivCode problems |
|---|--:|--:|--:|--:|--:|--:|---|
| Spacedhan-Han | 1 | 0 | 4 | 4 | 8 | 815 / 206 | contexts that are not fixed sequences of sets |
| my-my_FONIPA | 4 | 1 | 1 | 1 | 38 | 1,235 / 395 | contexts that are not fixed sequences of sets |
| Latin-NumericPinyin | 1 | 5 | 2 | 6 | 114 | 2,489 / 697 | rules with captures |
| ThaiLogical-Thai | 1 | 1 | 1 | 2 | 20 | 652 / 169 | rules with captures |
| Zawgyi-my | 3 | 1 | 4 | 1 (+1 warned) | 301 | not measured (the 1-round compile was stopped after 15 minutes) | rules with captures; string rules with runs |

Where a transform has several regular-plan groups, the Pablo figure is its largest correction kernel.

**Status (2026-11-19).** Spacedhan-Han, my-my_FONIPA #4 and Zawgyi-my #3 (and #9 of class 1A) are on the While loop; Zawgyi-my's output against the rounds' and its Pablo sizes are still unmeasured (its compilation is long). Latin-NumericPinyin #1 has no correction kernel now (its before contexts, `e {`, are matched against the text). ThaiLogical-Thai stays on rounds: its one string rule's before context is matched against the converted text.

## 2. Staged sequential selection between rounds

Where string matches must be selected sequentially (cyclic or two-way overlaps, `TransformStep::sequentialSelection`) and the whole group is corrected, each round is a pipeline stage: a `FusedRules` kernel matching the before contexts against the previous round's converted text (`matchOnly`), a `LeftmostMatches` scalar scan, and a `FusedRules` kernel applying the rules where selected. That is 3 kernel calls per round, 27 at the defaults. The per-group kernel counts confirm it: each of these groups has exactly 21 more kernels at 8 rounds than at 1.

A single kernel with a While loop could fold all three: decided matches and contexts grow left to right, so the selection and the converted contexts resolve together. `LeftmostMatches` is exact and cheap on its own; the duplication, not the scan, is the cost.

| Transform | Group | String rules | Subgroups | Converted contexts | Kernels at 8 / 1 / 0 rounds | DivCode problems |
|---|--:|--:|--:|--:|--:|---|
| Any-Publishing | 1 | 47 | 4 | 18 | 34 / 13 / 27 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| InterIndic-Arabic | 2 | 1 | 0 | 1 | 35 / 14 / 12 | string rules with runs |
| Latin-ASCII | 1 | 1 | 0 | 1 | 38 / 17 / 16 | string rules with runs |
| Latin-ConjoiningJamo | 1 | 115 | 66 | 179 | 49 / 28 / 40 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| es_FONIPA-zh | 1 | 19 | 4 | 6 | 34 / 13 / 19 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| fa-fa_FONIPA | 3 | 79 | 132 | 168 | 186 / 165 / 326 | an alphabet of 30062 blocks; before contexts of two or more items matched against the converted text; context sets with strings; contexts that are not fixed sequences of sets; conversions changing the repeated set of a before context; string rule overlaps, cyclic; string rules with before contexts matched against the converted text; div64: an alphabet of 15098 blocks |
| ro-ro_FONIPA | 1 | 52 | 12 | 2 | 50 / 29 / 43 | string rule overlaps, two-way or self; string rules with runs |
| si-si_FONIPA | 1 | 1 | 1 | 1 | 34 / 13 / 15 | contexts that are not fixed sequences of sets |
| si-si_FONIPA | 9 | 5 | 0 | 2 | 35 / 14 / 17 | context sets with strings; string rule overlaps, two-way or self; string rules with runs |
| ta-ta_FONIPA | 3 | 4 | 13 | 9 | 49 / 28 / 34 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| und_FONIPA-fa | 3 | 142 | 41 | 102 | 52 / 31 / 45 | string rule overlaps, two-way or self; string rules with before contexts matched against the converted text |
| xh-xh_FONIPA | 1 | 27 | 24 | 2 | 50 / 29 / 35 | string rules with runs |
| zh_Latn_PINYIN-ru | 3 | 68 | 17 | 5 | 49 / 28 / 38 | string rule overlaps, two-way or self |

fa-fa_FONIPA #3 also has deferred rules (section 3).

## 3. Deferred-rule rounds

Rules copying a capture of their before context (`P (C) { T } A → R $1`) and the rules sharing their triggers are applied after the rest of the group, to the converted text, in `--deferred-rule-rounds` rounds. Each round is a sub-pipeline: `FilterByMask` compaction of the text, class kernels, one regular-expression-engine `matchPositions` per deferred rule on the converted text, `RunIndex`, `DeferredPlan`, `DeferredRegion`, `FilterByMask`, `DeferredFill`, `SpreadByMask`, and for self-reproducing rules `DeferredCandidates` and `DeferredRuns`.

| Transform | Direct use | Group kernels at 1 / 2 / 4 / 8 rounds | Kernels per round |
|---|---|---|--:|
| Kana-Latn | group 1 (210 rules) | 161 / 273 / 497 / 945 (transform 359 → 1,143) | 112 |
| fa-fa_FONIPA | group 3 (233 rules, also staged) | 260 at 1, 554 at 8 | 42 |
| Hira-Latn | through Katakana-Latin | transform 1,715 | |
| Jpan-Latn | through Kana-Latin and Hira-Latin (twice) | transform 3,203 | |

Comment: this is the largest duplication in xlit by kernel count (Jpan-Latn's cold compile was measured at 228 s in doc/ldml-xlit-coverage.md §3.2), but it is not a direct While candidate. The before contexts are matched by the regular expression engine on compacted text, and each round copies variable-length regions of converted text into reserved positions, a data movement across varying distances. Replacing it needs the contexts as fused sets on the spread text, seen through non-live positions as DivCode sees through deletions, and the copy as shifts by a computed distance. `DeferredRuns` already handles runs of self-reproducing rules at any length in one round, so most rounds do nothing on typical text. Lower-cost steps: section 4, and measuring whether fewer rounds lose anything on real Japanese text.

## 4. Blocking depth in DeferredPlan

Inside every `DeferredPlan` kernel, the triggers whose before contexts may reach an unreplaced trigger are found by an unrolled closure:

```
blocked = blockedBy(triggers ∧ matched)
repeat DeferredBlockingDepth (8) times:
    blocked = blockedBy(triggers ∧ (matched ∨ blocked))
```

That is 9 `MatchStar` steps per kernel, in each of the 8 rounds: 72 copies per deferred group, 144 in Jpan-Latn. The closure is monotone and moves rightward, exactly the form DivCode §10 shows a While loop computes exactly (accumulate `blocked`, frontier = triggers newly blocked). A chain of more than 9 triggers is currently under-blocked, so a trigger can be treated as ready while a trigger before it is unreplaced. Possibly a single `MatchStar` through the reach and the trigger texts suffices; that needs checking against the tails' classes. Small change, exact, cheap: recommended first.

**Status: committed in 5a372fd94 (2026-11-17).** `DeferredPlan` now computes the closure with a While loop (`tools/ldml/xlit.cpp`, kernel renamed `DeferredPlanW_…` so that cached kernels are not reused; `DeferredBlockingDepth` removed):

```
Blocked  = blockedBy(triggers ∧ matched)
Sources  = triggers ∧ matched
Frontier = triggers ∧ Blocked ∧ ¬matched
while (Frontier) {
    Sources  = Sources ∨ Frontier
    Blocked  = Blocked ∨ blockedBy(Sources)
    Frontier = triggers ∧ Blocked ∧ ¬Sources
}
ready = triggers ∧ ¬Blocked
```

Every Var only gains bits and each iteration starts again from all the blocking triggers, the form DivCode §10 shows exact although the loop's carries from the previous block reach its first iteration only. The initial values are built on the streams, not on the Vars.

Checks (all closed on 2026-11-19):

| Check | Result |
|---|---|
| `xlit_run_moves_test` (deferred rules deleting runs of triggers) | 0 failures |
| Kana-Latn, 5 MB Katakana, 1 deferred round | identical to the previous build |
| Kana-Latn, 8 deferred rounds (2026-11-19) | identical to the unrolled build (5a372fd94 reverted on the current source), on 650 KB of random Katakana with 23,953 iteration marks, and identical to ICU's Katakana-Latin. Rounds 2–8 change this text (the output at 1 round is 9 KB longer). Cold compile, `-compile-threads=12`: 597 s While, 609 s unrolled; 1.1 GB peak each |
| A chain of more than 9 triggers that the unrolled depth treated as ready too early (2026-11-19) | `QA/xlit_deferred_chains_test.py`: `([bc]) { s → d $1 ; s { s } y → z ;` on `b s…s y` (the copy rule writes `d` first, so it is not self-reproducing), and a second set whose triggers are apart, through the capture's `a*`. With `--deferred-rule-rounds=16`, the While build matches ICU in all 20 cases. The unrolled build (5a372fd94 reverted on the current source) applies `z` at the last trigger of every chain of 11 or more, 8 cases. 9.6 s with a cold cache |
| `-CheckWhileProgress` (2026-11-19) | passes: Kana-Latn, 650 KB of random Katakana with iteration marks, 1 deferred round, `--u21-nfc=0`; output identical to the run without the check |

## 5. Any-FCC passes (not recommended)

`fcc()` runs NFC on a barrier-masked text `FCC_Passes()` times (3: the most nonstarters in a primary composite, U+1F82). Each pass is a multi-kernel NFC pipeline (Any-FCC has 422 kernels against 114 for Any-NFC). The count is exact by Unicode data, and an NFC pipeline cannot run inside a Pablo While, so there is nothing to gain from a loop.

**Status (2026-11-19).** Each pass is now a masked NFC on the 21-bit basis (`NFC_U21`, `--u21-nfc`, a0fbb5024): Any-FCC has 318 kernels and Any-NFC 75. Still not a candidate.

**A crash found in this check, fixed.** With `--u21-nfc` (the default), the first compile of Any-FCC or Latin-Cyrillic into an empty object cache aborted with "double free or corruption" while freeing the kernel's module (`CPUDriverCompiler::materializeObject` → `llvm::Module::~Module`). It happened every time, also with `-compile-threads=1`, and so also with `-enable-object-cache=0` and `-CheckWhileProgress`. The cause was `Hangul_Composition` on a basis wider than 8 bits (`lib/kernel/unicode/normalization/normalization.cpp`). `Merged_LV` is as wide as the basis, so `Composed = AddFull(Merged_LV, TIndex)` is one bit wider. `ZeroExtend(Composed, Basis.size())` then wrote one element past the end of its vector (its assertion is compiled out in release builds), which corrupted the heap. The bug is as old as the kernel, but nothing used the kernel with a 21-bit basis before `NFC_U21`. Now the carry bit, which is always 0 because a syllable is at most U+D7A3, is truncated. Cold compiles, uncached runs and `-CheckWhileProgress` work at the defaults, and Any-NFC, Any-NFKC and Any-FCC compose random Hangul (NFD and mixed) as Python's NFC does.

## 6. Already replaced, and stale text

(Rechecked at `d7854835d`: all three items below were still as described. The two stale items were edited on 2026-11-20, step 7 and step 9: the comments of `include/ldml/divcode_plan.h` and `lib/ldml/divcode_plan.cpp` now describe the While loop, and doc/ldml-xlit-coverage.md §3.1 describes the correction kernel's While loop and §3.2 gives the deferred-rule default as 8.)

- DivCode applies 55 groups (by kernel counts with `--divcode-groups=0`), including all 42 eligible converted-context groups except ja_Latn-ko #2 and ro_FONIPA-ja #2.
- `include/ldml/divcode_plan.h`: `convertedRounds = 8` is now only an on/off switch for converted contexts, and its comments (lines 63–66, 155–156) and `lib/ldml/divcode_plan.cpp:328` still describe rounds.
- doc/ldml-xlit-coverage.md §3.1 says Pablo While loops cannot be used for this fixpoint, which DivCode §10 disproves. §3.2 gives the deferred-rule default as 4; the option's default is 8.

## Recommended order

Done: section 4 (5a372fd94); section 1 for subgroups and insertion rules with fixed items or a repeated set (21 of the survey's 34 groups, plus the Han-Latin group, Bopo-Latn and NumericPinyin-Latin).

Next steps (2026-11-19):

1. **Fixed (2026-11-19):** the cold-compile crash under `--u21-nfc`, an out-of-bounds write in `Hangul_Composition` (section 5).
2. **Done (2026-11-19):** the section 4 checks. Kana-Latn at 8 deferred rounds is identical to the unrolled build and to ICU, and `QA/xlit_deferred_chains_test.py` covers chains of more than 9 triggers that are not self-reproducing; the unrolled build fails it.
3. **Converted string rules in the correction kernel's While loop.** They are now the only reason a correction kernel uses rounds: 12 groups, 34 transforms. Work in two parts:
   - (a) **Done (2026-11-19):** converted string rules that no selection among the string matches depends on (`convertedStringResolvable`, `--converted-string-while`, default on). Such a rule precedes no other string rule in the overlap order and does not overlap itself, has no run and keeps no character, and its kernel has no insertion rules. Its candidates (the first positions of its matches, after the selection by the rules that precede it) are fixed before the loop. The loop decides each candidate from its before context as it decides a subgroup, at the candidate's first position. At offset o of a candidate's text the position's outcome is a fixed string: replacement character o, none beyond the replacement (deleted), and the rest of the replacement at the last offset. Where a match is known to apply, its outcome holds; where every match that may cover a position is known not to apply, or has the same value, the position's own outcome holds. The decisions shifted to the offsets are ORed into Vars (`ST`, `SF`), as the carries of the previous block reach the first iteration only. After the loop, the subgroups within an applied match (not at its first position) are dropped, as `settle` does.
     - Now on the While loop: ThaiLogical-Thai, ru-zh, ia-ia_FONIPA, Latn-Kana, Latn-Cyrl, el-el_Latn/BGN (18 converted string rules) and hy-hy_Latn/BGN (36). 28 of the survey's 34 groups use the loop.
     - Checks: `QA/xlit_converted_strings_while_test.py` (11 rule sets, chains to 1,500 characters across blocks, also with one thread and with four threads and one-stride segments) passes, and fails 12 cases with `--converted-string-while=0`. On 3,000 random words over the characters of each transform's rules, the While output equals the rounds' for all seven, and ICU's for ru-zh, ia-ia_FONIPA, Latn-Kana, el-el_Latn/BGN and hy-hy_Latn/BGN. Latn-Cyrl differs from ICU on 6 words with both the loop and the rounds. ThaiLogical-Thai (not in ICU): identical to the rounds on 5,000 lines dense in `; ` between Thai letters. All 39 xlit tests pass.
   - (b) **Done (2026-11-19):** selection depending on a converted rule. The five groups left after (a) each had a converted string rule that precedes another in the overlap order (ja_Latn-ru: `﷠ { [̂̄] \~e → е` before `e $lengthMarker → эй`; es-es_FONIPA: `$bow { xs → s` before `ss → s`, which overlaps itself; zu-zu_FONIPA: `n { sh → t͡sʼ` and `[^n] { sh → ʃ` before `hh → ɦ`, self, and `hl → ɬ`; blt-blt_FONIPA: 11 pairs, one self, and rules keeping and deleting characters of their texts; ja_Latn-ko: 106 pairs).
     - The rules decided in the loop (`loopStringRules`): the converted string rules, and transitively every string rule that a rule decided in the loop precedes, or whose first character may be at a later position of such a rule's text (`mayBeginWithin`). The second condition was found by a test: `[bq] { bb` beginning at the last position of an applied `q { zab`, which the overlap order does not have (it lists only matches within a text), and which the rounds drop through `appliedWithin`.
     - In the loop, each such rule's candidate is known to apply where its before context, if converted, is known to hold and every match decided in the loop that may cover its first position is known not to apply; known not to apply where its context is known not to hold or such a match is known to apply. Rules are decided in their selection order in each iteration; a covering match of a later rule, or of the rule itself (a rule overlapping itself: the alternation of `settle`), is taken from the previous iteration. The decisions are accumulated in Vars (`DT`, `DF`), and new decisions count as progress, as a run of a self-overlapping rule may be decided without any item changing. The rules decided in the loop must have fixed results at each position of their texts: no run; a kept character (`StringRuleKept`) is the position's own, so its item values are classes of the position, and a deleted one (`StringRuleDeleted`) has an empty result.
     - All 12 groups now use the loop. A re-survey of all 453 transform names (`--kernel-counts`, the temporary trace) finds no correction kernel on rounds; 91 transforms have a While correction kernel.
     - Checks: `QA/xlit_converted_strings_while_test.py` gained 10 rule sets for (b) and passes (21 failures with `--converted-string-while=0`). On 3,000 random words each, all 12 transforms equal the rounds, and ICU where ICU has them (except Latn-Cyrl's 6 older differences). 1,207 random rule sets of two to four rules over `abcx` with converted contexts (`fuzz.py`, not committed): no output where the loop is wrong and the rounds right. All 39 xlit tests pass.
     - Found by the random rule sets, older than this work (the rounds give the same output), now known to be two bugs:
       - Fixed (2026-11-19): a string rule with a converted before context beginning at a later position of another string rule's applied match (`x { bb → c ; aab → ;` on `xaabb`), which ICU never applies. The overlap analysis leaves the pair out because the context cannot match there in the source text; in the converted text it can. Both applied, and their changes combined into U+D801. The regular plan's correction kernel now applies such rules only outside the applied matches of the others (`correctedWithin`), and DivCode leaves such a group to the regular plan (no group of the 453 transforms changes). `QA/xlit_converted_within_test.py`.
       - Fixed (2026-11-19, d2bd0845b, issue #44): within a filter (or `--lines` regions), `ccb → a ; cc → ;` lost the final partial stride of the output. A pipeline termination bug: `SelectStringMatches` shared the partition of one of its producers, whose root terminated a segment before the producer in the other partition, and a partition whose root has terminated is skipped. A partition root's final invocation now waits for the inputs that the other kernels of its partition take from other partitions (`partitionExternalInputsClosed`). `QA/xlit_partition_final_stride_test.py`.
     - Cold compile of the whole transform, `-compile-threads=12`, no object cache (While / rounds, s): ThaiLogical-Thai 0.9 / 0.9, ru-zh 45.0 / 47.5, ia-ia_FONIPA 21.4 / 22.9, ja_Latn-ru 22.6 / 22.8, Latn-Kana 64.1 / 67.8, Latn-Cyrl 57.9 / 58.7, zu-zu_FONIPA 4.2 / 5.0, es-es_FONIPA 22.1 / 22.0, el-el_Latn/BGN 8.2 / 10.4, hy-hy_Latn/BGN 29.9 / 29.7, blt-blt_FONIPA 21.7 / 26.7, ja_Latn-ko 18.8 / 21.0. The correction kernel is one of many in each transform, so the gain is small except where it dominates (blt, el, zu: 16–21%); no transform is slower beyond noise.
4. **Raise `--whole-group-rules`, now that 3(b) works.** Hans-Hant, Latin-InterIndic, Latn-Ethi/SERA and und_FONIPA-ar have their subgroups on the While loop already, but their string rules' before contexts are still matched against the unconverted text. For Latin-InterIndic that is the known mismatch with ICU, 41 of 988 random words. With one copy of the loop body, the guard against nine copies is no longer needed.
   - **Not done (2026-11-19): it depends on step 6.** All four groups have two-way overlaps among their string rules, so their whole-group selection is sequential (`step.sequentialSelection`), which 3(b) does not cover. With the limit raised (`--whole-group-rules=100000`), `--plan` shows each of them "selected sequentially between the 9 rounds": the staged path of section 2, each round a matching kernel with the whole group, which is what the guard prevents. Measured: Latn-Ethi/SERA's cold compile goes from 2 s to 9 s, with the same output (and ICU's) on 988 random words; Latin-InterIndic's takes 616 s at the raised limit (`-compile-threads=12`), and its output still differs from ICU on 19 of 988 random words (18 at the default limit). The limit stays until the staged selection is in the While loop (step 6), which would make the four groups both exact and compact.
5. **Class 1B:** let DivCode follow prefix runs that delete. This brings in ja_Latn-ko #2 and ro_FONIPA-ja #2, and removes the largest group from 3(b).
   - **Done (2026-11-19).** `conversionGroup` kept the regular plan for every group after prefix runs that delete (`mDeletedRunEnds`, `mDeletedRunAfter`). Those streams matter only to the rules with blocking marks (`ldml::DeletedRunBlockMark`, in their texts or after contexts), which DivCode does not match; a group without them now goes to DivCode if eligible (`hasBlockingMarks`).
   - ro_FONIPA-ja #2 (ja-t-ro-fonipa) is now applied by DivCode, 47 kernels to 42, and sk_FONIPA-ja #2 (ja-t-sk-fonipa) too, 39 to 36. ja_Latn-ko #2 is not: since dc76ec6a5 DivCode leaves it to the regular plan for another reason ("a string rule with a before context matched against the converted text may begin within the text of a string rule", `correctedWithin`), and it is on the While loop since 3(b), so it stays there.
   - Checks: the nine CLDR transforms with test data and prefix runs give the previous build's output and their expected results; ro_FONIPA-ja on 3,000 random words (Romanian through ro-ro_FONIPA) equals the regular plan and ICU, also with four threads and one-stride segments. `QA/xlit_divcode_test.py` has the case `after-prefix-runs` (by default), and all 41 xlit tests pass.
6. **Section 2, staged selection** (13 groups, 59 transforms, 21 more kernels each at 8 rounds than at 1): fold the matching, `LeftmostMatches` and applying kernels into one While kernel. This reuses the selection state of 3(b).
   - **Done (2026-11-19) for the groups without runs.** A whole group selected sequentially (`TransformStep::sequentialSelection`) goes to the correction kernel's While loop where every string rule can be decided there (`FusedRulesConfig::sequential`, `--sequential-while`, default on). All its string rules are decided in the loop (`loopStringRules`), with no overlap order: a candidate applies where its before context (if converted) holds and no match that may cover its first position applies (`mayBeginWithin`, by the sets of the texts). This is the leftmost-first selection of `LeftmostMatches`, decided over as many iterations as the chains need; matches of different rules at the same position are settled before, by their yields.
   - Eight groups move to the loop, 26 kernels fewer each: Any-Publishing (37 to 11), Latin-ConjoiningJamo (49 to 23), es_FONIPA-zh, fa-fa_FONIPA, si-si_FONIPA (one of its two groups), ta-ta_FONIPA, und_FONIPA-fa and zh_Latn_PINYIN-ru. InterIndic-Arabic, Latin-ASCII, ro-ro_FONIPA and xh-xh_FONIPA keep the staged rounds: their string rules have runs, which the loop does not decide (`loopStringResolvable`).
   - Checks: 3,000 random words over each of the eight transforms' scripts: the loop's output equals ICU's, also with four threads and one-stride segments; the staged rounds differ from ICU on one Latin-ConjoiningJamo word (`dbpktbkmgq`, a chain longer than the rounds). `QA/xlit_sequential_while_test.py` (a two-way and a cyclic rule set, chains to 1,400 characters) passes, and fails 14 cases with `--sequential-while=0`. All 42 xlit tests pass. On the CLDR test data (the source column of each of the 290 files, `--lines`), the staged rounds (`--sequential-while=0`) and the loop give identical output for every file. Six files exit with status 1 in both, as xlit does not know their transforms (`d0-morse-t-am-Ethi`, `byn-Latn-t-byn-ethi-m0-tekie-alibekit` and four `und-Latn-t-und-ethi-m0-*` variants). The test data has no chains longer than the rounds, so this checks that the move loses nothing, not the loop's exactness, which the random words and the test above check. Cold compile times are still to be measured.
7. **After 3 to 6:** `--converted-context-rounds` is left only as a switch, like DivCode's `convertedRounds`. Remove the round bodies of `FusedRules` (round 0 is still needed) and the staged loop, then update the stale text of section 6.
   - **Done (2026-11-20), except the staged loop.** A trace of the `FusedRules` kernels built for all 453 transform names (`--kernel-counts`) and all 42 xlit tests found no correction kernel on rounds; 28 transforms still use the staged loop, for the groups with runs that step 6 left there (InterIndic-Arabic, Latin-ASCII, ro-ro_FONIPA, xh-xh_FONIPA), so it stays, and `--converted-context-rounds` still sets its rounds as well as switching the correction on and off.
   - Removed: the round body of `FusedRules` (round 0, then rounds 1 to N, used when the While loop could not resolve a group), and `--converted-context-while` and `--converted-string-while`, whose only use was to force it. A correction kernel that the loop cannot resolve is now an error, naming the reason (`resolvableByWhile`), with `--converted-context-rounds=0` to match the before contexts against the text itself instead. No rule set tried reaches it: the forms the loop rejects are either refused by the planner or go to the staged loop. The While loop's own `settle` passes and the staged rounds still read `FusedRulesConfig::rounds`.
   - DivCode's `convertedRounds` is now `bool convertedContexts`, as it was only tested for nonzero.
   - Checks: all 42 xlit tests pass; seven CLDR test files with converted contexts (el, hy, ja-hrkt BGN, ko-t-ja-Latn, ru-t-ja-Latn, und-Latn-t-und-beng and -deva, the last two by DivCode) give the previous output and their expected results; with `--converted-context-rounds=0`, hy-Latn-t-hy-m0-bgn differs from its expected results on 6 lines, as before.
8. **Section 3, deferred rounds:** still only after measuring against real Japanese and Persian text. As a cheap interim step, measure whether `--deferred-rule-rounds=4` loses anything on real text. On random Katakana it is identical to 8, and it saves about 440 s of Kana-Latn's cold compile.
   - **Measured (2026-11-19).** Real text from the Wikibooks dumps, 1 MB each, in one run per transform, at `--deferred-rule-rounds` 4, 8 (the default) and 16:
     - Kana-Latn on the 5,349 lines of jawikibooks with Katakana: the three outputs are identical, and equal ICU's.
     - Hira-Latn on the 6,524 lines with Hiragana: the three outputs are identical; they differ from ICU's on 273 lines, equally at every number of rounds (see below).
     - fa-fa_FONIPA on 7,468 lines of fawikibooks with Persian letters: the three outputs are identical, and equal ICU's.
     - So 4 rounds lose nothing on this text, as on random Katakana. Lowering the default to 4 would save the compile time of half the rounds (about 440 s of Kana-Latn's cold compile, measured earlier), at the cost of exactness for chains of 5 to 8 triggers, which these texts do not have. Not changed: a decision on the default.
     - Found on the way, older than this work (the build before step 7 gives the same output): Hira-Latn leaves a voiced Katakana before a small vowel unconverted where ICU converts it: `ディ` stays `ディ` (ICU `deィ`), `ジェ` stays `ジェ` (ICU `jiェ`); `デ` alone gives `de` in both. Hira-Latn's global filter has the voiced Katakana (`デ`, `ジ`) but not the small vowels, so the run ends before the small vowel. ICU ends the text of a rule (`ディ → di`) with the run, but not its after context. The order disambiguation of `デ → de` after `ディ → di` gave `デ } [^ィ…]`, which did not match the `ィ` outside the run. Fixed: such a negated set, where the earlier rules still matching have no after contexts, also matches at the end of a run (`ldml::DisambiguateOrder` with a global filter and transform rules, `QA/xlit_filter_run_end_test.py`). That left 34 lines, all `ビュ`, `ギャ` and the like across a run end, where ICU gives `b~ュ`. The rule `b | '~' ← ヒ ゙} $small_y ;` has an after context that sees beyond the run, and the `~` it revisits fails there. xlit composed it into `ビュ → byu`, after which the pass-through rule `ビ } $small_y → b~` was masked. Fixed as well: where only earlier rules without after contexts continue on the characters of an item of a later rule's after context, the later rule also matches those characters outside the filter (`ldml::FilterRunOutsideMark`, `ビ } [ュ﷖] → b~`). These sets are ANDed with the run ends in the class kernels, the engine and DivCode (`DivContextItem::outside`), and the overlap analysis knows they never meet an item of a text. Hira-Latn now equals ICU on all 6,524 lines, in one thread and in four. Kana-Latn still equals ICU on the Katakana text. 10 BGN transforms gain such subgroups; their CLDR test files are unchanged and correct, and they keep UTF-8 mode.
9. **Small items:**
   - Make `-CheckWhileProgress` usable on the correction kernels: skip the first iteration, or start `Progress` from the first iteration's new bits (see 1C).
   - Measure Zawgyi-my (While against rounds, and its Pablo size).
   - Recover the 1–8% execution-cycle regression of the Title transforms under case-map marks (1D).
   - **Done (2026-11-20):** the stale comments and doc text listed in section 6.
