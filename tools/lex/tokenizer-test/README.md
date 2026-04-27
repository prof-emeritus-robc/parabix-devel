# Tokenizer Test Suite

This directory contains Python scripts that compare the Parabix tokenizer output
against the equivalent HuggingFace tokenizers library output.

---

## Prerequisites
### 1. Install the HuggingFace tokenizers library

```bash
pip install tokenizers
```

This installs the `tokenizers` package from HuggingFace. No HuggingFace account
or API token is required — it is a plain PyPI package.

Verify the install:

```bash
python -c "import tokenizers; print(tokenizers.__version__)"
```

---

## Test scripts

### `compare_normalizers.py` — Normalizer tests

Compares Parabix `--normalize=X` against the equivalent HuggingFace normalizer.
No pre-tokenizer is used — normalization is a pure string transformation.

**Supported modes:**

| Mode flag | What it does |
|---|---|
| `nfd` | Unicode NFD decomposition |
| `stripaccents` | NFD then remove combining accent marks |
| `stripleft` | Remove leading whitespace |
| `stripright` | Remove trailing whitespace |
| `strip` | Remove both leading and trailing whitespace |
| `bytelevel` | GPT-2 byte alphabet remapping |
| `lowercase` | Map all uppercase to lowercase (SLC) |

**Run all modes (compare Parabix vs HuggingFace):**

```bash
python compare_normalizers.py
```

**Run specific modes only:**

```bash
python compare_normalizers.py lowercase nfd strip
```

**Run HuggingFace only (no Parabix):**

```bash
python compare_normalizers.py --hf-only
```

**Run Parabix only:**

```bash
python compare_normalizers.py --parabix-only
```

**Use a custom input file:**

```bash
python compare_normalizers.py --input /path/to/your/file.txt
```

Output is written to `compare_normalizers_output.txt` in this directory and
also printed to the terminal.

---

### `compare_pretokenizers.py` — Pre-tokenizer tests

Compares Parabix `--pretokenizer=X` against HuggingFace pre-tokenizers.
Also runs behavior tests defined in `tokenizertest.xml`.

**Run all modes:**

```bash
python compare_pretokenizers.py
```

**Run specific modes:**

```bash
python compare_pretokenizers.py whitespace bert
```

Output is written to `compare_pretokenizers_output.txt`.

---

## Updating the tokenizer binary path

Each script hardcodes the path to the Parabix tokenizer binary and the default
input file. If your build directory is different from `build19`, edit these two
lines near the top of each script:

```python
DEFAULT_INPUT     = "/path/to/your/parabix-devel/build/test.txt"
DEFAULT_TOKENIZER = "/path/to/your/parabix-devel/build/bin/tokenizer"
```
