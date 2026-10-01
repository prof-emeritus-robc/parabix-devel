# Parabix Tokenizer

Parabix technology is a high-performance programming framework for streaming text
processing applications, leveraging both SIMD and multicore parallel processing
features. The Parabix tokenizer is a Unicode tokenizer built on this framework —
text is processed as parallel bitstreams so all characters are transformed
simultaneously rather than one at a time.

## Requirements

To build the tokenizer, you need a development environment that satisfies the
Parabix project requirements:

- Standard C++ development tools including git, C++, etc.
- A modern C++ compiler supporting at least C++17.
- The cmake build system version 3.12 or better.
- Boost libraries version 1.61 or better (Ubuntu: `libboost-all-dev`).
- LLVM system version 12 or later (built in Release mode).
- ICU development libraries, components `uc` and `i18n` (Ubuntu:
  `libicu-dev`; macOS/Homebrew: `brew install icu4c`). `tools/lex/CMakeLists.txt`
  does `find_package(ICU REQUIRED COMPONENTS uc i18n)` and links `ICU::uc` +
  `ICU::i18n` — `ICU_Boundaries.cpp` uses ICU's `BreakIterator` for locale-aware
  word boundaries, so the tokenizer does not build without it. If Homebrew's
  keg-only `icu4c` is not found, configure with
  `-DCMAKE_PREFIX_PATH=$(brew --prefix icu4c)`.
