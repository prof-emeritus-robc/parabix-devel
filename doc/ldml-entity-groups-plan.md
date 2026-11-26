# Entity-based conversion groups (und_FONIPA-ar): implementation plan

Status: plan, 2026-11-26. Nothing below is implemented. Unicode mode first;
UTF-8 mode is outlined in §9.

## 1. Idea

The regular plan expands every variable and set with strings into its members
(`ExpandFiniteTexts`, order disambiguation, the class partition). For
und_FONIPA-ar, this turns 2 source rules into 211 string rules (group 2) and
about 70 into 440 string rules and 100 subgroups (group 3). The group 3
correction kernel then has 55,111 Pablo statements (doc/ldml-divcode-plan.md
§13.2).

Instead, the plan keeps the rules as written and gives each **named entity** of
a group a stream computation of its own:

- each **Unicode property** (`[:L:]`, `[:M:]`, `[:N:]`): a
  `UnicodePropertyKernelBuilder`, shared and cached across groups and transforms
  (as with `--ucd-property-kernels`);
- the **character classes** of the group, made by one `CharClassesKernel` per
  group (§3.1). They are:
  - the pure sets, both `$variables` and inline sets without strings;
  - the literal characters;
  - where the group is coded, also the characters of the strings and the
    single-character members of the string sets.

  The kernel outputs one stream per class, or where the class layer's
  criterion holds, the bits of its Div32 code;
- each **set with strings**, a `$variable` or an inline set such as
  `[{m̥} m ɱ]`: a new `StringSet` kernel, giving what longest-leftmost matching
  of the set needs (§4). On a coded group, its REs are over the code.

The conversion group's logic then reads these streams. A rule is a sequence of
items; each item is matched through its entity's streams, never through the
entity's members.

## 2. The groups of und_FONIPA-ar

| group | source rules | regular plan | entity plan |
|---|--:|---|---|
| 1 (NFD … NFC) | 5 | 1 subgroup | trivial: pure sets only |
| 2 (NFC … `::NULL`) | 2 | 211 string rules | driver for steps 1–3 |
| 3 (`::NULL` … `::NULL`) | ≈ 70 | 440 string rules, 100 subgroups, correction loop | driver for step 4 |
| 4 (last) | 2 | run `ووو+` | stays on the regular plan (§8) |

**Group 2:** `$UVowel [j $IVowel] [e {e̞} $SchwaVowel] → uia ; yʉ → iu ;`. It
has no contexts. It has one overlap, which is one-way: the first rule may begin
at the `ʉ` of `yʉ`, as in `yʉie`, and must then be rejected.

**Group 3: the entities.**

- Properties: gc = L, M, N. `$Boundary = [^[:L:][:M:][:N:]]` is derived from
  them: ¬(L ∨ M ∨ N), plus the text boundary.
- `$variable` string sets: `$IVowel` `{e̞}`, `$UVowel` (10 strings), `$AVowel`
  `{ä} {ɒ̈}`, `$SchwaVowel` `{ɵ̞}`.
- A derived union: `$Vowel`, used only as the after context `[$Vowel]`.
- A pure `$variable`: `$Click`.
- Inline string sets, about 15 distinct: `[{t͡ʃ} ʧ]`, `[{m̥} m ɱ]`,
  `[{n̼̊} … n …]`, `[{ŋ̊} ŋ {ɴ̥} ɴ]` (used twice, deduplicated),
  `[p b {p̪} {b̪} ɓ]`, `[{d̼} d ɗ ᶑ]`, `[{t̼} t]`, `[{θ̼} θ {θ̱}]`,
  `[{ð̼} ð {ð̠}]`, `[h ɦ {ʔ̞}]`, `[{ɹ̥} … r]`, `[{ʀ̥} ʀ]`, `[{ʎ̥} ʎ]`,
  `[{l̼} … ʎ]`, `[ʟ {ʟ̠}]`.
- Inline pure sets and literal characters, about 35: `[ɡ g ɠ k]`, `[q ɢ ʡ ʛ]`,
  …, the complement `[^ $IVowel j ʝ]` (the parser's complement drops strings,
  `SetValue::chars`), and `ʔ ː c ɟ k s z β ç ʝ ħ ʕ ʋ ʙ ʜ ʢ j ɬ ɮ`.
- Literal multi-character texts: `{r̝}`, `yʉ` (group 2). These are sequences
  of single characters, not entities.

The variables are defined once for the transform. Groups 2 and 3 compute the
same entities over different texts, so they share compiled kernels (same name)
but not streams.

## 3. Entity analysis (ldml library, no code generation)

New files: `include/ldml/entity_plan.h` and `lib/ldml/entity_plan.cpp`, in the
module `ldml.transforms`.

`EntityGroupPlan planEntityGroup(const TransformStep & step)` returns the plan
below, or the problems that make the group ineligible.

**Entities**, deduplicated by content (the codepoint set, the strings, and
whether the text boundary is a member):

- `Property`: a `re::PropertyExpression` or property `re::Name`, one stream.
- `PureSet`: a `UnicodeSet`. Its stream index is in the group's CC kernel.
- `StringSet`: chars + strings. Its lengths are the distinct lengths of its
  longest matches; for `$UVowel`, {1, 2}.
- `Derived`: an expression over other entities.
  - A **union** of string sets, pure sets and properties is derived, not
    computed by a kernel of its own. The longest match of a union at x is the
    longest of its operands' longest matches at x, so
    `Lₖ(∪) = ∨ Lₖ(op) ∧ ¬∨_{k'>k} L_{k'}(∪)`. This covers `$Vowel`,
    `[j $IVowel]` and `[e {e̞} $SchwaVowel]`.
  - Complement, intersection and difference of pure sets and properties are
    also derived (`$Boundary`).
  - Set algebra other than union over strings is a `StringSet` of its own,
    computed from the resolved members.
- Each entity carries its name (the `$variable`, or `inline_n`), its uses
  (rule text, after context, before context) and therefore which direction it
  needs (forward for texts and after contexts, backward for before contexts).

**Rules:** one `EntityRule` per source conversion rule, in order:

- the text as a sequence of items `(entity | literal char, optional)`;
- the before and after contexts, each a sequence of items;
- the result string;
- the derived values: the maximum text length `Tmax`, whether the rule's
  before context is matched against the converted text, and whether its
  result is empty.

Order disambiguation and expansion are not run: priority is the rule order
itself (§5.2).

### 3.1 The group's classes: the class layer's code, or per-class streams

The entity path codes its classes by the **class layer's rule**
(doc/ldml-divcode-plan.md §13.14–13.18), not by a rule of its own. One code
assignment then serves both paths, with one check (`--class-layer-check`) and
one class compiler.

The group's sets are:

- **Stream sets:** its pure sets (`$variables` and inline), its literal
  characters, and the sets of characters within property sets;
- **Coded sets:** the stream sets, plus the characters of the strings of its
  string sets, each a singleton set, and the single-character parts of the
  string sets.

Properties stay external, as kernels of their own, and are not part of either
list.

- **The criterion** is `classLayerCriterion`'s, applied to the entity rules
  (structural, not timed). It counts the source rules by the Div32 blocks of
  their first characters, the union over the leading optional items and the
  first required one. It requires 20 or more rules in 4 or more leaves, none
  with more than half, and `--class-layer` on.
