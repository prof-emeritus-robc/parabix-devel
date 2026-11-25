# DivCode kernels for the conversion groups of xlit: implementation plan

Status: plan, 2026-11-15. Nothing below is implemented. The analysis numbers come
from `xlit --string-rule-chars` (an uncommitted, hidden report option in
`tools/ldml/xlit.cpp`) over the 212 conversion groups with string rules in the
151 CLDR transforms xlit implements.

## 1. Idea

The rules of a conversion group are applied by a few kernels built around a small
alphabet of character classes:

- The **Div32Code** (or **Div64Code**) of a character is the number of its block
  `cp >> s` (s = 5 or 6) among the blocks holding the characters that the
  left-hand sides of the group mention explicitly. The blocks are numbered from 1,
  and 0 means any other character. Over all groups, 135 of 212 need at most 3
  classes (2 code bits) and 187 at most 7 (3 bits).
- A **pseudo-basis**, made of the code bits and the low s basis bits, encodes each
  explicitly mentioned character exactly. Its pseudo-codepoint is
  `(code << s) | (cp & (2^s − 1))`. Every set of the left-hand sides is compiled as
  an ordinary CC over the k + s pseudo-basis bits.
- The **translation kernel** is an If-hierarchy on the code bits. Each rule sits in
  the leaf of its first character (the anchor), and its match is computed by
  lookaheads and written by the XOR technique.

## 2. Scope of phase 1

Unicode mode (21-bit basis) only. A conversion group is eligible if all of the
following hold:

- Its rules are string rules and single-character rules, both with literal results.
  Single-character rules are treated as strings of length 1, in the same kernels.
  The group has no runs, kept characters (`StringRuleKept`), captures or
  MultipleCaptureMultipleReplacement sets, insertion rules, deferred rules or
  revisits.
- Text positions are sets of explicit characters. A **first set spanning several
  classes is split** into one rule per class. For the 18 groups with such sets
  (excluding the 4 that have one-rule runs) the rule count goes from 1,378 to 1,765
  with div32, or 1,641 with div64.
- The group has no string-rule overlaps: `stringOrder` is empty and selection is
  not sequential. This holds for 103 of the 212 groups.
- No before context is matched against the converted text (no
  `convertedContextMatters`).
- Contexts are limited to:
  - **Before:** at most one item, a single-character CC (explicit, negated, or a
    property).
  - **After:** at most one item, a single-character CC or property expression.
  - A set may include the text boundary: a negated set `[^X]` does, and `[^X$]`
    excludes it. A context of the boundary alone (`[$]`) and anything longer
    are deferred.
- The explicit sets fit the alphabet. A set spanning more than N blocks (N to be
  tuned, initially 16) is treated like a property and computed as an external class
  stream. The group fails if its code would need more than 10 bits. Hant-Hans needs
  450 classes, 9 bits.

The reasons a group is not eligible are reported, and an ineligible group keeps the
regular plan (fused kernels).

## 3. Planning (ldml library)

New files `include/ldml/divcode_plan.h` and `lib/ldml/divcode_plan.cpp` (module
`ldml.transforms`, no code generation):

- `DivCodePlan planDivCodeGroup(const TransformStep & step, unsigned shift)` returns
  the following, or the problems that make the group ineligible:
  - `blocks`: the sorted block numbers. The code of a block is its index + 1.
  - `externalSets`: the property and large sets, one stream each, deduplicated.
  - `rules`: one `DivRule` per rule after splitting:
    - anchor class
    - the pseudo-CCs of the text positions
    - before item and after item (each a pseudo-CC or an external set index, with a
      negated flag)
    - result string, expansion `e = max(0, m − n)`, and deletions `max(0, n − m)`
    - the earlier rules it yields to at the same position (from `stringPriority` /
      `subgroupPriority`), and the order among single-character rules (earliest
      rule for a character wins)
  - `leaves`: the anchor classes, with the rules in each.
  - `contentHash`: a hash of s, `blocks`, `externalSets` and the printed rules.
    It is used in kernel names.
- `ldml::TransformStep` gains nothing. The plan is computed by xlit from the step,
  like the shape groups.

`--string-rule-chars` becomes `--divcode-report`. Per group it reports eligibility
(or the problems), classes, code bits for s = 5 and 6, rules after splitting, leaf
sizes, expanding rules, and the XOR bit counts.

## 4. Kernels and pipeline

The Div32Code / Div64Code kernel is `kernel::DivCodeKernel` (`include/kernel/unicode/divcode.h`, module `kernel.unicode`); it depends only on the shift, the blocks and the basis. The kernels specific to the plans of conversion groups are in `tools/ldml/xlit.cpp`.
`RulePipelineBuilder::conversionGroup` calls `divCodeGroup(step, U21)` for an
eligible group when `--divcode-groups` is on (off by default until measured). It
uses `--divcode-shift=5|6` (default 5).

```
U21 ─┬─ Div32Code ──────────────── Code (k bits) ─┐
     ├─ property / large-set classes ─ Ext (1 each)┤
     │                                             ├─ Div32CodeCount ─ InsertBixNum
     │                                             │        │
     │                         InsertionSpreadMask ◄────────┘ (only if some rule expands)
     │                                 │ SpreadMask
     └─ SpreadByMask(U21, Code, Ext, filter mask) ─┤
                                                   ├─ Div32CodeTranslate ─ U21' + Deleted
                                                   │
                         FilterByMask(¬Deleted) ◄──┘ ─ U21 out
```

### 4.1 Div32Code / Div64Code

A Pablo kernel using the UTF compiler over the U21 basis. Output bit j of the code
bixnum is the union of the blocks whose code has bit j set. In phase 2 (UTF-8) it
runs on the UTF-8 basis in `BitMovementMode::LookAhead`, marking first bytes.

Name: `Div32Code` (or `Div64Code`) + the alphabet + basis shape, with **no
transform or group**. The kernel depends only on the block list, so groups with
the same alphabet share one compiled kernel. Over CLDR, 77 of the 186 groups of
≤ 64 blocks share their div32 alphabet with another group (25 alphabets), or 101
groups under div64. Examples are my-my_FONIPA's 6 to 10 groups within one
transform, and the Cyrillic BGN, Ethiopic and InterIndic transforms across
transforms. A short block list is written in the name itself, readable and exact
(e.g. `Div32Code_x3_x21_x22_U21` for blocks 0x60, 0x420, 0x440); a long one is
replaced by a hash of it.

The count and translation kernels are specific to their group: `Div32CodeCount` /
`Div32CodeTranslate` + transform name + `G` + group number (for readability) + `_`
+ `contentHash` (for the object cache: rules files, CLDR changes, planning
options) + basis shape.

### 4.2 External classes

There is one class stream per distinct property or large set, from the existing
`classes()` / `CharClassesKernel` machinery.

### 4.3 Count kernel (only if some rule expands)

Inputs are the basis, the code, the external classes and the filter mask. It
computes the full matches of the **expanding** rules only (m > n, including
single-character rules with m > 1), and the full matches of every rule they yield
to (a yielded match does not expand). It ORs the matches by expansion e,
placed at the last text position by `Advance(M, n − 1)`, into the bits of the
insertion bixnum. The match code is generated by the same C++ functions as the
translation kernel's (§4.5), so the two cannot diverge.

### 4.4 Spreading

`InsertionSpreadMask(InsertBixNum, InsertPosition::After)` puts the gaps after the
end of each expanding match. The basis, the code bits, the external class streams
and the filter mask are spread once each (`SpreadByMask`), with zeros in the gaps.
The code is spread, not recomputed: a few streams are cheaper than a second UTF
compiler kernel. There are no per-slot streamsets.

### 4.5 Translation kernel

Inputs are the spread basis, code, external classes, filter mask and spread mask
(`gap = ¬SpreadMask`). Outputs are the new basis (21) and `Deleted` (1). The
structure is as follows.

1. **Top level, before the hierarchy:**
   - **Before contexts:** each distinct before item C becomes
     `Before_C = MatchStar(Advance(C), gap)`. It is computed outside the Ifs (it
     does not depend on any leaf condition) and ANDed in inside them.
   - An item with the text boundary, such as a negated set `[^X]` (which ICU
     matches at the start and end of the text), is tested as `¬Advance(CC(X))`.
     That holds at the start of the text by itself. Without the boundary, the
     item is `Advance(CC(S))`.
2. **If-hierarchy on the code bits.** A branch condition is "the code is nonzero
   within this subtree", e.g. `¬b2 ∧ (b1 ∨ b0)`, not `¬b2`; there is no leaf for
   code 0. Classes that occur together get numbers sharing high bits.
3. **In the leaf for class A,** with `leaf_A` the stream where code = A:
   - **Anchor:** `leaf_A ∧ CC_low(s bits)`. The If is only a block-level skip, so
     every test is ANDed with `leaf_A`; this also gives every statement the
     dependency on the If condition that Pablo requires.
   - **Later text positions** are tested by `Lookahead` of the input pseudo-basis
     streams at their offsets (contiguous: the text has no gaps).
   - **After items** are tested at offset `n + e_r`, past the rule's own gap.
     The test of the item's set is always ANDed with a lookahead of the in-text
     positions (`InText`), and an item with the boundary is the negation,
     `¬(Lookahead(CC(X)) ∧ Lookahead(InText))`, which holds past the end of the
     text, as in ICU. The AND is needed: beyond the text the code is 0, and a set
     given by its complement (such as `[$]` alone, or a word-boundary set)
     includes the characters of code 0, so it would see a character there.
   - **Priority:** a rule yields to the matches of the earlier rules at its anchor
     (same leaf, same anchor), as planned.
   - **Accumulators** (Vars escaping the If):
     - for strings: per (offset i, target class B), the OR of matches, for the
       upper bits; per (offset i, low bit), for the low bits; per offset, for the
       deletions;
     - for single-character rules, the same at offset 0, kept apart for coverage
       (step 4).
4. **Top level, after the hierarchy:**
   - **String coverage:** `Cov = ⋁_{i ≥ 1} Advance(StringMatchesWithLength>i, i)`.
     Single-character changes apply only on `¬Cov`.
   - **Changed bits:** for each basis bit b, `Changed_b` is the OR of the advanced
     accumulators whose constant has bit b set. OR before Advance: at most (max
     offset) × 21 Advances, however many rules there are. Upper bits use the
     per-(i, A, B) streams: (A ⊕ B) << s is shared, which cuts the upper-bit
     operations by about 93%.
   - **Outputs:** `U21'_b = basis_b ⊕ Changed_b`, and
     `Deleted = ⋁_i Advance(Del_i, i)`.
5. **Filter:** anchors are ANDed with the filter mask at every text position,
   following the existing fused kernels' semantics for contexts at the edges of
   filtered runs.

Large groups (am_Ethi-am_Ethi/Geminate: 7,718 rules; Hant-Hans: 450 leaves) may
need splitting into several translation kernels, by top-level code subtrees,
with their accumulators ORed. This is decided by measuring compile times.

### 4.6 Deletion

`FilterByMask(¬Deleted)` on the output basis. Gaps are never deleted: each gap
belongs to an expanding match that writes it.

## 5. Testing

- `QA/xlit_divcode_test.py`: rules files (`--rules-file`) for each case, each run
  with `--divcode-groups` on and off and with ICU (`--use-ICU`), on generated and
  random strings:
  - literal strings
  - split first sets
  - single-character rules inside string matches (coverage), and priority at the
    same position
  - before contexts (explicit, negated, property), including after an expanding
    match (across its gap)
  - after contexts (explicit, negated, property) at the end of the text
  - expansions, deletions, and both in one group
  - filters
  - s = 5 and s = 6
- CLDR: `xlit_coverage.py --test-data` with the option on and off; results must
  be identical for every eligible transform.
- With `-EnableAsserts`, the translation kernel checks that the positions written
  after its expanding matches are exactly the gaps.
- Experiments use `-object-cache-salt=<tag>`.

## 6. Measurement

On the largest eligible groups (candidates: Latn-Kana, Latin-InterIndic,
my-my_Latn, Bopo-Latn, Latn-Bopo, Sinh-Latn/UNGEGN, am_Ethi-am_Ethi/Geminate,
Hant-Hans), against the regular plan, one run at a time, with `-thread-num=4`:

- kernels (`--kernel-counts`)
- cold-start compile time
- throughput on realistic text for each source script
- leaf If entries (Pablo branch counters / `-EnableCycleCounter`)

Do this for s = 5 and s = 6. Div64 needs fewer classes and splits; div32 has sparser
leaves and about 13% fewer bit operations with upper-bit sharing.

### Results (step 5)

DivCode is on by default (`--divcode-groups`, div32: `--divcode-shift=5`);
`--divcode-groups=0` gives the regular plan.

Inputs: 8 MB of `QA/TestCorpora/Twain.txt`, transliterated into each source
script by the reverse transform (14–23 MB; Sinh-Latn has none, and is left out).
Each configuration runs alone, with `-thread-num=4` and its own object-cache
salt; the cycles are the totals of one `-EnableCycleCounter` run, and the cold
start a run on one line with the object cache off. All four configurations give
the same output.

| transform | group kernels (regular / div32 / div64) | total cycles, div32 vs regular | div64 vs regular | DivCode kernels' share of cycles (div32) | cold start, s (regular / div32) |
|---|--:|--:|--:|--:|--:|
| Thaa-Latn | 28 / 13 / 13 | −47.4% | −46.1% | 24.2% | 1.78 / 0.82 |
| byn_Ethi-byn_Latn/Xaleget | 36 / 16 / 16 | −27.8% | −25.8% | 17.0% | 1.66 / 0.91 |
| ja_Hrkt-ja_Latn/BGN | 50 / 26 / 26 | −22.3% | −20.0% | 5.6% | 5.35 / 4.80 |
| Latn-Armn | 22 / 13 / 13 | −12.7% | −13.2% | 2.0% | 4.82 / 4.66 |
| Armn-Latn | 22 / 13 / 13 | −12.4% | −12.8% | 2.9% | 4.73 / 4.64 |
| Halfwidth-Fullwidth | 11 / 6 / 6 | −6.8% | −6.4% | 2.8% | 0.48 / 0.44 |
| Hira-Kana | 12 / 8 / 8 | −5.3% | −5.2% | 0.3% | 8.78 / 8.48 |
| InterIndic-Devanagari | 22 / 13 / 13 | −5.0% | −7.9% | 3.6% | 0.55 / 0.50 |
| Cyrl-Latn | 26 / 18 / 18 | −2.2% | −2.8% | 1.9% | 4.85 / 5.08 |