- A JSON parser: [nlohmann/json](https://github.com/nlohmann/json) (header-only).
  `bpe.cpp` does `#include <nlohmann/json.hpp>` to parse `vocab.json`, and
  `CMakeLists.txt` puts `tools/lex/third_party` on the include path — so the
  single header must be present at `tools/lex/third_party/nlohmann/json.hpp`:

  ```bash
  mkdir -p tools/lex/third_party/nlohmann
  curl -L -o tools/lex/third_party/nlohmann/json.hpp \
      https://github.com/nlohmann/json/releases/latest/download/json.hpp
  ```

## Build

Clone the repository and build the tokenizer:

```bash
git clone https://cs-git-research.cs.sfu.ca/cameron/parabix-devel.git
cd parabix-devel
mkdir build19
cd build19
cmake -DCMAKE_BUILD_TYPE=Release ..
make tokenizer
```

All compiled tools land under `build19/bin/`; the tokenizer binary is
`build19/bin/tokenizer`. The test scripts in `tokenizer-test/` hard-code
`build19`, so prefer that directory name.

## Tokenizer

`tokenizer.cpp` is the CLI entry point. It reads an input file, runs optional
normalization, optional pre-tokenization, and optional BPE encoding, writing the
result to stdout.

```
input file → [normalize] → [pre-tokenize] → [BPE encode] → stdout
```

Basic usage:

```bash
bin/tokenizer input.txt
bin/tokenizer --normalize=lowercase input.txt
bin/tokenizer --normalize=nfd --pretokenizer=bert input.txt
bin/tokenizer --merges=tools/lex/tokenizer_files/merges.txt input.txt
```

If only `--normalize` is given with no `--pretokenizer`, the tokenizer outputs
the raw normalized text and stops — normalization alone is not tokenization.

### All CLI flags

| Flag | Meaning |
|---|---|
| `<input file>` | Positional, required. |
| `--normalize=` | Normalization mode(s); repeatable. See below. |
| `--pretokenizer=` | Pre-tokenizer mode. See below. |
| `--behavior=` | Delimiter handling for the pre-tokenizer. |
| `--delimiter=` | Delimiter character for `--pretokenizer=chardelimiter` (default `,`). |
| `--vocab=` | Path to a HuggingFace `vocab.json`. Enables BPE mode. |
| `--merges=` | Path to a HuggingFace `merges.txt`. Enables BPE mode. |
| `--strings` | BPE mode: print token strings instead of integer IDs. |
| `--merges-limit=N` | BPE mode: use only the first N merges by rank (0 = all). Fewer rules = smaller kernels = faster JIT, for size sweeps and smoke tests. |
| `--strip-newlines` | BPE mode with no `--pretokenizer`: drop `\n` bytes as pre-token separators instead of treating them as content. |
| `--bench-loop=N` | BPE mode: tokenize the input N times inside one process, suppressing output, then print a `BENCH_RESULT` line to stderr. Excludes process spawn, merges load, JIT, and file I/O. |

## Normalizer

`normalize.h` / `normalize.cpp`

Normalization is stage 1. It transforms the input text before any splitting
happens. Each mode is a Parabix pipeline operating directly on the UTF-8
bitstreams. Available via `--normalize=`:

- `none` — No normalization (default).
- `nfc` — Canonical decomposition followed by canonical composition.
- `nfd` — Canonical decomposition.
- `nfkc` — Compatibility decomposition followed by canonical composition.
- `nfkd` — Compatibility decomposition.
- `lowercase` — Map all uppercase codepoints to lowercase (Unicode SLC, 1-to-1).
- `stripaccents` — Remove Mn (Mark, Nonspacing) codepoints. Apply *after* `nfd`
  so accents are isolated codepoints.
- `stripleft` — Remove leading Unicode whitespace (`\p{White_Space}`).
- `stripright` — Remove trailing Unicode whitespace.
- `strip` — Remove both leading and trailing Unicode whitespace.
- `bytelevel` — GPT-2 byte alphabet: remap every input byte to a unique
  printable Unicode character.
- `nmt` — Google NMT preprocessing: control-character cleanup, whitespace → space.
- `bertcleantext` — BERT `clean_text`: drop `\p{C}` (except `\t\n\r`) and U+FFFD;
  map `\p{Zs}`, `\t`, `\n`, `\r` to U+0020.
- `bertchinesechars` — BERT `handle_chinese_chars`: surround each CJK character
  with spaces.

`--normalize` is repeatable, so modes compose in the order given:

```bash
bin/tokenizer --normalize=nfd --normalize=stripaccents input.txt
```

## Pre-tokenizer

`pretokenizer.h` / `pretokenizer.cpp`

Pre-tokenization is stage 2. It splits the (possibly normalized) text into tokens
by inserting newline separators at token boundaries. Available via
`--pretokenizer=`:

- `uax29` — Unicode UAX#29 word boundaries (default).
- `whitespace` — Split on whitespace characters.
- `whitespacesplit` — Split on whitespace, output delimiters as separate tokens.
- `digits` — Split on digit sequences.
- `punctuation` — Split on punctuation characters.
- `simplewordboundaries` — Boundaries between `\w` and `\W` characters.
- `bytelevel` — GPT-2 pre-tokenization with byte remapping (see below).
- `chardelimiter` — Split on a specific character, set with `--delimiter`.
- `bert` — BERT pre-tokenizer: separates punctuation and words.
- `sequence_whitespace_punctuation` — Whitespace then punctuation splitting.

`--behavior` controls what happens to the delimiter at each split boundary:

- `isolated` — Keep token boundaries and add space boundaries (default).
- `removed` — Only keep word/punctuation boundaries, exclude whitespace.
- `mergedwithprevious` — Attach whitespace to the previous word.
- `mergedwithnext` — Attach whitespace to the next word.
- `contiguous` — Keep punctuation with words, separate spaces.

Examples:

```bash
bin/tokenizer --pretokenizer=whitespace input.txt
bin/tokenizer --pretokenizer=chardelimiter --delimiter=, input.txt
bin/tokenizer --pretokenizer=whitespace --behavior=removed input.txt
bin/tokenizer --normalize=lowercase --pretokenizer=bert input.txt
```

### The `bytelevel` pre-tokenizer

`bytelevel` reproduces GPT-2's pre-tokenization regex:

```
'(?:[sdmt]|ll|ve|re)| ?\p{L}++| ?\p{N}++| ?[^\s\p{L}\p{N}]++|\s+(?!\S)|\s+
```

It is **not** implemented by compiling that regex. Parabix's generic regex engine
(`RE_Kernel`) evaluates all alternatives in parallel and marks every match end,
which loses the leftmost-first alternation priority the GPT-2 regex depends on.
For example at a space before a word, both `\s+` and ` ?\p{L}+` match, and both
ends get marked, so `hello world` splits as `hello|Ġ|world` instead of
`hello|Ġworld`.

Instead, `GPT2PretokenBoundaryKernel` computes pre-token starts from
**character-class transitions**. A GPT-2 pre-token is an optional single leading
space plus a maximal run of one class (Letter / Number / Other), or a whitespace
run, plus contraction glue. For disjoint class runs, leftmost-longest is
equivalent to "boundary at each class transition", which is one parallel pass.

Two subtleties the kernel encodes:

- HF's optional leading space ` ?` is the **literal byte 0x20**, not `\s`. So the
  last space of a whitespace run attaches forward into the following word, but a
  tab, newline, or NBSP cannot attach and forms its own `\s+` token.
- A contraction (`'s`, `'t`, `'d`, `'m`, `'ll`, `'ve`, `'re`) fires whenever the
  scanner *resumes* at the apostrophe. It resumes there unless the previous
  codepoint swallowed it, which happens in exactly two cases: the previous
  codepoint is a literal 0x20 (` ?[^\s\p{L}\p{N}]+` takes `" '"`), or it is an
  Other-class character (that run already absorbed the apostrophe). Letter,
  Number, non-0x20 whitespace, and start-of-input all produce a contraction.

## BPE

`bpe.h` / `bpe.cpp`

BPE encoding is stage 3. It performs a **real rank-ordered BPE merge** on a
token-id stream — it applies HuggingFace `merges.txt` rules lowest-rank-first, so
a later merge sees the ids stamped by earlier ones (the `Ġthe` → `Ġthey`
cascade). It is not a longest-match vocabulary scan.

BPE activates when `--merges` **or** `--vocab` is supplied. `--merges` is the
primary input and is self-sufficient: `buildBaseAlphabet()` regenerates the GPT-2
base ids 0..255 internally, so no `vocab.json` is needed. A real `vocab.json` is
optional, used for an id cross-check and for `--strings` decoding. `loadVocab`
sniffs the first non-space byte and delegates to `loadMerges` when it is not `{`,
so `--vocab=merges.txt` also works.

### Why bytes, not codepoints?

GPT-2 stores tokens as UTF-8 byte strings. Working on bytes skips UTF-8 → U21
decode in the BPE stage, shrinks the `EQ` comparison to a log2(8) AND-tree, and
matches the internal representation HuggingFace `tokenizers` uses.

### Preprocessing — `buildMergeRuleRanges()`

Runs on the CPU before any kernel is built.

`loadMerges` reads `merges.txt`: each non-header line `"A B"` is a merge
A + B → AB with **rank = line index** and **id = 256 + rank**. That id identity
is a GPT-2 property, so the merged token's vocab id equals `256 + rank` exactly,
and a rank-ordered partition is also an id-ordered partition.

`buildMergeRuleRanges` then resolves each raw merge's parts into a
`MergeRule {idA, idB, lenA, lenB, idAB}` using the base alphabet plus earlier
merge outputs (unresolvable parts are skipped with a count on stderr), sorts by
`idAB` ascending (= rank ascending), and partitions into **clean ranges**. A
range `[lo, hi)` holds only rules that are both:

1. **dependency-independent** — both part ids are `< lo`, so every part was
   already stamped by a *lower* kernel; and
2. **conflict-free** — no two rules in the range compete for a shared middle
   token, i.e. the right part id of one equals the left part id of the other
   (`cur.idB == p.idA || p.idB == cur.idA`).

`lo` is the first rule's own `idAB` (a merge's parts always precede it, so the
first rule always fits); the range extends while both conditions hold, and the
first violator starts the next range, so ranges tile contiguously.

