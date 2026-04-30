# Tokenizer Test Suite

This directory contains three Python scripts that compare the **Parabix tokenizer**
against the **HuggingFace `tokenizers` library**. Each script runs both sides on
the same input and reports MATCH, MISMATCH, or CRASH per test case.

The three scripts test the tokenization pipeline in the order it executes:

| # | Script | Stage tested |
|---|--------|--------------|
| 1 | `compare_normalizers.py`   | Text normalization (e.g. NFD, lowercase, strip accents) |
| 2 | `compare_pretokenizers.py` | Pre-tokenization / word splitting (e.g. whitespace, bytelevel) |
| 3 | `compare_bpe.py`           | BPE encoding (subword merging + vocab lookup → token IDs) |

---

## Requirements (apply to all three scripts)

- **Python 3.10 or later**
- **A built Parabix tokenizer binary** at `<repo>/build19/bin/tokenizer`. See
  the [main README](../README.md) for build instructions.
- **HuggingFace `tokenizers` package** (PyPI — no account or API token required)
- **For `compare_bpe.py` only**: GPT-2 model files in `tools/lex/tokenizer_files/`
  (`vocab.json`, `merges.txt`, `tokenizer.json`)

### One-time setup

Create a Python virtual environment and install the HuggingFace tokenizers
library:

```bash
cd ~/parabix-devel
python3 -m venv .venv
source .venv/bin/activate
pip install tokenizers
```

Verify the install:

```bash
python -c "import tokenizers; print(tokenizers.__version__)"
```

