# Multiplexed character classes in the regular expression engine (plan)

## Goal

With full Unicode indexing, every RE kernel, every class stream and every
reversal works over the 21 streams of the Unicode basis (`u21`), although an RE
distinguishes only a few classes of characters. A **multiplexed alphabet**
partitions the codepoints into the *exclusive sets* of the CCs of an RE (the
sets of characters that belong to exactly the same CCs), numbers them, and
represents the text by the `k = ⌈log2(n + 1)⌉` bit streams of those numbers
(code 0: the characters of no CC). Each CC of the RE is then a union of codes,
compiled over `k` streams. For `\p{L}+\d` there are three exclusive sets
(letters, digits, other) and `k = 2`.

The aim is to use multiplexing wherever the engine uses full Unicode indexing,
forward and in the reverse modes (starts and lookaheads by reversal, the
lookbehinds matched forward for them, and the capture walks back from match
ends), so that

- the text is reduced to `k` streams once (from the UTF-8 basis, without
  decoding to `u21`), and filtered to characters as `k` streams rather than 21;
- each CC in each kernel is a small Boolean function of `k` bits rather than a
  Unicode class over 21 bits;
- each `RegionReversal` reverses `k` streams rather than 21, with `k × L`
  rather than `21 × L` bits of state and lookahead;
- the barriers of a reversal (the characters no match can contain) are
  code 0, a NOR of the basis, rather than a separate class kernel.

## State of multiplexing support today

### What exists

| Piece | Where | Status |
|---|---|---|
| `cc::MultiplexedAlphabet` | `re/alphabet/multiplex_CCs.h`, `lib/re/adt/alphabet/multiplex_CCs.cpp` | breakpoints → exclusive sets → `k` basis CCs (`getMultiplexedCCs`); codes numbered 1, 2, … in codepoint order of first appearance |
| `MultiplexedAlphabet::transformCC` | same | any union of exclusive sets, including code 0 (milestone 1); before, only a CC *in the collected set*, any other was `llvm_unreachable` |
| `re::transformCCs` | `re/transforms/re_multiplex.h` | an `RE_Transformer` replacing the CCs of the source alphabet by their codes |
| `re::collectCCs` | `lib/re/analysis/collect_ccs.cpp` | the CCs of one alphabet, optionally within the definitions of names |
| several alphabets in one RE | `RE_Compiler::addAlphabet`, `RE_CompilerContext::addAlphabet`, `RE_Kernel` | each alphabet used by an RE is bound as `<name>_basis`; its shape is in the kernel signature; CCs print with their alphabet name (`mpx_1,3`) |
| length and validation | `getLengthRange`, `FixedUTF8Validator` (`re_analysis.cpp`) | a multiplexed CC has the length of its source alphabet |
| `CharClassesKernel` | `kernel/unicode/charclasses.h` | computes the `k` basis streams of an alphabet from the UTF-8 basis (`UTF_Compiler`, marked at final bytes) or from `u21` |

### Where it is used

- **Boundary properties only** (`UnicodePropertyLogic`, `regexp_engine.cpp`):
  `\b{g}` and `\b{w}` multiplex the classes of their boundary expressions
  (`b_mpx`) and match over that alphabet. This is the one live use.
- **tconv** multiplexes its large mapping subgroups itself (02e77ee59), outside
  the engine.

### What is disabled or lost

- `RE_PipelineBuilder::ensurePrepared` keeps the multiplexed Unicode path as
  dead code (`useMultiplexedUnicode = false`). Its condition refers to
  `hasCodepointReference`, which no longer exists. It was carried over from
  `GrepEngine` in 3a6507545, which dropped the earlier `MultiplexedExternal`
  machinery and the exclusion of external symbols (65ae2faeb, 73ca7209d).
- `-UnicodeIndexing` is described as "Enable CC multiplexing and Unicode
  indexing" but only sets `unicodeIndexingOverride`.
- `doc/re-engine-modes-plan.md` lists `collectCCs` / multiplexing as
  "(disabled)" in its tables.

