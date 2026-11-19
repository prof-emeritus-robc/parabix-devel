# Tokenizer Test Suite

Test and benchmark scripts for the Parabix tokenizer (`tools/lex`). Most of them
compare Parabix with **HuggingFace `tokenizers`** and **OpenAI `tiktoken`**
running the same GPT-2 model: these two are the references for correctness and
speed. `bench_configs.py` compares Parabix option sets with each other, and
`selfmerge_test.py` checks Parabix against a small reference BPE of its own.

None of them run under `make check`. Run them from this directory with the repo's
venv.

| Script | Kind | What it covers |
|---|---|---|
| `compare_normalizers.py` | correctness | Normalizers, against HF `tokenizers.normalizers` |
| `compare_pretokenizers.py` | correctness | Pre-tokenizers and `--behavior`, against HF `tokenizers.pre_tokenizers` |
| `compare_bpe.py` | correctness + timing | BPE token ids against HF `tokenizers` 0.23 and tiktoken |
| `compare_bpe_hf1.py` | correctness + timing | The same against HF `tokenizers` 1.0.0rc4 |
| `bench_pretokenizers.py` | correctness + performance | Pre-tokenizers on their own (no BPE) against HF 0.23: whole-file pre-token lists, then time |
| `bench_bpe.py` | performance | Throughput (MB/s) against HF 0.23 and tiktoken |
| `bench_bpe_hf1.py` | performance | The same against HF 1.0.0rc4 |
| `bench_configs.py` | performance (Parabix only) | Several Parabix option sets timed against each other, with an output-identity check |
| `selfmerge_test.py` | correctness (no HF) | Runs of repeated tokens (X+X→XX merges), against a built-in reference BPE |

Each script copies its report to `<script>_output.txt` in this directory. Those
files are not committed, and they hold whatever the last local run wrote: re-run
before quoting one.

---

## Requirements

- **Python 3.10+**.
- **A built tokenizer**, by default `<repo>/build19/bin/tokenizer`
  (`make -C build19 -j8 tokenizer`); every script takes `--tokenizer PATH`.
- **HuggingFace `tokenizers`** and **`tiktoken`** from PyPI. The results below use
  `tokenizers` 0.23.3 and `tiktoken` 0.14.0. `compare_bpe.py` and `bench_bpe.py`
  import both.
- **`tokenizers` 1.0.0rc4**, only for the `*_hf1.py` scripts, in a side folder of
  the same venv (it has the same package name as 0.23).
- **`datasets`**, only for `bench_bpe.py --openwebtext`.

### One-time setup

```bash
cd <repo>
python3 -m venv .venv
.venv/bin/pip install "tokenizers==0.23.3" "tiktoken==0.14.0"
.venv/bin/pip install datasets                      # optional: --openwebtext

# HF 1.0 for compare_bpe_hf1.py / bench_bpe_hf1.py
.venv/bin/pip install numpy huggingface-hub
.venv/bin/pip install --no-deps --target .venv/hf1-site "tokenizers==1.0.0rc4"
```

The `*_hf1.py` scripts put `<venv>/hf1-site` (or `$HF1_SITE`) first on
`sys.path` before importing `tokenizers`, and stop unless they get version 1.x
and `tiktoken` 0.14.0 (`--any-tiktoken` allows another). The other scripts in the
same venv still get 0.23.3.

### Paths

Each script finds the repo from its own location (three levels up), so it runs
from any directory. Defaults and overrides:

| Option | Default | Scripts |
|---|---|---|
| `--tokenizer PATH` | `<repo>/build19/bin/tokenizer` | all |
| `--merges PATH` | `<repo>/tools/lex/tokenizer_files/merges.txt` | `compare_bpe*`, `bench_bpe*`, `bench_configs`, `selfmerge_test` |
| `--vocab PATH` | `<repo>/tools/lex/tokenizer_files/vocab.json` | `compare_bpe*` |
| `--tokenizer-json PATH` | `<repo>/tools/lex/tokenizer_files/tokenizer.json` | `compare_bpe*`, `bench_bpe*` |
| `--input PATH` | none (required) | `compare_bpe*`; `bench_bpe*` take `--input FILE …`, `--sweep --sweep-source FILE` or `--openwebtext`; `bench_pretokenizers` takes `--input FILE …` |
| `--input PATH` | `<repo>/build19/test.txt` (not in the repo) | `compare_normalizers`, `compare_pretokenizers` |
| `--input PATH` | `tokenizer_files/val_2MB.txt` (no longer in the repo) | `bench_configs` |