Condition 2 matters: dependency-independence alone removes intra-kernel
*dependency* but not byte *conflict*. Two competitors sharing a kernel — e.g.
`Ġt` = (`Ġ`,`t`) and `ter` = (`t`,`er`), which both claim the `t` token — could
both fire on the same bytes. Splitting them into rank-ordered kernels lets the
lower-rank write reach the later kernel's input and starve the other. The cost is
many more kernels, which is what drives the fixed per-call dispatch floor (see
`tokenizer-test/README.md`).

The Python ground truth for this partition is
`tools/lex/merge_analysis.py::clean_range_analysis`.

### Alternative partition — ASAP level scheduling (`--level-partition`)

The clean-range walk closes a group at the **first** violating rule and never
rewinds, so groups are contiguous rank intervals. That contiguity, not the two
conditions, sets the kernel count: 1098 of the 1123 groups close on a conflict,
and at each break ~179 of the next 200 rules would still have fit. Group size
saturates near 44, and 50000 / 44.5 = 1123.

`--level-partition` drops contiguity only. Each rule takes its earliest legal
kernel:

```
level(r) = 1 + max( level(producer of idA), level(producer of idB),
                    level of any lower-rank rule sharing a seam with r )
```

A violator defers itself instead of ending the group, so the count becomes the
constraint DAG's longest path — a provable minimum. **1123 → 283 kernels**, mean
group size 44.5 → 176.7. Levels are not rank intervals: level 1 holds rank 0
alongside rank 47815.