### What the reverse modes do now (milestone 9)

- `reversedStarts` reverses `mCtxt.mCodeUnitStream`, the whole `u21` basis
  (`RegionReversal` over 21 streams), and builds the backward
  `RE_PipelineBuilder` over the reversed `u21`.
- `reversalBarriers` runs a `CharClassesKernel` over `u21` for the
  complement of `matchableCodepoints(re)`.
- The lookbehinds matched forward (`LBfwd_`) run a forward builder over `u21`,
  and each has its own reversal kernel.
- Nested reversal (a lookahead by reversal within the backward pass) reverses
  the reversed `u21` again.
- Capture walks (`BackwardWalkStreams`, `CaptureBoundaries`) compile their
  classes over `u21` with `Parabix_CC_Compiler_Builder`, and already reverse
  only the derived streams.
- The open item in the modes plan: "reversal of only the streams the reversed
  RE needs (rather than the 21-bit basis)… one reversal shared by the uses of
  a pipeline". Multiplexing answers the first half directly.

### Every place the Unicode basis is consumed

A multiplexed context must replace `u21` at each of these, or exclude the RE:

| Consumer | Site | With a multiplexed basis |
|---|---|---|
| `RE_Kernel` | all matching | CCs → codes over `k` bits |
| `CharClassesKernel(ccs, mCodeUnitStream)` | `classStream`, `applyModePasses` (string-class reps), `reversalBarriers`, property bases | classes of codes over `k` bits |
| `StringClassKernel` | chains, string-class reps (`codeUnitBasis()`) | CCs → codes |
| `BackwardWalkStreams`, `CaptureBoundaries` | capture walks | CCs → codes |
| `RegionReversal` | `reversedStarts`, `captureBoundaries` | `k` streams |
| `ExtendByOne` | reversed text without regions | `k` streams |
| `UnicodePropertyLogic`, `UnicodePropertyKernelBuilder` | property externals, boundaries | fold into the partition, or compute from the UTF-8 basis |
| `CodePointMatchKernel`, `FixedDistanceMatchesKernel`, `matchDistanceCheck` | references, codepoint properties | **need codepoint identity: not multiplexable** |
| S&R output | `replaceMatches` (`text = mCtxt.mCodeUnitStream`) | needs `u21` (or UTF-8) for the output only |

## Design

### 1. A complete partition: any union of exclusive sets

`transformCC` should accept every CC that is a union of exclusive sets, not
only the collected ones. Passes and pipeline functions create CCs that were
never collected: `unionCC`, `firstCharClass`, complements (barriers, negated
lookaheads), the classes of string-class runs and chain segments. All of these
are Boolean combinations of collected CCs, so they are unions of exclusive
sets, possibly including code 0.

- Keep the partition as an interval map (codepoint range → code), from the
  breakpoints already computed.
- `codes(cc)`: the codes of the ranges of the partition within `cc`; an error
  if a range is partly within it (the CC is not expressible).
- Code 0 becomes an ordinary code (the characters of no collected CC), so that
  complements and `Any`-like classes are expressible.
- `expressible(cc)` for preconditions, and a hash of the partition (its
  breakpoints and codes) for signatures.

### 2. Multiplexing at the kernel boundary

Keep every RE, and every analysis and transformation, in the Unicode alphabet.
The analyses of the modes plan (`lengthClasses`, chains, unique prefixes,
`matchableCodepoints`, capture walks, `reverse`) compare and combine Unicode
CCs; rewriting the RE to codes early would break them.

Instead, the code-unit context of a pipeline gets an optional multiplexed
alphabet and its `k`-stream basis (`RE_CompilerContext::setCodeUnitContext(mpx,
basis)`, with `mCodeUnitAlphabet` the alphabet), and the CCs are translated
where a kernel is made:

- `RE_Kernel`: `transformCCs(mpx, re, TransformDefinition)` in the constructor,
  before the signature, so that the printed RE (codes such as `mpx_1,3`) and
  the basis shape determine the kernel exactly and the kernel cache stays
  correct. (7a72f8359 was a cache bug from two multiplexed REs that printed
  the same over different alphabets. Translating at the kernel boundary means
  the codes, not the source CCs, are in the signature.)