> **Note**: `compare_bpe.py` requires three GPT-2 model files in
> `tools/lex/tokenizer_files/`:
> - `vocab.json` — token → ID mapping (used by Parabix BPE)
> - `merges.txt` — BPE merge rules (used by Parabix BPE)
> - `tokenizer.json` — bundled HuggingFace tokenizer config (used by HF side)
>
> These files are committed to the repo but to download them automatically
> from HuggingFace Hub, install `huggingface_hub`:
> `pip install huggingface_hub`
>
> Otherwise download them manually from
> [huggingface.co/openai-community/gpt2](https://huggingface.co/openai-community/gpt2/tree/main)
> and place them in `tools/lex/tokenizer_files/`.

### Build directory path

The scripts default to `<repo>/build19/bin/tokenizer`. If you build into
`build19` (the standard setup used here), no changes are needed.

If your build directory has a different name (e.g. `build`), update these two
lines near the top of each script:

```python
DEFAULT_INPUT     = "/path/to/your/parabix-devel/build/test.txt"
DEFAULT_TOKENIZER = "/path/to/your/parabix-devel/build/bin/tokenizer"
```

---

## 1. `compare_normalizers.py`

### What it does

Compares the **raw normalized string output** of each Parabix `--normalize=X`
mode against the equivalent HuggingFace normalizer. Normalization is a pure
string transformation — no splitting happens at this stage.

Supported modes: `nfd`, `stripaccents`, `stripleft`, `stripright`, `strip`,
`bytelevel`, `lowercase`.

### Inputs

- **Text input file** (default: `<build>/test.txt`) — any UTF-8 text. Each
  normalization mode is tested on the same input.
- **Parabix tokenizer binary** — the compiled C++ executable.

### How the comparison works

For each mode (e.g. `lowercase`):

1. **Parabix side**: runs `tokenizer --normalize=lowercase input.txt` as a
   subprocess and captures stdout — the normalized string.
2. **HuggingFace side**: runs the equivalent normalizer (e.g.
   `tokenizers.normalizers.Lowercase()`) directly in Python.
3. Compares the two output strings character-for-character.
4. Reports MATCH if identical, MISMATCH otherwise.

### How to run

```bash
python compare_normalizers.py                        # all modes
python compare_normalizers.py lowercase nfd strip    # specific modes only
python compare_normalizers.py --hf-only              # HuggingFace only
python compare_normalizers.py --parabix-only         # Parabix only
python compare_normalizers.py --input file.txt       # custom input file
```

Output is written to `compare_normalizers_output.txt` and printed to terminal.

---

## 2. `compare_pretokenizers.py`

### What it does

Compares each Parabix `--pretokenizer=X` mode against the equivalent HuggingFace
pre-tokenizer. A pre-tokenizer splits a text string into a list of pre-tokens
(rough word-level chunks) before BPE merging happens.

Supported modes: `whitespacesplit`, `whitespace`, `bytelevel`, `punctuation`,
`digits`, `chardelimiter`, `sequence_whitespace_punct`.

It also runs **behavior tests** defined in `tokenizertest.xml` — Parabix-specific
edge cases that may have no HuggingFace equivalent.

### Inputs

- **Text input file** (default: `<build>/test.txt`).
- **Parabix tokenizer binary**.
- **`tokenizertest.xml`** — XML file in this directory listing additional
  behavior tests (used in `--parabix-only` mode).

### How the comparison works

For each mode (e.g. `bytelevel`):

1. **Parabix side**: runs `tokenizer --pretokenizer=bytelevel input.txt` as a
   subprocess and parses stdout into a list of pre-tokens.
2. **HuggingFace side**: runs the equivalent pre-tokenizer (e.g.
   `tokenizers.pre_tokenizers.ByteLevel()`) directly in Python on the same input.
3. Compares the two lists of pre-tokens element by element.
4. Reports MATCH, MISMATCH, or CRASH.

### How to run

```bash
python compare_pretokenizers.py                      # all modes
python compare_pretokenizers.py whitespace bert      # specific modes only
python compare_pretokenizers.py --hf-only            # HuggingFace only
python compare_pretokenizers.py --parabix-only       # Parabix + XML behavior tests
python compare_pretokenizers.py --input file.txt     # custom input file
```

Output is written to `compare_pretokenizers_output.txt`.

---

## 3. `compare_bpe.py`

### What it does

Compares the **full BPE encoding** of Parabix against HuggingFace's GPT-2
tokenizer. This is the final stage that produces integer token IDs from raw
text, so it tests both the pre-tokenizer (step 1) and the BPE encoder (step 2)
together.

### Inputs

- **Text input file** (default: `<build>/test.txt`). Each non-empty line is
  treated as a separate test case.
- **Parabix tokenizer binary**.
- **`vocab.json`** — GPT-2 token → ID mapping. Used by Parabix BPE.
- **`merges.txt`** — GPT-2 BPE merge rules. Used by Parabix BPE.
- **`tokenizer.json`** — HuggingFace's bundled tokenizer config (vocab + merges
  + pre-tokenizer settings in one file). Used by the HuggingFace side.

All three model files live in `tools/lex/tokenizer_files/` and originate from
the same GPT-2 model on HuggingFace Hub (`openai-community/gpt2`). They are
**committed** to the repo as well.

### How the comparison works

For each test case (line in the input file):

1. **Parabix side** runs **two subprocesses**:
   - **Step 1**: `tokenizer --pretokenizer=bytelevel input.txt` — produces
     byte-encoded pre-tokens (one per line).
   - **Step 2**: `tokenizer --vocab=vocab.json --merges=merges.txt pretokens.txt` —
     applies BPE merges and vocab lookup, outputs integer IDs.
2. **HuggingFace side** runs `Tokenizer.from_file("tokenizer.json").encode(text)`
   directly in Python — one call, returns IDs.
3. Compares the two lists of integer IDs.
4. Reports MATCH / MISMATCH with a side-by-side token table on mismatch.

### How to run

```bash
python compare_bpe.py                              # default input
python compare_bpe.py --verbose                    # always show full token table
python compare_bpe.py --parabix-only               # skip HuggingFace
python compare_bpe.py --hf-only                    # skip Parabix
python compare_bpe.py --input path/to/cases.txt    # custom input file
```

Output is written to `compare_bpe_output.txt`.


## License

Parabix is governed by the Open Software License 3.0. See `OSL3.0.txt` in the
repository root.
