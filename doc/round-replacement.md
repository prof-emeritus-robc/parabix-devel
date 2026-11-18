# Rounds in tconv: candidates for Pablo While loops

Survey of branch LDML at `d3d0aadc1` (2026-11-17). DivCode now resolves before contexts matched against the converted text with a monotone Pablo `While` loop (doc/ldml-divcode-plan.md §10–§12). This document lists every other place in `tools/ldml/tconv.cpp` that iterates by a fixed number of rounds, classifies them, gives the transforms whose own rules use each one, and counts the unrolled Pablo copies or duplicated kernel calls.

## Method

- `tconv --plan` and `tconv --divcode-report` for all 453 transform names: groups with before contexts matched against the converted text, and their DivCode eligibility and problems.
- `tconv --kernel-counts` per group at `--converted-context-rounds` 8, 1 and 0, and with `--divcode-groups=0`, to tell DivCode groups from regular-plan groups and staged chains from single kernels.
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

Why it has not been a While loop: each round recomputes from scratch and drops matches, so it is not monotone, and in a carry-collapsing While an `Advance` reads the previous block's carry only in the first iteration. doc/ldml-tconv-coverage.md §3.1 still says While loops cannot be used. DivCode §10 shows they can, if the state only accumulates (known-in / known-out streams, `K1 |= MatchStar(next(In) ∨ K1, Del)`), with a frontier of newly decided positions. The same reformulation applies here: a before context depends only on positions to its left, so a three-valued state decided left to right converges, with as many iterations as the longest dependency chain, and none in blocks without a frontier.

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

- Exact at any length: `QA/tconv_converted_while_test.py` (chains of two and three items longer than 8, a deletion and an expansion seen at distance 2, negated sets at the start of the text; with one thread and with four threads and segments of one stride) matches ICU, where the rounds fail 3 of its lines; also on random texts with chains of up to 3000 across blocks, and with dependent positions at block starts after blocks with none.
- Groups of this table now resolved: uz_Cyrl-uz_Latn #1 and the es-ar, es-fa, es_419-ar, es_419-fa group. Still on rounds: hy-hy_Latn/BGN #1, blt-blt_FONIPA #2 and Latn-Kana (a converted string rule), ky-ky_FONIPA (a repeated set, `S T*`), ConjoiningJamo-Latin (54 insertion rules).
- The mechanism also applies to correction kernels of the other classes with only such subgroups: by default 45 transforms have one (Latin-InterIndic and the Latn-* Indic transforms through it, und_FONIPA-ar and the *-ar transforms, my-my_FONIPA, Hans-Hant, Latn-Ethi/SERA, NumericPinyin-Latin, ps-ps_Latn/BGN, Zawgyi-my, the es-* groups, uz_Cyrl-uz_Latn); 122 with `--divcode-groups=0`. On the CLDR test data where there is some, and on random text of each script, the output is identical to the rounds' for all of them (Zawgyi-my not yet checked: its compilation takes too long), and identical to ICU's except Latin-InterIndic, Latn-Ethi/SERA and Bopo-Latn, which differ from ICU with the rounds as well. Latn-Ethi/SERA and Bopo-Latn are ICU's (doc/ICU-uconv-issues.md: the global filter ignored for a single rule set, here the inverse filter; `::` transforms misplaced in the backward direction). Latin-InterIndic is tconv's: its 2,388 string rules exceed `--whole-group-rules`, so the before contexts of its string rules are matched against the text itself (the `--plan` warnings, class 1A), and a vowel after a consonant that a string rule writes (`x → क्स`) or after a deleted inherent `a` is written as an independent vowel (`xi`: `क्सइ`, ICU `क्सि`; `hao`: `हऒ`, ICU `हॊ`), on 41 of 988 random words.
- Cost, uz_Cyrl-uz_Latn: the correction kernel has 2,250 Pablo lines against 6,916 at 8 rounds; execution cycles (4 threads, 5 MB of random Cyrillic) 205,853 against 381,735 (−46%); hy-hy_Latn/BGN's two resolved kernels −55% and −59%. Whole transforms: cold compilation and warm run time unchanged within noise (uz_Cyrl-uz_Latn, es-ar, hy-hy_Latn/BGN), as their class kernels dominate.

Next increments for this class: a repeated set in an item (`S T*`, ky-ky_FONIPA, and class 1D), insertion rules (ConjoiningJamo-Latin), then converted string rules (with class 1A).

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