- One helper, `RE_PipelineBuilder::compileClasses(ccs, out)`, replaces each
  `CharClassesKernel(ccs, mCodeUnitStream)` site: over a multiplexed basis it
  makes a class kernel of codes (the `Parabix_CC_Compiler_Builder` path of
  `ByteClassesKernel`, over `k` bits); otherwise it makes the current kernel.
- `StringClassKernel`, `BackwardWalkStreams` and `CaptureBoundaries` take the
  alphabet with the basis and translate their CCs. They already compile with
  `Parabix_CC_Compiler_Builder` over a stream set.
- Nested builders (`backward` and `forward` in `reversedStarts`, the boundary
  builders) inherit the alphabet with the basis, instead of
  `RE_context{&cc::Unicode, …}`.

### 3. When to multiplex (a precondition, decided once)

The partition is built in `ensurePrepared` from the prepared RE (after
`resolveModesAndExternalSymbols` and `regular_expression_passes`), with
`NameProcessingMode::ProcessDefinition`, so that the CCs of lookaheads,
lookbehinds and other named externals are included. Mode passes only combine
those CCs, which §1 makes expressible. Decide it like the other mode decisions:
in the mode (`RE_Mode`, from `determineREMode`), with a reason when it does not
apply.

Not multiplexed:
- back-references and fixed-distance references, and codepoint properties
  (they compare codepoints, not classes);
- `k` too large: more than `--multiplexed-stream-limit` streams (default
  8, see "Automatic choice" below), where the cost of computing the basis
  from UTF-8 (below) may exceed decoding to `u21`;
- `unicodeBasisOverride`, and a new `--re-multiplex=off`.

S&R with references in the replacement keeps `u21` (or UTF-8) for the output
only and still matches, walks and reverses over the multiplexed basis.

Boundaries `\b{g}` and `\b{w}`: first compute them as today, from the UTF-8
basis with the index stream, as externals of the multiplexed pipeline. Later,
fold their `b_mpx` classes into the main partition so they use the same basis
(§5, milestone 5). In reverse, a boundary is a property of the text, not of a
direction. It must be computed forward and reversed as an external, as the
lookbehinds matched forward are. Check what the backward pass does with
boundaries today before multiplexing them (open question).

### 4. Forward pipeline

In `ensurePrepared`, for full Unicode indexing:

```
UTF-8 basis (8) ─CharClasses(mpx basis CCs)─▶ k, at final bytes ─FilterByMask(u8index)─▶ k per character
```

in place of

```
UTF-8 basis (8) ─UTF8_Decoder─▶ 21 ─FilterByMask(u8index)─▶ 21 per character
```

The cost of the first kernel is the UTF-8 compilation of the `k` basis sets.
Each is the union of the exclusive sets whose code has that bit, so the
numbering of codes matters (§6). For csvgrep, where the source is already
`u21`, the basis is computed from `u21` (a `CharClassesKernel` over 21 bits to
`k`). That still pays off when the RE is matched by several kernels or by
reversal.

Because full Unicode indexing costs much less this way, the mode decision
needs another look: today the MatchStarts and S&R modes fall back to "byte or
full Unicode only", and the reverse modes force Unicode indexing. With
multiplexing those REs pay `k` streams rather than 21. `useIndexedUTF8` stays
the default for MatchEnds until measured (milestone 6).

### 5. Reverse pipelines

`reversedStarts` over a multiplexed basis:

1. **Barriers.** The complement of `matchableCodepoints(re)` is a union of
   exclusive sets, so it is expressible. When the matchable characters are
   exactly the union of the collected CCs (no `Any`, no Diff right-hand side
   adding CCs), the barriers are code 0: a NOR of the `k` streams, ORed with
   the region follows, in the kernel that needs them, with no class kernel.