Both conditions are preserved; only the packing changes. Two mechanics follow:

* `idA < lo` tests condition 1 only because `idAB == 256 + rank` **and** groups
  are contiguous make id order equal kernel order. Without contiguity it becomes
  an explicit `prodLevel[id]` lookup.
* Condition 2 must consider all prior rules, not just the current group, so
  `maxLeft[tok]` / `maxRight[tok]` hold the highest level using that token as a
  left / right part. Only `maxRight[idA]` and `maxLeft[idB]` are consulted —
  same-side sharing is harmless, since those rules' B parts differ.

Group `hi` becomes a running maximum across levels (a kernel's output carries ids
from every lower level), keeping `W_out` monotonic at the cost of the narrow early
streams (mean 14.2 → 16.0 bits). `hi` is now a **width bound only**, not an
"already stamped" watermark — see [Compaction and slot distances](#compaction-and-slot-distances).

### Runtime pipeline — `buildBPEPassPipeline()`

A `source` id stream and a 1-bit `inPlayMask` thread kernel → kernel.

**1. `BPERangeSeed`** turns each raw byte into its base-alphabet id. `id(byte)` is
a fixed byte→id bijection (GPT-2's `bytes_to_unicode`, a piecewise
`id = byte + offset`). It is compiled as **character classes**: for each of the 8
id output bits, the set of bytes whose id has that bit set is a byte character
class, so `compileCC` over the byte basis yields each id-bit stream directly — no
BixNum arithmetic and no 256-way lookup. The mapping is constant, so the kernel is
data-independent and its cache name is constant. It also seeds `active` (all ones
— every byte starts as a live token start) and `end` (all zeros, legacy/unused).

**2. One `BPEMergeKernel` per clean range**, ascending = rank order. Each keeps a
mutable id accumulator `idAcc` (starting from the input `source`) and the 1-bit
`inPlayMask` threaded in via `meIn`. Every rule (A, B → AB) is
**start-anchored** — the merged id is written at A's start:

```
Astart    = compileCC(idA) AND meInFrozen      // A starts here; read from the FROZEN entry ids
                                               //   (srcFrozen), not the mutating idAcc
BstartAtA = compileCC(idB) over LookAhead(sourceIn, lenA)  // B starts lenA bytes ahead, also
                                               //   read frozen (no same-kernel stamps)
fire      = Astart AND BstartAtA               // (Astart already AND-ed meInFrozen)
idAcc[i]  = fire ? idAB_bit_i : idAcc[i]       // stamp idAB at A's start (inside createIf(Astart))
inPlayMask &= NOT(Advance(fire, lenA))         // consume B's start; A's start survives as AB's
```

Both compares are `compileCC` character classes over the id bit-planes (the
frozen entry ids), replacing BixNum `EQ`. Reading the **frozen** entry state
(`srcFrozen` / `meInFrozen`) rather than the mutating `idAcc` / `inPlayMask` is
what makes the rules within a kernel order-independent — guaranteed safe by the
clean-range partition's independence + conflict-freedom.

`lenA` is A's **raw-byte length = codepoint count** (`rawByteLen` counts
non-continuation bytes), not its display byte size. The id stream carries one id
per raw byte and the byte-level remap is a byte↔codepoint bijection, so `Ġ` is
two display bytes but `lenA` 1.

The kernel's output width is `ceil_log2(hi + 1)` bits, so low ranges move fewer
bits and only the top ranges reach the full 16.

**3. Returns** `BPEPassResult{matchEnd = inPlayMask, vocabID = source}`.
`matchEnd` marks surviving token **starts** — the field name is historical.

### Self-merges