Comment: this is the largest duplication in tconv by kernel count (Jpan-Latn's cold compile was measured at 228 s in doc/ldml-tconv-coverage.md §3.2), but it is not a direct While candidate. The before contexts are matched by the regular expression engine on compacted text, and each round copies variable-length regions of converted text into reserved positions, a data movement across varying distances. Replacing it needs the contexts as fused sets on the spread text, seen through non-live positions as DivCode sees through deletions, and the copy as shifts by a computed distance. `DeferredRuns` already handles runs of self-reproducing rules at any length in one round, so most rounds do nothing on typical text. Lower-cost steps: section 4, and measuring whether fewer rounds lose anything on real Japanese text.

## 4. Blocking depth in DeferredPlan

Inside every `DeferredPlan` kernel, the triggers whose before contexts may reach an unreplaced trigger are found by an unrolled closure:

```
blocked = blockedBy(triggers ∧ matched)
repeat DeferredBlockingDepth (8) times:
    blocked = blockedBy(triggers ∧ (matched ∨ blocked))
```

That is 9 `MatchStar` steps per kernel, in each of the 8 rounds: 72 copies per deferred group, 144 in Jpan-Latn. The closure is monotone and moves rightward, exactly the form DivCode §10 shows a While loop computes exactly (accumulate `blocked`, frontier = triggers newly blocked). A chain of more than 9 triggers is currently under-blocked, so a trigger can be treated as ready while a trigger before it is unreplaced. Possibly a single `MatchStar` through the reach and the trigger texts suffices; that needs checking against the tails' classes. Small change, exact, cheap: recommended first.

**Status (2026-11-17): implemented in the working tree, not yet committed.** `DeferredPlan` now computes the closure with a While loop (`tools/ldml/tconv.cpp`, kernel renamed `DeferredPlanW_…` so that cached kernels are not reused; `DeferredBlockingDepth` removed):

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

Checks so far:

| Check | Result |
|---|---|
| `tconv_run_moves_test` (deferred rules deleting runs of triggers) | 0 failures |
| Kana-Latn, 5 MB Katakana, 1 deferred round | identical to the previous build |
| Kana-Latn, the same text, 8 deferred rounds | running |
| A chain of more than 9 triggers that the unrolled depth treated as ready too early | test not yet written: most simple copy rules are self-reproducing, whose runs `DeferredRuns` handles |
| `-CheckWhileProgress` | not yet run; a run carried from the previous block can block a trigger before the current frontier in the first iteration, which the check may report although the result is exact |

## 5. Any-FCC passes (not recommended)

`fcc()` runs NFC on a barrier-masked text `FCC_Passes()` times (3: the most nonstarters in a primary composite, U+1F82). Each pass is a multi-kernel NFC pipeline (Any-FCC has 422 kernels against 114 for Any-NFC). The count is exact by Unicode data, and an NFC pipeline cannot run inside a Pablo While, so there is nothing to gain from a loop.

## 6. Already replaced, and stale text

- DivCode applies 55 groups (by kernel counts with `--divcode-groups=0`), including all 42 eligible converted-context groups except ja_Latn-ko #2 and ro_FONIPA-ja #2.
- `include/ldml/divcode_plan.h`: `convertedRounds = 8` is now only an on/off switch for converted contexts, and its comments (lines 63–66, 155–156) and `lib/ldml/divcode_plan.cpp:328` still describe rounds.
- doc/ldml-tconv-coverage.md §3.1 says Pablo While loops cannot be used for this fixpoint, which DivCode §10 disproves. §3.2 gives the deferred-rule default as 4; the option's default is 8.

## Recommended order

1. Section 4: the `DeferredPlan` blocking closure as a While loop (or one `MatchStar`). Small, exact, also fixes chains longer than 9. In progress (see its status).
2. Section 1, in the regular `FusedRules` correction kernel: a monotone three-valued While loop for single-character subgroups first (classes 1C–1E, 21 groups), reusing the DivCode §10 formulation, then string-match selection (class 1A, then section 2). Lift `--whole-group-rules` once the body is not unrolled. In progress: subgroups with fixed items and no insertion rules done (see the status in 1C).
3. Class 1B: let DivCode follow prefix runs that delete (brings in ja_Latn-ko #2 and ro_FONIPA-ja #2).
4. Section 3: only after measuring rounds against real Japanese and Persian text; the redesign is large.