- **When it holds and `div32Classes` codes the coded sets:** the group's
  `CharClassesKernel` outputs the bits of the Div32 pseudo-codepoint and, after
  them, the external sets, each as a stream of its own (sets that neither they
  nor their complements fit in 16 blocks, as in step 4d).
  - Each set is the CC of its pseudo-codepoints (`pseudoCodes`) over those bits.
  - Pseudo-codepoints identify every character of the layer's blocks exactly,
    so a string's characters, and any subset of a string set's characters,
    are exact sets of codes.
  - The StringSet kernels and the match kernel compile over the code bits. A
    StringSet whose single-character part is external reads the 21-bit basis
    instead.
- **Otherwise, in Unicode mode:** one stream per stream set, as the regular
  kernels without the layer have, and the StringSet kernels read the 21-bit
  basis.
- **In UTF-8 mode (§9):** where the layer does not apply, the multiplexed code
  of the hybrid groups (`multiplexedClasses`), as `absolutely` chooses.

The ldml plan gives the two lists (`streamSets`, `codedSets`). xlit makes the
choice, as it does for the fused kernels, and `--entity-report` prints it.

**Path precedence.** With `--entity-groups`, a group the entity plan accepts
is applied by it before DivCode or the regular plan are tried. DivCode and the
regular plan take the other groups. The option is off by default, so nothing
changes until it is enabled.

## 4. The StringSet kernel

`kernel::StringSetKernel`, in `include/kernel/unicode/string_set.h` and the
module `kernel.unicode`.

```
StringSetKernel(ts, const UCD::UnicodeSet & chars, std::vector<std::u32string> strings,
                StreamSet * Basis, StreamSet * Lengths,
                Direction dir = Forward, BitMovementMode mode = Advance)
```

**Output:** one stream per length k in the set's length list (ascending).

- **Forward:** stream Lₖ marks the positions x where the longest member
  matching at x has length k. This is what ICU's `UnicodeSet::matches`
  chooses.
- **Backward:** Lₖ marks the positions where the longest member *ending*
  there has length k. This is needed only for a string set used in a before
  context.

The streams are mutually exclusive. `Start = ∨ Lₖ`, and the end of each match
is `∨ Advance(Lₖ, k−1)`, both derived by the consumer.

**Why length-indexed rather than two streams (start, end):**

- Start and end pair unambiguously only when no match can lie strictly inside
  another; that holds for every set here, but it has to be checked.
- Even then, going from a start to its end needs a ScanThru (an unbounded
  carry).
- With the lengths, a consumer moves by bounded Advances and Lookaheads, so
  kernels keep `LocalCarries`, and a start knows its end directly. The
  two-stream view is derived when wanted.
- For a set with many lengths, a bixnum of the length can replace the one-hot
  form; no set here has more than 3.

**Implementation:** the RE engine's longest-match resolution for possessive
repetitions.

- `unambiguousStrings` in `lib/re/transforms/resolve_possessive.cpp` already
  computes, for `PossessiveChoice::Longest`, which alternative is the only one
  that can match at a position:
  - a string with a longer preferred string as a prefix is guarded by
    `w (?!t′)`;
  - the single characters that begin strings become literals;
  - backward, the same with `(?<!t′) w` at the ends.
- Refactor it to return that result as a structure as well as an RE: the
  remaining single characters, and per literal its text and its guard
  suffixes (prefixes when backward). `unambiguousAlternatives` keeps its
  interface and builds its RE from the structure. The structure's limits
  (64 single characters that begin strings, literal strings only) are met by
  UnicodeSet strings.
- The kernel compiles it directly in Pablo, over its basis:
  - for a literal `c₁…cₖ`: `cc(c₁) ∧ Lookahead(cc(c₂), 1) ∧ … ∧ ¬guards`, ORed
    into L_{k};
  - the remaining single characters go into L₁.
  - Backward, Advance replaces Lookahead.
- **Two bases:**
  - **U21** (a group below the threshold): the CCs come from
    `cc::Parabix_CC_Compiler` over the 21 basis bits (the UTF compiler in
    UTF-8 mode).
  - **The class layer's code** (§3.1): the kernel is given the structure with
    each character and set replaced by its pseudo-codepoints (`pseudoCodes`),
    or in UTF-8 mode without the layer, by its codes in the
    `MultiplexedAlphabet` (`transformCC`).
    - A string character is a single code.
    - The single-character part of the set is the union of its codes.
    - The CCs are compiled over the k code bits.
    - It is the same algorithm with a different alphabet, so the kernel takes
      the alphabet's basis and the transformed CCs, and the analysis does the
      translation.

**Name:**

- **Over U21:** `StringSet_{fwd|bwd}_` + a hash of the sorted members + the
  basis shape. It depends on no transform or group, so it is shared by groups
  2 and 3, by und_FONIPA-fa, and by the 22 `*-ar` transforms that invoke
  und_FONIPA-ar.
- **Over a code:** the hash also covers the code assignment (the
  translated sets). Groups with the same classes share the kernel, and others
  do not. This is the cost of the smaller basis.

**Unit test:** `tests/string_set_test`, registered with
`parabix_add_kernel_test`. It runs random sets (strings that are prefixes of
one another, single characters that begin strings, combining marks) on random
text, and compares against a C++ reference longest match.

## 5. The conversion-group logic

### 5.1 Matching a rule at its start

The match of rule R must be known at its start position x, with its length.
Items are possessive, so from x the match is deterministic: one path, one end.

Let `P_{j,t}` be the positions from which items j+1..n and the after context
match with total length t. It is computed backwards over the items:

```
P_{n,0}     = After                                   (the after context at the match end)
P_{j-1,t}   = ∨ₖ Lₖ(i_j) ∧ Lookahead(P_{j,t-k}, k)                         (i_j required)
P_{j-1,t}   = (∨ₖ Lₖ(i_j) ∧ Lookahead(P_{j,t-k}, k)) ∨ (¬Start(i_j) ∧ P_{j,t})   (i_j?, possessive)
Match_R,t   = P_{0,t} ∧ Before
```

- **Single characters and pure sets:** L₁ is their class stream.
- **After contexts** are sequences of items with the same recursion, and only
  existence matters: `Start(X)` for a set item.
- **Before contexts** matched against the text itself: Advances of class
  streams, and backward `Lₖ` for string sets.
