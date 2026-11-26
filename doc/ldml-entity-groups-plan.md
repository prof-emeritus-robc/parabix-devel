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
  - where the group is multiplexed, also the characters of the strings and the
    single-character members of the string sets.

  The kernel outputs one stream per class, or above a threshold, the bits of a
  multiplexed code;
- each **set with strings**, a `$variable` or an inline set such as
  `[{m̥} m ɱ]`: a new `StringSet` kernel, giving what longest-leftmost matching
  of the set needs (§4). On a multiplexed group, its REs are over the code.

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

### 3.1 The group's classes: per-class streams or a multiplexed code

The class set of a group is:

- its pure sets (`$variables` and inline);
- its literal characters;
- the characters of the strings of its string sets, each a singleton class;
- the single-character parts of the string sets.

Properties stay external, as kernels of their own, and are not part of the
class set.

- **At or below the threshold** (`--entity-mux-threshold=N`, the number of
  distinct classes): the `CharClassesKernel` outputs one stream per class,
  except for the characters of strings and the single-character parts of
  string sets, and the StringSet kernels read the U21 basis.
- **Above the threshold:** the classes are multiplexed with
  `cc::MultiplexedAlphabet` (`multiplexedClasses` in xlit, as for the hybrid
  groups' `classCodes`).
  - The kernel outputs the k bits of the code of each character's exclusive
    class, with 0 for characters in no class. Each class is then the CC of its
    codes over those k bits.
  - Each string character is a singleton class in the input, so it has a code
    of its own.
  - The consumers (StringSet, EntityMatch) compile their classes over the k
    code bits instead of the 21 basis bits.
  - A union of pure sets is the union of their codes, not an OR of streams.

The threshold is structural, like the class-layer criterion: the number of
classes, and so the number of streams that the consumers read, against the k
code bits. It is not chosen by timing. Initial value: 16. Step 0 reports
the counts per group. For group 3, an estimate: about 35 pure sets and
literals, plus about 20 characters within strings. That gives about 60–80
exclusive classes and a code of 7 bits, against 21 basis bits.

The Div32 code of the class layer (doc/ldml-divcode-plan.md §13.14) is another
such code. The multiplexed code is used here because its classes are exact
codes (each class a union of whole codes), which a StringSet's literals need.
The Div32 alternative can be evaluated later, as a different code behind the
same interface.

**Eligibility (Unicode phase 1).** A group is eligible if:

- every text and context item is a set (any kind), a literal, or `X?` of one;
- `Tmax` ≤ 16;
- there are no cursors, revisits, captures, references, function calls or `+`/`*`;
- every converted before context is handled by §5.3, or the group falls back
  to the selection loop (§5.4).

**Report:** `xlit --entity-report` prints one JSON object per group: entities
by kind, with members, lengths and uses; rules with their item kinds; overlap
depth (§5.4); eligibility or problems; estimated kernel and stream counts.
Running it on all transforms tells how far the plan reaches beyond
und_FONIPA-ar.

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
  - **The group's multiplexed code** (above the threshold): the kernel is
    given the structure with each character and set replaced by its codes in
    the `MultiplexedAlphabet` (`transformCC`).
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
- **Over a multiplexed code:** the hash also covers the code assignment (the
  transformed CCs). Groups with the same classes share the kernel, and others
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
     │   StringSet $IVowel, $UVowel, …, inline_n (U21, or Cls when multiplexed) ── Lₖ per set
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
     `--entity-mux-threshold` at 0 (always multiplexed) and at a large value
     (never), so that both class paths are covered.
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
   and the literal characters.** Above a threshold of classes, it outputs a
   multiplexed code, and the string characters are classes of that code, over
   which the StringSet kernels are compiled (§3.1).
3. **Unions of string sets are derived** in the consumer (`$Vowel`,
   `[j $IVowel]`), not given StringSet kernels of their own.
4. **Group 4 stays on the regular plan** in phase 1.

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

`--entity-mux-threshold` (default 16) sets the threshold for multiplexing.

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