A self-merge X + X → XX (`idA == idB`) fired at every X start over-consumes:
position `i` glues `(i, i+L)` while `i+L` glues `(i+L, i+2L)`, so the shared
token is pulled two ways and a whole run of X collapses to one surviving start.
Real BPE pairs left-to-right, using each token once.

`selfMergeFireStarts()` fixes this by firing only at the 1st, 3rd, 5th … X of
**each maximal run**. `createEveryNth(isX, 2)` gives the *global* parity of X
starts; `createMatchStar(runStart & parity, body)` spreads each run's starting
parity across that run so the odd/even choice **resets per run** — otherwise a
preceding odd-length run flips the next run's pairing. The idiom is ported from
`lib/kernel/unicode/normalization.cpp::SelfComposableLogic`. Note the parity is
computed on the **live** starts (`Astart AND inPlayMask`): a lower-rank neighbour
merge may already have consumed the run's head, and counting that dead position
would flip the parity.

### Pre-token boundary gating

`buildBPEPassPipeline` takes an optional `boundary` stream — a 1-bit per-byte
mask of pre-token starts. When present, each `BPEMergeKernel` also requires that
B does *not* begin a new pre-token:

```
fire = fire AND NOT LookAhead(boundaryIn, lenA)
```

so merges never cross a pre-token boundary. This is what stops contraction
merges (`'t`, `'s`, …) from forming across a boundary HuggingFace enforces.
Kernels built with a boundary stream are named `BPEMerge_b1_h{hash}` rather than
`BPEMerge_h{hash}`, keeping their cache entries distinct.

`tokenizer.cpp` supplies this stream when `--pretokenizer=bytelevel` is given in
BPE mode: the bytelevel pre-tokenizer's only job there is to mark pre-token
starts, and BPE is fed the **raw** bytes (using the Ġ-remapped bytes would
double-encode, since `BPERangeSeed` already applies `bytes_to_unicode` once).

### Emission

In `tokenizer.cpp`: `P2S16Kernel` packs `vocabID` into a single 1×16 stream,
`ScanIndexGenerator` reads `matchEnd` to drive `scan::Reader`, and
`bpe_emit_token` dereferences the 16-bit id at each surviving-token-start byte
position and prints it (or the token string with `--strings`).

Do **not** split the id into low/high byte streams read through the scan
`additionalStreams` channel — that channel indexes by scan-iteration counter, not
byte position, which silently zeroes the high byte.

### Usage

```bash
# Full GPT-2 BPE from merges.txt alone → integer token IDs
bin/tokenizer --merges=tools/lex/tokenizer_files/merges.txt input.txt

# With GPT-2 pre-tokenization (supplies the boundary mask that gates merges)
bin/tokenizer --pretokenizer=bytelevel \
              --merges=tools/lex/tokenizer_files/merges.txt input.txt

# Token strings instead of IDs (needs a real vocab.json)
bin/tokenizer --vocab=tools/lex/tokenizer_files/vocab.json \
              --merges=tools/lex/tokenizer_files/merges.txt --strings input.txt

# Smoke test with a truncated rule set (fast JIT)
bin/tokenizer --merges=tools/lex/merges.txt --merges-limit=64 input.txt
```

When BPE is enabled **without** `--pretokenizer`, `\n` bytes are treated as real
content by default — a newline seeds to id 198 (GPT-2 `Ċ`) and flows through the
merges, matching HuggingFace byte-level BPE. `--strip-newlines` restores the
legacy behaviour where `\n` is only a pre-token separator and is filtered out via
`FilterByMask` (`buildLinePretokens`).

### Data files

Each data file has a full GPT-2 copy under `tokenizer_files/` and a truncated dev
copy directly under `tools/lex/`:

| File | Size | Purpose |
|---|---|---|
| `tokenizer_files/merges.txt` | 456 KB | Full GPT-2 merges. The primary BPE input. |
| `merges.txt` | 13 KB | Truncated dev copy; tiny partition, instant JIT. |
| `tokenizer_files/vocab.json` | ~50 k tokens, 1 MB | Optional id cross-check + `--strings` decode. |
| `vocab.json` | 260 tokens | Truncated dev copy. |
| `tokenizer_files/tokenizer.json` | 3.5 MB | HuggingFace-side config, used by the test scripts. |

Check what actually loaded via the `BPE: loaded vocab with N tokens` and
`[BPE] N merge-range kernels` lines on stderr.