- **Size:** at most `items × Tmax` streams per rule, with only bounded
  Lookaheads. Rules that share a suffix of items share `P` (memoized by
  suffix and after context; Pablo's CSE does the rest).
  - `$AVowel ː? ʔ $AVowel ː?`: 5 items, t ≤ 7.
  - A rough estimate for all of group 3 is a few thousand statements, against
    the 55,111 now; step 4 measures it.

The forward direction from a start is also deterministic. Deletion and
insertion only need the start and the length, so no forward walk is
required.

### 5.2 Priority

At a cursor position, the first matching rule wins (ICU tries rules in order):

```
Win_R = Match_R ∧ ¬Prior;  Prior |= Match_R        (in rule order)
```

- The match kernel outputs the winner as a bixnum `W` (rule index + 1;
  0 = none; 7 bits for 70 rules) and its length as a bixnum `N` (4 bits for
  Tmax ≤ 16).
- No order disambiguation: the not-at-a-boundary contexts that it adds, and
  the 960 overlap pairs of the expanded rules, do not arise.
- A rule's span is `[x, x + N)`.

### 5.3 Before contexts matched against the converted text

In ICU, a before context is matched against the output so far. In group 3,
every such context is one item, `$Boundary` or its complement.

Each position therefore has a two-valued state b: is the last output character
before x in `$Boundary` (or is x at the text start)? The match kernel computes
`W⁰, N⁰` and `W¹, N¹`. Rules without converted contexts are shared by both.

**Closed form.** It is exact when the analysis proves three conditions, which
hold for group 3:

- (a) no character of `$Boundary` occurs in any rule text, so a boundary
  character is never covered and is never converted;
- (b) every non-empty result and every unconverted character settles the state
  by its own class (results are Arabic letters and marks: state 0);
- (c) under state 1, every rule with an empty result has a span of one
  position. Here this is only `ʔ → ;`, since a schwa at the boundary goes to
  `$Boundary {ʔ? $SchwaVowel} → أ`.

Then the state is a run of deletions after a boundary:

```
D¹    = starts where W¹ is a rule with an empty result
B     = MatchStar(Advance(Bnd) ∨ TextStart, D¹)
W, N  = B ? W¹, N¹ : W⁰, N⁰
```

That is one MatchStar, with no loop. Example: ` ʔʔa`. The first `ʔ` is not
followed by a vowel, so `ʔ → ;` deletes it. The second `ʔ` is then at a
converted boundary, and `$Boundary {ʔ? $AVowel} → أ` applies. Against the
original text it would not, because the `ʔ` before it is a letter.

**Fallback.** A group failing (a)–(c) uses the existing correction-loop
algorithm (K1/K0 known-in/known-out, doc/ldml-divcode-plan.md §10), with the
source rules' `W^b` in place of the expanded rules' matches.

### 5.4 Cursor positions: selection with overlaps

A winner at x applies only if x is a cursor, that is, not inside the span of an
earlier accepted match:

```
Accepted = Win ∧ ¬Interior(Accepted)
Interior(S) = ∨_{k=2..Tmax} ∨_{j=1..k-1} Advance(S ∧ (N = k), j)
```

The analysis computes, at entity level, which rules may begin strictly
within another's match:

- for each offset 1..len−1 of R's items, including the inner positions of a
  string set's strings, does R′'s first item intersect the characters
  possible there?
- This gives a graph between rules. A rule with no incoming edge is accepted
  whenever it wins.

**Stages.** If the graph is acyclic with depth d (group 2: d = 1, the first
rule within `yʉ`), selection is d stages, each with bounded Advances:

```
A₀ = Win of rules nothing may cover
A_i = A_{i-1} ∨ (Win of depth-i rules ∧ ¬Interior(A_{i-1}))
```

Positions where a winner lies within another winner's span are the
**contested** positions, `Win ∧ Interior(Win)`. The stages run in an If on
them, so blocks without any skip the stages.

**Cycles.** A rule that may begin within itself, `$AVowel ː? ʔ $AVowel ː?` on
`aʔaʔa`, forms a chain resolved leftmost-first by alternation.

- The existing sequential-selection While loop handles it, restricted to the
  rules of the cycle, in an If on contested positions.
- §13 step 6's alternation form replaces the loop if the chain's links have
  fixed structure.
- §13.2 measured almost no contested blocks on the corpus, so the loop rarely
  runs.

### 5.5 Applying the accepted rules

The result does not depend on the span length:

- **Rule with a non-empty result:** its first character replaces the start
  position, the rest of the span is deleted, and m − 1 characters are
  inserted after the start.
- **Rule with an empty result:** the whole span is deleted.

The new characters come from the rule index:

- for offset o and output bit β, the set of rule indices whose result
  character at o has bit β set is compiled as a CC over the bits of `W`;
- the code goes in an If on Accepted.

Deleted positions:

```
Deleted = Interior(Accepted) ∨ (Accepted ∧ emptyResult(W))
```

The DivCode kernels are reused for counting, insertion and compression
(doc/ldml-divcode-plan.md §4.3–4.6): Count (m − 1 by W), `InsertBixNum`,
`SpreadByMask`, Translate (by W), and `FilterByMask(¬Deleted)`.

## 6. Kernels and pipeline (Unicode mode)

```
U21 ─┬─ CharClasses(group classes) ── Cls: one stream per class, or k code bits (§3.1)
     ├─ UnicodeProperty gc=L │ gc=M │ gc=N ────── Prop (shared, cached)
     │
     │   StringSet $IVowel, $UVowel, …, inline_n (U21, or Cls when coded) ── Lₖ per set
     │
     │   EntityMatch(Cls, Prop, Lₖ…) ─ W⁰ N⁰ W¹ N¹   (bounded: LocalCarries)
     │          │
     │   EntitySelect(W^b, N^b, Bnd) ─ Accepted, W, N   (MatchStar, stages/loop: no LocalCarries)
     │          │
     ├── Count(W) → InsertBixNum → SpreadByMask(U21, Accepted, W, N)
     │                                    │
     │                              Translate(W) → U21′, Deleted
     └──────────────────────────────────── FilterByMask(¬Deleted) → U21 out
```

- **Separate kernels** (the project's rule): the entity kernels, the match
  kernel (no unbounded carries) and the selection kernel (the only one with
  MatchStar or a loop).
- **Names:**
  - `EntityMatch_<transform>_G<n>_<hash>` and
    `EntitySelect_<transform>_G<n>_<hash>`, where the hash covers the entity
    plan, the rule items, priorities and stages;
  - the CC kernel is named by its classes, as now;
  - the StringSet kernels are named as in §4.

  Every configuration gets its own object-cache entries.
- **Regions and text end** (`--lines`, EOF): `TextStart` and the text end, used
  as members of `$Boundary`, come from the existing region-start and in-text
  streams.

**Switch:** `--entity-groups` (default off until checked) applies eligible
groups by this plan. Ineligible groups keep the regular plan, so plans can be
mixed within a transform.

## 7. Steps

0. **Analysis and report.** Write `planEntityGroup` and `--entity-report`.
   Run it on und_FONIPA-ar, reporting:
   - entities, lengths and the overlap graph;
   - the check of conditions (a)–(c);
   - the class counts and code bits of §3.1.

   Then run it on all transforms for reach.
1. **StringSet kernel.**
   - Refactor `unambiguousStrings` to expose its structure; `greptest` must be
     unchanged.
   - Write the kernel over U21 and over a multiplexed code, and
     `string_set_test`: both bases on the same sets, against the reference.
2. **Match kernel and application, without contexts or overlaps.**
   - `EntityMatch`, with W and N, plus the apply path; the selection kernel
     passes Win through.
   - Check: rules files of string sets and `?` items (`QA/xlit_entity_test.py`,
     small, ≤ 5 s), against the regular plan and ICU. Run them with
     `--class-layer` on and off (rules files with 20 or more rules over 4 or
     more blocks), so that both class paths are covered.
3. **Selection stages** (acyclic overlaps). Driver: group 2.
   - Check: und_FONIPA-ar with group 2 by the entity plan, on
     `pool-und_FONIPA.txt` and targeted cases (`yʉie`, `e̞` and `ɵ̞` at each
     item position, `o̞`/`ø̞` starts), identical to the regular plan.
4. **Converted before contexts and cycles.** Driver: group 3.
   - The two-state matching, the closed form of §5.3, and the cycle loop in an
     If.
   - Check: identical output on `pool-und_FONIPA.txt`.
   - Targeted cases: ` ʔʔa`, ` ʔəa`, `kəa`, `aʔaʔa`, `aːʔaːʔa`, `iːʔ` before a
     vowel, a boundary, and the text end, and `ʎ̥` before `[^ $IVowel j ʝ]`.
   - The same with `--lines`, with 4 threads and segments of one stride, and
     with an assertion option comparing the closed form with the loop
     (debugging, as `--class-layer-check`).
   - Report the statement counts of the match and selection kernels against
     #289's 55,111.
5. **Reach.** The other eligible groups from step 0's survey, each checked
   against the regular plan and ICU before `--entity-groups` becomes the
   default for it.

At every step:

- the xlit and ldml tests;
- `-object-cache-salt` for uncommitted code;
- outputs compared in every timing.

Timing the entity plan against the regular plan (4 threads) only with your go-ahead.

## 8. Not in phase 1

- **Runs** (`ووو+`, group 4) and other unbounded repetitions. A run entity
  would be a MatchStar with an unbounded length, which does not fit the `N`
  bixnum. Group 4 stays on the regular plan.
- **Cursors and revisits, captures, function calls.**
- **Before contexts of several converted items.** Here the state is more than
  two-valued: the product of the context sets' classes. The fallback is the
  loop.
- **String sets in before contexts** matched against the converted text.
  Backward `Lₖ` of the output is not available before the output exists.

## 9. UTF-8 mode (to follow)

- **Entity kernels on the bytes.**
  - The CC and property kernels use `BitMovementMode::LookAhead`, marking the
    first byte of a character (as the class layer does).
  - A multiplexed code is likewise placed at the first byte, as the hybrid
    groups' `classCodes` are.
  - StringSet works on the UTF-8 basis, with Lₖ indexed by the length in
    **bytes**. All the members that match at one position are prefixes of
    the same text, so the longest in codepoints is also the longest in bytes.
    Its literals compile to byte sequences.
- **Lengths in code units.** `P_{j,t}` and `N` count bytes, so Tmax grows
  (≈ 2–3×), and a pure set whose characters have different UTF-8 lengths
  becomes several `Lₖ` (its length split), like a string set.
- **Application.** The result bytes are written by the hybrid groups'
  selectors (`FusedRulesConfig::absolute`) instead of U21 bits, with insertion
  and deletion in bytes. The selection kernel is unchanged apart from the
  length unit.

## 10. Decisions (confirmed 2026-11-26)

1. **The StringSet interface is length-indexed starts (§4)**, not start and
   end streams. The (start, end) pair is derived where needed.
2. **The group's CC kernel holds the pure `$variables`, the inline pure sets
   and the literal characters.** Its code follows the class layer's rule
   (§3.1): the Div32 code where the layer's criterion holds, with the string
   characters as codes over which the StringSet kernels are compiled;
   otherwise one stream per class (Unicode mode), or the hybrid's multiplexed
   code (UTF-8 mode). This revises the first version of this decision, which
   had a threshold of its own (`--entity-mux-threshold`) and the multiplexed
   code: that duplicated the class layer and could diverge from it.
3. **Unions of string sets are derived** in the consumer (`$Vowel`,
   `[j $IVowel]`), not given StringSet kernels of their own.
4. **Group 4 stays on the regular plan** in phase 1.
5. **With `--entity-groups`, the entity path takes precedence** over DivCode
   for the groups it accepts.

## 11. Status of step 0 (2026-11-26)

`ldml::planEntityGroups` (`include/ldml/entity_plan.h`, `lib/ldml/entity_plan.cpp`)
plans each conversion group of the forward rules as written: no capture
elimination, order disambiguation or expansion. `xlit --entity-report` prints
one JSON object per group, with:

- the entities: kind, uses, members, strings, lengths and operands;
- the rules as items;
- the classes of §3.1;
- the overlap graph and its cycles and depth;
- the before-context states, with conditions (a)–(c);
- the problems.

(The class counts and multiplexed codes below were computed with the first
version of §3.1, `--entity-mux-threshold`, since replaced by the class layer's
rule: decision 2 of §10.)

**The analysis:**

- **Overlaps.** "R′ may begin within R" is tested on a character-level
  automaton of each text, with strings as chains and optional items passed
  over. It compares pairs of consecutive characters within R with R′'s first
  two, and allows for R′ beginning at R's last position or matching one
  character.
  - A test on single characters was too coarse: it found group 2's first
    rule overlapping itself through `ɪ̈` of `$UVowel` (`ɪ` is in `$IVowel`),
    which the pairs rule out.
  - Possessive matching is not modelled, so the test is conservative.
  - The pruned graph drops R′ whose before context is one item, a set with no
    character of R's text (`$Boundary`), or the boundary alone.
- **Condition (c)** accepts a deletion of several characters under the state
  when an earlier rule of the state, with a non-empty result, has the same
  items after leading optional items that cannot begin the deletion's match.
  That is how `$Boundary {ʔ? $SchwaVowel} → أ` precedes `$SchwaVowel → ;`.
- **Context loop.** A before-context state failing (a)–(c) does not make a
  group ineligible. The group is flagged `context_loop`, since the K1/K0 loop
  will compute the state.

**und_FONIPA-ar:**

| group | rules | entities | classes (stream / mux / exclusive, bits) | overlaps (pruned edges, cycles, depth) | states |
|---|--:|---|---|---|---|
| 1 | 5 | 1 pure set | 5 / 5 / 5, 3 | none | – |
| 2 | 2 | `$UVowel`, `$IVowel`, `$SchwaVowel`, inline `[e {e̞}]`, the unions `[j $IVowel]` and `[e {e̞} $SchwaVowel]`, the pure set `j` | 3 / 19 / 19, 5 | 1 (rule 1 within `yʉ`), none, 1 | – |
| 3 | 68 | 3 properties, `$Boundary` (a property set), 10 pure sets, 19 string sets, the union `$Vowel` | 33 / 88 / 84, 7: multiplexed | 86, rule 16 (`$AVowel ː? ʔ $AVowel ː?`), 3 | `$Boundary`: (a), (b), (c) hold |
| 4 | 2 | – | – | – | ineligible: `ووو+` |

The plan's assumptions hold for group 3: the closed form for `$Boundary`, a
7-bit code, and one cyclic rule. Group 2 is acyclic, with depth 1.

**All transforms** (425 with rule files, 327 conversion groups in 214
transforms):

- **192 groups are eligible.** Of these:
  - 124 need neither loop;
  - 36 need the context loop;
  - 39 have overlap cycles.
- **Acyclic depths:** 72 groups have depth 0, 38 depth 1, and 15 depth 2 to
  5.
- **Multiplexing:** 105 of the eligible groups would be multiplexed (5–13
  code bits, mostly 6–7).
- **Problems of the ineligible groups** (a group may have several):

  | problem | groups |
  |---|--:|
  | segments (captures) | 74 |
  | results that are not fixed strings (references, function calls) | 72 |
  | cursors | 49 |
  | insertion rules | 49 |
  | repetitions | 48 |
  | before contexts of several items | 20 |
  | sets with strings in a before context | 7 |
  | others | 9 |

  The single most common sole problem is a cursor (13 groups), then
  repetitions (11).
- **Sets with strings are rare.** Only 9 eligible groups have string sets or
  unions:
  - und_FONIPA-ar and und_FONIPA-fa, groups 2 and 3 of each;
  - und_FONIPA-chr;
  - am_FONIPA-am;
  - de-ASCII;
  - si-si_FONIPA;
  - blt-blt_FONIPA.

  Elsewhere, the gain of the plan would come from not expanding pure-set
  variables and from the order-based priority, not from StringSet kernels.

**Not checked by the analysis yet:**

- whether rule texts written decomposed can match after `::NFC` (ICU
  matches them as written; the plan takes them as written too);
- the exactness of the pruning under the closed form (§5.3), which the step 4
  check compares against the loop.

## 12. Status of step 1 (2026-11-26)

**The refactoring.** `re::unambiguousStringClass`
(`include/re/transforms/resolve_possessive.h`) returns the structure that
`unambiguousStrings` computed:

- the single characters that begin no string (`others`);
- the literals kept, each with its guards (the rests of the preferred strings
  it begins, or backward, their beginnings);
- `firsts` and `singles`.

`unambiguousStrings` builds its RE from that structure, with the same
alternatives in the same order, so `unambiguousAlternatives` and the
possessive resolution are unchanged. `greptest` (255 cases) and
`ldml_trules_test` pass. `maxInvolved` (64 by default, the old limit) bounds
the single characters that begin strings. `makeStringSetSpec` lifts the limit,
since it builds no RE.

**The kernel.** `kernel::StringSetKernel` is in
`include/kernel/unicode/string_set.h` and `lib/kernel/unicode/string_set.cpp`.

- **Input:** a `StringSetSpec`, which holds the lengths (ascending, one output
  stream each), the terms (a sequence of sets and its guards) and the
  direction. `makeStringSetSpec(chars, strings, backward, code)` builds it
  from a set's members by `unambiguousStringClass(…, Longest, …)`. The
  optional `code` maps the sets to a multiplexed alphabet's codes.
- **Compilation:** the sets are compiled as sets of codepoints over the basis,
  padded to 21 bits: the 21-bit Unicode basis, or the bits of a code.
  - **Forward:** each offset uses the basis bits looked ahead by it, with a
    `LookAhead` on the input (the longest term plus its longest guard, less
    1).
  - **Backward:** Advances of the classes, with no lookahead.
- **Name:** `StringSet_{fwd|bwd}_<hash of the basis shape and the spec>_<basis shape>`.

**The test.** `tests/string_set.cpp` and `string_set_test.py` (ctest
`string_set_test`, about 2 s once compiled, 9 s cold).

- **Sets:** 4 of und_FONIPA-ar's (`$UVowel`, the `n` set with `{n̼̊}` of 3
  characters, `[{t͡ʃ} ʧ]`, `$IVowel`) and 6 random ones (fixed seed). The
  random sets have strings that extend others and strings that end with
  others, and combining marks and characters of 2, 3 and 4 UTF-8 bytes, on
  texts of 3,000 codepoints.
- **Runs:** each set forward and backward, over the 21-bit basis and over a
  multiplexed code of the set's classes, that is 40 runs, compared with a
  Python reference of the longest match at each position.
- **Result:** all 40 pass. With the guards removed (a mutation check, then
  restored), 30 of the 40 fail.

**Not yet:** UTF-8 mode (lengths in bytes, §9); the kernel in xlit (step 2).

## 13. Status of step 2 (2026-11-26)

**The code (revision of §3.1).** The entity path's classes follow the class
layer's rule, not a threshold of their own. The ldml plan gives the sets
(`streamSets`, `codedSets`) and each rule's first set (`firstSets`).
`entityClassLayer` (xlit) applies `classLayerCriterion`'s thresholds to the
leaves of the source rules' first sets, and `div32Classes` to the coded sets.
`--entity-report` prints the choice with the leaves. `--entity-mux-threshold`
is gone.

For und_FONIPA-ar's group 3, the criterion does not hold: the block
0x60–0x7F has 37 of its 68 source rules, more than half (13 leaves). The
expanded rules of the regular plan pass it, because the expansion splits
them by characters. So group 3 has a stream for each of its 33 classes, and
its StringSet kernels read the 21-bit basis.

**Which steps it applies to.** `TransformAnalysis` keeps, with
`--entity-groups`, the entity plans of the forward rules as given
(`entityGroups`), and the step of the plan corresponding to each
(`alignEntityGroups`). The transform rules delimit the same segments in
both, and a segment's group corresponds to its step if the segment has one
conversion step. `applyAnalysis` applies a step by its entity plan, before
DivCode and the regular plan, if all of the following hold:

- not in UTF-8 mode's hybrid;
- not within a filter (a mask);
- not with bounded regions;
- not after deleted runs;
- the step has no parts of other kinds;
- the plan has no before contexts and no rule may begin within another
  (`entityStep2Problem`).

`--entity-groups-trace` reports each group, and the code used, or why the
path does not apply.

**The kernels** (`tools/ldml/xlit.cpp`):

- **Inputs:** the classes (a `CharClassesKernel` of the code bits, with
  another for its external sets, or one of the stream sets), `TextPositions`,
  a `UnicodePropertyKernelBuilder` for each property, and a `StringSetKernel`
  for each string set used. A string set reads the code bits where its sets
  have codes, and U21 otherwise.
- **`EntityMatch_<label>_<hash>`:** each rule's matches by paths. Each path
  takes one length for each item (each length of a string set or union, 0 for
  an absent optional item, which must not begin there: possessive), and is a
  conjunction of the inputs looked ahead (Pablo Lookahead reads inputs only,
  so the backward recursion of §5.1 is enumerated as paths).
  - After contexts are paths too, with the text boundary (`¬InText` ahead)
    for `[$]` and for sets that include it.
  - Property sets are evaluated from the property streams and classes. A
    union's longest match is derived from its operands'.
  - The first rule that matches wins (rule order).
  - Outputs: W (the rule's index + 1), Deleted (the rest of each match's text
    by its length, and the texts of empty results) and Ins (the result's
    length − 1).
- **Insertion and deletion:** `InsertionSpreadMask` and `SpreadByMask` of
  U21, W and Deleted where a rule expands.
- **`EntityApply_<label>_<hash>`:** for each offset j, the rules whose results
  have a character there are a CC over W advanced by j. Their bits replace
  U21's at the first position (j = 0) and at the inserted ones.
- `FilterByMask(¬Deleted)` follows.

**Checks:**

- **Four real transforms:** the outputs of
  - de-ASCII (`nat-de.txt`, string sets),
  - am-am_FONIPA (`pool-am.txt`, 436 rules),
  - und_FONIPA-und_FONXSAMP (`pool-und_FONIPA.txt`),
  - InterIndic-Bengali (`gen-Deva-InterIndic-from-pool-Deva.txt`)

  are identical to the regular plan's on 1 MB of each, in Unicode mode, with
  the entity kernels in the pipeline.
- **Filters:** Cyrl-Latn and Latin-ASCII are within global filters, so the
  regular plan applies them. Supporting filters is a gap, since many
  transforms have a global filter.
- **`QA/xlit_entity_test.py`** (ctest `xlit_entity_test`, 2 s) has two rules
  files:
  - **small:** a stream for each class, with sets with strings of 2 and 3
    characters, an optional item, a property after context, an expansion and a
    deletion;
  - **layered:** 30 rules in 6 Div32 blocks (the Div32 code, 9 bits), with
    the union `[$A $G]`, a negated set after context holding at the text end,
    and optional items.

  Each is checked against the regular plan and ICU, with `--class-layer`
  on and off, and the trace confirms the path and the code. With the rule
  priority removed (a mutation check, then restored), the layered checks
  fail.
- `string_set_test`, `xlit_class_layer_test`.

**A first check that proved nothing.** A rules file the regular plan cannot
implement (`$C } [:L:] → C` with `{k͡h}` in `$C`: "a string may not follow the
text, which has an after context") was applied by ICU in all three runs, so
their outputs agreed trivially. The tests now run with `--use-ICU=0`, and
check that the path was taken. The entity plan implements that rule. Letting
such transforms count as implementable when their groups are applied by the
entity path is for later.

**Not yet:**

- filters (masks), bounded regions;
- step 3 (overlaps), step 4 (before contexts);
- UTF-8 mode;
- timing (only with your go-ahead).

## 14. Status of step 3 (2026-11-26)

**The stages.** The ldml plan gives each rule its stage (`stage`): its depth
in the graph of `mayBeginWithin`, not pruned, since step 3 has no before
contexts to prune by. The depth is the longest chain of rules within whose
matches it may begin, and the rules on cycles are `stageCycleRules`
(`stagesOf`, also used for the report's pruned graph). `stagesOf` takes the
strongly connected components of the graph and the longest paths of their
condensation, in time linear in the graph; a transitive closure, cubic in the
number of rules, took minutes on the groups of thousands of rules of the
Ethiopic Geminate transforms (6,888 in am_Ethi-am_Ethi/Geminate).

`EntityMatch` selects by the stages (§5.4):

- a rule of stage 0 applies where it wins;
- a rule of stage d applies where it wins outside the matches applied by
  the rules of stages below d, which are the only rules within whose matches
  it may begin. The rest of their texts, by their lengths, are the bounded
  Advances of `interior`.

A rule that wins at a covered position is dropped, and no later rule applies
there: the position is not a cursor. The stages are in the kernel's
signature. `entityStep2Problem` now admits overlaps without cycles, and
cycles are left to step 4. The report gives `stages` and `stage_cycle_rules`.

**Checks:**

- **und_FONIPA-ar** (Unicode mode, `--entity-groups`):
  - group 1 is applied by its entity plan, and group 2 too (stage 1: its first
    rule may begin within `yʉ`);
  - groups 3 and 4 by the regular plan (a before context; a repetition);
  - the output is identical to the regular plan's on 1 MB of
    `pool-und_FONIPA.txt`;
  - on 998 targeted lines (every `$UVowel` × `[j $IVowel]` × `[e {e̞}
    $SchwaVowel]`, with `y` and `yʉ` before them, `e̞`, `ɵ̞` and `ɪ̈` at each
    position), the output is identical to both the regular plan's and ICU's.
- **45 more groups are in scope** (acyclic overlaps, no before contexts).
  Their outputs are identical to the regular plan's on 1 MB of their corpora:
  - ru-ja group 2 (depth 5);
  - Latn-Cans (depth 3);
  - InterIndic-Devanagari;
  - am_FONIPA-am groups 1, 3, 7, 8 (296 rules, depth 3) and 9.

  Bopo-Latn and uk-uk_Latn/BGN are within filters, so the regular plan
  applies them.
- **`QA/xlit_entity_test.py`** has a third rules file of overlaps over three
  stages (`y z` within `x y`, `z` within `y z`, `$V ː?` within `k $V` at a set
  with strings): 9 checks, 1.5 s. With the stages' coverage removed (a
  mutation check, then restored), both overlap checks fail.
- **Cold compile time.** The regular plan's Unicode-mode und_FONIPA-ar
  pipeline took 16 minutes to compile cold, even with
  `-backend-optimization-level=none` (its correction kernel). With the
  entity path, groups 1 and 2 add a few seconds.

**Not yet:**

- the If on contested positions around the stages (§5.4; the stages are
  bounded Advances computed in every block);
- step 4 (before contexts, cycles);
- filters;
- UTF-8 mode.

## 15. Status of step 4 (2026-11-26)

**The closed form (§5.3).** The plan's `closedForm` holds when:

- every before context is the one item of a single state's set
  (`stateEntity`, a set);
- that state's conditions (a) to (c) hold.

Other before contexts (literal characters, `[$]` alone, several sets or
items) are left to the regular plan; the correction loop for them is not yet
written.

`EntityMatch` decides the winning rule at each position twice, once for each
state; the rules with before contexts match only in state 1. Then:

```
state = InText ∧ MatchStar(Advance(C) ∨ [text start, if C has the boundary], D¹)
D¹    = the winners in state 1 that delete one position
wins  = state ? wins¹ : wins⁰          (with their lengths)
```

**Stages and cycles.** With before contexts, the stages come from the pruned
graph (`stagePruned`, `cycleRules`). That graph is exact for the closed form:
within a match, `state` is 0, since the character before an inner position
is a character of the text, not in C, or a deletion within the match, which
state 1 does not reach. So a rule with a before context cannot win there.

The rules on cycles are selected after the stages below them have removed
their covered candidates, in a While loop with two accumulating Vars,
accepted and rejected:

- an undecided candidate within no live (not rejected) candidate is
  accepted;
- the candidates within newly accepted ones are rejected. Only the new bits
  are advanced, so the carry-outs, ORed over the iterations, are the
  interiors of all the matches accepted.

The live test advances a value that is not accumulated. Its carry-in, read
in the first iteration only, may still include the previous block's
rejected candidates. That only delays an acceptance to the second
iteration; a candidate covered by a previous block's accepted match is
rejected in the first by the carry of the accepted. The rules on cycles join
`startLen` after the loop, for the later stages.

The stages, the cycles, the state's set and the rules with before contexts
are in the kernel's signature.

**und_FONIPA-ar** (Unicode mode, `--entity-groups`):

- groups 1 to 3 are applied by their entity plans. Group 3 has the closed
  form for `$Boundary`, 33 class streams and the loop for rule 16
  (`$AVowel ː? ʔ $AVowel ː?`). Group 4 is applied by the regular plan
  (`ووو+`).
- **Corpus:** on 1 MB of `pool-und_FONIPA.txt`, the output is identical to
  the regular plan's and ICU's.
- **Compile time:** the whole run took 5.8 s, compilation included
  (`-backend-optimization-level=none`), against 16 minutes to compile the
  regular plan's Unicode-mode pipeline cold (§14).
- **Targeted text:** 464 lines (68 KB) are identical to ICU's, with the
  default threads and with `-thread-num=4 -segment-size=1`. They include:
  - boundary chains through deletions (` ʔʔa`, ` ʔəa`, ` əʔə`), schwa
    deletion (`kəa`), `ʔɵ̞a`, `ɵ̞ːa`;
  - self-overlap chains (`aʔaʔa`, `aːʔaːʔa`), `iːʔ` before a vowel, a
    boundary and the end, `ʎ̥` before `[^ $IVowel j ʝ]`;
  - chains of 50 to 300 repetitions of `aʔ`, ` aːʔ` and `ʔ` (across
    blocks);
  - 400 random lines of vowels, glottal stops, consonants and boundaries.
- **Mutation checks** (then restored): without the chain of state 1 through
  deletions (`MatchStar`), 78 lines differ; with the cycle's candidates all
  accepted (no loop), 29.

**`QA/xlit_entity_test.py`:** a fourth rules file (`$B {ʔ? $V} → W`,
`$V ʔ $V → XY`, `ʔ → ;`: a closed-form state through deletions and a cycle)
brings it to 12 checks, 2 s.

**Not yet:**

- the correction loop for before contexts outside the closed form;
- an If on contested positions around the stages and the loop;
- filters and bounded regions;
- UTF-8 mode;
- timing (only with your go-ahead).

## 16. Status of UTF-8 mode (2026-11-26)

**The design: the hybrid, not §9's outline.** §9 planned matching on the
bytes, with lengths in bytes. What is built instead uses the hybrid of
doc/masked-transforms-plan.md step 16, which xlit already has
(`selectorGroup`):

- **Matching** is at one position per character. The inputs are computed on
  the bytes and compressed by the UTF-8 index (`classKernel`, and the
  property kernels, then `FilterByMask`).
- **Application** is by selectors spread back to the last bytes of the
  characters and applied by `UTF8_SelectorPipeline`.

`EntityMatch` and its paths, states, stages and loop are therefore the same
kernel in both modes. Lengths stay in characters, and the text is never
decoded to 21 bits.

- **Classes:** a code on the bytes, compressed:
  - the class layer's Div32 code where its criterion holds and it codes every
    string set's characters;
  - otherwise the multiplexed code of the coded sets
    (`cc::MultiplexedAlphabet`, its partition kept in `EntityCoding` to give
    the codes of any union of its classes), as `absolutely` gives the
    hybrid's.

  The StringSet kernels compile over the compressed code bits. The hybrid
  reads no 21-bit basis.
- **Selectors** (`EntityMatch` in selector mode, output `Sel`): at the first
  position of each match applied, the selector of the rule's result (the
  empty string deletes); at the rest of the match, the deletion's selector.
  The strings are `selectorFor`'s.
- **Dispatch:** `UTF8PipelineBuilder::applyAnalysis` applies a step by
  `entityHybridGroup` before the other paths when `entityStepProblem` allows
  it and no filter is active. `utf8StepProblem`, `allUTF8Step` and
  `mixedSegments` take the analysis and whether entities apply (no masks), so
  such a group stays in UTF-8 mode rather than starting a Unicode segment.
  `--plan` reports the modes likewise.
- **Unicode segments:** `unicodeSegment` slices the step-to-group map with the
  steps.

**Checks (UTF-8 mode, `--entity-groups`):**

- **und_FONIPA-ar:**
  - groups 1 to 3 are applied in the hybrid (group 3: a multiplexed code of 7
    bits);
  - the output is identical to ICU's on 1 MB of `pool-und_FONIPA.txt`, on
    the 464 targeted lines of §15 (default threads, and
    `-thread-num=4 -segment-size=1`), and on the 998 group 2 lines of §14;
  - the run takes 3.5 s, compilation included.
- **Other transforms:** the outputs are identical to the regular plan's
  (UTF-8 mode) on 1 MB for de-ASCII, am_FONIPA-am (groups 1, 3, 5, 7, 8 and 9;
  5 has cycles), Latn-Cans, ru-ja, InterIndic-Devanagari and
  und_FONIPA-und_FONXSAMP.
- **`QA/xlit_entity_test.py`:** each rules file in both modes, with
  `--class-layer` on and off: 24 checks, 2.7 s. The trace confirms the path
  and the code (UTF-8 mode: Div32 for the layered file, multiplexed
  otherwise). With the selectors' deletions removed (a mutation check, then
  restored), the 8 UTF-8 checks fail and the 16 others pass.
