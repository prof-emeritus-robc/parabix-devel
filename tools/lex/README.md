# Parabix Tokenizer

`tokenizer` is a GPT-2 byte-level BPE tokenizer built on the Parabix framework,
together with HuggingFace-style normalizers and pre-tokenizers. Parabix processes
text as parallel bit streams (one bit per input byte in each stream, a whole SIMD
register of positions per operation) and compiles its kernels at run time.

For GPT-2, the BPE output is the same token ids as HuggingFace `tokenizers` and
OpenAI `tiktoken` (see [Validation](#validation)). Only GPT-2-style vocabularies
are supported (see [Limitations](#limitations)).

| File | Contents |
|---|---|
| `tokenizer.cpp` | Command line, pipeline assembly, token output |
| `normalize.h` / `normalize.cpp` | Normalizers |
| `pretokenizer.h` / `pretokenizer.cpp` | Pre-tokenizers, including the GPT-2 boundary kernels |
| `bpe.h` / `bpe.cpp` | BPE: rule loading, cut and per-length kernels, lookup tables, the C++ miss path, the emitter, and the older merge-kernel design |
| `ICU_Boundaries.h` / `.cpp` | ICU `BreakIterator` word boundaries |
| `merge_analysis.py` | Python reference for the merge-kernel clean-range partition |
| `bpe_pipeline.drawio` | An early design sketch (depth-stratified merge passes); not the current pipeline |
| `tokenizer_files/` | GPT-2 model files and test inputs ([Data files](#data-files)) |
| `tokenizer-test/` | Correctness and benchmark scripts ([README](tokenizer-test/README.md)) |
| `third_party/nlohmann/json.hpp` | JSON parser for `vocab.json` (committed) |

## Requirements

The Parabix requirements from the top-level [README](../../README.md): a C++17
compiler, cmake 3.12+, Boost 1.61+, LLVM 17 or later (Release build). The
tokenizer also needs:

- **ICU** (`uc` and `i18n`; Ubuntu `libicu-dev`, Homebrew `icu4c`).
  `CMakeLists.txt` does `find_package(ICU REQUIRED COMPONENTS uc i18n)`. If
  Homebrew's keg-only `icu4c` is not found, configure with
  `-DCMAKE_PREFIX_PATH=$(brew --prefix icu4c)`.
- **nlohmann/json**: the single header is committed at
  `third_party/nlohmann/json.hpp`, so there is nothing to install.

## Build

```bash
git clone https://cs-git-research.cs.sfu.ca/cameron/parabix-devel.git
cd parabix-devel
mkdir build19 && cd build19
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8 tokenizer
```

The binary is `build19/bin/tokenizer`. `build19` is only a name: it is the
default path of the scripts in `tokenizer-test/`, which take `--tokenizer=PATH`
for any other build.

Kernels are compiled at run time and cached in `~/.parabix/objcache` (see the
top-level README). Cache entries belong to the build that made them, so a rebuild
never reuses stale kernels and there is no need to clear the cache. BPE kernel
names also encode their tables and options, so switching options is safe.

## Quick start

```bash
T=build19/bin/tokenizer
M=tools/lex/tokenizer_files/merges.txt

# Pre-tokenize only (default UAX#29 word boundaries), one pre-token per line
$T input.txt

# GPT-2 BPE with the recommended options: one token id per line
BEST="--pretokenizer=bytelevel --tail-from-bytes --tail-vocab-table --tail-cache-pool \
--tail-scan-kernel --tail-emit-from-segs --tail-long-cache --tail-batch-bpe --tail-hard-split \
--tail-simd-emit --skip-identity-normalize --pretok-compress-classes --pretok-class-code \
--fast-emit --memsource-in-place --pretok-byte-domain --pretok-utf8-classes \
--tail-unit-seed --pair-cuts=3 --len-scan=15"
$T $BEST --merges=$M input.txt
```

Every optimization is opt-in and defaults off, so that each one could be added and
measured on its own. That is why the recommended command is long. Without these
options, `$T --pretokenizer=bytelevel --merges=$M input.txt` runs the original
[merge-kernel design](#older-design-merge-kernels): the same output, but 1,123
kernels, a long first compile and a much slower run.

Example. Input (42 bytes: a line, a blank line, a line indented by 4 spaces):

```
Hello world, it's 日本 code;

    x = 1
```

`$T $BEST --merges=$M --strings --offsets=byte input.txt` prints the id, the
byte span and the token string (GPT-2 shows a space as `Ġ` and
a newline as `Ċ`):

```
15496   0   5   Hello
995     5   11  Ġworld
11      11  12  ,
340     12  15  Ġit
338     15  17  's
10545   17  19  Ġæ
245     19  20  Ĺ
98      20  21  ¥
17312   21  23  æľ
105     23  24  ¬
2438    24  29  Ġcode
26      29  30  ;
628     30  32  ĊĊ
220     32  33  Ġ
220     33  34  Ġ
220     34  35  Ġ
2124    35  37  Ġx
796     37  39  Ġ=
352     39  41  Ġ1
198     41  42  Ċ
```

Without `--strings` and `--offsets`, only the ids are printed. [How BPE
works](#bpe) walks through this example.

Status lines go to stderr: rules loaded, table sizes, build and compile times,
and the run time (`[BPE] run (execute pipeline): … ms`).

## Command line

```
tokenizer [options] [input file]
```

With no input file the tokenizer reads standard input. Two modes:

- **Pre-tokenizer mode** (no `--merges` or `--vocab`): normalize, pre-tokenize,
  and write the pre-tokens one per line. With `--normalize` and no
  `--pretokenizer`, it writes the normalized text and stops.
- **BPE mode** (`--merges` or `--vocab` given): token ids, one per line.

Useful Parabix framework options: `--thread-num=N` (worker threads, default 3,
capped at the number of cores), `--segment-size=N`, `--output=FILE`,
`-EnableCycleCounter` (cycles per kernel), `-object-cache-dir=DIR`.

### Tokenizer options

Defined in `tokenizer.cpp` (shown by `--help`). The BPE design options live in
`bpe.cpp` and are listed under [Options of the current design](#options-of-the-current-design)
and [merge-kernel options](#merge-kernel-options).

| Option | Meaning |
|---|---|
| `--normalize=M[,M…]` | Normalizers, applied in order; comma-separated or repeated ([Normalizer](#normalizer)). |
| `--pretokenizer=M` | Pre-tokenizer mode (default `uax29`) ([Pre-tokenizer](#pre-tokenizer)). |
| `--behavior=B` | Delimiter handling of the pre-tokenizer (default `isolated`). |
| `--delimiter=C` | Delimiter for `--pretokenizer=chardelimiter` (default `,`). |
| `--merges=FILE` | HuggingFace `merges.txt`. Enables BPE mode and is enough on its own. |
| `--vocab=FILE` | HuggingFace `vocab.json`. Also enables BPE mode; optional with `--merges`, which alone gives the token strings for `--strings` and `--offsets`. |
| `--merges-limit=N` | Use only the first N merges by rank (0 = all), for quick tests. |
| `--strings` | Print `id<TAB>token` instead of the id. |
| `--offsets=none\|byte\|char` | Also print each token's span. `byte`: byte offsets. `char`: character offsets as HuggingFace `Encoding.offsets` (a token that covers part of a character reports the whole character, so spans can overlap). Not with `--strip-newlines`; with `--normalize` the spans refer to the normalized text. |
| `--fast-emit` | Write plain ids with a direct integer formatter (same output; ignored with `--strings`/`--offsets`). |
| `--emit-id-table` | With `--fast-emit` and the tail emitter: format ids from a precomputed table (same output). |
| `--strip-newlines` | BPE without `--pretokenizer`: drop `\n` bytes as separators. By default `\n` is content (id 198, `Ċ`), as in HuggingFace. |
| `--skip-identity-normalize` | BPE without `--normalize`: feed BPE the input bytes, not decoded and re-encoded codepoints (the same bytes for valid UTF-8). |
| `--pretok-compress-classes` | Bytelevel pre-tokenizer, no normalizer, with `--skip-identity-normalize`: compress only the pre-tokenizer's character classes to one slot per codepoint, not all 21 codepoint bits. |
| `--pretok-class-code` | With the above: compress a 4-bit code of the classes instead of 9 class streams. |
| `--pretok-byte-domain` | With `--pretok-class-code`: find the pre-token starts on the bytes, with no compression at all ([GPT-2 boundaries](#the-bytelevel-pre-tokenizer)). |
| `--pretok-utf8-classes` | With `--pretok-byte-domain`: compile the classes on the UTF-8 bytes, with no UTF-8 decoder. |
| `--memsource-in-place` | Load the input once into a padded buffer that the source kernel reads in place, instead of copying it on every pipeline call. |
| `--auto-segment-size` | Pick `--segment-size` from the input size and `--thread-num` (16K to 256K, at least 64 segments per thread) unless `--segment-size` is given. |
| `--bench-loop=N` | Tokenize the input N times in one process with output off, after one warm-up run, and print a `BENCH_RESULT` line to stderr. Excludes process start, rule loading, compiling and file reading. |
| `--bench-cold-cache` | With `--bench-loop`: empty the tail's caches before every run, so each run starts cold like a one-shot run. |

## Normalizer

`normalize.h` / `normalize.cpp`. Each mode is a Parabix pipeline on the UTF-8 bit
streams. `--normalize=` takes:

- `none` (default), `nfc`, `nfd`, `nfkc`, `nfkd` (Unicode normalization forms).
- `lowercase`: map uppercase codepoints to lowercase (Unicode simple lowercase, one to one).
- `stripaccents`: remove nonspacing marks (`Mn`). Use after `nfd`.
- `stripleft`, `stripright`, `strip`: remove leading, trailing or both
  `\p{White_Space}`.
- `bytelevel`: GPT-2 byte alphabet, every byte mapped to a printable character.
- `nmt`: Google NMT clean-up (control characters removed, whitespace to space).
- `bertcleantext`: BERT `clean_text` (drop `\p{C}` except `\t\n\r`, and U+FFFD;
  `\p{Zs}`, `\t`, `\n`, `\r` become U+0020).
- `bertchinesechars`: BERT `handle_chinese_chars` (spaces around each CJK character).

Modes compose in order: `--normalize=nfd,stripaccents,lowercase` (or the option
repeated).

## Pre-tokenizer

`pretokenizer.h` / `pretokenizer.cpp`. `--pretokenizer=` takes:

| Mode | Splits | HuggingFace counterpart |
|---|---|---|
| `uax29` (default) | Unicode UAX #29 word boundaries | none |
| `whitespace` | `\w+` and `[^\w\s]+` runs, whitespace dropped | `Whitespace()` |
| `whitespacesplit` | On whitespace, whitespace dropped | `WhitespaceSplit()` |
| `digits` | Each digit on its own | `Digits(individual_digits=True)` |
| `punctuation` | At punctuation, by `--behavior` | `Punctuation(behavior)` |
| `simplewordboundaries` | Between `\w` and `\W` runs, by `--behavior` | `Split(Regex(r"\w+"), behavior)` |
| `bytelevel` | GPT-2 pre-tokenization ([below](#the-bytelevel-pre-tokenizer)) | `ByteLevel(add_prefix_space=False)` |
| `chardelimiter` | On `--delimiter` | `CharDelimiterSplit` |
| `bert` | Whitespace and punctuation | `BertPreTokenizer()` |
| `sequence_whitespace_punctuation` | Whitespace, then punctuation | `Sequence([WhitespaceSplit(), Punctuation()])` |

`--behavior=` decides what happens to the delimiters:

- `isolated` (default): delimiters become pre-tokens of their own.
- `removed`: HuggingFace's `removed`, the matches are deleted
  (`simplewordboundaries` deletes the `\w+` runs, `punctuation` the punctuation).
- `mergedwithprevious`, `mergedwithnext`: a delimiter joins the pre-token before or after it.
- `contiguous`: adjacent delimiters form one pre-token.
- `excludewhitespace`: keep the boundaries, drop whitespace (the old meaning of
  `removed`).

`bert`, `sequence_whitespace_punctuation`, `whitespace`, `whitespacesplit` and
`chardelimiter` always use `excludewhitespace`. For `simplewordboundaries` and
`punctuation` the behaviors follow HuggingFace exactly; for the other modes they
act on whitespace. The output has one pre-token per line, so a `\n` in the input
always ends a pre-token.

### The bytelevel pre-tokenizer

`bytelevel` gives the pre-token boundaries of GPT-2's regex:

```
'(?:[sdmt]|ll|ve|re)| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
```

It does not compile that regex. Parabix's regex engine finds every match end of
every alternative at once, which loses the leftmost-first priority this regex
relies on. Instead the boundaries come from **character-class transitions**: a
pre-token is an optional single space plus a run of one class (letter, number,
other), or a whitespace run, plus contractions, and for runs of one class the
leftmost-longest split is "a boundary at each class change", which bit streams
compute in one pass. Two details:

- The optional leading space is the literal byte 0x20, not `\s`. The last space
  of a whitespace run joins the next word; a tab, newline or NBSP cannot, and
  stays in the whitespace run.
- A contraction (`'s`, `'t`, `'d`, `'m`, `'ll`, `'ve`, `'re`) is split off unless
  the apostrophe was already taken by the run before it: after a literal space
  (` ?[^\s\p{L}\p{N}]+` takes `" '"`) or after an other-class character.

In the example above: `Hello`, ` world`, `,`, ` it`, `'s`, ` 日本`, ` code`, `;`,
`\n\n   ` (the whitespace run without its last space), ` x`, ` =`, ` 1`, `\n`.

There are three implementations, chosen by the `--pretok-*` options; all give the
same boundaries:

1. **Codepoint domain** (`GPT2PretokenBoundaryKernel`, the default): decode
   UTF-8, compute the classes per codepoint, find boundaries, spread them back to
   the bytes.
2. **Compressed classes** (`--pretok-compress-classes`, `--pretok-class-code`):
   only the class bits are compressed to one slot per codepoint. The nine classes
   the rules need (whitespace, 0x20, letter, number, apostrophe, and the
   contraction letters) fall into 10 disjoint categories, so a 4-bit code holds them.
3. **Byte domain** (`--pretok-byte-domain`, `--pretok-utf8-classes`):
   `GPT2ByteBoundaryLA` applies the rules to the bytes. One `CharClassesKernel`
   compiles the 4 code bits (and an "any codepoint" stream) directly on the UTF-8
   bytes, marked at each codepoint's first byte. The previous codepoint's class
   is moved to its last byte by advances under nested `If`s on the lead bytes,
   and the next codepoint's start is found from the lead byte's length. No
   decoder, no compression, no spreading back. Valid UTF-8 is assumed, as in
   HuggingFace, which takes a decoded string.

## BPE

`bpe.h` / `bpe.cpp`. This section describes the design the recommended options
run. The older design is in [Older design: merge kernels](#older-design-merge-kernels).

### Model files

`merges.txt` is enough. Each line `A B` after the `#version` header is a merge
with **rank** = its line index and **id = 256 + rank**, which is how GPT-2
numbers its vocabulary. The 256 base tokens (GPT-2's `bytes_to_unicode`) are
generated internally, and each merge's string is the two parts joined, so the
token strings for `--strings` and `--offsets` need no `vocab.json` either.
`vocab.json` is optional.

GPT-2's one added token, `<|endoftext|>` (id 50256), is matched as a literal
before BPE, as HuggingFace does: `tokenizer.cpp` runs the pipeline on the text
around each occurrence and prints 50256 in between. (`--bench-loop` runs the
whole input as plain text.)

BPE result: within each pre-token, repeatedly merge the adjacent pair with the
lowest rank (leftmost on a tie) until no pair is a rule.

### Cuts: where no merge can happen

A byte pair `x y` is **inside a token** when some rule joins a token ending in
`x` to a token starting with `y`. For GPT-2 that is 2,689 of the 65,536 byte
pairs. Between two bytes that form no such pair, no merge can ever happen, so
**the BPE of a pre-token is the BPE of its pieces between such pairs, put
together**. The pipeline cuts the text at every pre-token start and at those
pairs, then finishes each piece on its own. Pieces are short, and most of them
are themselves one vocabulary token.

In the example, the pre-token starts are at bytes 0 5 11 12 15 17 24 29 30 35 37
39 41, and the cuts add 20 21 23 (inside ` 日本`, bytes E6 97 A5 E6 9C AC) and
32 33 34 (before and between the spaces of the indent: no GPT-2 token holds a
newline followed by a space, or two spaces). Some of the pieces:

| Piece | Bytes | Finished by | Tokens |
|---|---|---|---|
| `Hello` | 5 | length-5 table, hit | 15496 |
| ` world` | 6 | length-6 table, hit | 995 |
| `,` | 1 | its base id | 11 |
| `'s` | 2 | pair table, a rule | 338 |
| ` ` E6 97 | 3 | length-3 table misses; pair check from the pair rules | 10545 `Ġæ`, 245 `Ĺ` |
| A5 | 1 | base id | 98 `¥` |
| E6 9C | 2 | pair table, a rule | 17312 `æľ` |
| `\n\n` | 2 | pair table, a rule | 628 `ĊĊ` |

### Pipeline

The recommended options build this pipeline of 24 kernels (names as printed by
`-EnableCycleCounter`). No kernel applies merge rules one by one: the bit-stream
kernels find pre-tokens, cuts and piece lengths, and the scan kernels look pieces
up in tables.

| # | Kernel | Work |
|---|---|---|
| 1 | `memory_source` | The input buffer (read in place with `--memsource-in-place`). |
| 2 | `s2p8` | Bytes to 8 bit streams. |
| 3 | `cc_…` | One character-class kernel on the UTF-8 bytes: the 4-bit pre-tokenizer class code, "any codepoint", and the 3 cut classes of `--pair-cuts=3`. |
| 4 | `GPT2ByteBoundaryLA` | Pre-token starts, on the bytes. |
| 5 | `BPE_Cuts` | All cuts in one kernel (below). |
| 6 | `BPE_BaseIds` | Every byte's GPT-2 base id (8 bit streams, BixNum arithmetic: four range tests choose an offset, one 8-bit add). |
| 7 | `p2s_16` | Base ids to a 16-bit id per byte, the stream the scan kernels update in place (`--tail-unit-seed`). |
| 8 | `BPE_LenSelect15_long` | From the cuts: the starts of the pieces of each length 2..15, the starts of longer pieces, and the bytes of short pieces. |
| 9 | `BPE_LongPieces` | Pieces over 15 bytes and the input's last piece, through the C++ path. |
| 10–23 | `BPE_len2_pretokenScan` … `BPE_len15_pretokenScan` | One two-level scan kernel per piece length (below). |
| 24 | `BPE_TailV` | Output, in input order. |

Pieces of one byte need no work: their base id is already in place.

**Cuts** (`BPE_Cuts`, `--pair-cuts=3`). The cut stream is the OR of:

- pre-token starts and the input's first byte;
- before and after each **frozen byte**: a byte whose base id is in no rule (66 for
  GPT-2) is a piece of its own;
- **run cuts** (`--byte-run-cuts`, default 2): between the copies inside a run of
  spaces, and before a run where the byte before it and the space form no pair;
- the **cut classes**: codepoint classes compiled in kernel 3 together with the
  pre-tokenizer's classes. `Before`: no byte can precede this codepoint inside a
  token (cut before it). `BeforeCond`: some can (cut before it when the previous
  byte and its lead byte form no pair). `AfterLead`: its first two bytes form no
  pair (cut after the lead byte);
- byte-pair tests for what classes cannot express cheaply: after a codepoint end
  that never joins what follows, and between two continuation bytes that form no pair.

All tables come from the merge rules at start-up, so the kernel fits any merge list.

**Per-length kernels** (`--len-scan=15`). Kernel `BPE_len<L>_pretokenScan` walks
the starts of the L-byte pieces with Parabix's two-level scan (find the 64-bit
words with a set bit, then the bits):

- L = 2: one load from a 64K-entry table of byte pairs (the rule's id, or none:
  the two base ids stay).
- L = 3..15: a hash table of the L-byte tokens **whose own BPE gives back the
  token**, in 64-byte buckets compared at once (8 keys for L ≤ 4, 6 for L ≤ 8, 3
  above). A hash multiplier is searched per length so that every token fits
  (`--len-hash-search`; GPT-2: 3.7 MB, built in about 35–45 ms). A hit writes one id.
- A miss of L = 3 or 4 is finished in generated code (`--len3-pair-check`,
  `--len4-pair-check`): the piece is not one token, so its BPE follows from which
  pairs are rules, their ranks, and (for L = 4) which 3-byte parts are tokens.
- Other misses call C++ (`bpe_len_miss`).

A piece that starts in a segment and ends in the next is finished by the kernel of
the segment it starts in (its lookahead covers the rest). The scan kernels update
the id stream and the "token starts here" stream in place (`InOut`).
`BPE_LongPieces` and the length kernels keep no state between calls, so Parabix
runs them on several segments at once.

**C++ miss path.** Two entry points:

- `bpe_len_miss` (a piece of up to 15 bytes that its length table misses): the
  thread cache, else BPE.
- `bpe_long_block` (from `BPE_LongPieces`): a piece over 15 bytes is looked up in
  the long cache (`--tail-long-cache`, pieces of 16..4096 bytes); else it is split
  at the byte pairs that no token holds and that `BPE_Cuts` did not cut
  (`--tail-hard-split`), and each part is finished on its own: one byte by its
  base id, up to 15 bytes by the vocabulary table, the thread cache, else BPE;
  longer by BPE.

The structures behind them:

- **Vocabulary table** (`--tail-vocab-table`): tokens of 2..15 bytes whose own BPE
  gives back the token, in 4-entry sets. Built on a background thread while the
  pipeline compiles (about 14 ms; pieces before that just merge).
  `--tail-vocab-file` keeps it in a file for later runs.
- **Thread cache**: 2-way sets, 2 MB per thread, keyed by the piece's bytes.
  `--tail-cache-pool` keeps the caches across pipeline calls and threads.
- **BPE**: lowest rank first, leftmost on a tie. With `--tail-batch-bpe`, misses
  of up to 15 bytes are queued and merged several at a time without branches (one
  min over 16 lanes of `rank << 4 | position` picks the next pair; rules in a
  cuckoo table).

**Output** (`BPE_TailV`, `--tail-simd-emit`). It runs one call at a time in input
order. For each 64 positions it moves the ids at token starts to the front with a
vector compress (IDISA `mvmd_compress`) and stores them; words with a token
continued from another call go to a scalar C++ loop. The ids go to the output in
one batch per call.

### Why the output is exact

- The pre-token starts are GPT-2's regex boundaries (tested against HuggingFace).
- Cuts are only at pre-token starts and between bytes that no token holds next to
  each other. No merge crosses such a spot, so pieces can be finished separately.
- Every table entry is checked when it is built: a token is stored only if BPE on
  its own bytes gives that token back. A hit is therefore BPE's answer.
- Everything else goes to plain rank-order BPE.
- No piece is split by a segment edge: a short piece is finished by the length
  kernel of the segment it starts in, and a long piece cut by an edge is finished
  whole by `BPE_TailV`, which runs in input order.

### Options of the current design

All in `bpe.cpp`, off by default unless stated.

| Option | Meaning |
|---|---|
| `--tail-from-bytes` | With a pre-tokenizer: no merge kernels; BPE is finished per pre-token from the bytes. Without a pre-tokenizer it is ignored (with a message). |
| `--tail-scan-kernel` | Walk the pre-tokens in a two-level scan kernel that finishes one-byte pre-tokens and table hits in generated code. Needed by the options below; turns on `--tail-emit-from-segs`. |
| `--len-scan=N` | Pieces of 2..N bytes (N ≤ 15) in kernels of their own, `BPE_len<L>_pretokenScan`. 0 = off. |
| `--pair-cuts=0..3` | Cut between byte pairs that no token holds. 1: all, in a kernel `BPE_PairCuts`; 2: only pairs with a byte ≥ 0x80; 3: the cut classes and codepoint pair tests in `BPE_Cuts` (needs `--len-scan` and `--pretok-class-code --pretok-byte-domain --pretok-utf8-classes`). |
| `--tail-unit-seed` | Start the ids as every byte's base id from bit streams (`BPE_BaseIds` + `p2s_16`). Implies `--tail-unit-cuts`; with `--len-scan` the long pieces go to `BPE_LongPieces`. |
| `--tail-unit-cuts` | Cut at frozen bytes; one-byte pieces are finished as their base id. |
| `--byte-run-cuts=N` | Cut runs of N or more spaces into single bytes (default 2; 0 = off; active only with `--tail-scan-kernel`). `--byte-run-edges` (default 1), `--byte-run-bytes` (0: space only), `--byte-run-guard` (default on) tune it. |
| `--tail-vocab-table` | Answer single-token pieces from the vocabulary table. `--tail-vocab-file=PATH` reads it from, or writes it to, a file. `--tail-vocab-layout=1` is an experiment (one region per length; slower). |
| `--tail-cache-pool` | Reuse the per-thread caches across pipeline calls and threads. |
| `--tail-long-cache` | Cache pieces over 15 bytes. `--tail-long-admit=0..3` chooses which are kept (default 0: all). |
| `--tail-batch-bpe` | Merge misses of up to 15 bytes in batches, without branches. |
| `--tail-hard-split` | Split a miss at byte pairs that no token holds and look each part up. `--tail-trigram-split` also cuts pieces over 15 bytes where no token can cover 3 bytes around a spot. |
| `--tail-simd-emit` | Vector-compress output in generated code (`BPE_TailV`). |
| `--tail-emit-from-segs` | Start the emit stream from the pre-token starts (on whenever `--tail-scan-kernel` is). |
| `--tail-scan-ir-cache` | Check the thread cache's first entry in generated code (only for the general scan kernel, not with `--len-scan` + `--tail-unit-seed`). |
| `--len-hash-search=N` | Hash multipliers tried per length table so no token is left out (default 4096; 0 = fixed multiplier). |
| `--len3-pair-check`, `--len4-pair-check` | Finish 3- and 4-byte misses from the pair rules in generated code (default on). |
| `--len-long-kernel` | Long pieces in `BPE_LongPieces` (default on). |
| `--len-select-if=K[,K…]` | Compute the longer lengths' starts inside nested `If`s. |
| `--pair-cut-guard` | `--pair-cuts=1/2`: non-ASCII pairs inside an `If` (default on). |
| `--memsource-block-capacity` | Round the source buffer up to a whole block. Added for wrong last tokens when the last segment had 1..127 bytes; that case (70 × 16384 + 38 bytes) now gives the right ids without it. |

### Limitations

- **GPT-2-style vocabularies only.** Ids are 16 bits, ids are `256 + rank`, and the
  GPT-2 regex is built into the boundary kernels. GPT-2 and GPT-Neo match
  HuggingFace; larger vocabularies (`cl100k`, `o200k`, …) are not supported.
- `<|endoftext|>` is the only added token.
- The byte-domain pre-tokenizer assumes valid UTF-8.
- `bpe.cpp`'s top-of-file comment and the comment on `BPEPassResult` in `bpe.h`
  describe only the merge-kernel design.

## Older design: merge kernels

`buildBPEPassPipeline` without `--tail-from-bytes` builds one `BPEMergeKernel` per
group of merge rules. It gives the same ids (see [Validation](#validation)) but is
much slower: most merges happen on repeated words, which the current design's
tables and caches answer once, while every merge kernel scans every live
position. The code remains for comparison and for the options below.

**Partition.** `buildMergeRuleRanges` resolves each merge into
`MergeRule {idA, idB, lenA, lenB, idAB}` and groups rules into kernels run in rank
order. A group may only hold rules that are

1. dependency-independent: both parts were built by an earlier kernel; and
2. conflict-free: no rule's right part is another rule's left part (they would
   compete for the same token).

`--partition=clean` (default) cuts contiguous rank ranges: 1,123 kernels for GPT-2
(Python reference: `merge_analysis.py::clean_range_analysis`). `level` puts each
rule in its earliest legal kernel (ASAP scheduling): 283 kernels. `bit-tier` keeps
each kernel to merged ids of one bit length. `asymmetric` and `chain` relax the
conditions further.

**Merge kernel.** The id stream starts as the base ids (`BPE_BaseIds`); each kernel
reads the ids written by earlier kernels and, for each rule, writes `idAB` at A's
start where B starts `lenA` bytes later, then clears B's start from the live mask.
Rules within a kernel read the kernel's input, so their order does not matter.
`lenA` is A's length in input bytes, not in `Ġ`-style display bytes. Merges may not
cross a pre-token start (with `--pretokenizer=bytelevel`).

**Self-merges** (`X + X`): only the 1st, 3rd, 5th, … X of each run may fire, so a
run pairs left to right. `selfMergeFireStarts` takes the parity per run
(`EveryNth` + `MatchStar`, as in `SelfComposableLogic` of the normalizer).

**Compaction** (`--compaction`, `--compact-base`): `FilterByMask` between kernels
keeps only live token starts, so later kernels scan fewer positions. Lookahead
distances then count slots, not bytes: a token built before the compaction is one
slot, a later one spans the slots of its parts.

**Bit transformations** (`--bit-xfrm-limit`): a kernel whose output ids have at
most N bits computes each output bit as the input bit XOR a character class over
the `(idA, idB)` pair, instead of one test per rule; ids over 10 bits are split
into subtiers.

**Hybrid** (`--bpe-tail=P`): merge kernels up to compaction point P, then the
emitter finishes BPE per segment in C++. `--retire-words` and its options drop
finished words from the stream at compaction points.

### Merge-kernel options

| Option | Meaning |
|---|---|
| `--partition=clean\|level\|asymmetric\|chain\|bit-tier` | Rule partition (default `clean`). |
| `--max-merges-per-kernel=N` | Split groups over N rules (default 500; 0 = no cap). |
| `--compaction=linear\|geometric\|by-output-bits`, `--compact-base=N`, `--geometric-compaction` | Where `FilterByMask` compactions go. `BPE_COMPACT_EVERY` overrides `--compact-base`. |
| `--if-group-lower-limit=N`, `--if-group-count=K`, `--if-group-size=S` | Gate groups of rules by one `If` on their id range, in kernels from index N on. |
| `--if-test-significant-bits=K`, `--embedded-if-bits=E` | Gate rules by the K bits of idA from its highest 1 bit; nested inner gates. |
| `--key-cluster`, `--key-cluster-cap=N` | Move rules so that rules with one gate key share a kernel. |
| `--lookahead-in-gate`, `--lookahead-in-group` | Where the B lookahead is built. |
| `--indexed-shift=N` | First N kernels read the next live id with `IndexedShiftBack` instead of `LookAhead`. |
| `--batch-writeback` | One `Advance` per distinct `lenA` and one select per id bit per kernel, instead of per rule. |
| `--infile-once`, `--gate-mask-fold`, `--if-reduce-test`, `--if-unlikely-weight=N`, `--skip-instcombine` | Code-generation variants. |
| `--chain-fuse-siblings`, `--chain-ungate-roots` | With `--partition=chain`. |
| `--bit-xfrm-limit=N`, `--subtier-gating-bits=K`, `--subtier-grouping-by-min-ID=N` | Bit-transformation kernels. |
| `--self-merge-block` | All self-merges of a kernel in one gated block. |
| `--bpe-tail=P`, `--tail-direct-input` | The hybrid. |
| `--retire-words`, `--retire-points=…`, `--retire-frozen`, `--retire-stuck=N` | Drop finished words at compaction points. |
| `--bit-frequency-statistics`, `--merge-frequency-statistics` | Report id widths and merges per bit tier. |

## Data files

`tokenizer_files/`:

| File | Size | Contents |
|---|---|---|
| `merges.txt` | 456 KB | GPT-2 merges (50,000), the BPE input |
| `vocab.json` | 1.1 MB | GPT-2 vocabulary (optional for the tokenizer; used by the test scripts) |
| `tokenizer.json` | 3.6 MB | HuggingFace's GPT-2 config, used by the test scripts |
| `val_4MB.txt` | 4.2 MB | Web text (OpenWebText-style), CRLF line ends, some NUL runs |
| `code_4MB.txt` | 4.2 MB | C++ source code |
| `multi_4MB.txt` | 4.2 MB | Text in many languages |
| `stress.txt` | 5.0 MB | Adversarial mix: long runs, odd bytes, scripts |
| `test_vocab.txt` | 374 KB | Every GPT-2 vocabulary token, one per line |

The model files come from
[openai-community/gpt2](https://huggingface.co/openai-community/gpt2/tree/main).
`tokenizer_files/langs/` (per-language samples) is ignored by git.

## Debugging

Environment variables, set before a run:

| Variable | Effect |
|---|---|
| `BPE_DUMP=PREFIX` | Write the byte offsets of the pre-token starts and the cuts to `PREFIX.segs` and `PREFIX.cuts` (current design). |
| `BPE_DUMP_CLASSES=1` | With `BPE_DUMP`: also the class streams of the byte-domain pre-tokenizer. |
| `BPE_EMIT_DBG=1` | Merge kernels: print every emitted id and its two bytes. |
| `BPE_VOCAB_SYNC=1` | Build the vocabulary table before running, not on a background thread. |
| `BPE_VOCAB_BITS=N`, `BPE_LEN_LOAD=F` | Vocabulary table size; length table load (default 0.5). |
| `BPE_DBG=seed`, `BPE_DBG=bound` | Merge kernels: print base ids, or the pre-token boundary mask, and stop. |
| `BPE_GROUPS=1`, `BPE_RULES=1`, `BPE_RULES_N=k` | Merge kernels: print the rule groups, and the rules of each (k per group, default 4). |

`-EnableCycleCounter` prints each kernel's cycles. To check that two option sets
give the same output, compare their id streams (`cmp`) on the same input, or use
`tokenizer-test/bench_configs.py --verify`.

## Validation

Run on 2026-11-15, commit `a02100f9e`, macOS (Apple M3), scripts in
`tokenizer-test/`:

- **BPE**, recommended options: same ids as HuggingFace `tokenizers` 0.23.3 and
  `tiktoken` 0.14.0 on `val_4MB.txt` (1,302,487 tokens), `code_4MB.txt`
  (1,729,186) and `multi_4MB.txt` (2,230,628), at 1 and 4 threads; also the same
  as HuggingFace `tokenizers` 1.0.0rc4 on `val_4MB.txt`.
- **BPE**, merge kernels (no options, 1,123 kernels): same ids on `val_4MB.txt`.
- **Self-merges** (`selfmerge_test.py`, merge kernels, no pre-tokenizer): all 8
  built-in option sets match the reference BPE.
- **Pre-tokenizers**: all 9 modes with a HuggingFace counterpart, and all 5
  behaviors for `simplewordboundaries` and `punctuation`, match on 200 lines of
  each test file.
- **Normalizers**: all 19 modes and sequences match on the same lines.

**Speed**, same machine, `bench_bpe.py` with cold caches, median of 10 runs per
engine on each 4 MB file: 12.7–17.5 ms at 1 thread (10.8–15.4× faster than
tiktoken, 78–88× faster than HuggingFace 0.23.3), 3.9–5.9 ms at 4 threads
(13.1–15.8× and 58–86×). Tokenization only: start-up and output are not timed.
Full table and commands: [tokenizer-test/README.md](tokenizer-test/README.md#reference-results).

## Testing

`tokenizer-test/` holds the scripts that compare against HuggingFace and tiktoken
and the benchmarks. See [tokenizer-test/README.md](tokenizer-test/README.md).

## License

Parabix is governed by the Open Software License 3.0. See `OSL3.0.txt` in the
repository root.