2. **One reversal of `k` streams.** The basis, the forward anchor marks
   (`ForwardAnchorMarks`) and the ends of the lookbehinds matched forward
   (`LBfwd_`) all share the same barriers. Reverse them in **one**
   `RegionReversal` over their concatenation (`k + 2 + #lookbehinds`
   streams) rather than one kernel each. This is the "one reversal shared by
   the uses of a pipeline" item of milestone 9.
3. **Backward pass** over the reversed multiplexed basis, with the same
   alphabet. `reverse(R)` has the same CCs as R, so there is no new partition.
   `ExtendByOne` for a text without regions extends `k` streams.
4. **Lookbehinds matched forward** use the forward multiplexed basis.
5. **Nested reversal** (`LArev_` within the backward pass, or a lookahead by
   reversal within a lookahead) reverses the reversed `k` streams with the same
   alphabet. The partition is built once for the outermost RE, from the CCs of
   all its assertions.
6. **Capture walks**: `BackwardWalkStreams` and `CaptureBoundaries` compile
   their run classes over `k` bits. Their reversal is unchanged; it already
   carries only derived streams.

With `k` streams, the state and lookahead of each reversal shrink by `21 / k`.
The same memory then allows a larger `--reverse-region-limit`.

### 6. Code assignment

Codes are now numbered in codepoint order of first appearance, so bit 0
alternates between neighbouring sets and the basis CCs are fragmented. Two
costs depend on the numbering:

- computing the basis from UTF-8: each basis CC should have few ranges, and
  ideally separate ASCII, 2-, 3- and 4-byte ranges;
- compiling each source CC over `k` bits: its codes should form few subcubes
  or ranges.

Heuristics to try, with measurement: give the largest sets, such as
"other" and the big properties, codes that keep the basis CCs coarse; give the
codes of each frequently used CC a contiguous range (a consecutive-ones
ordering where one exists); give code 0 to the most frequent set. Choose
between the multiplexed basis and `u21` by an estimate: the UTF-8 compilation
cost of the basis CCs against the fixed decode-and-filter of 21 streams, plus
the number of kernels and reversals that would consume the basis.

## Milestones

1. **Partition.** Implement the interval map, `codes(cc)` for any union of
   exclusive sets (with code 0), `expressible(cc)` and the partition hash
   (§1). Replace `llvm_unreachable`. Add a unit test of unions, complements
   and code 0 against `UnicodeSet`, which must run in under one second.
2. **Kernel boundary.** Make the alphabet with its basis part of the code-unit
   context. Translate CCs in `RE_Kernel`. Add `compileClasses` for all
   `CharClassesKernel(…, mCodeUnitStream)` sites. Take the alphabet in
   `StringClassKernel`, `BackwardWalkStreams` and `CaptureBoundaries` (§2).
   This changes no behaviour, because nothing multiplexes yet.
3. **Forward multiplexing.** Add the precondition (§3), and replace the dead
   path in `ensurePrepared` (§4) behind `--re-multiplex=off|on|force`
   (default `off`). Exclusions report their reason. Run greptest,
   `match_starts_test`, `replace_test` and `capture_spans_test` with `force`
   and compare with `off`.
4. **Reverse multiplexing.** Implement §5: barriers from the codes, one shared
   reversal, the backward and lookbehind passes and nested reversal over the
   alphabet, and capture walks. Run `region_reversal_test`,
   `reversed_lookahead_test` and the reversal groups of `match_starts_test`
   and `re_preconditions_test` with `force`.
5. **Boundaries and properties.** Fold the `\b{g}` and `\b{w}` classes into
   the main partition. Make property externals CCs of the partition. Settle
   boundaries under reversal.
6. **Cost model and default.** Code assignment heuristics (§6) and the
   choice of multiplexed or `u21` by estimate. Revisit `useIndexedUTF8` and
   the "full Unicode only" fallbacks of the code-unit goals. Measure
   per-kernel times on property-heavy, case-insensitive, starts-by-reversal
   and lookahead-by-reversal REs over a large UTF-8 text. Turn on `auto` by
   default. Update the `collectCCs` row of the modes plan and the
   `-UnicodeIndexing` description.