- `xlit_utf8_mode_test`, `xlit_utf8_masks_test`, `xlit_hybrid_test`,
  `xlit_class_layer_test`.

**Not yet:**

- filters (masks), in both modes;
- bounded regions;
- the correction loop for before contexts outside the closed form;
- an If on contested positions;
- timing (only with your go-ahead).

## 17. Status of filters (2026-11-26)

**Semantics.** Within a filter, ICU applies the rules to each run of the
filter's characters as if the text ended at the run's end, and contexts see
the whole text. `EntityMatch` takes the active mask (the innermost: each mask
lies within those before it), in both modes. A rule's text matches only
within the run of its first position, and the after contexts are not
restricted:

- **Longest match within the run.** A string set or union matches the
  longest member that fits within the run (`longestAt`), not the longest
  overall. With `e̞` whose mark is outside the filter, `$V` matches `e`.
  Within a filter, the StringSet kernels therefore give all the matches by
  length (the spec without its guards), and `EntityMatch` chooses. Unions take
  their operands' matches without exclusion.
- **Optional items.** An optional item is absent where nothing of it fits
  within the run, as `ː?` before an `ː` outside the filter.
- **Unchanged:** the closed form for the before contexts (characters outside
  the filter are never converted), the stages and the loop.
- **UTF-8 mode:** the masks are compressed to the characters for the hybrid
  and restored for the bytes (as `hybridGroup`), and `UTF8_SelectorPipeline`
  keeps them in step.