- The gains follow the group's part of the pipeline. Where the group is the
  whole transform (Thaa-Latn, byn_Ethi, InterIndic-Devanagari,
  Halfwidth-Fullwidth), the saving is the group's own; elsewhere the other steps
  (normalizations, 150–350 kernels) dilute it. The group's kernels drop by
  40–55% everywhere.
- div32 is ahead by 1–3 points on the largest groups (Thaa-Latn, byn_Ethi,
  ja_Hrkt), consistent with its sparser leaves and fewer bit operations; div64
  is slightly ahead on the alphabetic groups (up to 3 points). div32 is the
  default, as it wins where the gains are greatest. (Single cycle-counter runs:
  differences of a point or two are within the noise.)
- The warm runs take 0.06–0.16 s, mostly startup, so their wall times are not
  reliable at these sizes; they point the same way as the cycles (e.g.
  Thaa-Latn 157 MB/s regular, 201 MB/s div32).
- The default plan with shape groups is no faster than the regular plan on any
  of these.
- Leaf If entries were not measured: `-EnableProfiling` (Pablo branch counters)
  ran over 12 minutes without finishing on one transform.

A correction to these figures: the CYCLES column of `-EnableCycleCounter`
includes the time a kernel spends synchronizing with other threads, so the
totals above overstate some kernels. Counted in execution cycles (CYCLES ×
EXEC%), DivCode in Unicode mode is still ahead of the regular plan on all nine
transforms (from −1% to −45%; see the next section).

### UTF-8 mode (phase 2)

DivCode groups also apply to UTF-8 code units, and a transform whose
conversion groups are character maps or DivCode-eligible groups now runs in
UTF-8 mode (the default; `--utf8-mode=0` for Unicode mode), as do rules files
(`--rules-file`).

- **Codes and pseudo-basis.** `DivCodeKernel` marks the code at the first byte
  of each character (LookAhead mode). The low s bits of a character are those of
  its last byte, at offset + L − 1 for its length L: each block has one UTF-8
  length (the length boundaries 0x80, 0x800 and 0x10000 are multiples of 64), and
  s ≤ 6 bits fit in the last byte.
- **Lengths.** A rule's offsets, gaps, insertions and deletions count bytes.
  Each text position after the first has characters of one UTF-8 length
  (`DivCodeOptions::utf8`; the first is split by blocks). The characters whose
  length is not known (context items, the character before an anchor) take it
  from their first byte: the low bits of an item are selected by it, and a before
  item is moved past the character by an Advance of each length.
- **Changes** are per byte: the bits that a set position does not vary (the
  leading bytes, and the high bits of the last) by constant XORs, the low bits
  of the last byte flipped where they differ from the result's.
- **After items** are tested once per position by a kernel of their own
  (`DivCode Items`), read by the translation and Count kernels with a lookahead
  (Pablo lookahead applies to the inputs of a kernel only); in both modes.
- **Filters** are masks over the bytes of the selected characters, as in the
  other kernels of UTF-8 mode.

Execution cycles (CYCLES × EXEC%) and single-thread times, on the corpora of
step 5 (each configuration alone, `-thread-num=4` for the cycles):

| transform | DivCode Unicode vs regular | DivCode UTF-8 vs regular | 1 thread, s: regular / Unicode / UTF-8 |
|---|--:|--:|--:|
| Latn-Armn | −14.4% | −59.4% | 0.194 / 0.180 / 0.119 |
| Armn-Latn | −14.1% | −55.7% | 0.161 / 0.147 / 0.106 |
| ja_Hrkt-ja_Latn/BGN | −22.8% | −53.2% | 0.309 / 0.269 / 0.201 |
| InterIndic-Devanagari | −4.4% | −53.1% | 0.123 / 0.120 / 0.081 |
| Cyrl-Latn | −1.9% | −51.4% | 0.203 / 0.202 / 0.133 |
| byn_Ethi-byn_Latn/Xaleget | −31.9% | −44.2% | 0.183 / 0.163 / 0.154 |
| Hira-Kana | −0.9% | −27.9% | 0.387 / 0.383 / 0.315 |
| Thaa-Latn | −44.6% | −35.4% | 0.212 / 0.159 / 0.187 |
| Halfwidth-Fullwidth | −6.0% | +19.4% | 0.093 / 0.092 / 0.104 |

- UTF-8 mode is the best for seven of the nine, about halving the execution
  cycles: the DivCode kernels cost 2–4 times as much on bytes, but the decoding,
  encoding and other steps of the transform cost much less.
- Thaa-Latn, a single large group (430 rules), is best in Unicode mode, where
  the group's own cost dominates.
- Halfwidth-Fullwidth is worse in UTF-8 mode than the regular plan: every ASCII
  character becomes 3 bytes, so the whole text is counted, spread and filled in
  gaps, while in Unicode mode nothing expands.
- UTF-8 mode stays the default. To do: choose Unicode mode for a transform of
  DivCode groups alone with large groups or dense expansions in bytes, once such
  a rule is validated on more transforms.

## 7. Results of step 1

`xlit --divcode-report <transform>` (one JSON object per conversion group with
string rules) over the 212 groups of the CLDR transforms xlit implements:

- **45 groups in 40 transforms are eligible** under both div32 and div64. They
  have 4,223 rules (string rules and mappings), or 4,445 after splitting (div32)
  and 4,422 (div64).
  - Code bits: div32 needs 1–2 bits for 12 groups, 3 for 24, 4 for 8 and 5 for 1;
    div64 needs 1–2 bits for 25 groups, 3 for 17, 4 for 2 and 5 for 1.
  - 29 groups have expanding rules; 3 have external sets.
  - The largest are Sinh-Latn/UNGEGN (786 rules), Thaa-Latn (430),
    byn_Ethi-byn_Latn/Xaleget (347), ja_Hrkt-ja_Latn/BGN (298) and
    Halfwidth-Fullwidth (252).
- The reasons for the others are listed below. "Only" is the number of groups
  where that reason is the only one.

  | reason | groups | only |
  |---|--:|--:|
  | string rule overlaps | 109 | 16 |
  | contexts of two or more items | 105 | 13 |
  | before contexts matched against the converted text | 64 | 11 |
  | string rules with runs | 23 | 7 |
  | rules with captures | 22 | 5 |
  | self-overlapping string rules | 16 | 1 |
  | contexts of the text boundary alone | 7 | 0 |
  | context sets with strings | 4 | 0 |
  | other (repetitions, revisits, alphabets of more than 10 bits) | 7 | 0 |

## 8. Steps

1. `divcode_plan` (eligibility, alphabet, splitting, rules, hash) and
   `--divcode-report`; check the eligible groups and the reasons for the rest.
2. Div32Code/Div64Code kernel and external classes; a test that dumps the code
   bixnum (done: `xlit --divcode-codes`, `QA/xlit_divcode_test.py`).
3. Translation kernel without expansions (groups where m ≤ n everywhere), with
   deletion; QA tests; the CLDR comparison. (Done: `xlit --divcode-groups`,
   `DivCodeTranslate` in `tools/ldml/xlit.cpp`. The output is identical to the
   regular plan's for the 16 eligible CLDR groups without expansions, on random
   text and their CLDR test data, under div32 and div64, and to the regular
   plan's and ICU's for 13 rules files of the cases. The kernels of these groups
   go from 4–20 to 3–8, typically from 10–12 to 6.)
4. Count kernel, spreading, gap-aware before contexts; QA tests. (Done:
   `DivCodeTranslate` in Count mode; the translation on the spread text. All 45
   eligible CLDR groups match the regular plan, and 22 rules files match the
   regular plan and ICU. Two corrections to §4.5 came out of testing:
   - In the spread text, the after item of every rule, and of the earlier rules
     it yields to, reads the first character after any gap (a gap of exactly g
     positions, for g up to the greatest expansion), as on the text before it
     was spread. A rule then applies only with exactly its own expansion of gap
     after it (none, if it does not expand), as the Count kernel found.
     Otherwise a rule at the same anchor as an expanding one reads the other's
     gap.
   - A single-character rule's coverage is applied at its anchor, before its
     changes are advanced to their offsets, which inserted positions may have.)
5. Measurement (§6); choose the default s and whether `--divcode-groups` is on by
   default. (Done: on by default, div32. With the default on, the other xlit
   suites showed that the end of a run of a global filter (`FilterRunEndMark`
   in a context set) also holds outside the filter's mask; the plan and the
   kernel now do so, and a text position with the mark is not eligible.)
6. Phase 2:
   - UTF-8 mode: codes at first bytes; one UTF-8 length per class (s ≤ 6); offsets
     in code units; set positions of mixed lengths split by length or excluded.
     (Done, with set positions of mixed lengths excluded; see "UTF-8 mode" in §6.)
   - One-way overlaps (topological stages).
   - Longer contexts and text boundaries.
   - Converted before contexts.

## 9. Plan: one-way overlaps

Status: plan, 2026-11-15.

### Scope

A string rule B *overlaps* a string rule A one way if B may match strictly
within the text of a match of A, but not A within B (`StringRuleOverlaps`, see
`include/ldml/transform_plan.h`). ICU, scanning from the left, then applies A
wherever both match, and skips B's match. If all the overlaps of a group are
one-way and the precedence graph (A → B) is acyclic, the rules apply in a single
pass in topological order: a match of B applies unless it begins within an
applied match of a rule that precedes it. This is how the regular plan applies
them (`TransformStep::stringOrder`, in *stages*: the rules of the longest chain
of the graph).

Of the 212 groups with string rules, 48 have one-way overlaps only:

- **12 groups (1,727 rules) are blocked by them alone**:
  - Ethi-Latn/ES3842 (452 rules, 34 pairs)
  - Latn-Ethi/Williamson (268, 124 pairs)
  - und_FONIPA-ar and und_FONIPA-fa group 2 (211 each, 15 pairs)
  - es_FONIPA-ja (121)
  - the BGN romanizations of kk, uk, be and bg (76–112 each, 4 pairs)
  - la-la_FONIPA, Latn-Geor, pl-pl_FONIPA
- **33 of the other 36** are also blocked by contexts of two or more items,
  mostly the contexts order disambiguation adds. Latn-Kana and Bopo-Latn are
  among them. These contexts are the next limit, after this plan.

Two-way overlaps, self-overlaps and cycles (sequential selection) stay out of
scope.

### Design

**Stages.** Each string rule gets a stage. Stage 1 is the rules with no
predecessor; a rule's stage is one more than the highest stage of the rules it
yields to. These are the stages of the regular plan. Single-character rules
have no stage: they apply outside every applied string match, as now. Parts
split from one rule by blocks share its stage.

**One hierarchy per stage.** The translation kernel, and the Count kernel, run
the If-hierarchy on the code bits once per stage. Each pass contains only the
leaves with rules of that stage (`hasRules` filtered by stage).

- Between passes, at the top level, `Cover_s` is the set of positions strictly
  within the applied matches of all string rules of stages before s. It is
  computed as now for coverage, from the Vars of the string matches by number of
  units: `Cover = ⋁_j Advance(longer_j, j)`.
- Stage s's rules are ANDed with `¬Cover_s` at their anchors, inside their
  leaves. `Cover_s` depends on the earlier passes' matches, and `¬Cover_s` is
  ANDed into a match that depends on the leaf condition, so the If invariant
  holds.
- Using the union of all earlier stages, rather than only a rule's own
  predecessors, is exact. A rule that is not a predecessor of B cannot have a
  match containing B's first position, or the analysis would have paired them.
  Rules of the same stage cannot overlap one another either.
- The Vars of the string matches hold the *applied* matches: after priority,
  after the stage's suppression, and in the spread text after the gap check.
  The coverage of single-character rules after the last pass is then, as now,
  the coverage by the applied string matches.