So pass `--input` to every script except `selfmerge_test.py`, which writes its
own input.

---

## Inputs

The GPT-2 model files and three 4 MB test files are committed in
`tools/lex/tokenizer_files/` (see the parent [Data files](../README.md#data-files)):

| File | Contents | Tokens (GPT-2) |
|---|---|---|
| `val_4MB.txt` | Web text, CRLF line ends, some NUL runs | 1,302,487 |
| `code_4MB.txt` | C++ source | 1,729,186 |
| `multi_4MB.txt` | Many languages | 2,230,628 |

`merges.txt` is the Parabix input; `tokenizer.json` is what HuggingFace loads;
`tiktoken` uses its built-in `gpt2` encoding, which has the same vocabulary and
merges. So all three run the same model.

Two notes on line ends:

- `compare_bpe*.py` read the input in text mode, so `\r\n` becomes `\n`, and all
  engines tokenize that text. `bench_bpe*.py` keep `\r\n`, so every engine reads
  the file's own bytes.
- `compare_pretokenizers.py` and `compare_normalizers.py` split on `\n` only. Give
  them LF text (`tr -d '\r'`), or a leftover `\r` shows up as a mismatch.
  `bench_pretokenizers.py` reads the file's own bytes, CRLF included.

---

## `compare_normalizers.py`

Compares the normalized text of each `--normalize=X` with the matching HF
normalizer, character by character. 19 modes: `nfd`, `nfc`, `nfkd`, `nfkc`,
`stripaccents` (HF: `NFD` + `StripAccents`), `stripleft`, `stripright`, `strip`,
`bytelevel`, `lowercase`, `nmt`, `bertchinesechars`, `bertcleantext`, and the
sequences `bertcleantext,stripaccents,lowercase`,
`bertcleantext,bertchinesechars,stripaccents,lowercase`, `bytelevel,lowercase`,
`lowercase,strip`, `bytelevel,strip`, `bytelevel,strip,lowercase`.

```bash
python compare_normalizers.py --input FILE                 # all modes
python compare_normalizers.py --input FILE lowercase nfd   # some modes
python compare_normalizers.py --input FILE --hf-only       # HF side only
python compare_normalizers.py --input FILE --parabix-only  # Parabix side only
```

---

## `compare_pretokenizers.py`

Compares each `--pretokenizer=X` with the matching HF pre-tokenizer, line by line
(Parabix always ends a pre-token at `\n`). At most `--max-lines` lines (default
200; 0 = all).

| Parabix mode | HF pre-tokenizer |
|---|---|
| `whitespacesplit` | `WhitespaceSplit()` |
| `whitespace` | `Whitespace()` |
| `bytelevel` | `ByteLevel(add_prefix_space=False)` |
| `bert` | `BertPreTokenizer()` |
| `punctuation` | `Punctuation()` |
| `digits` | `Digits(individual_digits=True)` |
| `chardelimiter` | `CharDelimiterSplit(",")` |
| `sequence_whitespace_punct` (`--pretokenizer=sequence_whitespace_punctuation`) | `Sequence([WhitespaceSplit(), Punctuation()])` |
| `simplewordboundaries` | `Split(Regex(r"\w+"), "isolated")` |

`--behaviors` instead checks all 5 `--behavior=` values (`removed`, `isolated`,
`mergedwithprevious`, `mergedwithnext`, `contiguous`) for `simplewordboundaries`
(against `Split(Regex(r"\w+"), behavior)`) and `punctuation` (against
`Punctuation(behavior)`): 10 cases. `--parabix-only` also runs the
Parabix-specific cases in `tokenizertest.xml`, which have no HF counterpart.

```bash
python compare_pretokenizers.py --input FILE                       # all modes
python compare_pretokenizers.py --input FILE whitespace bytelevel  # some modes
python compare_pretokenizers.py --input FILE --behaviors           # the 10 behavior cases
python compare_pretokenizers.py --input FILE --parabix-only        # + tokenizertest.xml
python compare_pretokenizers.py --input FILE --hf-only
```

---

## `compare_bpe.py`

End-to-end check: GPT-2 BPE token ids from Parabix, HF and tiktoken. The whole
input is one test case: one `encode(text)` call for HF, one
`encode(text, allowed_special="all")` for tiktoken, one tokenizer run for Parabix:

```
tokenizer --pretokenizer=bytelevel --vocab=vocab.json --merges=merges.txt <PARABIX ARGS> <input>
```

`--parabix-args="…"` adds tokenizer options (use the `=` form). The script adds
`--pretokenizer=bytelevel` only when the arguments name no pre-tokenizer. It
prints `Extra tokenizer flags: …` when any are given; no such line means the
default options were tested.

**Pass the recommended options.** With none, the tokenizer runs the old
merge-kernel design: correct, but its first run compiles 1,123 kernels. With the
options from the parent README:

```bash
BEST="--pretokenizer=bytelevel --tail-from-bytes --tail-vocab-table --tail-cache-pool \
--tail-scan-kernel --tail-emit-from-segs --tail-long-cache --tail-batch-bpe --tail-hard-split \
--tail-simd-emit --skip-identity-normalize --pretok-compress-classes --pretok-class-code \
--fast-emit --memsource-in-place --pretok-byte-domain --pretok-utf8-classes \
--tail-unit-seed --pair-cuts=3 --len-scan=15"

python compare_bpe.py --no-timing --input ../tokenizer_files/code_4MB.txt \
    --parabix-args="$BEST --thread-num=4"
```

Output: `MATCH` or `MISMATCH` and the token count of each engine. A second
Parabix run with `--strings` gives the token strings, so a mismatch prints a
side-by-side table (Parabix, HF, tiktoken) and the first position where they
differ. `--verbose` always prints the table.

**Timing section** (on by default; `--no-timing` turns it off, `--runs N` sets
the count, default 5): Parabix with `--bench-loop=N` and the same arguments, HF
and tiktoken as warm in-process loops. It adds N tokenizations per engine; for
proper benchmarks use `bench_bpe.py`.

```bash
python compare_bpe.py --input FILE --parabix-args="$BEST"   # check + timing
python compare_bpe.py --input FILE --no-timing ...          # check only
python compare_bpe.py --input FILE --no-tiktoken ...        # Parabix vs HF only
python compare_bpe.py --input FILE --parabix-only | --hf-only | --tiktoken-only
python compare_bpe.py --input FILE --show-input             # print the input text
```

The docstring at the top of `compare_bpe.py` still describes an older two-step
run and line-by-line cases; the code does what is described here.

## `compare_bpe_hf1.py`

`compare_bpe.py` with HF `tokenizers` 1.0.0rc4 (its "bitcannon" bit-stream
pre-tokenizer, word cache and whole-word fold) and tiktoken pinned to 0.14.0. HF
1.0's `Tokenizer.from_file()` only loads; `compile()` builds the
`CompiledTokenizer` that encodes (setup, not timed). Its results have ids only, so
token strings come from `id_to_token()`. `--cold` empties the caches before every
timed run (a fresh `compile()` for HF); `--bench-cold-cache` in `--parabix-args`
has the same effect.

```bash
.venv/bin/python compare_bpe_hf1.py --no-timing --input FILE --parabix-args="$BEST"
```

---

## `bench_bpe.py`

Throughput of Parabix, HF and tiktoken, in the style of HF's own criterion
benchmarks: a fixed input, N warm runs, MB/s. Each engine is timed in process,
without start-up:

- **Parabix**: `--bench-loop=N` builds and compiles the pipeline once, runs it N
  times on the in-memory input with output off (after one warm-up run), and
  prints `BENCH_RESULT bytes=… iters=… min_ms=… median_ms=… mean_ms=… mbps=…`.
  The script parses that line. `--pretokenizer=bytelevel` is added unless
  `--parabix-args` names a pre-tokenizer.
- **HF**: `encode()` of the whole text, model loaded by `Tokenizer.from_file()`.
- **tiktoken**: `encode(text, allowed_special="all")` (`--tiktoken-ordinary`:
  `encode_ordinary`).

It prints min, median and mean ms and peak MB/s (from the min) for each engine and
input, and a speed-up line computed from the min.

**Threads.** `--baseline-threads N` splits the text at safe pre-token boundaries
and times `encode_batch` on N threads for HF (`RAYON_NUM_THREADS=N`) and tiktoken.
Give Parabix the same number with `--thread-num=N` for an equal-threads
comparison.

**Caches.** Parabix keeps its caches across `--bench-loop` runs unless
`--bench-cold-cache` is given. `--hf-cache=match` (default) clears HF's word
cache before every run exactly when Parabix runs cold; `warm` and `cold` force it.

**Iterations.** Above 1 MB the count is scaled down (to at least 3) to hold the
time per input; `--no-adaptive-iters` uses `--iters` for every input, which gives
every engine the same number of runs. `--parabix-iters N` sets Parabix's count alone.

**Size sweep.** `--sweep --sweep-source FILE` cuts prefixes from 4 KB to 20 MB
(`--sweep-sizes 1,4,8` in MB, `--sweep-max-mb`) and fits
`min_ms = floor + slope · bytes` per engine: the floor is a fixed cost per call,
`1e-3 / slope` the marginal MB/s. This mattered for the merge-kernel design, whose
floor was large; the current design has a small one. The fit needs inputs of
different sizes, so ignore it when the inputs are all the same size.

```bash
python bench_bpe.py --input ../tokenizer_files/val_4MB.txt ../tokenizer_files/code_4MB.txt \
    ../tokenizer_files/multi_4MB.txt --iters 10 --no-adaptive-iters \
    --parabix-args="$BEST --thread-num=1 --bench-cold-cache"
python bench_bpe.py --input FILE --baseline-threads 4 --parabix-args="$BEST --thread-num=4"
python bench_bpe.py --sweep --sweep-source FILE --parabix-args="$BEST"
python bench_bpe.py --openwebtext                   # stream and cache an OpenWebText sample
python bench_bpe.py --input FILE --svg bench.svg    # also an MB/s bar chart
python bench_bpe.py --input FILE --parabix-only | --hf-only | --tiktoken-only | --no-tiktoken
```

## `bench_bpe_hf1.py`

`bench_bpe.py` with HF 1.0.0rc4 and tiktoken 0.14.0. Differences:

- HF 1.0 is cold when Parabix is (`--hf-cache=match`): a fresh `compile()`, not
  timed, before every run.
- HF 1.0's `encode_batch` uses its own pool of one thread per core and ignores
  `RAYON_NUM_THREADS`. So with `--baseline-threads N` the script runs N Python
  threads, each calling `encode()` on pieces of the text (`encode()` releases the
  GIL), and checks the process CPU time against N. `--hf-native-batch` times HF's
  own `encode_batch` instead (all cores: not an equal-threads comparison).

---

## `bench_pretokenizers.py`

Each pre-tokenizer on its own (no normalizer, no BPE), Parabix against HF, one
thread on both sides. The 9 modes of `compare_pretokenizers.py`, plus
`bytelevel_frontend`: `--pretokenizer=bytelevel` with the BPE front end's
boundary builder (`--pretok-compress-classes --pretok-class-code
--pretok-byte-domain --pretok-utf8-classes`), as the recommended BPE options run it.

1. **Check.** One tokenizer run with `--pretok-separator=0x10FFFF` on the whole
   file, compared with HF's `pre_tokenize_str`: the complete pre-token list,
   strings and order (not offsets). U+10FFFF must not be in the input, so, unlike
   `\n`, it cannot be mistaken for a line break inside a pre-token. A case that
   does not match is reported and not timed.
2. **Time.** `--rounds` rounds (default 5) of `--iters` timed calls (default 10)
   per side, the side that goes first alternating by round:
   - **Parabix**: `--bench-loop=N --thread-num=1 --segment-size=16384`. Built,
     compiled and loaded once; one untimed run, then N timed runs, each writing
     its pre-tokens to `/dev/null`. The script reads `BENCH_SAMPLES_MS`.
   - **HF**: per call, a fresh `PreTokenizedString(text)` and an in-place
     `pre_tokenize` (strings and offsets stay native, no Python lists). One
     untimed call, then N timed.

It reports the median of the round medians, their range, and HF median / Parabix
median. The two sides are complete pipelines with different outputs (serialized
text vs native strings with offsets), not boundary finding alone. Each case runs
in a fresh process, so HF allocations do not pile up from case to case.

```bash
python bench_pretokenizers.py --input ../tokenizer_files/val_4MB.txt \
    ../tokenizer_files/code_4MB.txt ../tokenizer_files/multi_4MB.txt
python bench_pretokenizers.py --input FILE --modes whitespace bytelevel_frontend
python bench_pretokenizers.py --input FILE --rounds 1 --iters 1     # quick check
python bench_pretokenizers.py --input FILE --json results.json      # every sample too
```

---

## `bench_configs.py`

Parabix option sets timed against each other; no HF. One run times N sets:

- **Interleaved rounds**: each round runs every set once, and each set keeps its
  best time over the rounds (`--rounds`, default 3; `--iters`, the `--bench-loop`
  count, default 5), so machine drift hits all sets alike.
- **Output identity** (`--verify`): each set runs once more without
  `--bench-loop`, and its ids are hashed against the first set's: `IDENTICAL` or
  `DIFFERS`. A faster set with different output is a bug, not a win.
- **Sizes** (`--sizes 0.25,1,4`, in MB): the floor/marginal fit per set, as in
  `bench_bpe.py --sweep`.

`--common-flags` (default `--pretokenizer=bytelevel`) is put before every set, so
**leave `--pretokenizer` out of the sets** (the tokenizer rejects it twice). The
first `--config NAME=FLAGS` is the baseline of the `vs base` column. The `kernels`
column counts merge kernels and shows `-` for the current design. Example, the
per-length kernels against a single scan kernel:

```bash
B="--tail-from-bytes --tail-vocab-table --tail-cache-pool --tail-scan-kernel \
--tail-emit-from-segs --tail-long-cache --tail-batch-bpe --tail-hard-split --tail-simd-emit \
--skip-identity-normalize --pretok-compress-classes --pretok-class-code --fast-emit \
--memsource-in-place --pretok-byte-domain --pretok-utf8-classes --tail-unit-seed"

python bench_configs.py --input ../tokenizer_files/code_4MB.txt --verify \
    --config "best=$B --pair-cuts=3 --len-scan=15" \
    --config "single-scan=$B"
```

Add `--bench-cold-cache` to time cold caches. Without `--config`, the default
sets are the merge-kernel design with and without `--partition=level`.

---

## `selfmerge_test.py`

Self-merges (X+X→XX) need care in the merge-kernel design: a run of equal tokens
must pair left to right, each token used once (`XXXXX` → `XX XX X`). The script
writes its own input (runs of 1 to 12 copies of single bytes, multi-byte
characters and short strings, in several contexts, plus mixed patterns), and
compares the tokenizer's ids with a reference BPE in Python built from
`merges.txt` (lowest rank first, leftmost on a tie). It needs neither HF nor
`vocab.json`.

The tokenizer runs **without a pre-tokenizer** (the whole input is one sequence,
`\n` is content), so the test applies to the merge-kernel design; the current
design needs a pre-tokenizer.

```bash
python selfmerge_test.py                                   # 8 built-in merge-kernel option sets
python selfmerge_test.py --config 'mine=--partition=level --compact-base=2' --keep /tmp/sm
```

`--keep DIR` saves the input, the reference ids and each output. It prints
`MATCH` or `MISMATCH` per set and exits 1 on any mismatch. A new set's first run
compiles its kernels, which takes minutes.

---

## Tokenizer options

The tokenizer's options are documented once, in the parent
[README](../README.md): [Tokenizer options](../README.md#tokenizer-options),
[Options of the current design](../README.md#options-of-the-current-design) and
[merge-kernel options](../README.md#merge-kernel-options).

---

## Gotchas

- **Pass `--input`.** Only `selfmerge_test.py` has an input of its own.
- **Pass the recommended options to `compare_bpe*.py` and `bench_bpe*.py`.**
  Without them they test the slow merge-kernel design.
- **No need to clear `~/.parabix/objcache`** after a rebuild or between option
  sets: cached kernels belong to the build that made them, and kernel names
  encode their options. Use `-object-cache-dir=DIR` for a private cache.
- **Benchmark on an idle machine.** Other work on the machine (a build, another
  test, file writes) changes the timings, most of all at 4 threads.
- **Compare equal threads and equal caches**: `--thread-num=N` with
  `--baseline-threads N`, and `--bench-cold-cache` with `--hf-cache=match`.
- **`dyld: Library not loaded: @rpath/libparabix_*.dylib`** means the framework
  libraries are stale: run `make -C build19 -j8 tokenizer`.

---

## Reference results

Run on 2026-11-15, commit `a02100f9e`, Apple M3 (macOS), `tokenizers` 0.23.3,
`tiktoken` 0.14.0, with the recommended options (`$BEST` above).

**Correctness**

| Script | Input | Result |
|---|---|---|
| `compare_bpe.py`, `$BEST`, 1 and 4 threads | `val_4MB.txt` | MATCH, 1,302,487 tokens (Parabix, HF, tiktoken) |
| | `code_4MB.txt` | MATCH, 1,729,186 tokens |
| | `multi_4MB.txt` | MATCH, 2,230,628 tokens |
| `compare_bpe_hf1.py`, `$BEST` | `val_4MB.txt` | MATCH with HF 1.0.0rc4 and tiktoken |
| `compare_bpe.py`, no options (merge kernels) | `val_4MB.txt` | MATCH |
| `selfmerge_test.py` | its own input | 8/8 sets MATCH |
| `compare_pretokenizers.py` and `--behaviors` | first 200 lines of each 4 MB file, LF | 9/9 modes and 10/10 behavior cases PASS on each |
| `compare_normalizers.py` | the same lines | 19/19 PASS on each |
| `bench_pretokenizers.py`| 10/10 modes MATCH on each |

**Speed** (`bench_bpe.py --iters 10 --no-adaptive-iters`, cold caches:
`--bench-cold-cache`, HF word cache cleared before every run). Time per
tokenization of the whole 4 MB file, median of 10 runs (min in brackets);
speed-ups from the medians:

| Threads | Input | Parabix ms | tiktoken ms | HF 0.23.3 ms | vs tiktoken | vs HF |
|---|---|---|---|---|---|---|
| 1 | `val_4MB.txt` | 12.69 (12.63) | 137.0 (136.4) | 1,123 (1,095) | 10.8× | 88× |
| 1 | `code_4MB.txt` | 15.71 (15.67) | 241.3 (239.6) | 1,218 (1,203) | 15.4× | 78× |
| 1 | `multi_4MB.txt` | 17.53 (17.47) | 221.3 (216.9) | 1,396 (1,364) | 12.6× | 80× |
| 4 | `val_4MB.txt` | 3.95 (3.73) | 51.8 (50.5) | 338.5 (336.5) | 13.1× | 86× |
| 4 | `code_4MB.txt` | 5.84 (5.70) | 91.8 (86.9) | 340.4 (337.8) | 15.7× | 58× |
| 4 | `multi_4MB.txt` | 5.85 (5.71) | 92.3 (88.9) | 390.9 (385.2) | 15.8× | 67× |

At 4 threads HF and tiktoken run `encode_batch` over the text split at pre-token
boundaries (`--baseline-threads 4`). These times exclude start-up: a one-shot run
of the tokenizer also loads the rules, builds the tables and the pipeline (about
60 ms, `[BPE] pipeline graph build` on stderr), compiles or loads the kernels,
and formats the output.

**Pre-tokenizers alone** (`bench_pretokenizers.py`, defaults: 5 rounds of 10
calls, 1 thread; 2026-11-19, uncommitted change on top of `a2d03a697`, same
machine). HF median / Parabix median, both from the median of the 5 round
medians:

| Mode | `val_4MB.txt` | `code_4MB.txt` | `multi_4MB.txt` |
|---|---|---|---|
| `whitespace` | 5.36× | 5.16× | 5.43× |
| `whitespacesplit` | 4.63× | 3.99× | 4.43× |
| `digits` | 1.90× | 1.87× | 1.84× |
| `punctuation` | 6.90× | 9.47× | 6.22× |
| `simplewordboundaries` | 10.22× | 7.42× | 9.64× |
| `chardelimiter` | 1.14× | 1.18× | 1.29× |
| `bert` | 11.61× | 11.67× | 11.02× |
| `sequence_whitespace_punctuation` | 11.95× | 11.78× | 10.96× |
| `bytelevel` | 15.14× | 16.30× | 13.87× |
| `bytelevel_frontend` | 23.42× | 24.68× | 20.25× |

Parabix takes 17–33 ms per file in every mode; HF from 25 ms (`chardelimiter`)
to 529 ms (`bytelevel`).