### Debugging

`buildBPEPassPipeline` unconditionally prints the merge-range groups to stderr:
`[BPE] N merge-range kernels`, then `[lo,hi) xN maxLen=M` per group. Environment
knobs, set before running:

| Variable | Effect |
|---|---|
| `BPE_DBG=seed` | Return right after the seed, dumping base-alphabet ids at every byte. Verifies the seed map. |
| `BPE_DBG=bound` | Return the pre-token boundary mask, printing the base id at each boundary byte. Verifies boundary placement. |
| `BPE_RULES=1` | Dump resolved merge rules to stderr. |
| `BPE_RULES_N=k` | Cap rules dumped per group (default 4). |

The JIT object cache lives in `~/.parabix/objcache/`, keyed by kernel signature.
`BPEMergeKernel` folds an FNV rolling hash of its rule list into its name, so
swapping `merges.txt` invalidates correctly without manual eviction. Wipe with
`rm -rf ~/.parabix/objcache/` if you suspect a stale entry.

Smallest useful probe: a `merges.txt` containing just `Ġ t` then `Ġt he`, run on
the input `Ġthe`, should cascade `Ġ`+`t` → `Ġt` then `Ġt`+`he` → `Ġthe`.

### Validation status

- All 7 comparable pre-tokenizer modes match HuggingFace on the test corpus
  (`compare_pretokenizers.py`).
- BPE on 24 MB of openwebtext produced 5,445,573 tokens against HuggingFace's
  5,445,574 — a single divergence in 5.4 M tokens, at a GPT-2 contraction
  (`'d` inside `'default'`). The contraction-context rule described above was
  added to fix that case and is verified on targeted probes; **re-verification
  over the full 24 MB corpus is still pending.**

Note that `bpe.cpp`'s top-of-file header comment and part of `bpe.h`'s comment on
`BPEPassResult` still describe an older end-anchored, emit-everything design
(`matchEnd = active`, "Stage-B"). The kernel code no longer implements that —
trust this document and the code over those comments.

## BPE optimization parameters

The `BPEMergeKernel` design has several **optional, opt-in** optimizations, each
behind a CLI flag. **All default OFF**, so a bare `--merges=…` run is the plain,
unoptimized pipeline; every flag is added on the command line to turn one on.
This keeps a clean baseline to A/B against, and lets each optimization be
benchmarked in isolation. They are experimental — measure before trusting.

> Note: **character-class (CC) compilation is always on, not a flag.** Every id
> comparison (Astart, B-detection, the grouped-if range gate) and the seed id
> derivation are compiled with `re::cc::Parabix_CC_Compiler_Builder` /
> `compileCC` over the id (or byte) bit-planes, replacing the earlier BixNum
> `EQ`/`UGE`/`ULE`/`Select`/`AddModular` arithmetic. Output is byte-identical;
> this was a straight port, not a tunable.

Each flag encodes itself into the kernel cache name (tags below) so switching a
flag never serves a stale compiled body from `~/.parabix/objcache/`. You can
therefore A/B two flag settings back-to-back **without** wiping the cache.