7. **Optional: byte mode.** Multiplex the UTF-8 code-unit CCs of `toUTF8`'d
   REs, computed from the `i8` source by `ByteClassesKernel`
   (`Direct_CC_Compiler`). Use it when `byteTestsWithinLimit` fails, in place
   of `S2P`. Adopt it only if measurement shows a gain.

## Open questions

- Boundaries in the backward pass: are `\b{g}` and `\b{w}` within an RE matched
  by reversal computed forward and reversed, or over the reversed basis? (Their
  rules are not symmetric.)
- Whether one partition per pipeline (all REs and externals of a grep run,
  including the lines-mode externals) beats one per RE: a larger `k`, but one
  basis kernel and one filter.
- The threshold on `k`, and whether a partition can be split: two alphabets of
  `k/2` bits each for disjoint parts of an RE, both bound to the kernel
  through `addAlphabet`.

## Milestone 1 status

- `cc::MultiplexedAlphabet` keeps the partition of the codepoints
  (`getPartition`: the ranges in increasing order, each with its code,
  covering all codepoints, code 0 included), built with the basis CCs by
  `doMultiplexCCs`. The numbering of codes is unchanged (code 0 for the
  codepoints of no CC, then codepoint order of first ranges), so the basis
  CCs, and the `\b{g}` and `\b{w}` boundaries that use them, are as before.
- `codes(s, codes)`: the codes of the exclusive sets whose union is the
  set `s`, by a merge of the ranges of the partition with the intervals of
  `s`. It is false if a range is partly in `s`, or if the ranges of one
  exclusive set disagree (an exclusive set may have several ranges).
  `expressible(s)` is the same test. `codeOf(cp)` is the code of one
  codepoint.
- `transformCC` takes any CC of the source alphabet that is expressible:
  unions, intersections and complements of the collected CCs, with code 0
  where they include the codepoints of no CC. A CC that is not expressible
  is a fatal error naming it, in place of `llvm_unreachable`. The
  per-CC lists of exclusive sets (`mExclusiveSetIDs`, `findTargetCCIndex`)
  are gone.
- `getPartitionSignature()`: the number of codes and the ranges with their
  codes, for kernel signatures (milestone 2). It does not depend on the
  order or the names of the CCs.
- `getSize()` is the number of codes, code 0 included (as before). An
  alphabet of no CCs has one code (0) for all codepoints and no basis CCs.
  Its source alphabet defaults to Unicode (it was uninitialized).
- Test: `multiplexed_alphabet_test` (`tests/multiplexed_alphabet.cpp`, new:
  fixed cases of codes, unions, complements, code 0, inexpressible sets,
  exclusive sets of several ranges, the ends of the codepoints and
  signatures; random CCs with their unions, complements, intersections and
  random sets checked against a direct computation of the codes from the
  basis CCs; 0.03 s). The full ctest suite passes.

## Milestone 2 status

- `RE_CompilerContext::setMultiplexedCodeUnitContext(mpx, basis)`: the code
  units are characters of the source alphabet of `mpx` (the code-unit and
  length alphabets stay that source, Unicode, so the analyses, mode passes
  and checks of the engine are unchanged), represented by the basis of its
  codes (`mMultiplexed`, bound to RE kernels as `<name>_basis`).
- `RE_context` accepts a `cc::MultiplexedAlphabet` of Unicode as its
  encoding: the source is then the basis of its codes, used as a Unicode
  basis is (`ensurePrepared`). `modeFailures` takes such an encoding as
  Unicode. This is how the client of milestone 3 (and the test below) gives
  a multiplexed basis; nothing in the engine multiplexes yet.