**Count kernel.** It runs the same stages, so a suppressed expanding match
inserts nothing. The translation on the spread text suppresses the same matches:
covers count units of text, and gaps come only after the end of a match, never
within it. Its gap check (exactly the rule's own expansion after its match)
stays as it is.

**Contexts.** Nothing changes. A before context of B never sees an applied A
that contains B's anchor, because B is then suppressed. One that sees the
converted text of an earlier match is ruled out, as now, by the
converted-context check.

**UTF-8 mode.** Covers are in code units, as are the lengths of the matches.

**Cost.** Each stage adds one pass of the hierarchy's conditions, over the
leaves of its rules only, plus one cover of at most (longest text − 1)
Advances. Groups have few stages: Latn-Kana has 3. Groups of more than a limit
of stages (initially 8) stay ineligible.

### Steps

1. **Plan** (`divcode_plan`):
   - accept groups whose overlaps are one-way only and acyclic
     (`StringRuleOverlaps`: no two-way pairs or self-overlaps, `acyclic`), with
     at most `maxStages` stages (`DivCodeOptions`, default 8)
   - set `DivRule::stage` (0 for single-character rules) and include it in the
     content hash
   - `--divcode-report` gives the stages and pairs of each group; check the 12
     groups above become eligible
2. **Kernels:** the stage loop in `DivCodeTranslate` (both modes, Count and
   Translate), with `Cover_s` between passes; groups without overlaps keep a
   single pass and the same code as now.
3. **Tests** (`QA/xlit_divcode_test.py`):
   - rules files:
     - a chain of 3 stages (`abcd > 1 ; bcd > 2 ; cd > 3 ;`)
     - the kana shape `kya > キャ ; ya > ヤ ;` with single-character rules
       within
     - a suppressed match that would expand, and one that would delete
     - an inner rule with an after context reaching past the outer match
     - multibyte characters, and a filter
   - the 12 CLDR groups, on random text and test data, in UTF-8 and Unicode
     modes
   - all compared with the regular plan, and the rules files also with ICU
4. **Measurement:** Ethi-Latn/ES3842, Latn-Ethi/Williamson, und_FONIPA-ar and
   a BGN romanization against the regular plan, on corpora in their source
   scripts. Count execution cycles (CYCLES × EXEC%) and single-thread times,
   each configuration run alone.
5. **Then:** contexts of two or more items (fixed sequences of sets, as the
   fused kernels' tier A), which would make most of the other 36 groups with
   one-way overlaps eligible.

### Status (2026-11-15)

- **Steps 1 and 2 are done.** Stages are planned (`DivRule::stage`) and applied
  by one If-hierarchy pass per stage, in Count and Translate modes and in both
  encodings. Priority works across stages: raw matches are computed per leaf as
  needed, for a pass's own rules and the earlier rules they yield to.
  - The Vars of the applied string matches are keyed by stage. Each cover reads
    only the Vars of the stages before it. Otherwise the Pablo builder reuses
    the expression of an earlier cover over the same Vars, computed before a
    later pass assigned them, and the single-character rules within the
    matches of that pass are not covered.
- **Contexts of two or more items are done.** A context is a fixed sequence of
  items, from the text outward; only the outermost may hold at the text
  boundary (a bare `[$]` included).
  - Before contexts are chained at the top level, by moving each item's value
    past the character before it (`next`).
  - After contexts are chained by the Items kernel on the text before it is
    spread, each item at a lookahead offset (in UTF-8, one for each length of
    the characters before it). The streams are then spread with the text.
- **Eligibility:** 76 of the 212 groups with string rules (12,442 rules), against
  45 before stages. The new ones include my-my_Latn (1,527 rules, 4 stages),
  ru-zh (1,207 rules, 8 stages) and Bopo-Latn (871 rules, 4 stages).
- **What remains:**
  - before contexts matched against the converted text (65 groups, the only
    problem of 30)
  - two-way and self overlaps (61, the only problem of 20)
  - contexts with repetitions or alternatives (30)
- **Tests** (`QA/xlit_divcode_test.py`): the 31 CLDR groups with stages or
  multi-item contexts, and 16 rules files of these cases, in both encodings,
  give the regular plan's output (and ICU's, for the rules files). None of the
  other xlit suites has an eligible group with one-way overlaps. Four have
  such groups, each blocked by another limit; Bopo-Latn in
  `xlit_shape_groups_test` is now eligible.

### Measurements (step 4)

The groups made eligible by stages and contexts of several items, on text in
the source script (8 MB of Twain, transliterated by the reverse transform: 8–23
MB; Myanmar: a 10 MB Zawgyi file; IPA: `es-es_FONIPA` of the same text). Each
configuration ran alone. The cycles are execution cycles (CYCLES × EXEC%) at
`-thread-num=4`; "default" is DivCode in UTF-8 mode where the transform can be
in it. All three configurations give the same output.

| transform | group kernels, regular / DivCode | DivCode Unicode vs regular | DivCode default vs regular | 1 thread, s: regular / Unicode / default | cold start, s: regular / default |
|---|--:|--:|--:|--:|--:|
| my-my_Latn | 86 / 30 | −80.8% | −81.8% | 1.111 / 0.572 / 0.579 | 31.1 / 10.2 |
| ru-zh | 47 / 19 | −65.3% | −65.4% | 1.022 / 0.770 / 0.761 | 30.5 / 8.4 |
| uk-uk_Latn/BGN | 33 / 21 | −20.7% | −64.9% | 0.230 / 0.201 / 0.118 | 4.5 / 4.2 |
| Bopo-Latn | 64 / 21 | −62.5% | −48.0% | 1.283 / 0.950 / 0.968 | 31.5 / 11.2 |
| Telugu-InterIndic | 13 / 7 | −2.3% | −61.7% | 0.111 / 0.110 / 0.067 | 0.48 / 0.24 |
| Latn-Ethi/ALALOC | 29 / 21 | −46.4% | −58.9% | 0.263 / 0.184 / 0.161 | 5.1 / 3.9 |
| Latn-Ethi/Williamson | 16 / 13 | −56.6% | −54.0% | 0.138 / 0.093 / 0.096 | 1.95 / 0.60 |
| und_FONIPA-ar | 125 / 110 | −49.3% | −52.5% | 1.152 / 1.171 / 1.115 | 21.1 / 17.9 |
| es_FONIPA-zh | 94 / 72 | −50.8% | −52.1% | 0.539 / 0.405 / 0.410 | 11.2 / 5.7 |
| bg-bg_Latn/BGN | 29 / 21 | −19.3% | −49.6% | 0.169 / 0.147 / 0.114 | 1.6 / 2.3 |
| ru-ja | 57 / 20 | −37.9% | −48.9% | 0.481 / 0.369 / 0.338 | 10.2 / 6.7 |
| Gurmukhi-InterIndic | 10 / 7 | −0.0% | −46.3% | 0.111 / 0.111 / 0.079 | 0.51 / 0.27 |
| Latn-Cans | 17 / 16 | −47.3% | −38.8% | 0.128 / 0.091 / 0.098 | 1.27 / 0.56 |
| Ethi-Latn/ES3842 | 42 / 24 | −32.7% | −28.5% | 0.203 / 0.177 / 0.189 | 1.78 / 1.00 |
| Latn-Geor | 17 / 16 | −10.5% | −13.5% | 0.088 / 0.082 / 0.080 | 0.48 / 0.33 |

All 15 are faster, by 13–82% in execution cycles. The largest groups also
compile much faster cold (ru-zh, Bopo-Latn and my-my_Latn from about 31 s to
8–11 s). The Bopomofo, Gurmukhi and Telugu texts are transliterated English,
so their frequencies are not those of real text.

## 10. Plan: before contexts matched against the converted text

Status (2026-11-16): the design below is implemented (see "Implementation"
below), replacing a first implementation by rounds (commits 799be3d0d and
b9113b35f, described under "First implementation: rounds").

### The problem

ICU matches a before context against the text to the left of its cursor,
which it has already converted (§3.1 of
[ldml-xlit-coverage.md](ldml-xlit-coverage.md)). It looks through deleted
characters, sees the last character of a longer replacement, and sees the
results of earlier rules of the same kind, so chains propagate (`a { b → a ;
a { c → C` turns `abbbc` into `aaaaC`). `convertedContextMatters` flags a
context when some conversion of its group changes whether a character is in an
item's set, deletes a character, or (for items beyond the one next to the rule)
changes the length of the text. The regular plan applies such rules in a
correction kernel, in a fixed number of rounds (`--converted-context-rounds`,
default 8), and so differs from ICU on longer chains.

`xlit --divcode-report` describes these groups (field `converted`):

- 65 groups have such contexts; for 30 of them (4,013 rules) it is their only
  problem.
- In 21 of the 30, the rules with such contexts are single-character rules,
  usually few: Ethi-Latn/SERA has 18 such mappings among its 475 rules,
  InterIndic-Latin 73 among 319, ko-ko_Latn/BGN 18 among 255. In the other 9,
  string rules have them too (ja_Latn-ko group 2: 60 string rules and 51
  mappings).
- 28 of the 30 groups delete, and 28 expand; 13 have stages.

Expansion rules out patching the output after the translation, as the regular
plan's correction kernel does. The Count kernel fixes the gaps of the spread
text from the matches it finds, so the converted contexts must be resolved
before it counts, and the translation kernel must see the same resolution.

### Design

**The converted character before a position.** The converted text needs to be
known at only one place: the last converted character before an anchor, past
any deleted ones. A match of rule r at anchor a, with N units of text and M
units of result, writes its result in place in the spread text: unit j of the
result at a + j, and the units of the text beyond M are deleted.

- Before the text is spread (Count kernel), the result's last character counts
  as ending at a + min(N, M), where its gap will be inserted. In the spread text
  (translation kernel) it ends at a + M.
- Positions inside a match are never anchors of rules that apply there: they
  are covered, or exclusive by order disambiguation. So only the last character
  of each match's result matters, and the deleted units after it.

**Three kinds of positions.** For the item sets S of the converted contexts:

- **Independent:** no converted rule can match at the position. Its outcome
  (deleted, or the last character of its result) does not depend on what is
  before it.
- **Consumers:** a converted rule can match, so which rule applies depends on
  the converted character before. But every possible outcome agrees: all are
  deleted, or all end in characters with the same membership in every S. What
  the next positions see is therefore known without resolving the position.
  Example (InterIndic-Latin): `[^[:L:]…] { aa → ̔ā` and the rule applied
  otherwise both end in `ā`, a letter.
- **Propagating:** the possible outcomes differ in deletion or set membership,
  so what the next positions see depends on the state here. Examples: the
  InterIndic-Latin nukta, deleted only after a non-letter; ko-ko_Latn/BGN `ᄋ`,
  deleted only at the start of the text.

The plan classifies each character of the converted rules (see "Which
characters propagate" below). Only chains of propagating positions need
iteration. They are rare, and a third of the groups have none.

**State.** For each item set S, streams that only gain bits:

- `In[S]`, `Out[S]`: the positions whose converted character is known to be in S,
  or known not to be;
- `Del`: the positions known to be deleted;
- `K1[S]`, `K0[S]`: the positions before which the last converted character is
  known to be in S, or known not to be:
  - `K1[S] = MatchStar(next(In[S]), Del)`;
  - `K0[S] = MatchStar(next(Out[S]) ∨ start, Del)`. A negated set includes the
    text boundary.

A converted context reads `K1[S]`, and a negated one reads `K0[S]`, never
`¬K1[S]`. "Unchanged character in S" is a known outcome too, never
"in S ∧ ¬Within", so every stream stays monotone.

**Resolution.**

1. **Before the loop:** the outcomes of all independent and consumer positions
   go into `In`, `Out` and `Del`, from the rules as now. Then `K1` and `K0` are
   computed.
2. **The loop**, only if the group has propagating characters. Its condition,
   the frontier, is the propagating positions not yet evaluated (`Done`) whose
   `K1[S] ∨ K0[S]` holds. Each iteration:
   1. evaluates the rules at the frontier: the If-hierarchy over the leaves of
      the propagating characters, ANDed with the frontier;
   2. adds their outcomes to `In`, `Out` and `Del`, and the frontier to `Done`;
   3. extends `K1` and `K0`;
   4. sets the frontier to the propagating positions that have just become
      known, which lie after the positions evaluated.
3. **After the loop:** one pass over the converted rules' leaves, at every
   dependent position (consumers and propagating), with `K1`/`K0` as their
   before contexts. This gives their matches and changes, as for any other
   rule.

Chains are exact at any length, as in ICU.

**Why a Pablo While loop is exact here.** In a `While` (default,
carry-collapsing mode, `carry_manager.cpp`):

- an `Advance` or `MatchStar` reads its carry from the previous block in the
  first iteration only;
- the carry-outs of all iterations are ORed into the carry that the next
  block's first iteration reads;
- the loop is entered when its condition has a 1 bit or carries are pending.

This is exact when every loop value only accumulates (ORs in new bits), each
bit of the condition is an independent start position, and each iteration
clears the condition's bits and adds new ones only at later positions. The RE
compiler's loops for `R*` (`pending`, `accum` in `re_compiler.cpp`) have this
form.

The accumulation is required. If `K1`/`K0` are recomputed from `In`/`Out` in each
iteration instead of ORed into, the carry from the previous block is lost after
the first iteration, and a chain of deletions stops one position into the next
block. Either form below is exact:

- the accumulated values are sources again:
  `K1 |= MatchStar(next(In) ∨ K1, Del)`;
- only the new bits propagate (the RE compiler's `pending`):
  `K1 |= MatchStar(next(newIn) ∨ (K1 ∧ newDel), Del)`.

**Kernels: less spreading.** Two changes cut what is spread and keep the
resolution out of the Count and translation kernels.

*After contexts on the spread basis.* Before this change, `DivCodeItems` tested
every distinct after context on the text before it was spread, and all of these
streams were spread (34 for InterIndic-Latin). In the spread text, the
translation kernel read an after item past any gap, ORing over the gap lengths
0..E. Instead:

- **A context of one item** is tested directly, by its CC on the looked-ahead
  pseudo-basis (spread `Code` and the low bits of `Basis`), as the texts of the
  rules are tested. `Externals` are read at the offset. The test is ANDed with
  `InText` there.
- **A rule's own after item** lies at an exact offset, `N + e`, where e is the
  rule's expansion: a match in the spread text already requires exactly its own
  gap (`gapOf(N, e)`), so the other terms of the OR are 0.
- **The after items of the earlier rules a rule yields to** keep the OR over the
  gap lengths. When the earlier rule's text is longer or shorter than the
  rule's, its after item can lie past a gap of another length.
- **A context of several items** is still chained by `DivCodeItems` on the text
  before it is spread. Only these streams are spread, and only for the
  translation kernel. The Count kernel reads them unspread.
- **The Count kernel** tests single items directly too, so `DivCodeItems` is
  needed only for contexts of several items.

*Skipping inserted positions.* The OR over gap lengths remains for earlier
rules and, if contexts of several items were chained in the spread text, for
inner items. Two ways keep it cheap:

- **Statically:** an inner item can be followed by a gap only if some expanding
  rule's text can end with a character of its set (E_j, the largest such
  expansion). With E_j = 0, only the term without a gap remains.
- **Dynamically:** the terms with gaps can be put in an `If` on "inserted
  positions in this block or in the lookahead window". The term without a gap
  stays outside it. Each gap term is ANDed with `gapOf`, which depends on the
  condition, as an `If` requires. The same `If` can guard `next()`'s `MatchStar`
  over gaps for before contexts.

The caveat for computing anything on the spread basis: inserted positions have
all-zero basis bits, which reads as U+0000 (or byte 0). Spread `Code` is 0
there, but a code or external computed on the spread basis instead of spread
must be ANDed with the `Spread` mask.

*The resolution in its own kernel, on the spread text.* The Count kernel needs
to know which rule applies at a dependent position, because the rules'
expansions can differ. To remove that dependency:

1. **Count** (before spreading) reserves at each dependent position the largest
   expansion among its possible outcomes (the propagation analysis lists them
   per character). Count then never needs `K1`/`K0`. Where an outcome with a
   shorter result applies, the surplus positions are marked deleted, and the
   existing `Deleted` → `FilterByMask` removes them. For these positions, "a
   match has exactly its own gap" becomes "at least its own gap, with the
   surplus deleted".
2. **Spread:** `Basis`, `Code`, `Externals`, the masks, and the chained after
   contexts only.
3. **Translate** (spread): all rules except the converted ones. It leaves the
   input character and its reserved positions at dependent positions, and
   outputs, besides `Out` and `Deleted`:
   - `In[S]` and `Out[S]` per converted item set. Each rule's result is known,
     so whether its last character is in S is a constant, as for the rounds'
     `End_S` key. No set (some are properties, such as `[^[:L:]…]`) is tested on
     the output.
   - `Dep`, the dependent positions.
4. **Converted** (spread, new): reads the spread `Basis`, `Code` and `Externals`
   with lookahead (for its rules' texts and after contexts), and Translate's
   `Out`, `Deleted`, `In`/`Out` and `Dep`. In the spread text, positions are
   output positions, so `K1 = MatchStar(Advance(In, 1), Del)`, with no `next()`
   over character lengths and no gaps. `Del` is `Deleted` plus the surplus, and a
   reserved position counts as unknown until its position is evaluated. The
   kernel runs the loop over the propagating positions, then one pass of the
   converted rules at every dependent position, XORing their results into `Out`.
   Translate left the input character there, so the changes are constants.
5. `FilterByMask` as now.

Neither Count nor Translate then has rounds or a loop. A group without
expansions has no Count kernel and no spread, and the same Translate and
Converted kernels apply. Phase 2 (converted string rules) fits this split less
well: the rules that yield to them, and later stages, would move into the
Converted kernel.

**Which rules depend on the resolution.** A converted-context rule's changes
can change other rules:

- a rule that yields to it (priority);
- a string rule of a later stage, whose cover depends on its matches if it is a
  string rule;
- the single-character rules within its matches.

Order disambiguation gives the rules whose contexts it made complementary
contexts on the same items, so they are flagged together, as
`convertedContextMatters` flags each.

- **Phase 1:** the converted-context rules are single-character rules that no
  rule yields to. Nothing else depends on their resolution: single-character
  rules cover nothing, and the string rules' covers and the stage passes are
  unaffected.
- **Phase 2:** converted-context string rules. The rules that depend on them
  (those that yield to them, later stages, and the single-character rules
  within them) are evaluated in the loop body too.

**Contexts of several items.** Items beyond the one next to the rule need the
converted characters before the last; within one match's result these are known
too (keyed by their end offsets). Phase 1 takes contexts whose items beyond the
first see only characters a single conversion maps to one character each.
That is the case the check allows, as it flags length changes for those
items.

### Experiment: the While loop

`tools/util/convwhile.cpp` (uncommitted) applies sets of single-character
rules with converted contexts over ASCII, by the loop above. Every position
with a converted rule is treated as propagating, the worst case. The rule sets:

- a chain (`a { b → a ; a { c → C`);
- a toggle (`[^a] { b → a ; b → c`);
- deletions seen through (`x { d → ; x { b → x ; d → D`);
- a mix of two sets, negations, deletions and rules without contexts.

A checker compares it with an ICU-style left-to-right simulation on texts of up
to 3 MB, with runs thousands of characters long. Every case matches, with 1
and 4 threads and segment sizes 1, 3 and 7. The variant that recomputes
`K1`/`K0` fails as described above.

Cost on 30 MB, one thread, relative to text with no dependent positions:

| Text | Relative cost |
|---|---|
| `bxbx…` (chains of one) | 2× |
| random `abcx` | 8× |
| random, mixed rules | 20× |
| all `b` (one chain through the text) | 210× (0.2 s) |

Blocks with no frontier and no pending carries skip the loop. A block where
every position is in one chain takes as many iterations as the block has
positions. That cost is linear, and it is the case a fixed number of rounds
gets wrong.

### Which characters propagate

The analysis (patch saved with the experiment; `xlit --divcode-report` with
`DIVCODE_PROPAGATION=1`) works per character c of the converted subgroups:

- **States:** the cells of the partition of the characters by the item sets
  (one representative each), and the text boundary.
- **Free variables:** the other conditions (after contexts, and before contexts
  not matched against the converted text), each distinct pattern one variable.
  Impossible combinations, such as `$x` and `[^$x]` both false, can only make
  the analysis conservative.
- **The test:** c propagates if, for some assignment of the variables, the
  outcome at c (the first rule in order that applies) differs in deletion or
  set membership between two states.
- **String rules:** a string rule beginning with c after a state-dependent rule
  counts as propagating (conservative).

Over every transform:

- 87 groups have converted single-character subgroups (46 also have converted
  string rules), and 29 of them have no propagating character.
- Of the 21 eligible groups, 10 have none: Ethi-Latn/SERA, Ethi-Cyrl/Gutgarts,
  Ethi-Latn/ALALOC, Arab-Latn, sat_Olck-sat_FONIPA, hy-hy_Latn/BGN group 3,
  uz_Cyrl-uz/BGN, sk-sk_FONIPA, hy-hy_FONIPA and es_FONIPA-am.
- The other 11 have 29 propagating characters among their 126 converted ones:

| Group | Propagating characters |
|---|---|
| InterIndic-Latin | 1 of 37: nukta U+E03C, deleted after a non-letter |
| ko-ko_Latn/BGN | 5 of 23: ᄋ deleted at the start of the text; ᄎ ᄏ ᄐ ᄑ gain `’` there |
| ru_Latn-ru/BGN | 6 of 6 |
| ar-ar_Latn/BGN | 4 of 5 |
| fa-fa_Latn/BGN | 4 of 4 |
| Latn-Arab group 2 | 2 of 2 (string rules only, conservative) |
| blt-blt_FONIPA group 6 | 2 of 2 |
| my-my_FONIPA group 6 | 2 of 2 |
| InterIndic-Arabic | 1 of 3 |
| uz_Latn-uz_Cyrl | 1 of 1 |
| Laoo-Latn/UNGEGN | 1 of 1 |

- Many propagating characters differ only between the text boundary and the
  other states, so their chains start at the start of the text, or after a
  deleted run there.
- Some groups not yet eligible have many: und_FONIPA-ar group 3 has 29 of 33,
  all through string rules (conservative), and Latin-ConjoiningJamo group 1 has
  25 of 26.

### First implementation: rounds (to be replaced)

Commits 799be3d0d (plan: `DivRule::converted`) and b9113b35f (kernels) apply
the converted-context rules in rounds:

- The rules are first matched against the text itself.
- Each further round, up to `--converted-context-rounds` (8), recomputes the
  converted characters from the previous round's matches. It then runs a full
  pass of the If-hierarchy over the converted rules' leaves, in both the Count
  and the translation kernels.
- The output uses the last round's matches.

The 21 CLDR groups and 6 rules files of `QA/xlit_divcode_test.py` match the
regular plan, and the rules files also match ICU, in both modes. 97 of the 212
string-rule groups are eligible, against 76 before. Chains longer than 8
differ from ICU, as in the regular plan.

Measurements (corpora made from the Latin text by Latin-Hangul and
InterIndic-Arabic; execution cycles, CYCLES × EXEC%, 4 threads, relative to the
regular plan):

| Transform (corpus) | Regular | DivCode, Unicode | DivCode, UTF-8 | Group kernels |
|---|---|---|---|---|
| Ethi-Latn/SERA (14.7 MB) | 3.49M | −34% | −36% | 42 → 19 / 24 |
| InterIndic-Latin (23.4 MB) | 5.58M | −22% | +62% | 40 → 19 |
| ko-ko_Latn/BGN (10.9 MB) | 2.01M | −42% | −54% | 42 → 24 |
| Arab-Latn (14.6 MB) | 9.47M | −14% | −47% | 34 → 24 |

Why the rounds are replaced:

- **They are always run, and inexact.** All 8 rounds run in every block, in two
  kernels, and chains longer than 8 are still wrong. The resolution above runs
  only where propagating characters are, and is exact.
- **InterIndic-Latin in UTF-8 mode.** Its DivCode kernel costs 3.4 times as much
  as in Unicode mode (7.8M against 2.3M). Yet only its nukta propagates.
- **Threads.** Wall-clock times with 4 threads do not always follow the cycles:
  InterIndic-Latin takes 0.138 s (Unicode) and 0.287 s (UTF-8) against 0.123 s.
  A single large kernel limits the pipeline.

A separate observation: the `Items` streamset of InterIndic-Latin has 34
streams, spread together. 21 are contexts written in the rules, and 13 are
complements made by order disambiguation. Most of the complements could be
derived from the positive streams after spreading.

### Implementation (2026-11-16)

- **Plan** (`divcode_plan`): `DivCodePlan::convertedItems`, `propagating`,
  `consumerDeleted` and `consumerIn` (the classification above, per character
  of the converted rules; the character itself counts as an outcome unless the
  rules on it have the same after context and before items that hold in every
  state). `rounds` is gone from the plan and its content hash;
  `--divcode-report` gives `propagating` (InterIndic-Latin 1, ko-ko_Latn/BGN 5,
  ru_Latn-ru/BGN 6, Ethi-Latn/SERA 0).
- **Kernels** (`DivCodeTranslate`, modes Count, Translate and Converted):
  - The dependent positions: a converted rule matches, ignoring its before
    context; outside the string matches; no other single-character rule
    applies, nor does a string match begin (no rule yields to a converted
    rule, so where another applies, they do not). Count and Translate compute
    them alike.
  - Count reserves there the largest expansion of the converted rules that
    match.
  - Translate applies the other rules, and outputs `Dep` and `Ends` (for each
    item, the positions just after a converted character in its set and not in
    it).
  - Converted: `K1`/`K0` from `Ends`, the consumers' known outcomes and the
    deleted positions; the `While` loop over the propagating positions (none:
    no loop); one pass of the converted rules at every dependent position,
    XORed into Translate's output, with the reserved positions not filled
    deleted.
- **Pablo fixes found on the way** (`lib/pablo/passes/codemotionpass.cpp`):
  - `hoistLoopInvariants` hoisted out of a `While` the readers of a Var assigned
    only within an If nested in the loop. Every Var assigned in the loop is now
    a variant.
  - `sinkIfAcceptableTarget` moved statements with carries (`Advance`,
    `MatchStar`, ...) into a nested If whose users they were. In blocks where
    the If is skipped their carries were lost; this showed before the first
    block entering the If (`¬Advance(1, 1)`, the start of the text, held at a
    block start). Statements with carries are no longer sunk.
  - In the kernel: two rules with the same match and change gave `Or(v, x)`
    twice in a leaf, which the builder built once, dropping what was assigned
    in between. Each Var is now assigned once per leaf.
- **Tests** (`QA/xlit_divcode_test.py`): results of different lengths at
  dependent positions (`conv-reserve*`), chains up to 3000 long compared with
  ICU (`conv-long-*`), a dependent position at the start of a block before any
  other (`conv-sparse`, which fails without the sinking fix), and every
  converted-context case also with one thread and with four threads and
  segments of one stride. The full ctest suite passes (91 tests).

Measurements (execution cycles, 4 threads; the regular plan's own figures
vary by up to 12% between runs):

| Transform | DivCode, Unicode (rounds) | DivCode, UTF-8 (rounds) | DivCode kernels, Unicode / UTF-8 (rounds) |
|---|---|---|---|
| Ethi-Latn/SERA | −38% (−34%) | −51% (−36%) | 0.73M / 1.22M (1.05M / 1.73M) |
| InterIndic-Latin | −48% (−22%) | −27% (+62%) | 1.27M / 3.77M (2.26M / 7.78M) |
| ko-ko_Latn/BGN | −38% (−42%) | −42% (−54%) | 0.09M / 0.32M (0.07M / 0.21M) |
| Arab-Latn | −24% (−14%) | −50% (−47%) | 0.24M / 0.52M (0.26M / 0.51M) |

InterIndic-Latin in UTF-8 mode, the case the rounds made worse than the regular
plan, is now 27% better. ko-ko_Latn/BGN's DivCode kernels cost more: it has 5
propagating characters, so its Converted kernel has a loop, and the kernel is
an extra pass over its few dependent positions.

### Steps

1. Done: after contexts of one item tested directly (commit 9ea25c9a3); the
   classification, the kernels and the tests above.
2. **Measured, not worthwhile** (2026-11-16, after Count and Translate became
   LocalCarries and the after items of one item were tested once, commit
   2bef051f7):
   - The static and dynamic skipping of the gap terms (see "Kernels: less
     spreading"). An upper bound, from a build without any gap terms (the after
     items of earlier rules past gaps of 1..E positions, `next()`'s steps over
     gaps): no change in Translate (within ±4%) for the four transforms above,
     and only in InterIndic-Latin's Converted kernel in Unicode mode (229K →
     180K cycles, 1.5% of the transform's 3.28M); no change in wall time. Most
     matches use the exact term of their own gap, and with the after items
     tested once, a gap term is a lookahead and an Advance.
   - The cost of the Converted kernel for groups with few dependent positions
     (ko-ko_Latn/BGN): 94K of 1.06M cycles (9%) in UTF-8 mode, 30K of 1.26M
     (2.4%) in Unicode mode, an upper bound on any saving; the only stateful
     DivCode kernel, but too small a share to limit 4 threads.
3. **Phase 2:** converted-context string rules (see §11).

## 11. Plan: phase 2, string rules with before contexts matched against the converted text

### Scope

`xlit --divcode-report` over the 453 CLDR transforms (2026-11-16):

- 36 groups have string rules whose before contexts are matched against the
  converted text ("string rules with before contexts matched against the
  converted text").
- For 9 of them it is the only problem: cs_FONIPA-ko #1 (3 rules), es_FONIPA-zh
  #2 (6), hy_AREVMDA-hy_AREVMDA_FONIPA #1 (2), ja_Latn-ko #2 (60), ja_Latn-ru
  #3 (1), pl-pl_FONIPA #1 (3), pl_FONIPA-ja #3 (2), ro_FONIPA-ja #3 (1),
  sk_FONIPA-ja #4 (1).
- The other 27 have other problems too, mostly two-way or self overlaps and
  contexts that are not fixed sequences of sets. They are out of scope, as are
  the 8 groups with "before contexts of two or more items matched against the
  converted text".

### Families

The rules come in families: order disambiguation splits a rule into variants
with the same text whose before contexts are complementary, on the same items:

- cs_FONIPA-ko: `" " { ks } [^" "$] → j`, `[$] { ks } [^" "$] → j`,
  `[^" "$] { ks } [^" "$] → gs`;
- pl-pl_FONIPA: `$voiceless { rz → ʂ`, `[^$voiceless] { rz } $voiceless → ʂ`,
  `[^$voiceless] { rz } [^$voiceless] → ʐ`;
- ja_Latn-ko: `[^$consonant] { u~a → 와`, `[^[^$consonant$]$] { u~a → ᅪ`.

A family is the rules with the same text (the same sets at each position).
It **covers** its text if, for every after context its rules have, the before
contexts of the rules with that after context (or none) hold for every
converted character before it and at the text boundary.

- **Covering family:** where its text matches (with an after context), some
  rule of the family applies, whatever the converted text is. The match and the
  positions it covers are certain; only which result is written depends on the
  converted character before the anchor. Then:
  - the covers of the string rules, the stage passes and the priorities (rules
    yielding to the family) are as for a rule without a converted context;
  - the single-character rules within its matches never apply there;
  - the results differ only in their characters, their lengths, and the last
    character the next positions see.
  This is phase 1's case, generalized from one position to a match of N units.
- **Non-covering family:** some converted text leaves the text to other rules
  (shorter strings, single characters), so the covers, later stages and
  yielding rules depend on the resolution. ja_Latn-ko's `[^$consonant] { wa →
  와` may be one, if no rule covers `wa` after a consonant. These would move
  whole regions into the Converted kernel. They are out of scope unless the
  analysis (step 1) shows they keep most of the 9 groups out.

### Design (covering families)

The positions of a family's match are written in the spread text: units
`a .. a + N - 1` of the text, then the gap reserved after them.

1. **Planning.** `planDivCodeGroup` groups the converted string rules into
   families and checks that each covers its text, with the same item sets
   (`convertedItems`) as phase 1. A non-covering family keeps the problem
   ("string rules with before contexts matched against the converted text, not
   covering").
2. **Propagation.** `classifyConverted` classifies a family as a unit, as a
   character is now: its outcomes are deletion (an empty result) or the
   membership of its results' last characters in each item set. A family whose
   outcomes all agree is a consumer; one whose outcomes differ propagates.
3. **Count** (before spreading) reserves at the family's match the largest
   expansion among its results, at its last unit (offset N - 1), as `atLeast`
   does for the dependent single-character positions now. A family's
   expansions are known, so this needs no `K1`/`K0`.
4. **Translate** (spread) treats the family as one string rule without its
   before context, for its match (text, after context, cover, stage, the rules
   yielding to it). It writes no changes there: it leaves the text and its
   reserved positions, and marks the anchor in `Dep`. For a consumer family it
   adds the known outcome to `Ends` (In/Out at the last position of the
   reserved span, the positions within it deleted for propagation, so `K1`/`K0`
   after the match are the same for every outcome).
5. **Converted** (spread) evaluates the family's rules at their anchors with
   `K1`/`K0` before the anchor, as for single-character rules: in the loop for
   propagating families, in the final pass for all. A rule's changes cover the
   N units and the reserved positions: unit j of its result at a + j (XOR
   against the text, as Translate does for string rules, including set
   positions), the surplus up to N + reserve deleted. The loop's frontier and
   `Done` are at anchors; the outcome is marked at the end of the span.
6. **Stages.** A family in a later stage is evaluated only at the anchors
   Translate marked (outside the covers of earlier stages). Its own cover of
   later stages is certain, so Translate computes it.

The Converted kernel's leaves today skip the earlier/cover/gap checks. They
need the string rules' offsets and set positions, which the leaf code already
handles for Translate.

### Steps

1. **Analysis.** In `divcode_plan`, form the families of the 36 groups and
   report (`--divcode-report`, field `converted_families`) whether each covers
   its text, and its outcomes. Decide from the counts whether covering
   families alone bring in most of the 9 groups; if not, revisit the
   non-covering case before step 2.
2. **Planning and propagation** for covering families (eligibility, families
   in `DivRule`, `classifyConverted` over families, the content hash).
3. **Count and Translate:** the reserve at a family's match; the family as one
   match for covers, stages and priorities; `Dep` and `Ends` at families.
4. **Converted:** the families' rules in the loop and the final pass, with
   multi-unit changes and the surplus deleted.
5. **Tests** (QA/xlit_divcode_test.py): rules files of families with
   results of different lengths, empty results (deletions seen through),
   chains through families' results, a family in a later stage, set positions
   in the text, UTF-8, sparse dependent positions, one thread and four threads
   with segments of one stride; the 9 groups in `TRANSLATE` (extended). One
   case in the default selection, within the time limits.
6. **Measure** the 9 groups against the regular plan (4 threads, kernel
   cycles), as in §10.

### Status (2026-11-16)

- **Step 1:** covering families alone bring in 4 of the 9 groups:
  cs_FONIPA-ko #1, hy_AREVMDA-hy_AREVMDA_FONIPA #1, pl-pl_FONIPA #1 and
  pl_FONIPA-ja #3. The other 5 have a non-covering family: es_FONIPA-zh #2
  (`ao`, before `[^jw]` only), ja_Latn-ko #2 (`tji~`, `tzu~`, `tz`: before `" "`
  and the text boundary only), ja_Latn-ru #3 (﷠), ro_FONIPA-ja #3 and
  sk_FONIPA-ja #4 (`^ { ia }`). Decision: implement covering families (steps
  2–6), then analyse completing the non-covering ones with a fallback.
- **Steps 2–4** (e2eb574e0): the 4 groups match the regular plan on random
  text and the CLDR test data (div32 and div64, UTF-8 and Unicode). The
  Converted kernel's loop must run when only families propagate
  (`DivCodePlan::propagates`): without it, a family match after another
  (cs_FONIPA-ko `ksks`) was not evaluated.
- **Step 5:** the `conv-family*` and `conv-sparse-family` rules files (results
  propagating, known and deleted; lengths, deletions, chains, a later stage,
  set positions, after contexts, converted single-character rules, UTF-8,
  sparse positions), compared with the regular plan and ICU, one thread and
  four threads with segments of one stride; the 4 groups in `TRANSLATE`.
  `conv-family` is in the default selection (2.8 s cold, 0.94 s warm); with
  the loop condition reverted it fails.
- **Step 6** (measurements below): all 4 are faster, by 9–89% in execution
  cycles, almost all of it phase 2's own.
- **A pipeline hang found on the way:** with the loop missing, an invalid UTF-8
  input made the pipeline spin, even with one thread, on the pre-invocation
  lock of `NFC_Initial_Insertion8x1+wkmsk+CMCompressed` (a state-free kernel
  through LocalCarries; not with `-LocalCarries=0`): its lock for segment 1
  was never released. The wrong output only exposed it; the pipeline bug is
  open.

### Measurements (step 6)

Text in the source script, as in the step 4 measurements: Armenian and Latin (8–14 MB of
Twain), and IPA from `cs-cs_FONIPA` and `pl-pl_FONIPA` of the Latin text (10
MB; the Czech has 12150 `ks`, 232 of them in `ksks`). Execution cycles of the
whole transform (CYCLES × EXEC%) at `-thread-num=4`, each configuration
alone. "Without families" is DivCode with `--divcode-families=0`: the family
groups on the regular plan, the others DivCode, as before phase 2. All
configurations give the same output.

| transform | group kernels, regular / DivCode | DivCode Unicode / UTF-8 vs regular | phase 2 alone (vs without families), Unicode / UTF-8 | 1 thread, s: regular / Unicode / UTF-8 | cold start, s: regular / DivCode |
|---|--:|--:|--:|--:|--:|
| pl_FONIPA-ja | 48 / 42 | −88.6% / −88.6% | −88.6% / −88.6% | 0.742 / 0.137 / 0.137 | 13.6 / 1.05 |
| pl-pl_FONIPA | 39 / 20 (UTF-8 23) | −34.1% / −68.8% | −32.0% / −67.8% | 0.239 / 0.178 / 0.116 | 5.5 / 3.8–4.0 |
| hy_AREVMDA-hy_AREVMDA_FONIPA | 37 / 30 | −32.9% / −32.5% | (the only DivCode group) | 0.263 / 0.196 / 0.197 | 3.35 / 1.5 |
| cs_FONIPA-ko | 31 / 17 | −8.9% / −8.7% | (the only DivCode group) | 1.052 / 0.980 / 0.989 | 7.7 / 7.4–7.6 |

pl_FONIPA-ja's group 3 on the regular plan dominates the transform (its
converted contexts take the regular plan's rounds): DivCode removes almost all
of it, and the cold start drops from 13.6 s to 1 s. pl-pl_FONIPA can run in
UTF-8 mode only once its family group is DivCode. cs_FONIPA-ko's time is
mostly outside the conversion group (the Latin-to-Hangul steps and NFC).

### Fallback analysis: the non-covering families (2026-11-17)

The 5 groups left out have 6 non-covering families. Why each before context
is matched against the converted text (`convertedContextMatters`), and what
applies to the family's text where its before context does not hold:

| group | family | why converted | where the before context fails |
|---|---|---|---|
| ro_FONIPA-ja #3, sk_FONIPA-ja #4 | `^ { ia → ヤ` | string rules with results shorter than their texts: their inner units are modelled as deleted, and any deletion makes every before context converted | `i` → イ (before `a`), `a` → ア |
| es_FONIPA-zh #2 | `[^jw] { ao } [^n]`, `… } n $vowel` → `au̯` | `j` → `i` (before `an`), `jau̯` → `iau̯`, `wa…` → `ua…` | nothing: `a` and `o` have no rules |
| ja_Latn-ko #2 | `" "`/`^ { tji~ \| tzu~ \| tz }` (4 rules each, by after context) | deletions: `' - ^ ̂` always, `t`/`d` before `ji`/`zu`, `w` after a consonant | `t` by its single-character rules with the complementary before context `[^" "$]` (→ ᄐ), then the rules of the other units (single-character rules and covering families: `i~`, `zu`) |
| ja_Latn-ru #3 | `﷠ { [̂̄] ~e → е` | `﷠` is written by the group (`i` before a mark, a mark after `﷠`) | the mark deleted, `~` kept, then `e` by `e [^$lengthMarker] → э` or the string rule `e $lengthMarker → эй`, which starts at the family's last unit and extends past it |

Three fallbacks, in order of cost:

1. **Boundary items and partial deletions** (ro_FONIPA-ja, sk_FONIPA-ja). A
   before context whose item is only the text boundary (`^`, `[$]`) changes
   only where all the converted text before it can be deleted: by a conversion
   whose whole result is empty. The inner units of a string rule with a
   non-empty result never empty the text before. Refining
   `convertedContextMatters` so (for boundary-only items, only whole
   deletions count) makes `^ { ia` an ordinary before context: both groups are
   then eligible without the Converted kernel (no other problems). This also
   changes the regular plan's classification (fewer rounds), which must stay
   exact: the CLDR corpus check and the DivCode tests cover it.
2. **Completion by the rules that apply** (es_FONIPA-zh, ja_Latn-ko). Where a
   family's before contexts miss some converted characters, give it synthetic
   members for the missing regions: for each after context of the family and
   each region (the converted characters, or the boundary, not covered), the
   result of the rules that apply at its units in order. The completion holds
   if, for each such region, the rules applied are determined: the first
   unit's rule by the region (its before context implied by the region), the
   later units' rules by the family's text and its after context (no rule's
   after context reaching beyond them, no string rule starting within the text
   and extending past it), and the results of covering families met within
   the text by the converted characters before them, which the evaluation
   gives. The family then covers its text, and phase 2 applies as it is. For
   es_FONIPA-zh the synthetic member is the identity (`[jw] { ao } … → ao`);
   for ja_Latn-ko, `[^" "$] { tz } [^ju] → ᄐ` followed by `z`'s result, and so
   on. The evaluation is a small interpreter over the plan's own rules
   (`DivRule`: text, contexts, priority), run while planning.
3. **Matches whose extent depends on the converted text** (ja_Latn-ru). Where
   the family fails, a string rule starts at its last unit and extends past
   it (`e $lengthMarker`), so the positions covered depend on the outcome.
   Completion would need the family split by the character after its text,
   with members of different lengths (not a family of one text). Not worth it
   for one group of one rule.

Recommendation: fallback 1 (small; 2 groups), then fallback 2 (3 families of
ja_Latn-ko and 1 of es_FONIPA-zh; 2 groups). That brings in 8 of the 9 groups;
ja_Latn-ru #3 stays on the regular plan.

### Status of the fallbacks (2026-11-17)

- **Fallback 1** (7291400d9): an adjacent before item that is only the text
  boundary is affected only by whole deletions (`ConversionOutcome::partial`).
  ro_FONIPA-ja #3 and sk_FONIPA-ja #4 are eligible and applied. The contexts of
  5 other transforms are no longer matched against the converted text; all 7
  match ICU on their test data and random text.
- **Fallback 2**: `planDivCodeGroup` completes a non-covering family with
  synthetic rules (`DivConvertedFamily::completed`). For each after region it
  takes the converted characters (and boundary) the before contexts miss, and
  evaluates the group's own rules on the family's literal text as ICU applies
  them: the rule matching at each position (text, after context on the text
  and the region's items, before context on what is written so far or on the
  missing characters), less those yielding to another matching there; none,
  the character kept. A test that differs within a region (an after item, or
  the missing characters) splits it (at most 32 cases); a match beyond the
  text, or a context beyond the region's items that does not always hold,
  leaves the family as it is. The synthetic rules' before contexts are
  disjoint from the family's; they yield as its rules do to the rules outside
  it, and take its stage.
  - es_FONIPA-zh #2 (`ao`, 2 synthetic rules: the text kept) is eligible and
    applied; it matches ICU on random text around `ao`, `jao`, `wao`, `aon`.
  - ja_Latn-ko #2 (`tji~` 4, `tzu~` 3, `tz` 4, propagating) is eligible, but
    not applied: it follows prefix runs that delete, after which
    `RulePipelineBuilder::conversionGroup` keeps the regular plan (as for
    ro_FONIPA-ja #2). With the regular plan it matches ICU on its test data.
  - es_FONIPA-zh #1 has 4 families completed (`ei̯`, `eu̯`, `ou̯`, `ui`) but
    other problems (two-way and self overlaps); its `jo` is not completed (a
    match beyond the text).
  - Tests: `conv-family-complete` (the text kept) and
    `conv-family-complete-split` (regions split), and the 3 groups in
    `TRANSLATE`.

Of the 9 groups of §11's scope, 7 are applied by DivCode; ja_Latn-ko #2 is
eligible but after deleting prefix runs, and ja_Latn-ru #3 stays out.

## 12. Plan: groups without string rules whose before contexts are matched against the converted text

### Scope

On the regular plan, a group whose before contexts are matched against the
converted text is implemented with rounds (`ConvertedContextRounds`, 8). DivCode
replaces the rounds by the Converted kernel (§10: its While loop for the
propagating characters), but only for groups with string rules: the planner
raises "no string rules", and `RulePipelineBuilder::conversionGroup` tries
DivCode only when `step.stringRules` is not empty.

24 groups have no string rules and before contexts matched against the
converted text (survey of 2026-11-17, `--plan` and a temporary
`--divcode-report` of all groups). By what keeps them from DivCode:

| | groups | contexts |
|---|---|---|
| A. only "no string rules" (10) | hy-hy_Latn/BGN (5 converted rules, 2 propagating), Han-Latin/Names (2, 1), Kana-Hira (5, 1), InterIndic-Gurmukhi (3, 0), ru-ru_Latn/BGN (13, 8), es-es_FONIPA (3, 0), Ethi-Latn/{Aethiopi, Beta_Metsehaf, IES_JES_1964, Lambdin} (14, 0 each) | fixed sequences of sets |
| B. a repeated set in a before context (5) | az-Title, tr-Title (`[:cased:] [:caseignorable:]* {`), el-Upper (`[:greek:] [marks]* {`), lt-Upper (`[:softdotted:] [marks]* {`), Spacedhan-Han (`[:letter:] [:mark:]* {`) | before: `S T*` |
| C. B and an after context that is not a fixed sequence (4) | el-Title (`} [:caseignorable:]* [:cased:]`), lt-Title (`} [^…]* [:ccc=Above:]`), pl_FONIPA-ja #2 (`t ͡* {`, `} (?!(ʂa \| … \| ʂ))`), blt-blt_FONIPA (`$C { } $V123`, a set with strings) | after: repetitions, negative lookahead |
| D. before contexts of two items (4) | es-ar, es-fa, es_419-ar, es_419-fa (`$Boundary [^Vowel] { } [^$Vowel]* $Vowel`) | before: 2 items; after: `T* S` |
| E. out of scope (1) | ConjoiningJamo-Latin (`[^$latinMedialEnd] k {`, insertion rules, 170 converted contexts) | |

### Steps

1. **Groups without string rules (A, 10 groups).** Drop the planner's "no
   string rules" problem and the `stringRules` condition in
   `conversionGroup`, for groups with converted rules (`plan.dependent()`):
   a group of single-character rules without them stays on its present path.
   Report such groups in `--divcode-report`. Their single-character rules
   are phase 1's: Translate leaves the dependent positions, Converted
   resolves them, with the While loop for the 4 groups with propagating
   characters (ru-ru_Latn/BGN has 8). Check what the empty set of string
   rules leaves in the kernels (stages, covers, the Count kernel without
   expansions of string rules), the groups after deleting prefix runs (not
   applied, as ja_Latn-ko #2), and the transforms whose mode changes
   (`utf8DivCodePlan`: a group now eligible in UTF-8 mode may move the
   transform to UTF-8 mode). Verify: the 10 transforms against ICU on their
   test data and on random text of their converted characters; measure
   against the regular plan (rounds) as in §11 step 6. If DivCode does not
   gain on these, stop here.
2. **A repeated set in a before context (B, and the before contexts of C).**
   A before item `S T*` (`FusedItem::star`, from `ContextExpansion`) holds
   where the last converted character not in T is in S. In the Converted
   kernel this is the look-through that deletions already have: `K1`/`K0`
   (the items known in and out) are carried by `MatchStar` through the
   deleted positions and, for such an item, through the positions whose
   converted character is in T. Exact if no conversion of the group changes
   whether a character is in T (as `convertedContextMatters` checks for the
   sets of the items): then the positions in T are those of the text's
   characters in T (the case-ignorable characters and the marks are not
   converted by the Title and Upper transforms; to be checked per group).
   Otherwise the item's T is an item of the outcomes (`convertedItems`),
   known at the dependent positions only after their evaluation (the loop).
   In Translate (contexts matched against the text itself), `S T*` is a
   `MatchStar` of S's positions through T's. Brings in the 5 groups of B.
3. **After contexts that are not fixed sequences (C, and the after contexts
   of D).** ICU matches after contexts against the text as it is, so any
   after context can be computed beforehand by the regular expression
   engine, as the regular plan computes them (`matchPositions`): a stream
   marking the positions where it holds after the anchor, an external of
   the DivCode kernels (`DivContextItem::Kind::Stream`, tested at the
   anchor). Covers repetitions, sets with strings and negative lookaheads.
   One engine kernel per distinct context. Brings in the 4 groups of C
   (with step 2).
4. **Before contexts of two items (D).** The item at distance 2 sees the
   converted character before the one the item at distance 1 sees: the
   outcomes of the converted characters are then pairs (the last two
   characters written, through deletions), so `K1`/`K0` per item and per
   distance, the distance-2 ones carried from the end of the character
   before. Only for the 4 es-* groups (variants of one rule); do after
   measuring 1–3, if they gain.

ConjoiningJamo-Latin (E) has insertion rules and 170 converted contexts, of
two items; it stays on the regular plan.

Tests for each step: rules files of the shapes (QA/xlit_divcode_test.py,
one default case within the time limits), and the groups brought in, in
`TRANSLATE` (extended). Checks for each step: only the transforms whose
groups the step brings in, against ICU on their test data and random text
of their characters.

### Status (2026-11-17)

**Step 1 done.** `planDivCodeGroup` raises "no string rules" only for a group
without converted rules (`DivCodePlan::dependent`); `conversionGroup` tries
DivCode for every group; `--divcode-report` lists the groups without string
rules that have converted rules. The 10 groups of A are eligible and applied;
the 10 transforms match ICU (the regular plan, DivCode in Unicode and in the
default mode) on their test data (5 of them) and on random text of their
characters. Test: `conv-no-strings` (a chain, a deletion looked through), and
the 10 groups in `TRANSLATE`.

Execution cycles of the whole transform at 4 threads against the regular plan
(rounds), as in §11 step 6; outputs identical:

| transform | corpus | DivCode Unicode / UTF-8 | 1 thread, s: regular / Unicode / UTF-8 |
|---|---|--:|--:|
| InterIndic-Gurmukhi | InterIndic | −21.8% / −53.2% | 0.147 / 0.126 / 0.093 |
| Ethi-Latn/Beta_Metsehaf | Ethiopic | −31.9% / −31.9% | 0.260 / 0.222 / 0.223 |
| Ethi-Latn/Lambdin | Ethiopic | −30.5% / −30.6% | 0.258 / 0.220 / 0.220 |
| Ethi-Latn/IES_JES_1964 | Ethiopic | −29.4% / −29.5% | 0.261 / 0.226 / 0.227 |
| Ethi-Latn/Aethiopi | Ethiopic | −27.1% / −29.2% | 0.264 / 0.228 / 0.227 |
| Kana-Hira | Katakana | −6.8% / −24.2% | 0.402 / 0.380 / 0.332 |
| es-es_FONIPA | Latin | −7.3% / −7.0% | 0.295 / 0.283 / 0.285 |
| ru-ru_Latn/BGN | Cyrillic | −5.1% / −5.2% | 0.301 / 0.299 / 0.302 |
| hy-hy_Latn/BGN (with its group 3, also DivCode) | Armenian | −15.0% / −14.8% | 0.377 / 0.341 / 0.341 |

(Han-Latin/Names: no corpus.) All gain; step 2 can proceed.

The rounds are replaced by the Converted kernel; its While loop is built only
where outcomes propagate (`DivCodePlan::propagates`): ru-ru_Latn/BGN,
hy-hy_Latn/BGN, Kana-Hira and Han-Latin/Names. In the other 6 groups every
outcome is known, and one pass resolves them.

**Step 2 done.** A before item `S T*` of a context matched against the
converted text has `DivContextItem::through` (T), with T's set tested as an
explicit or external set. The planner accepts it when no conversion of the
group moves a character into or out of T: a text of T's characters gives T's
characters or nothing; another text, nothing or some character not in T
(the item then tests the result's last character not in T, `lastSeen`). The
Converted kernel passes over T's characters (with the positions their
results fill) as over the deleted positions, and ignores the ends of
characters just after them. Not yet in UTF-8 mode (the group is then in
Unicode mode).

- el-Upper and lt-Upper are eligible and applied (the While loop: 9 and 1
  propagating characters). They match ICU on 20,000 characters of random
  Greek and Latin text with accents (also with four threads and segments of
  one stride); no CLDR test data. On 6.5 MB of such text: −2.7% and −3.3% in
  execution cycles (4 threads); the groups are small within their transforms
  (NFC, Any-Upper).
- az-Title and tr-Title are not: U+0345 COMBINING GREEK YPOGEGRAMMENI is
  case-ignorable (in T) and cased; the rule that lowercases after a cased
  character keeps it, the one that titlecases at a word start makes it Ι. So
  whether it is in T in the converted text depends on the converted context
  itself, and T's positions are not those of the text.
- Spacedhan-Han needs two items (step 4).
- Test: `conv-through` (a deletion within the run, a multibyte character of
  T), and the 2 groups in `TRANSLATE`.

**Step 3 done.** A subgroup's after context that is not a fixed sequence of
sets (repetitions, sets with strings, lookaheads) is computed by the regular
expression engine: `matchPositions(nullptr, after, {all}, U21)` marks the
anchors after which it matches, appended to the DivCode kernels' externals
(`DivContextItem::Kind::Stream`, `DivCodePlan::afterStreams`), and tested at
the anchor. Single-character rules only (a string rule's still a problem);
not yet in UTF-8 mode. `--divcode-report` gives `after_streams`.

Of the 24 groups with such after contexts, 5 become eligible and are
applied; all match ICU on their test data (4 of them) and random text of
their characters, in every configuration:

- pl_FONIPA-ja #2 and blt-blt_FONIPA #5 (the targets of C);
- ru-ru_Latn/BGN #2, und_FONIPA-chr #1 and vec-vec_FONIPA #2: groups with
  string rules whose only problem was such a context.

el-Title and lt-Title stay out (U+0345, step 2); the others have other
problems. Test: `conv-after-engine` (a repetition, a set with a string, both
at the end of the text), and pl_FONIPA-ja #2 and blt-blt_FONIPA #5 in
`TRANSLATE`, with the 3 others.

**Step 4 not done: measured not worthwhile** (2026-11-17). Its 5 groups:

- es-ar, es-fa, es_419-ar, es_419-fa: one rule each,
  `$Boundary [^Velow] { e } [^$Vowel]* $Vowel → ə`. Its conversion of `e`
  changes what `[^Velow]` sees, so the context depends on the converted text,
  but a chain needs punctuation between the characters (` ,e,e`). The
  transforms' work is in `es-es_FONIPA`, which they invoke.
- Spacedhan-Han: one subgroup with `[:letter:] [:mark:]* [mark set] {` (S T*
  U), which would need the two-item outcomes and step 2's passing over
  together.

Two items would make each converted character's outcome a pair (the last two
characters written, through deletions): `K1`/`K0` per item and distance, in
the planner's signatures, Translate's ends, and the Converted kernel and its
loop. The most complex step, for 5 groups of one rule each, in transforms
whose time is elsewhere. They stay on the regular plan.

**Outcome of §12.** 14 of the 24 groups are applied by DivCode instead of
rounds (step 1: 10, step 2: 2, step 3: 2). Step 3 also brought in 3 groups
with string rules outside the scope: ru-ru_Latn/BGN #2 (rounds before),
und_FONIPA-chr #1 and vec-vec_FONIPA #2 (no converted contexts). The other 10
stay on the regular plan: az-Title, tr-Title, el-Title and lt-Title (U+0345,
step 2), the 5 groups of step 4, and ConjoiningJamo-Latin.

## 13. Plan: the DivCode technique in every conversion-group kernel

Status: plan, 2026-11-25. Nothing below is implemented.

### 13.1 Two layers

DivCode as built in §§1–12 has two layers, and only the second has the
eligibility limits that `--divcode-report` lists:

1. **The class layer.** The Div32 (Div64) code of the characters the rules
   mention, the k + s-bit pseudo-basis over which every set is an ordinary CC,
   the external streams for property and very large sets, and an If-hierarchy
   keyed on the code bits, so that the logic for characters absent from a block
   is not executed. Nothing in this layer depends on how the rules are selected.
2. **DivCode's translation logic.** The Count, Translate and Converted kernels:
   one-way overlap stages (§9), converted before contexts resolved by the
   Converted kernel (§§10–12), covering families and their completion (§11).
   The "problems" of `--divcode-report` (two-way and self overlaps, non-covering
   families, a converted rule beginning within another string rule's text) are
   limits of this layer: cases its selection logic was not extended to.

Everything the regular plan does in Pablo kernels (the fused kernels, the
correction kernel and its While loop, the subgroups, the yields, the classes
they read) works on character classes and on the decisions of rules, so the
class layer and the If-hierarchy apply to all of it. This section plans that:
the class layer and rule-level Ifs everywhere, the While loop reduced to what
propagates, and one implementation of the selection semantics instead of two.

### 13.2 The driver: und_FONIPA-ar group 3

`und_FONIPA-ar` (UTF-8 mode) on `tools/ldml/xlit-coverage/perf/corpora/pool-und_FONIPA.txt`
(20 MB, 14.0M characters). Group 3 is the main pass, the rules between the two
`::NULL;` (line 44 to 140 of the file).

**Cost.** `-EnableCycleCounter`, default threads:

- The group's correction kernel, `FusedRules_3092fc37…` (kernel #289), has 70.1%
  of all cycles and 94% of all execution cycles (EXEC). 53% of its own cycles are
  SYNC: the other threads wait to enter it.
- Two `RunMerge` kernels and a `streamExpand` (21.5%, 2.0% and 4.2% of all
  cycles) are almost all SYNC and PIPE, held back by #289.
- By category: FusedRules 70.6% of cycles (95.5% of EXEC); focus filter/merge
  23.6% (0.4% of EXEC); all compress/expand kernels 5.0% (2.0% of EXEC); all
  character-class kernels (`cc_`, `UCD:`) 0.3%; normalization logic 0.2%.

**Iterations** (`-pablo-count-while-iterations=FusedRules_3092`, committed in
0d8edbdff; provisional strides counted once): 108,664 blocks, 255,385
iterations, 2.35 per block; 70,607 blocks take 2 iterations and 38,057 take 3,
none fewer or more. The loop is not deep: the cost is its body.

**Size** (`-ShowOptimizedPablo`): 55,111 Pablo statements, no If anywhere.

| part | statements | main operations |
|---|--:|---|
| before the loop | 7,359 | And 1,486, Or 1,869, Advance 772, Not 288, Lookahead 49 |
| the While body | 43,655 | And 19,568, Or 12,206, Not 7,887, Advance 1,576, MatchStar 8 |
| after the loop | 4,097 | Or 2,933, Advance 820, And 315 |

Its inputs are 7 class streams (`Classes`), `InText` and the matches of the 57
static string rules (`Source0`); its output is the 21 change bits. So the
classes are already compact and cheap (the class kernels are 0.3% of cycles):
about 2.35 × 43,655 + 11,456 ≈ 114,000 statements are executed per
128-position block, almost all of them rule decisions.

**Rules.** 440 string rules (383 with before contexts matched against the
converted text, 57 static) and 100 single-character subgroups (79 converted),
from about 70 source rules. The converted string rules form 149 families
(§11): 54 covering (125 rules) and 95 not covering (258 rules). 3 families
propagate (`ʔɵ̞`, `ʔɵ`, `ɵ̞`). String overlaps: 960 pairs, 939 one-way, 15
two-way, 6 self, all 21 two-way and self pairs from the one source rule
`$AVowel ː? ʔ $AVowel ː? → اءا`, to which order disambiguation adds a
not-at-a-boundary before context: its text begins and ends with `$AVowel`, so
`aʔaʔa` chains.

**The before contexts.** All converted before contexts are one item, one of:
`$Boundary` (`[^[:L:][:M:][:N:]]`) or the text start; not `$Boundary`; or not
`$Boundary` less some vowels (added by order disambiguation). Every non-empty
result of the group is Arabic letters and marks (general categories Lo and Mn
only), none of them in `$Boundary` or in the vowel sets, so a converted
character before a position settles every item the same way (a character no
rule converts keeps its own class); what changes the contexts is deletion (`ʔ → ;`,
`$SchwaVowel → ;`, and the inner units of shortening string rules), through
which the item before is carried.

**The non-covering families** come from two source shapes (the
`--divcode-report` reasons: the before contexts miss characters or the text
boundary, 81 families; an after context of several items whose before
contexts miss characters, 14; 58 of the 95 also "not completed: a match beyond
the text"):

- word-initial rules, `$Boundary {ʔ? V ː} → …`, `$Boundary {ʔ? V} → …` (V each
  vowel class). Where the boundary fails, the text goes to the single-character
  rule of its first unit (`ʔ → ;`) and the rules starting at the next unit (the
  generic `V ː? → …`, or `{V ː? ʔ} …`);
- vowel-and-glottal-stop rules, `{V ʔ} $Boundary`, `{V ː ʔ} $Boundary`,
  `{V ː ʔ} [$Vowel]`, `V ː? ʔ V ː?`, given not-`$Boundary` before contexts by
  disambiguation (the word-initial rules take the boundary case). Where those
  fail, the word-initial rules apply, then the rest.

Completion (§11 fallback 2) fails for 58 of them because a match extends
beyond the family's text.

**Characters and texts in the corpus** (per 128-character block of the corpus
as given; group 3 sees it after groups 1 and 2 and the focus filter, so these are
approximations of its blocks):

- The first characters of the rules are common: `a` in 100% of blocks, `ː`
  99%, `ɪ` 94%, `i` 93%, `ɛ` 93%, `o` 91%, `ə` 90%, `ɑ` 83%, `ʔ` 35%. The Div32
  blocks of the vowels (0x60, 0x240–0x2DF) are in 100% of blocks. An
  If-hierarchy on the first character's code alone skips almost nothing here.
- The rule texts are rare: 142 of the 149 families never match in the corpus,
  and on average 3.1 families (9.0 of the 383 converted string rules) have any
  match in a block. The loop decides all 383 in every iteration.

**Across the CLDR transforms** (`--divcode-report` on all 453, 2026-11-25): 245
groups with string rules in 168 transforms; 122 are eligible for DivCode, 123
are not. The problems of those 123 (a group may have several): two-way or self
overlaps 61; a converted rule that may begin within another string rule's text
27; string rules keeping characters of their texts 25; contexts that are not
fixed sequences of sets 24; runs 23; captures 22; string rules with converted
before contexts 18; self-overlapping string rules 17; conversions changing the
repeated set of a before context 10; before contexts of two or more converted
items 9; context sets with strings 4; others 8. These are the groups the class
layer reaches only through the regular plan's kernels (13.4).

### 13.3 What follows

1. **Rule-level Ifs.** The If-hierarchy of the class layer, extended one level:
   by the code of the first character (as DivCode's leaves), then by the
   static matches of the rule's text (the candidates, computed once before the
   loop). Inside the loop, a rule's decision is `candidates ∧ …`, so an If on
   its candidates (or on the union of a family's) satisfies the If invariant:
   every statement depends on the condition, and the Vars only gain bits that
   are within it. For und_FONIPA-ar, about 97% of the converted string rules'
   loop logic would be skipped per block. The subgroups' first characters are
   common, so their logic is not skipped; how much of the 43,655 statements
   they and the cover tests (`mayBeginWithin`, pairwise over 960 overlaps)
   take is step 0's question.
2. **The loop holds what propagates.** A rule or subgroup whose outcomes all
   settle the items the same way (a consumer, §10) is decided after the loop,
   once, from the final known state. Here every non-empty result settles every
   item the same way, so the loop needs only the deletions and the rules that
   decide them (the 3 propagating families, `ʔ → ;`, `$SchwaVowel → ;`, and the
   string rules that cover a `ʔ` or a schwa).
3. **Separate kernels.** The decisions after the loop read only the final state
   (K1/K0 per item, deletions) and the static matches: they become a kernel of
   their own, with no loop carries, which can run on concurrent segments
   (LocalCarries where its CarryExtent allows) while the loop kernel, which
   cannot, stays small. The loop kernel outputs its state as streams.
4. **The class layer is already cheap here.** The gain is in skipping and moving
   rule logic, not in computing classes. Elsewhere (many classes, scripts in
   separate Div32 blocks, as the §12 measurements show) the class layer itself
   gains; both apply.

### 13.4 Design

**One selection implementation.** Today the regular plan (FusedRules, the
correction kernel's loop, `stringPriority`, yields, coverage, sequential
selection, static classes) and DivCode's Translate and Converted kernels each
implement ICU's matching. The class layer and the rule-level Ifs are added to
the regular plan's kernels; DivCode's kernels are then the regular plan's with
a pseudo-basis and an If-hierarchy. The two paths converge, and the
eligibility problems disappear as each case is covered by the one
implementation. `--divcode-groups` becomes the switch of the class layer.

**The class layer as the input of every fused kernel.**

- One code kernel per group (or per partial group): the Div32Code bixnum, named
  by a hash of its classes and code assignment (as `Div32Code_h…` now). Groups
  with the same classes share it.
- Sets that are properties (`[:L:]`, `$Boundary`) or span more than a limit of
  blocks stay external streams, as DivCode's external sets now.
- A criterion, structural, for using the layer: the number of explicit classes,
  their blocks, the code bits and the external sets (not a single-config
  timing; `doc` §6 measurements are reported, not used as gates). A group of
  one or two small classes reads its classes as now.
- Code numbering: classes that occur together (one script; the first
  characters of the same rules) share high code bits, so subtrees are skipped
  together.

**The If-hierarchy in the fused kernels.**

- Level 1, the first character's code (DivCode's leaves). Level 2, a rule or
  family's static candidates. A rule whose first set spans leaves goes in each
  (restricted) or at their common ancestor; a negated or property first set at
  the top.
- Context streams that do not depend on a leaf's condition (an Advance of a
  property class) are computed outside the hierarchy and ANDed in.
- In the loop: the same hierarchy, with the decisions accumulated into Vars
  declared outside it. The order of the sequential selection is kept: an If
  does not reorder decisions, it skips those without candidates.
- The cover tests (`within`/`notWithin` over the rules that may begin within a
  rule) go inside the If of the covered rule's candidates.

**The loop's contents** (extending §10's `propagates` to the regular plan):

- Classify each rule and subgroup outcome (as `classifyConverted` does) by how
  it settles each resolved item; a rule all of whose outcomes agree for every
  item is a consumer.
- The loop holds the decisions of propagating rules and those they depend on
  (a rule whose candidates may cover a propagating one's). Everything else is
  decided after the loop from the final K1/K0 and the deletions.
- A family whose fallback is "the first unit's single-character rule, then the
  rules from the next unit" (the two shapes of 13.2) needs no completion: its
  decision at the anchor, given K, is either the family's rule or that
  fallback, both static.

**Self-overlap chains** (`V ː? ʔ V ː?`). The loop takes an iteration per
candidate of a run of a rule overlapping itself. A chain of candidates each
beginning within the previous one is resolved leftmost-first by alternation
along the chain: the first applies, the next does not, and so on, which a
carry-propagating form computes without iterating where the chain's links have
fixed structure. Measured first: whether any block's third iteration comes
from such chains.

**UTF-8 mode.** und_FONIPA-ar runs in UTF-8 mode, as do most mixed-mode
transforms. The class layer works at code units as in DivCode phase 2 (§6,
"UTF-8 mode"): the code at the first byte, the low bits at the last; a set
position whose characters have different UTF-8 lengths expands the rule by
lengths or keeps the set external. The hybrid groups (selectors applied on
bytes, `FusedRulesConfig::absolute`) keep their output form; only how classes
and decisions are computed changes.

**Kernel names.** Every new or changed kernel is named by a hash of its
content (classes, code assignment, rule plan, If structure), so each
configuration has its own object-cache entries.

### 13.5 Steps

0. **Attribution.** An option reporting, for each fused kernel, the Pablo
   statements by part (before, loop, after) and by rule, family, subgroup and
   cover test, from the generator. Run on und_FONIPA-ar #3 and the survey's
   largest correction kernels. For attribution: what each later step removes,
   and how much of the loop is subgroup logic and pairwise cover tests, which
   neither step 1 nor step 2 addresses.

Steps 1 and 2 are independent: a rule's If is emitted the same way whether its
decision is in the loop or after it, so Ifs added first move with the code in
step 2, and the end state is the same in either order. Each step's measured gain
depends on whether the other is in (step 1 saves less in the loop once the
consumers have left it; step 2 saves less once their logic is skipped by Ifs),
which affects the reporting only. Step 1 comes first for convenience: it is a
local change to how a decision is emitted, while step 2 restructures the loop,
its state and the kernel boundary, and then starts from a smaller kernel.

1. **Rule-level Ifs in the correction kernel** (level 2 only, on the static
   candidates), inside and outside the loop. Check: identical output and ICU
   agreement on the perf corpora and the loop tests; the iteration counts
   unchanged; statement counts and cycles reported.
2. **Consumers out of the loop**, decided after it in the same kernel; then
   (step 3) in a kernel of their own.
3. **The post-loop kernel**, separate, with the loop kernel's state as streams;
   LocalCarries where its CarryExtent allows. Measured at 4 threads (SYNC and
   PIPE of the loop kernel and its neighbours).
4. **The class layer in the regular plan's fused kernels** (code kernel,
   pseudo-basis CCs, level-1 Ifs), by the criterion; Unicode mode first, then
   UTF-8 mode.
5. **Convergence.** DivCode's Translate and Converted kernels replaced by the
   regular plan's kernels with the class layer, group by group, each checked
   against the other path (outputs identical) before the old one is removed.
6. **Self-overlap chains without iterations**, if step 0 or the iteration counts
   show they matter.

Checks at every step: the loop tests (`QA/xlit_converted_*while_test.py`,
`xlit_sequential_while_test.py`, `xlit_static_loop_test.py`,
`xlit_static_apply_test.py`), the DivCode tests, all xlit and ldml tests, ICU
agreement on the perf corpora of the transforms changed, and every timing
comparison with its outputs compared. Compile time and kernel counts are
reported against the test time limits. Multithreaded timings at 4 threads.

### 13.6 Status of step 0 (2026-11-25)

`xlit --fused-statement-report` reports, for each FusedRules kernel generated
(compiled, not loaded from the object cache: use it with
`-enable-object-cache=0` or a salt), its Pablo statements before Pablo's
optimization, by part (before the While, in it, after it) and by what emits
them: each string rule's matches, cover setup, before contexts, cover tests,
decisions and cover updates; each subgroup; the item values and the
deletions of the state update; the static selection; the results. It also
gives the number of resolved items, item values (item, known in or out,
distance) and string covers (a rule's text offsets) of a correction kernel, and
the number of distinct values the covers have for each item value.

**und_FONIPA-ar #3** (`FusedRules_3092…`): 64,390 statements before
optimization (55,111 after); before the loop 8,497, in it 51,820, after it
4,073. 4 resolved items, 8 item values, 1,260 string covers, 17 subgroup
classes.

| part / emitted by | statements | |
|---|--:|---|
| loop / state update: item values | 29,717 | 57% of the loop: 8 item values × 1,260 covers |
| loop / string cover updates | 8,644 | 440 rules, at most 57 each |
| loop / string decisions | 5,382 | 440 rules, at most 13 each |
| loop / string cover tests | 5,181 | 66 rules; rule 0 (`ʔe̞ː`) alone 1,155 |
| loop / deletions (values and outcomes) | 1,296 | |
| loop / string before contexts | 766 | 383 rules, at most 2 each |
| loop / subgroup decisions | 763 | 17 classes |
| loop / deletions and K | 71 | |
| before / cover setup | 5,708 | 440 rules |
| before / string matches | 1,452 | |
| before / subgroup matches | 584 | |
| before / base results, static selection | 733 | |
| after / results, final decisions | 4,073 | |

What this shows:

- **The item values dominate.** `withStrings` computes, for each item value, an
  OR and an AND over all 1,260 covers (`covT[k] ∧ v`, `covF[k] ∨ v`), every
  iteration. The value `v` of a cover for an item comes from its rule's result,
  and the covers have only 2 distinct values for each of the 8 item values and
  for the deletions (the report gives them: "distinct values of the 1260 covers,
  by item value: 2 2 2 2 2 2 2 2 2"), as 13.2 leads one to expect. Two
  remedies, independent of each other:
  - **Shared cover unions.** Group the covers by their value for an item; OR
    (AND) each group's `covT` (`covF`) once, and share the unions among the
    items whose groupings agree. Here each item value needs 2 unions, shared
    where the groupings agree, instead of 1,260 terms.
  - **Rule-level Ifs (step 1)** apply only if the item values are accumulated
    per rule, under the rule's If (`any_x |= covT ∧ v` inside the If of the
    rule's candidates), not per item over all covers.
- **Cover updates and decisions** (27% of the loop) are per rule and small for
  each: step 1's Ifs cover them directly.
- **Cover tests** are pairwise: a rule's test ORs over every cover of every rule
  that may begin within it. They are concentrated (66 rules; rule 0 has 1,155
  statements). Under step 1 they go in the covered rule's If; grouping the
  covering rules (as for the item values) shortens them as well.
- **Subgroups are small** (1,347 statements in all): the concern of 13.3 that
  their common first characters keep them from being skipped matters little.

So step 1 is preceded by a step **1a, shared cover unions**: a change to
`withStrings` (and the cover tests), exact, with no If; it shrinks the loop by
up to about half by itself, and makes step 1's per-rule Ifs smaller.

### 13.7 Status of step 1a (2026-11-25)

`withStrings` (the item values and the deletions of the correction kernel's
state update) groups the covers by their value, in the order of first
appearance, and builds each group's union of `covT` and intersection of `covF`
once per iteration, shared by every call with the same group; a group whose
value is 0 needs no union, one whose value is 1 no intersection. Exact: the
same terms, reassociated.

und_FONIPA-ar #3, statements before optimization (`--fused-statement-report`):

| | before 1a | after 1a |
|---|--:|--:|
| item values | 29,717 | 2,348 |
| deletions (values and outcomes) | 1,296 | 1,955 (the shared unions are built here, first) |
| the loop | 51,820 | 25,110 |
| the kernel | 64,390 | 37,680 |

The output on `pool-und_FONIPA.txt` is identical; the iterations are unchanged
(108,664 blocks, 255,385 iterations: 2 in 70,607 blocks, 3 in 38,057).

Found on the way: the pipeline's signature (its cache key) took a kernel's
signature, where it has one, without its name, so annotations of the name (a
Pablo kernel's carry mode, `-LocalCarries`, `+WhileCounts`) did not reach it:
a pipeline cached by a run without `-pablo-count-while-iterations` was reused
by a run with it, and called kernels by names that run did not build ("JIT
session error: Symbols not found"). `PipelineBuilder` now adds the name before
the signature.

Remaining loop statements after 1a: string cover updates 8,644, string
decisions 5,382, string cover tests 5,181, item values and deletions 4,303,
subgroup decisions 763, before contexts 766. The per-rule parts (19,207) are
step 1's.

### 13.8 Status of step 1 (2026-11-25, in progress)

In the correction kernel's While loop, each string rule decided there is
decided in an If on its candidates (`--fused-rule-ifs`, default on; in the
kernel's signature when off). In the If: the rule's before context (known in,
known out), its cover tests, its decision, and the updates of its covers that
are not runs. Where the If is skipped (no candidate in the block, no pending
carry), nothing it assigns would change: applies and rejected are within the
candidates, the decisions DT and DF are, and the cover updates are Advances of
them. The covers of runs are updated after the If, from the Vars (they read
static streams, `passStatic`).

- The covers' values cross the Ifs as Vars: a cover's T (DT at offset 0) and
  CF (`notCand ∨ F`). A cover test reads the Var: as decided in this iteration
  for a rule before it, in the previous one otherwise, as before.
- Without the If (`--fused-rule-ifs=0`), the covers are given as the
  expressions just built, as before step 1: reading the Vars there let the
  builder's CSE reuse a cover test's chain built before a cover's assignment
  for a rule after it (a stale value). In the
  Ifs, each chain is in its own rule's If, so none is reused.
- The before contexts are computed once per context and iteration, outside
  the Ifs (they read only K, which the decisions do not change).
- The decision progress is a Var, `Decided`.

und_FONIPA-ar #3, statements before optimization:

| | step 1a | step 1, `--fused-rule-ifs=0` | step 1 |
|---|--:|--:|--:|
| the loop | 25,110 | 24,744 | 62,701 |
| of which the string rules | 19,207 | 19,606 | 58,829 (in 440 Ifs) |
| the kernel | 37,680 | 38,575 | 76,532 |

The static count grows: without the Ifs, the builder shared parts of the
cover-test chains among rules (the same covers, before any of them was
decided); in separate Ifs each rule builds its own. Per iteration, what runs
is the 3,872 statements outside the Ifs, 440 If tests, and the bodies of the
rules with candidates in the block (9 of 383 converted rules on average,
13.2), about 134 statements each: roughly 5,500 statements instead of about
24,700. The output on `pool-und_FONIPA.txt` is identical with and without the
Ifs; the iterations are unchanged.

Open:
- The static size (compile time) of the rule bodies: the cover tests could
  share chains among the rules that test the same covers, decided in the same
  order, or test them by groups (as step 1a groups the covers).
- Timing at 4 threads, with the outputs compared.

### 13.9 Status of step 2a (2026-11-25): which loop rules could be decided after it

`--fused-statement-report` now also reports, for a correction kernel, which
string rules decided in its loop could be decided after it, a static analysis
(no change to the kernels). A rule qualifies if:

- **(A)** at every offset of its text, for every resolved item, the value of
  the position (the state the next position's item sees) is the same constant
  whether or not it applies: its own result there; every alternative, namely
  the outcomes of the subgroups' mappings of the characters there (and the
  character unchanged, unless a subgroup without contexts maps it),
  and every string rule's offset that may hold the position. A deleted position
  takes the value of the position before it, which is unknown before the match.
  With absolute results, the texts of the selectors.
- **(B)** no rule that stays in the loop may begin within it (repeated until no
  rule changes).

Conservative: kept characters, run items, items at distance 2 or more,
insertion rules and sets of more than 512 characters fail.

und_FONIPA-ar #3: of the 440 string rules decided in the loop, 347 satisfy
(A). The other 93: 92 because a character at their first position may be
deleted (`ʔ → ;`, `$SchwaVowel → ;`), so that the value after it is that of the
text before the match; 1 because its own first position is deleted or kept.
This is 13.2's finding: only deletions carry an item forward. Of the 347, 142
also satisfy (B): the 93 rules that stay (mostly `ʔ`-initial) may begin within
the other 205 (the vowel-and-`ʔ` rules), which their cover tests read.

So moving rules out of the loop (step 2b) would take 142 of the 440. With
step 1's Ifs, a rule in the loop already costs only in the blocks where it has
candidates; moving it saves its body in the iterations after the first. To
take the 205, (B) would have to be relaxed: the loop rules' cover tests need a
consumer's decision only where one of their candidates lies within one of its
candidates, so a consumer could be decided in the loop only there.

### 13.10 Status of step 2b (2026-11-25): the consumers decided after the loop

The rules that satisfy 13.9's (A) and (B), the consumers, are decided after
the correction kernel's While loop (`--fused-loop-consumers`, default on; in
the kernel's signature when off). The analysis of 13.9 now runs for every
correction kernel, and is more conservative than in 2a: it checks the
mappings of every subgroup, not only those up to the first without contexts
(no assumption about the order of the subgroups), takes the character
unchanged as an alternative unless a subgroup with no contexts, no match
stream and no yields maps it, and fails all rules of a kernel with case
mappings. und_FONIPA-ar #3 still has 142 consumers.

- **In the loop**, the consumers are not decided, and their covers are left
  out of the state update (`withStrings`): where a consumer applies, the
  outcome it replaces has the same value (A), so the state is the same; no
  decision in the loop reads their covers (B).
- **After the loop**, a consumer is decided as in the loop (in an If on its
  candidates, from its before context, now final, and its cover tests), once.
  Its cover tests read the final decisions of the loop's rules and those of
  the consumers whose matches may cover its first position. These are decided
  first (a topological order), with their covers updated; the others' covers
  are not. Consumers in a cycle (a rule overlapping itself), or after one,
  would be decided in a While loop of their own; und_FONIPA-ar has none: one
  consumer (`[gkɠɡ][ǀǁǂǃ…]`) depends on 8 others (nasal + `k`/`g`).
- The loop's iterations are unchanged (255,385 on `pool-und_FONIPA.txt`), and
  the output is identical to step 1's.

und_FONIPA-ar #3, statements before optimization:

| | step 1 | step 2b |
|---|--:|--:|
| the loop | 62,701 | 55,388 |
| of which the string rules | 58,829 (440 Ifs) | 52,144 (298 Ifs) |
| outside the rules' Ifs (each iteration) | 3,872 | 3,244 |
| after the loop | 4,073 | 7,760 |
| of which the consumers (once) | — | 3,687 (142 Ifs) |
| the kernel | 76,532 | 72,906 |

Per iteration, the loop now runs 628 fewer statements outside the Ifs and 142
fewer If tests, and the consumers' bodies run once per block instead of in
each iteration (2.35 on average). Not yet measured: timing at 4 threads.


### 13.11 Step 3: design of the boundary (2026-11-25)

What the correction kernel does after its loop (und_FONIPA-ar #3, statements
before optimization, step 2b): the final decisions of the subgroups (1,819:
`known` from K, then not within an applied string match), the consumers
(3,687, in 142 Ifs) and the results (2,254). Before the loop: 9,758, of which
5,708 set up the 1,260 covers. Per block, roughly (estimated from 13.8's
figures): before 9,800, the loop 2.35 × ~4,300 ≈ 10,100, after ~4,600 (the
final decisions and results in full, the consumers' If tests and the bodies of
the few with candidates). So about a fifth of the kernel's work per block is
after the loop.

What the post-loop part reads from the loop:

- K, known in and known out, by item and distance: 8 streams.
- For the consumers' cover tests, the loop rules' covers: 139 covers, in 12
  distinct sets, so at most 24 streams (each set's union of covT and
  intersection of covF).
- For the results and the subgroups' coverage, the decisions DT of the 298
  loop rules: too many to pass. Instead the loop kernel applies the loop
  rules' results itself (adds them to the group's state, `FusedState`, as
  every fused kernel does) and passes the interiors of their applied matches
  (1 stream).

Proposed split:

- **Loop kernel**: the setup, the loop, the loop rules' results. Outputs: the
  state with those results, K (8), the cover-set summaries (≤ 24), the loop
  matches' interiors (1).
- **Post kernel**: the subgroups' candidates and the consumers' candidates
  (matched again, statically: subgroup matches 584, static selection 343, and
  the consumers' share of the string matches), the subgroups' final decisions
  and the consumers' (from K and the summaries), and their results added to
  the state. No While loop, no state across blocks other than Advances of
  bounded distance, so LocalCarries applies.

Open questions before implementing:

- The first positions: where a consumer and a subgroup are both candidates at
  a consumer's first position, which takes it (the subgroup coverage removes
  only the interiors), and the same for a loop rule and a consumer beginning
  at the same position.
- Whether the post kernel recomputes the candidates (a duplicate of part of
  the setup) or receives them per subgroup class (17 streams) and per
  consumer (142, too many).
- The kernel and its If structure are generated by the same generator, by a
  part flag in `FusedRulesConfig` (in the signature).

The expected gain is pipeline balance at 4 threads (the loop kernel is the
group's largest), not fewer statements: the post kernel adds the recomputed
candidates and the streams between the kernels.

### 13.12 Status of step 3 (2026-11-25): the loop kernel and the post kernel

A correction kernel whose loop has consumers is created as two kernels
(`--fused-loop-split`, default on; `RulePipelineBuilder::createFusedRules`, at
both places a correction kernel is created), both FusedRules, by
`FusedRulesConfig::loopPart` (1, 2; in the signature):

- The consumers are now found by a static function of the config
  (`loopConsumers`, with the covers as `LoopCover`s and `loopCoverMayBegin`),
  which the pipeline uses to decide the split and both kernels use to build
  their parts; their order after the loop also (`consumerOrder`).
- **The loop kernel** builds the setup and the loop as before, adds only the
  results of the rules decided in the loop to the state, and writes
  `LoopState` (`LoopStateLayout`): K, the cover-set summaries, and the
  interiors of its applied matches (the static classes' included). It also
  writes `StaticSelected`.
- **The post kernel** reads the state from the loop kernel and `LoopState`.
  It builds the same setup with the candidates of the rules decided by the
  loop kernel as none (Pablo's dead-code elimination removes what it does not
  use), takes K from `LoopState`, emits no loop, decides the consumers (their
  cover tests read the summaries for the loop kernel's covers) and the
  subgroups' final decisions (coverage: the interiors from `LoopState` and
  those of the consumers), and adds their results and those of the static
  string rules. No masking is needed: every result is ORed into the state, as
  in the whole kernel.
- The post kernel has `LocalCarries` (its extent from its contexts and texts,
  as for the other fused kernels) unless its consumers need a While loop, a
  rule has a run, or a selected rule overlaps itself. The extent is checked
  when it is compiled (CarryExtent).

und_FONIPA-ar #3 (statements before optimization): the loop kernel 69,093
(before 9,758, loop 55,388, after 3,947); the post kernel 10,040, most of it
setup that is dead there. `LoopState` has 8 + 24 + 1 = 33 streams. The output
on `pool-und_FONIPA.txt` is identical to step 1's.

Checks: all 52 xlit and ldml tests. The loop tests split kernels (with
consumers decided once, in a While loop of their own, and with a cover set).
Kernels split with static classes occurred in no test: a group made for it
(`xlit_static_loop_test`'s first, with `[a] { st → U ;`, `--staged-chunk-rules=0
--whole-group-rules=3` and `4`, 400 random lines) gives the same output split,
unsplit, and with the default options, and is now a group of
`xlit_static_loop_test` (12 cases, ICU's results, in UTF-8 and Unicode mode).

Not yet: timing at 4 threads (whole against split, with the outputs compared),
and the compile time of the post kernel's dead setup.


### 13.13 Step 4a (2026-11-25): what the class layer would be in the regular plan's kernels

`--class-layer-report` (with `--kernel-counts`, nothing is compiled) prints,
for each FusedRules kernel constructed, its classes as DivCode's class layer
would have them: explicit sets (the set or its complement within 16 Div32
blocks, as `maxSetBlocks`) and external ones, the blocks and code bits, and the
level-1 leaves (the Div32 blocks of the rules' first characters; a subgroup
counts as one rule), with the most rules in a leaf and the rules whose first
sets are external.

Run on the 88 implementable transforms with groups that DivCode cannot take
(the 123 groups of 13.2), default options (UTF-8 mode where it applies): 961
kernels, 872 distinct.

- Most are small: the regular kernels have a median of 4 rules and 1 leaf,
  the matching kernels 17–22 rules and 1 leaf.
- In UTF-8 mode most already compile their classes in the kernel from the
  bits of a multiplexed code of the classes (`classCodes`, the hybrid): 280 of
  504 regular kernels, 228 of 307 matching kernels, all the loop and post
  kernels. The class streams are cheap where they are not.
- Kernels where level-1 Ifs could skip much (20 or more rules, 4 or more
  leaves, no leaf with more than half of them): 50, in 9 transforms, all of
  them coded (UTF-8 mode): Hant-Hans (16 kernels, 2,205 rules, up to 129
  leaves), am_Ethi-am_Ethi/Geminate (12, 2,388 rules, up to 11 leaves),
  Hans-Hant (7, 1,107 rules, up to 128 leaves), und_FONIPA-ar and -fa (5 each:
  the matching kernels and the loop and post kernels, up to 13 leaves),
  nv-nv_FONIPA (2), Latin-InterIndic, am_FONIPA-am and und_FONIPA-chr (1
  each). In und_FONIPA-ar the leaves are structurally many but the vowels'
  blocks occur in every block of text (13.2), so this criterion, which is
  structural, overstates it there.

What this changes in step 4:

- The candidates run in UTF-8 mode, where the classes are already a code
  computed apart and compiled in the kernel; "Unicode mode first" would build
  the layer where it is not used by default. The If-hierarchy needs a code
  whose high bits are the Div32 block of the first character (the multiplexed
  class code is not ordered by blocks), so in UTF-8 mode the multiplexed code
  would be replaced by (or ordered as) the Div32 code at code units, as
  DivCode phase 2 computes it.
- The gain is the level-1 Ifs in about 50 kernels, mostly the CJK
  conversions (Hant-Hans, Hans-Hant: rare blocks, many leaves) and Ethiopic
  gemination; the pseudo-basis CCs alone change little where classes are
  already coded.