- **Bounded regions** (`--lines` with groups whose contexts are not
  line-bound) remain excluded.

**A fix to the committed code.** The kernel names had the group label, and a
transform's name may have '/' (uk-uk_Latn/BGN). The object cache could not
create the kernel's file, and the run failed with LLVM ERROR. The labels are
now as DivCode's (`entityKernelLabel`: other than letters and digits, '_').

**Checks (both modes):**

- **Real transforms**, 1 MB each:
  - Cyrl-Latn, Latin-ASCII and uk-uk_Latn/BGN (global filters): identical to
    the regular plan's and ICU's;
  - de-ASCII: Latin-ASCII's group 2, within its filter, invoked by de-ASCII,
    is now applied by its entity plan; identical to the regular plan's and
    ICU's;
  - Bopo-Latn: identical to the regular plan's; both differ from ICU's (as
    before).
- **`QA/xlit_entity_test.py`:** each rules file is also applied within a
  global filter (strings and optional items at a run's end, the second item
  of a rule, the deleted ʔ), as a compound (with `:: Null ;`), against ICU:
  32 checks, 5.4 s. With the run left out of the longest match (a mutation
  check, then restored), the 8 filtered checks fail.
- `xlit_utf8_mode_test`, `xlit_utf8_masks_test`, `xlit_hybrid_test`,
  `xlit_class_layer_test`, `string_set_test`.