- Translation at the kernel boundary:
  - `RE_Kernel` translates the CCs of its RE to codes before its signature
    (`CodeUnitMultiplexer`): within the definitions of the names that are
    not externals, which the RE compiler compiles from their definitions,
    keeping the externals as names. The RE compiler is given the
    multiplexed alphabet as code-unit alphabet.
  - `RE_PipelineBuilder::compileClasses` replaces the `CharClassesKernel`
    sites (`classStream`, the named CCs of `applyModePasses`,
    `reversalBarriers`): over a multiplexed basis, a
    `MultiplexedClassesKernel` of codes (`Parabix_CC_Compiler_Builder` over
    the `k` bits); otherwise the kernel as before, over the same basis.
  - `codeUnitCC` and `codeUnitStrings` translate the CCs given to
    `StringClassKernel` (string-class reps, chains, final stars), and the
    run classes of capture walks (`BackwardWalkStreams`, `CaptureBoundaries`,
    whose kernel names now include the number of basis streams).
  - Nested builders (the backward pass and the lookbehinds matched forward
    of `reversedStarts`) are given the same encoding (`codeUnitEncoding`),
    so the reversed basis is that of the codes.
- Not with a multiplexed basis (fatal errors naming the reason): property
  externals (`compileProperty`), references (`processReferences`), and the
  text of a replacement (`replaceMatches`), which need the codepoints. The
  precondition of milestone 3 is to keep such REs on the Unicode basis.
- `codeUnitBasis` transposes a byte stream by its field width (not its
  number of streams), so that a basis of one code bit is not taken for
  bytes.
- Test: `multiplexed_engine_test` (`tests/multiplexed_engine.cpp`,
  `multiplexed_engine_test.py`, new): 19 patterns in 5 groups, each compiled
  in one pipeline over the 21-bit basis and over the codes of the
  multiplexed alphabet of its CCs (as prepared, with the definitions of
  names), the results compared at every position. Covered: ends, case
  folding, lookaheads by reversal (positive and negative), starts by
  reversal, anchors (with and without regions), lookbehinds matched forward,
  negative lookbehinds within lookaheads by reversal, length classes, string
  classes, lookahead chains, and capture spans with walks back from the
  match ends. Each pattern must have results, and a corrupted code basis is
  detected. 0.2 s with the object cache. The full ctest suite passes.

## Milestone 3 status

- `--re-multiplex=off|on|force` (`re::Multiplexing`, default `off`, with the
  code generation options): `on` multiplexes where the RE allows it and its
  codes need at most 12 bits; `force` wherever the RE allows it. (`on` is
  now `auto`, see "Automatic choice".)
- `ensurePrepared`, full Unicode indexing from a UTF-8 source: the dead
  path is replaced. The codes are those of the multiplexed alphabet of the
  CCs of the RE as prepared (with the definitions of names, so with the CCs
  of its assertions), computed from the UTF-8 basis at the final code
  units of the characters (`CharClassesKernel` of the basis CCs) and
  filtered by the UTF-8 index (`k` streams), in place of `UTF8_Decoder` and
  the filtering of 21 streams. Every builder of the pipeline (reversal,
  lookbehinds matched forward, lookaheads by reversal) uses these codes (see
  milestone 2). A source already of Unicode (csvgrep, tconv) and the
  UTF-8 modes are unchanged.
- Not multiplexed, with the reason (`RE_PipelineBuilder::
  multiplexingFailure`, and printed with `-ShowREs`, as is the number of
  codes and bits otherwise): the option off, `unicodeBasisOverride`, the
  text of a replacement (`searchReplacePipeline`; `captureSpansPipeline`
  is multiplexed), properties and boundaries compiled from the codepoints,
  references, CCs of other alphabets, names without definitions, an RE
  without CCs, and, with `on` (now `auto`), codes of too many bits.
- Comparison with `off`: with `TEST_FLAGS=--re-multiplex=force`, every test
  of the engine passes, greptest included (the 6 failures are the test
  binaries that do not link the RE options, which reject the flag). On
  random valid UTF-8 text, icgrep gives the same lines with and without
  multiplexing.
