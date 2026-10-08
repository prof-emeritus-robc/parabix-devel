# Tokenizer Test Suite

This directory holds the test and benchmark harness for the **Parabix tokenizer**
(`tools/lex`). Scripts 1–4 compare Parabix against the **HuggingFace
`tokenizers` library** running the same GPT-2 model, so HuggingFace is the ground
truth for both correctness and performance. Script 5 is the exception: it
benchmarks Parabix configurations against **each other** and never loads HF.

None of these are wired into `make check` — run them directly from this directory
with the repo venv active.

| # | Script | Kind | What it covers |
|---|--------|------|----------------|
| 1 | `compare_normalizers.py`   | correctness | Text normalization (NFD/NFC/NFKD/NFKC, lowercase, strip accents, BERT, sequences) |
| 2 | `compare_pretokenizers.py` | correctness | Pre-tokenization / word splitting (whitespace, bytelevel, punctuation, …) |
| 3 | `compare_bpe.py`           | correctness + timing | Full BPE encoding → token IDs, plus a fair head-to-head timing section |
| 4 | `bench_bpe.py`             | performance | Throughput (MB/s), size sweep, fixed-floor vs marginal-rate fit, SVG charts |
| 5 | `bench_configs.py`         | performance (Parabix only) | N flag configurations timed against each other — interleaved rounds, per-config floor/marginal fit, output-identity check |
| 6 | `selfmerge_test.py`        | correctness (no HF) | Self-merges (X+X→XX): runs of repeated tokens of length 1–12, checked against a built-in reference BPE for several flag configurations |

Scripts 1–3 print `MATCH` / `MISMATCH` per case; all five tee their output to
`<script>_output.txt` in this directory (those result files are not committed).

---

## Requirements

- **Python 3.10+**
- **A built tokenizer binary**, by default `<repo>/build19/bin/tokenizer`
  (`make -C build19 -j8 tokenizer`). `compare_bpe.py` and `bench_bpe.py` take
  another one via `--tokenizer PATH`.
- **HuggingFace `tokenizers`** (PyPI, no account or token needed).
- **`datasets`** (PyPI) — only for `bench_bpe.py --openwebtext`, which streams a
  fresh corpus sample from the Hub instead of downloading the 40 GB dataset.

### One-time setup

```bash
cd ~/parabix-devel
python3 -m venv .venv
source .venv/bin/activate
pip install tokenizers
pip install datasets            # optional: only for --openwebtext
python -c "import tokenizers; print(tokenizers.__version__)"
```

### Paths

Every script finds the repo from its own location (three levels up from
`tools/lex/tokenizer-test/`), so it runs from any directory and any checkout.
Every path can be overridden on the command line:

| Option | Default | `compare_bpe` | `bench_bpe` | `bench_configs` | `compare_normalizers`, `compare_pretokenizers` |
|---|---|---|---|---|---|
| `--input PATH` | see each script below | ✓ | ✓ | ✓ | ✓ |
| `--tokenizer PATH` | `<repo>/build19/bin/tokenizer` | ✓ | ✓ | ✓ | ✓ |
| `--merges PATH` | `<repo>/tools/lex/tokenizer_files/merges.txt` | ✓ | ✓ | ✓ | — |
| `--tokenizer-json PATH` | `<repo>/tools/lex/tokenizer_files/tokenizer.json` | ✓ | ✓ | — | — |
| `--vocab PATH` | `<repo>/tools/lex/tokenizer_files/vocab.json` | ✓ | — | — | — |

Each script stops with an error when the tokenizer binary is missing (except
with `--hf-only`).

```bash
python compare_bpe.py --no-timing --input=/path/to/test.txt --tokenizer=/path/to/build/bin/tokenizer
python bench_bpe.py --input=/path/to/test.txt --tokenizer=/path/to/build/bin/tokenizer \
    --parabix-args="--pretokenizer=bytelevel --partition=level"
python compare_pretokenizers.py --input=/path/to/test.txt --tokenizer=/path/to/build/bin/tokenizer
```