| Flag | Default | Cache tag | What it does |
|---|---|---|---|
| `--level-partition` | off | (via rule set + `o{bits}`) | Partition merge rules by **ASAP level scheduling** (minimum kernel count) instead of contiguous clean id ranges. Full GPT-2: **1123 → 283 kernels**. Same two correctness conditions, only the packing changes — see [Alternative partition](#alternative-partition--asap-level-scheduling---level-partition). Verified byte-identical output. |
| `--partition-by-merge-id-bits` | off | (via rule set + `w{bits}`/`o{bits}`) | Confine each kernel to merged ids of **one bit length**: tier *b* = rules with `idAB` in `[2^(b-1), 2^b)`, partitioned on its own (clean ranges, or levels with `--level-partition`), tiers run in order. The first kernel of tier N+1 reads N-bit ids and writes N+1 bits; the rest of the tier reads and writes N+1 bits. Full GPT-2: clean ranges 1123 → 1126 kernels; levels 283 → 299 kernels, **but** ASAP piles most of a tier into its first level (tier 16's first kernel has 8023 rules), which makes Pablo compile time blow up — `--max-merges-per-kernel` (default 500) splits such kernels. |
| `--max-merges-per-kernel=N` | `500` | (via rule set) | Split any partition group with more than `N` rules into `ceil(n/N)` consecutive kernels of near-equal size, in rank order. Exact: rules within a group are mutually independent, and `needsLiveId` (chain) is recomputed per chunk. `0` = no cap. No effect on full GPT-2 with plain clean ranges (max 196) or plain `--level-partition` (max 373). |
| `--compact-by-merge-id-bits` | off | (via `L`) | `FilterByMask` after the **last kernel of each id bit width**, so the first kernel producing N+2-bit ids reads a compacted stream. Combines with `--compact-base` (a kernel compacts if either schedule asks). Meant for `--partition-by-merge-id-bits`. |
| `--compact-base=N` | `0` (off) | `L{maxLen}` | Base kernel index at which `FilterByMask` compaction first shrinks the inter-kernel streams. `0` = no compaction. Larger streams stay full-width; compaction trades a per-point compact/expand cost for cheaper downstream kernels. |
| `--geometric-compaction` | off | (via `L`) | Space the compaction points geometrically instead of arithmetically (denser early, sparser later). Only meaningful with `--compact-base > 0`. |
| `--if-group-lower-limit=N` | `-1` (off) | `g{size}_` | **Master switch for grouped-if.** For kernels at/after index `N`, replace the one-`createIf`-per-rule structure with range-gated group gates (rules sorted by `idA`, chopped into chunks, one `createIf` per chunk on the id range it spans). A block with no live id in a chunk's range skips that whole chunk. `-1` = per-rule ifs everywhere. Left at its default, it becomes `0` whenever `--if-group-size`, `--if-group-count` or `--if-test-significant-bits` is given. |
| `--if-group-count=K` | `1` | `g{rules/K}_` | Grouped kernels get **K gates each**; gate size = `rules/K` (scales with kernel). Only active with `--if-group-lower-limit >= 0`. |
| `--if-group-size=S` | `1` | `g{S}_` | **Fixed** `S` rules per gate regardless of kernel size (gate count = `rules/S`). **Overrides `--if-group-count` when `!= 1`.** `1` = defer to `--if-group-count`. |
| `--lookahead-in-gate` | off | `la1_` | Build the B-detection `LookAhead` **inside each rule's** `createIf` (skippable on cold blocks) instead of one shared shift hoisted outside all gates. Skippable but **duplicated per rule** (loses the per-lenA dedup). Works with or without grouping. |
| `--lookahead-in-group` | off | `lg1_` | **Grouped kernels only.** Build the B-detection `LookAhead` **inside each group's** `createIf`, **cached per distinct `lenA`** so the chunk's rules share it. Combines dedup (unlike `--lookahead-in-gate`) with cold-chunk skip (unlike the hoisted shift). Needs `--if-group-lower-limit >= 0`; no effect on ungrouped kernels. |

### Compaction and slot distances

`--compact-base=N` inserts a `FilterByMask` after every N kernels. Merges kill
positions but do not shrink the stream, so without it every later kernel keeps
scanning dead bytes. The filter keeps exactly the live token starts, one position
each — call those **slots**.

Distances then change meaning: a rule's `LookAhead` distance is the width of its
left part, and after a squeeze that must be a slot distance.
`applyCompactionSchedule` rewrites every `lenA`, which needs one fact per token —
*was it already built when we compacted?* Yes → it owns a position → 1 slot.
No → it sits on top of several, so sum its parts recursively, stopping at the
first part that was present (or is a base byte).

Kernels run in order and a compaction happens after a specific kernel, so "which
kernel built this token" is "when was it built":

```cpp
if (id < 256) return 1;                              // base byte, from the seed
auto k = kernelOf.find(id);
if (k != kernelOf.end() && (long) k->second <= lastCompactKernel)
    return 1;                                        // built at/before the squeeze
// otherwise: slotSpan(left part) + slotSpan(right part)
```

This was previously approximated by `id < frontier` (`frontier = g.hi`), which is
equivalent **only** when groups tile the id axis in rank order — true for clean
ranges, false under `--level-partition` where `hi` is a running max. Every id then
tested as already built, every `lenA` collapsed to 1, and merges whose left part
postdates the squeeze silently never fired (`Ġrestaurant` → `Ġrestaur` + `ant`;
3134 tokens instead of 2708 on `webtext_10`). Word prefixes stayed correct because
their left parts predate the squeeze, which made the failure look selective.

Slot width is a property of a token *measured against a particular squeeze*, so
the memo is cleared at every compaction point.

### How grouping is meant to help

The early (low-rank) kernels fire on almost every block — common merges — so
gating them saves nothing; the late (high-rank) kernels rarely fire, so a range
gate lets most blocks skip them. `--if-group-lower-limit` therefore groups only
kernels at/after an index. `--lookahead-in-group` follows the same logic: it
moves the peek-ahead work inside those gates so cold chunks skip it too, while
`--lookahead-in-gate` moves the peek inside but per-rule (no sharing). The three
peek placements are mutually exclusive — precedence in the kernel body is
indexed-nextId > in-group > in-gate > hoisted (default).

### Correctness testing

Every optimization must be **byte-identical** to the baseline. Two ways to check.

**1. Differential vs HuggingFace** (the ground truth), full GPT-2 vocab:

```bash
cd tokenizer-test
python3 compare_bpe.py --no-timing                              # baseline
python3 compare_bpe.py --no-timing --tok-flag=--level-partition # flag under test
```

`--tok-flag` is repeatable and forwards verbatim to the binary; use the `=` form
or argparse swallows the next `--...` token. The run prints
`Extra tokenizer flags: …` when any are active — no such line means the default
path was tested. `--no-timing` skips the throughput section, which builds its
command *without* `--pretokenizer=bytelevel` and would otherwise JIT and measure a
different pipeline than the one just verified.

**2. Self-consistency** (fast, dev merges) — the same input through two flag
settings must produce identical tokens:

```bash
IN=tokenizer_files/val_2MB.txt
build19/bin/tokenizer --merges=tools/lex/merges.txt "$IN" > /tmp/a.txt
build19/bin/tokenizer --merges=tools/lex/merges.txt \
    --if-group-lower-limit=0 --if-group-count=3 --lookahead-in-group "$IN" > /tmp/b.txt
diff -q /tmp/a.txt /tmp/b.txt && echo IDENTICAL
```

`tokenizer-test/bench_configs.py --verify` automates this — it hashes each
config's tokens against the baseline and reports `IDENTICAL` / `DIFFERS` beside
the timings, so "faster but wrong" is flagged rather than read as a win.

### Performance testing

`tokenizer-test/bench_bpe.py --sweep` drives the binary's `--bench-loop` and
reports MB/s. Pass the optimization flags through `--parabix-args`:

```bash
cd tokenizer-test
BASE="--geometric-compaction --compact-base=15 --if-group-lower-limit=50 --if-group-count=3"

# baseline (hoisted peek)
python3 bench_bpe.py --sweep \
    --sweep-source tokenizer_files/val_8MB.txt --sweep-sizes 4 \
    --parabix-args="$BASE"

# variant under test (in-group cached peek)
python3 bench_bpe.py --sweep \
    --sweep-source tokenizer_files/val_8MB.txt --sweep-sizes 4 \
    --parabix-args="$BASE --lookahead-in-group"
```

Notes:
- Distinct cache tags mean the two runs do **not** collide — no wipe needed
  between them. Wipe (`rm -rf ~/.parabix/objcache/`) only after a **rebuild**.
- `--lookahead-in-group` is a no-op on ungrouped kernels. Giving `--if-group-size`,
  `--if-group-count` or `--if-test-significant-bits` without `--if-group-lower-limit`
  groups every kernel (limit 0); an explicit `--if-group-lower-limit=-1` keeps grouping off.
- Cross-run absolute MB/s is noisy; trust only **back-to-back** A/B deltas.
- Confirm a flag engaged by inspecting the IR on a debug build:
  `build_debug/bin/tokenizer --ShowOptimizedPablo --ToShow="BPEMerge*" --merges=tools/lex/merges.txt <flags> build19/test.txt`.

## Testing

`tokenizer-test/` contains Python scripts that compare Parabix output against the
HuggingFace `tokenizers` library, plus a throughput benchmark. See
[tokenizer-test/README.md](tokenizer-test/README.md) for setup and usage.

## License

Parabix is governed by the Open Software License 3.0. See `OSL3.0.txt` in the
repository root.