- **Invalid UTF-8.** The two bases differed on invalid UTF-8: the 21-bit
  decoder gave the bits of an ill-formed prefix to the characters after it
  (fixed in `UTF8_Decoder`), and the UTF-8 classes and the decoder gave
  ill-formed input different values. Ill-formed input now has defined
  semantics, `--utf8-errors=abort-on-error|drop-bad-characters|
  use-replacement-char` (`codegen::UTF8Errors`, default
  use-replacement-char): each maximal ill-formed subpart (Unicode
  Standard 3.9) is a U+FFFD, is dropped, or stops the input before it
  (`kernel::UTF8_structure`, `UTF8_index`, `UTF8_InitialMask`,
  `UTF8_ErrorMarks`, `UTF8_Decoder`). The multiplexed codes give a subpart
  the code of U+FFFD (`CodeAtMarks`). Both bases then agree with Python's
  decoding (`utf8_errors_test`).
- Tests: `match_starts_multiplexed_test`, `capture_spans_multiplexed_test`
  and `reversed_lookahead_multiplexed_test` run the tests of the same
  names with `--re-multiplex=force` (each under a second with the object
  cache). The full ctest suite passes.

## Milestone 4 status

- The reverse pipelines run over the codes since milestones 2 and 3: the
  backward pass, the lookbehinds matched forward and nested reversal take
  the encoding of the forward pass (`codeUnitEncoding`), so the reversed
  basis is the `k` code streams; capture walks compile their run classes
  over the codes; the barriers are a class of codes over the `k` bits
  (`compileClasses`, then OR with the region follows).
- One reversal for the streams of the forward pass (`reversedStarts`): the
  code units, the anchor marks (`ForwardAnchorMarks`) and the ends of the
  lookbehinds matched forward share the barriers, and are reversed by one
  `RegionReversal` of their concatenation (`streamutils::Select`), then
  taken apart. Only the reversal back of the ends remains separate (it
  depends on the backward pass).
- Measured (`-EnableCycleCounter`, one thread, 65 MB of mostly ASCII
  text in lines of words), in
  thousands of cycles:

  | RE | 21-bit, separate | codes, separate | codes, shared |
  |---|---|---|---|
  | `x(?=[a-c]*(?<=c(?:a*b)*)d)` (lookbehind) | 43200 | 28127 | 20645 |
  | `x(?=(?:a*b)*c$)` (anchor) | 43325 | 26278 | 19198 |
  | `x(?=(?:a*b)*c)` | 33370 | 17793 | 17793 |

  `RegionReversal` dominates: about 6100 per kernel plus about 830 per
  stream (21 streams 23367, 3 streams 9276, 1 stream 6861). Its fixed
  cost, per position, comes from its bit-at-a-time loops
  (`parabix_reverse_regions`): the barriers are scanned and the bits of
  each region copied one at a time. A word-at-a-time reversal is the next
  gain, larger now than that of the stream count.
- The barriers in one kernel with the follows: not done, as both kernels
  are below 100 in these measurements.
- Tests: the reversal groups of `match_starts_test`,
  `reversed_lookahead_test` and `capture_spans_test` and their
  `_multiplexed` variants (anchored REs and lookbehinds matched forward
  included). The full ctest suite passes.

## Automatic choice

- `--re-multiplex=off|auto|force` (default `auto`): `auto` replaces
  `on`. It multiplexes unless the RE needs the full 21-bit basis (the
  reasons of milestone 3: references, which compare codepoints; properties
  and boundaries compiled from the codepoints; the text of a replacement;
  CCs of other alphabets; names without definitions; no CCs), and then only
  if its codes need at most `--multiplexed-stream-limit` streams
  (`re::MultiplexedStreamLimit`, default 8); otherwise the 21-bit basis is
  used. `force` ignores the limit.
- The decision is printed with `-ShowREs` ("Multiplexed: n codes, k
  streams", or "Not multiplexed: " and the reason).
- Test: `multiplex_auto_test` (`QA/multiplex_auto_test.py`, new: the
  decisions for a small RE, the same with a limit below its streams, the
  option off, a reference, a property, and an RE of 600 single-character
  CCs (10 streams) with `auto` and `force`; the lines selected agree with
  `off`; 0.2 s).