**Found in the regular plan (not changed):**

1. **Runs within a filter.** The regular plan does not end the runs as ICU
   does. For the rules `$V = [a e {e̞} i {ɪ̈}]; $V ː? → A; …` under
   `:: [^̞ ː] ;` (with `:: Null ;`), the text `ee̞ː` gives `AA̞ː`
   with ICU and the entity plan, `Ae̞ː` with the regular plan (the `e`
   before the excluded mark is not converted). Three of the four filtered
   rules files differ from ICU this way.
2. **The single-group filter.** In a transform of a single group with a
   global filter (which ICU 74 ignores), the regular plan lets a rule
   `[$A $G] ʼ → U` whose `ʼ` is outside the filter keep `$A ː? → A` from
   applying to the `a` before it. As a compound it applies (as ICU). The
   entity plan applies the same run semantics in both.

**`--lines`:** und_FONIPA-ar group 3's contexts are not line-bound, so with
`--lines` the regular plan applies it (the output is ICU's, in Unicode mode).

## 18. Status of bounded regions (2026-11-26)

With `--lines`, each line is a text of its own. A group whose contexts are not
line-bound (`groupLineBound`) must see the region's edges as the text's: the
regular plan bounds its fused kernels' contexts by the regions
(`FusedRulesConfig::region`: `withinAhead`, `pastEnd`, `beforeStart`).
`EntityMatch` now takes the regions too (`region`, from `boundedRegions()`,
the region-scope mask, compressed to the characters in the hybrid), in both
modes:

- **The text** lies within the active mask (the regions' mask, or a filter
  within it), as before.
- **After contexts:** each item lies within the region (`inRunOf(region, …)`:
  the positions of the items before it are in the region already), and a
  string set takes its longest match within the region. `[$]`, and a set with
  the boundary, holds at the first position past the region (`pastEnd`: not
  in the region).
- **The closed form (§5.3):** the state starts at each region's start if the
  set has the boundary, and after a character of the set only within the
  region (`Advance(C) ∧ Advance(region)`). The deletions of the chain lie in
  the regions.

`entityStepProblem` no longer excludes bounded regions, so with `--lines` such
groups stay in UTF-8 mode in the mixed segments.

**Checks** (both modes, against ICU line by line):

- **und_FONIPA-ar `--lines`** on the 464 targeted lines of §15: groups 1 to 3
  are applied by their entity plans (before, the regular plan applied group 3:
  its contexts are not line-bound).
- **Cyrl-Latn `--lines`** on 1 MB of `nat-ru.txt` (a global filter within the
  regions).
- **A rules file whose results depend on the edges:** `$S = [ $]` (a line
  feed is not in it) as a closed-form state, `a } [$]` and `a } [ $]`.
  With the region ignored (a mutation check, then restored), 10 of its 13 lines
  differ from ICU's. It is in `QA/xlit_entity_test.py` (34 checks, 5.8 s
  cold: a little over the 5 s of the test-time limit).

