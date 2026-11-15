# DivCode kernels for the conversion groups of tconv: implementation plan

Status: plan, 2026-11-15. Nothing below is implemented. The analysis numbers come
from `tconv --string-rule-chars` (an uncommitted, hidden report option in
`tools/ldml/tconv.cpp`) over the 212 conversion groups with string rules in the
151 CLDR transforms tconv implements.

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
- `ldml::TransformStep` gains nothing. The plan is computed by tconv from the step,
  like the shape groups.

`--string-rule-chars` becomes `--divcode-report`. Per group it reports eligibility
(or the problems), classes, code bits for s = 5 and 6, rules after splitting, leaf
sizes, expanding rules, and the XOR bit counts.

## 4. Kernels and pipeline

The Div32Code / Div64Code kernel is `kernel::DivCodeKernel` (`include/kernel/unicode/divcode.h`, module `kernel.unicode`); it depends only on the shift, the blocks and the basis. The kernels specific to the plans of conversion groups are in `tools/ldml/tconv.cpp`.
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

- `QA/tconv_divcode_test.py`: rules files (`--rules-file`) for each case, each run
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
- CLDR: `tconv_coverage.py --test-data` with the option on and off; results must
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

`tconv --divcode-report <transform>` (one JSON object per conversion group with
string rules) over the 212 groups of the CLDR transforms tconv implements:

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
   bixnum (done: `tconv --divcode-codes`, `QA/tconv_divcode_test.py`).
3. Translation kernel without expansions (groups where m ≤ n everywhere), with
   deletion; QA tests; the CLDR comparison. (Done: `tconv --divcode-groups`,
   `DivCodeTranslate` in `tools/ldml/tconv.cpp`. The output is identical to the
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
   default. (Done: on by default, div32. With the default on, the other tconv
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