---

## Model files

The three GPT-2 model files in `tools/lex/tokenizer_files/` are committed. The
truncated dev copies directly under `tools/lex/` (`vocab.json`, `merges.txt`) are
for fast manual smoke tests only — **the scripts never use them**.

| File | Size | Used by |
|------|------|---------|
| `merges.txt`     | 456 KB | Parabix side (`--merges`). Self-sufficient: `buildBaseAlphabet` regenerates GPT-2 ids 0..255 internally |
| `vocab.json`     | 1.0 MB | Parabix side, optional (id cross-check + `--strings` decode) |
| `tokenizer.json` | 3.5 MB | HuggingFace side (`Tokenizer.from_file`) — vocab + merges + pre-tokenizer config in one file |

All three come from `openai-community/gpt2` on the Hub, so both sides run an
identical model. To re-fetch them manually:
[huggingface.co/openai-community/gpt2](https://huggingface.co/openai-community/gpt2/tree/main).

## Corpora — supply your own

**No test corpus is committed.** The scripts' default input paths point at local
files that are not in the repo, so a fresh checkout must pass `--input`
(and, for `bench_bpe.py`, `--sweep-source`) explicitly, or create the corpora
first.

| Default the scripts expect | Used by |
|----------------------------|---------|
| `build19/test.txt` | `compare_normalizers.py`, `compare_pretokenizers.py` |
| `tokenizer_files/val_4MB.txt` | `compare_bpe.py` |
| `tokenizer_files/webtext_10.txt`, `webtext_100.txt`, `webtext_cases.txt` | `bench_bpe.py` default corpus list and sweep source |

Any UTF-8 text works for scripts 1–3. For `bench_bpe.py`, size matters — the
sweep needs multi-MB inputs to condition its fit (see below), and the easiest way
to get one without hunting for a corpus is `--openwebtext`, which streams a
~24 MB sample from the Hub and caches it as
`tokenizer_files/openwebtext_sample.txt`. Sweep sizes larger than the source are
skipped with a reason on stderr rather than failing.

---

## 1. `compare_normalizers.py`

Compares the **raw normalized string** from each Parabix `--normalize=X` mode
against the equivalent HuggingFace normalizer. Normalization is a pure string
transform — no splitting happens yet.

**Modes covered** (18 total, including sequences):
`nfd`, `nfc`, `nfkd`, `nfkc`, `stripaccents`, `stripleft`, `stripright`,
`strip`, `bytelevel`, `lowercase`, `nmt`, `bertchinesechars`,
`bertcleantext,stripaccents,lowercase`,
`bertcleantext,bertchinesechars,stripaccents,lowercase`,
`bytelevel,lowercase`, `lowercase,strip`, `bytelevel,strip`,
`bytelevel,strip,lowercase`.

Comma-separated names map to a HuggingFace `Sequence([...])` or a single
`BertNormalizer` with the matching flag combination.

**How it works:** for each mode, run `tokenizer --normalize=<mode> input.txt` as a
subprocess and capture stdout, run the equivalent Python normalizer on the same
text, compare the strings character-for-character.

```bash
python compare_normalizers.py                        # all modes
python compare_normalizers.py lowercase nfd strip    # specific modes only
python compare_normalizers.py --hf-only              # HuggingFace side only
python compare_normalizers.py --parabix-only         # Parabix side only
python compare_normalizers.py --input file.txt       # custom input
```

Output → `compare_normalizers_output.txt`.

---

## 2. `compare_pretokenizers.py`

Compares each Parabix `--pretokenizer=X` mode against the equivalent HuggingFace
pre-tokenizer. A pre-tokenizer splits text into pre-tokens (rough word-level
chunks) before any BPE merging.

**Modes covered:** `whitespacesplit`, `whitespace`, `bytelevel`, `punctuation`,
`digits`, `chardelimiter`, `sequence_whitespace_punct`.

It also runs **behavior tests** from `tokenizertest.xml` (committed alongside the
scripts) — Parabix-specific edge cases, mostly `--behavior=` delimiter handling,
with no HuggingFace counterpart. Those only run in `--parabix-only` mode.

```bash
python compare_pretokenizers.py                      # all modes
python compare_pretokenizers.py whitespace bytelevel # specific modes only
python compare_pretokenizers.py --hf-only
python compare_pretokenizers.py --parabix-only       # + XML behavior tests
python compare_pretokenizers.py --input file.txt
```

Output → `compare_pretokenizers_output.txt`.

---

## 3. `compare_bpe.py`

The end-to-end correctness check: full GPT-2 BPE encoding, Parabix vs
HuggingFace, compared as **integer ID lists**.

### One subprocess, not two

Earlier revisions ran Parabix in two passes (pre-tokenize to text, then BPE over
that text). That is gone. The script now makes **one** call per side:

```
tokenizer --pretokenizer=bytelevel --vocab=vocab.json --merges=merges.txt <input>
```

With `--merges` **and** `--pretokenizer=bytelevel`, `tokenizer.cpp` takes the
**boundary-gated** path: the bytelevel pre-tokenizer contributes only a
pretoken-**start mask** (`GPT2PretokenBoundaryKernel`), and BPE consumes the
**raw** byte basis plus that mask, so merges cannot cross a pretoken boundary.

> ⚠️ **Double-encode trap.** Do not pipe `--pretokenizer=bytelevel` *text output*
> into a second BPE call. `BPERangeSeed` already applies GPT-2 `bytes_to_unicode`
> to the raw bytes. Feeding it pre-encoded text encodes twice: a space `0x20`
> becomes `Ġ`, emitted as UTF-8 `0xC4 0xA0`, which the seed re-maps to ids
> 128/254 instead of the correct `Ġ` id 220.

### Test cases

The **entire input file is one test case** — a single `encode(text)` call on the
HuggingFace side, a single byte stream on the Parabix side. This matches the HF
API shape (one string in, one ID list out).

The script also collects token **strings** with a second `--strings` call, so a
mismatch prints a side-by-side table (`#`, Parabix ID, Parabix token, HF ID, HF
token, match), plus a first-divergence position and any length delta.

### Timing section

Compare mode also runs a fair timing pass. **It is on by default** — no flag
turns it on, `--no-timing` turns it off, `--runs N` changes the iteration count
(default 5). It costs N extra tokenizations per side on top of the correctness
pass, so on a multi-MB corpus it roughly doubles the runtime:

- **Parabix** — `--bench-loop=N`: pipeline built once, run N times in-process,
  token output suppressed; the script parses the `BENCH_RESULT …` stderr line.
- **HuggingFace** — warm best-of-N in-process `encode()` loop, model preloaded
  by `Tokenizer.from_file()`.

One-time setup (spawn + merges load + JIT on one side, `from_file()` on the
other) is excluded from both. Reported as min / median / mean ms and MB/s.

```bash
python compare_bpe.py --input path/to/corpus.txt   # no corpus is committed
python compare_bpe.py --verbose                    # always print the token table
python compare_bpe.py --show-input                 # print the input text, not just a byte count
python compare_bpe.py --runs 10                    # timed iterations per side (timing is ON by default, N=5)
python compare_bpe.py --no-timing                  # correctness only — the only way to skip timing
python compare_bpe.py --parabix-only
python compare_bpe.py --hf-only
```

Output → `compare_bpe_output.txt`.

---

## 4. `bench_bpe.py`

The **performance** counterpart to `compare_bpe.py`. It mirrors HuggingFace's own
criterion methodology (`tokenizers/benches/*.rs`): fixed corpus, N warm
iterations, primary metric **MB/s** rather than raw milliseconds. HF's repo only
benches itself for regressions, so this cross-engine comparison is
Parabix-specific.

### Fairness

A naive subprocess timing of the Parabix binary would also pay OS process spawn,
`merges.txt` load, Pablo pipeline build, and (cold) JIT compile on every call —
none of which is tokenization. `--bench-loop=N` builds the pipeline once, mmaps
the input once, then runs it N times in the same process with token output
suppressed, and prints:

```
BENCH_RESULT bytes=… iters=… min_ms=… median_ms=… mean_ms=… mbps=…
```

The script parses the **min** iteration — the steady-state peak, exactly what
criterion reports. HF is timed as a warm in-process `encode()` loop over the
whole-file string (not `encode_batch` over pre-split lines, so both sides see one
contiguous stream), with the model preloaded.

### Peak MB/s vs marginal MB/s

Parabix pays a **fixed per-call pipeline-dispatch floor** — the cost of invoking
its merge-range kernels (1123 for full GPT-2 merges), roughly 160–190 ms
independent of input size. On small inputs that floor dominates, so peak MB/s is
a floor artifact rather than a real rate.

`--sweep` cuts increasing-size prefixes (4 KB → 20 MB) from a source corpus and
least-squares fits

```
min_ms = floor + slope · bytes
```

per engine, reporting the intercept as the fixed floor and `1e-3 / slope` as the
**marginal MB/s** — the floor-free steady-state rate the SIMD data-parallelism
actually delivers. R² and point count are printed with the fit. The multi-MB tail
carries most of the leverage: at 1 MB the floor is still ~24% of `min_ms`, past
8 MB it is a few percent.

### Adaptive iterations

Multi-MB inputs cost seconds per iteration, so above 1 MB the iteration count
scales down to hold wall-clock roughly constant, with a floor of 3 samples
(criterion's own minimum). `min_ms`'s relative noise shrinks as the input grows,
so this costs almost no precision. Disable with `--no-adaptive-iters`.

### Usage

```bash
python bench_bpe.py --input file.txt         # one file
python bench_bpe.py --sweep --sweep-source file.txt   # prefix sweep + floor/marginal fit
python bench_bpe.py --sweep --sweep-max-mb 1 # cheap sub-MB sweep
python bench_bpe.py --sweep-sizes 1,4,8      # explicit sizes in MB
python bench_bpe.py --iters 20               # timed iterations per side
python bench_bpe.py --no-adaptive-iters
python bench_bpe.py --openwebtext            # stream + cache a real-world sample
python bench_bpe.py --openwebtext --owt-bytes 50000000
python bench_bpe.py --big                    # append the large corpus, if present
python bench_bpe.py --svg bench.svg          # also emit a MB/s bar chart
python bench_bpe.py --parabix-only
python bench_bpe.py --hf-only
```

`--parabix-args "<flags>"` passes raw tokenizer flags straight through to every
Parabix invocation — this is how the BPE optimization flags below get swept:

```bash
python bench_bpe.py --sweep --sweep-source corpus.txt \
    --parabix-args "--if-group-lower-limit=100 --if-group-size=8"
```

Output → `bench_bpe_output.txt`, plus an SVG chart when `--svg` is given.

---

## 5. `bench_configs.py`

Parabix **against itself** — no HuggingFace, no `tokenizers` import, no
`vocab.json`. `bench_bpe.py --parabix-only --parabix-args "…"` times ONE config
per invocation; this script times N configs in one run and tabulates them, which
is what you want when the question is "which flag combination is fastest", not
"are we faster than HF".

Three things it does that repeating `bench_bpe.py` cannot:

- **Interleaving.** Sequential benchmark runs are minutes apart and laptop
  clocks drift (thermal throttle, background load), so the config that runs last
  is penalised. Each round here round-robins every config, and each config keeps
  its **min across rounds** — drift hits all configs alike.
- **Floor vs marginal rate.** With `--sizes`, each config is timed at several
  prefix sizes and `min_ms = floor + slope·bytes` is fitted per config, so the
  fixed per-call dispatch floor (kernel-count driven) is separated from the
  floor-free per-byte rate. A single MB/s number at one size is mostly the
  floor — see [Peak MB/s vs marginal MB/s](#peak-mbs-vs-marginal-mbs).
- **Output identity.** `--verify` re-runs each config *without* `--bench-loop`
  (so ids actually print) and hashes stdout against the baseline config. A
  config that is faster but prints different tokens is flagged `DIFFERS` — a
  faster-but-wrong config is not a win.

```bash
# default pair: no flags vs --partition=level, on val_2MB
python bench_configs.py

# explicit configs — NAME=FLAGS, first one is the speedup baseline
python bench_configs.py \
    --config 'baseline=' \
    --config 'level=--partition=level' \
    --config 'level+compact=--partition=level --compact-base=40'

# floor / marginal split + output-identity check
python bench_configs.py --input ../tokenizer_files/val_4MB.txt \
    --sizes 0.25,1,4 --verify

# noisy machine: more interleaved rounds, fewer in-process iterations
python bench_configs.py --rounds 5 --iters 3

# time the raw-byte path instead of the boundary-gated one
python bench_configs.py --common-flags=''
```

`--iters` is the in-process `--bench-loop` count (default 5), `--rounds` the
number of interleaved passes (default 3). A warm-up run per (config, size) pays
the JIT and fills `~/.parabix/objcache/` before any timing starts. Prefix sizes
larger than the input are skipped with a reason on stderr. Output →
`bench_configs_output.txt`.

`--common-flags` (default `--pretokenizer=bytelevel`) is prepended to every
config, so all of them share a pipeline shape and only the flags under test
differ. That default is the boundary-gated pipeline `compare_bpe.py` verifies;
`--common-flags=''` selects the raw-byte path instead. The banner records what
was applied.

The `vs base` column and the relative marginal rate are against the **first
named** config, looked up by name — if it produces no result at some size, that
table says so and omits the speedups rather than promoting another config to
denominator. With `--sizes`, `--verify` hashes the smallest prefix: it runs
without `--bench-loop` so every token id prints, and two configs that tokenize
identically do so at any size.

The run that answers "which lever does what":

```bash
python3 bench_configs.py \
    --input ../tokenizer_files/val_8MB.txt \
    --sizes 0.25,1,2,4,7.5 \
    --config 'baseline=' \
    --config 'level=--partition=level' \
    --config 'level+compact=--partition=level --geometric-compaction --compact-base=15' \
    --verify
```

Read the floor/marginal table, not the per-size MB/s. `--partition=level` cuts
kernel count, so it moves the **floor** roughly in proportion; compaction shrinks
the stream every kernel scans, so it moves the **marginal MB/s**. A flag that
moves neither is doing nothing. `floor @ largest` says how much of the biggest
run is still dispatch overhead — while that is high, end-to-end speedups mostly
measure the floor and will shrink as the input grows.

Sweeping the compaction interval is the same command with several
`--compact-base=N` configs.

---

## 6. `selfmerge_test.py`

Correctness of self-merges, which the merge kernels resolve with run-parity logic: a
run of equal tokens must pair left to right, each token used once (`XXXXX` → `XX XX X`).
The script generates its own input (runs of length 1–12 of single bytes, multi-byte
characters and multi-character units, in several contexts, plus mixed alternations)
and compares the tokenizer's ids with a self-contained reference BPE built from
`merges.txt` (priority queue, lowest rank then leftmost first, as in HuggingFace), so
it needs neither HuggingFace nor `vocab.json`.

```bash
python selfmerge_test.py --tokenizer ../../../build22/bin/tokenizer
python selfmerge_test.py --config 'mine=--partition=level --compact-base=2' --keep /tmp/sm
```

Each `--config NAME=FLAGS` is one tokenizer run (the default is a built-in set of merge
strategies); `--keep DIR` saves the input, the reference ids and each output. Prints
`MATCH` / `MISMATCH` per configuration and exits 1 on any mismatch. The first run of a
new configuration pays its JIT compile.

---

## Tokenizer flags

The binary's CLI is documented once, in the parent
[tools/lex/README.md](../README.md):

- **[All CLI flags](../README.md#all-cli-flags)** — `--merges`, `--vocab`,
  `--strings`, `--offsets`, `--bench-loop`, `--merges-limit`,
  `--strip-newlines`, and the normalizer / pre-tokenizer modes.
- **[BPE optimization parameters](../README.md#bpe-optimization-parameters)** —
  `--compact-base`, `--geometric-compaction`, `--if-group-lower-limit`,
  `--if-group-count`, `--if-group-size`, `--lookahead-in-gate`,
  `--lookahead-in-group`, `--partition=level`, `--indexed-shift`, each with its
  objcache-name tag. All default OFF. Sweep them against HF via
  `bench_bpe.py --parabix-args`, or against each other via
  [`bench_configs.py --config`](#5-bench_configspy); the parent's
  [Correctness testing](../README.md#correctness-testing) and
  [Performance testing](../README.md#performance-testing) sections have the
  A/B recipe.

---

## Gotchas

- **No corpus ships with the repo.** Every script's default `--input` points at a
  local file that is not committed. Pass `--input` explicitly on a fresh
  checkout, or use `bench_bpe.py --openwebtext` to fetch one.
- **Wipe the JIT object cache after a rebuild**, not between flag variants.
  `BPEMergeKernel`'s cache name encodes a hash of its rule set, its lengths, its
  stream widths, and the active optimization flags, so two flag settings never
  collide — but a stale entry from an older *build* can fake both passes and
  failures. `rm -rf ~/.parabix/objcache/`.
- **Rebuild framework dylibs after a pull.** `dyld: Library not loaded:
  @rpath/libparabix_*.dylib` means the dependency chain is stale — run
  `make -C build19 -j8 tokenizer`.
- **First run of a new merge set is slow.** 1123 kernels have to JIT-compile.
  Subsequent runs hit the object cache. Use `--merges-limit` while iterating.
- **`*_output.txt` files are not committed and are not proof of the current
  build.** They are whatever the last local run wrote. Check the mtime, or just
  re-run, before quoting a result.
- **HF applies its regex pre-tokenizer; Parabix runs boundary-gated.** The two
  agree on the corpora tested so far, but the gating mechanisms differ —
  divergence first shows up on contraction / optional-space edge cases.

---

## Reference results

Measured locally on a 4.2 MB webtext-derived corpus (not committed). A sanity
reference, not a continuously verified state — the authoritative correctness
record is the parent's
[Validation status](../README.md#validation-status).

| Script | Result |
|--------|--------|
| `compare_normalizers.py` | 18/18 PASS |
| `compare_pretokenizers.py` | 7/7 PASS |
| `compare_bpe.py` | **MATCH** — 1,302,487 tokens on both sides (4,163,141 bytes) |
| `bench_bpe.py` (4 MB sweep point, 3 iters) | Parabix 974.9 ms / **4.30 MB/s** vs HF 1078.7 ms / 3.86 MB/s → Parabix 1.11x faster |

Same corpus through `compare_bpe.py`'s own timing section (5 runs) measured
Parabix at 2.34 MB/s against HF's 3.99 MB/s, while the `bench_bpe.py` sweep point
at the same size measured 4.30 vs 3.86 MB/s. The gap is the fixed dispatch floor
and the iteration count — always state which harness a number came from.

---

## License

Parabix is governed by the Open Software License 3.0. See `OSL3.0.txt` in the
repository root.