## 19. Status of the correction loop, and the default (2026-11-26)

**The correction loop.** Before contexts that the closed form does not give
(literal characters, `[$]` alone, several items, several sets, or a set
failing (a) to (c)) are matched against the converted text by a loop over the
cursors of the group (`EntityStates`, in `EntityMatch`).

- **Classes and states.**
  - The items of the before contexts are sets: a set, a literal character, or
    `[$]` (no characters, the boundary).
  - A character's class is the vector of the sets containing it. The start of
    the text (or region) has the vector of the sets with the boundary.
  - The state at a cursor is the classes of the last k characters of the
    converted text (k: the most items of a before context), at most 64 states
    (`EntityMaxStates`).
- **Transitions.** In each state, the winner at each position is the first
  rule whose text and after context match (statically) and whose before
  context holds in that state. A winner goes to the position after its match,
  in the state shifted by the classes of its result (unchanged by a
  deletion). No winner goes to the next position, shifted by the character's
  class.
- **Known positions.** These are the starts of the text or regions, and the
  positions after k positions that no rule's text can cover. Those are never
  converted, so the position after them is a cursor in the state of their
  classes.
- **The loop.** A While loop with accumulating Vars (`reach[σ]`, `done`)
  expands the new cursors along the transitions. Each iteration does one
  Advance per (state after, length), over precomputed transition streams. The
  carries are those of the pending/accumulating pattern, so block boundaries
  are handled.
- **Selection** is the loop's: a match applies where it wins at a cursor in
  its state. So overlaps, stages and cycles need nothing more in these groups.

The closed form remains the path where it applies; it needs no loop.

**The boundary in a text.** As ICU, a set of a text with the text boundary
(`[^c]`, or `[$]`) also matches nothing at the end of the text, or of a run
of a filter or region (`runEnd`), as `xlit_filter_run_end_test` requires. The
plan no longer excludes `[$]` in a text.

**`--entity-groups` is on by default.** `xlit_divcode_test` and
`xlit_class_layer_test`, which check the kernels of the other paths, run with
`--entity-groups=0`, and so do the regular-plan runs of `xlit_entity_test`.

**Checks:**

- **A rules file for the loop**, against ICU in both modes: `a {b}`,
  `[$] {d}`, `a c {e}`, `[aeiou] {n}`, `[kt] {n}`, with `x → ;`, `q → a`,
  `w → ka`, on 300 random lines and chains of deletions of 64 to 500
  characters (across blocks). It matches with 4 threads and segments of one
  stride, with `--lines`, and within a filter. With the results' classes left
  out of the transitions (a mutation check, then restored), 280 lines differ.
- **und_FONIPA-fa:** group 3, whose before contexts the closed form does not
  give, is applied by the loop. On 1 MB of `pool-und_FONIPA.txt` the output
  is ICU's, in both modes (3.4 to 5.2 s with compilation).
- **`QA/xlit_entity_test.py`:** the loop's rules file in both modes, with the
  class layer on and off, and within a filter: 42 checks.
- **The xlit and ldml tests:** all 55 pass with the entity groups on by
  default. Before the boundary fix, `xlit_filter_run_end_test` failed, and the
  two path tests needed `--entity-groups=0`.
- **und_FONIPA-ar and -fa with the defaults**, against ICU: 1 MB each, and the
  464 targeted lines.

## 20. Rule Ifs (2026-11-26)

**The problem.** The survey of 2026-11-26 showed `EntityMatch` slower than
the regular plan on groups of thousands of rules (Hant-Hans 0.38 → 1.92 s,
Jpan-Latn 2.8 → 7.7 s, the Ethiopic Geminate transforms 0.2–0.6 → 4.5–7 s),
and slightly slower on most other transforms (median +13 %). `EntityMatch`
evaluated every rule at every block, where the fused kernels test each rule
only within an If on the Div32 block of its first characters (the class
layer's leaves).

**The leaves.** With the class layer's Div32 code, `entityRuleLeaves` places
each rule by the blocks of its first set (`firstSets`):
- in the leaf of each of its blocks, if it has at most
  `ClassLayerMaxSetBlocks`;
- at the top otherwise: a first set with more blocks or none, a text that may
  be empty, or a leading item that is or contains the text boundary (a match
  of zero width at the end of a run, whatever the character there).

At a position, only the rules of its character's leaf and those at the top
may match.

**The kernel.** `EntityMatch` builds an If on each leaf's block (the class
`classAt(block)` from the code's high bits). Within it:
- the rules of the leaf and those at the top (matched outside the Ifs) are
  taken in order;
- each wins where it matches and no earlier one does.

At the positions of no leaf, the rules at the top are taken alone.

**The accumulated values.** Vars, created at first use, collect the results
over the leaves, ANDed with the leaf's condition:
- A rule that applies wherever it wins (stage 0, on no cycle) adds its
  results directly: its matches by length (`startLen`), and its W, Ins and
  deletion bits (or its selector bits in the hybrid).
- The other rules accumulate their wins, rule by rule, for the selection by
  stages and cycles, which runs after the Ifs as before.

The memos of the class, length and lookahead expressions are keyed by scope,
so no expression of one If's body is used in another's. The kernel's
signature carries `#ifs`.

**Not covered.** Groups with before contexts (the closed form and the
correction loop compute their states on all the rules' matches) and groups
without the Div32 code stay flat. `--entity-rule-ifs=0` turns the Ifs off.

**Tests.**
- `QA/xlit_entity_test.py` checks for the Ifs in the trace (`rule Ifs (n
  leaves, m rules at the top)`) where the code is Div32. Its layered case has
  a rule at the top, `[Ā-̠] ʼ`, of 18 blocks, before the IPA rules
  it competes with.
- Latin-ASCII (63 leaves) and InterIndic-Bengali (6 leaves): the output on
  1 MB of their survey corpora is the same with and without the Ifs, and the
  regular plan's.
