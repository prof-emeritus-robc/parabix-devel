# Tokenizer Test Suite

This directory contains Python scripts that compare the Parabix tokenizer output
against the equivalent HuggingFace `tokenizers` library output. Each script runs
both sides on the same input and reports MATCH, MISMATCH, or CRASH for each mode.

## Requirements

- Python 3.10 or later.
- The HuggingFace `tokenizers` package (PyPI — no account or API token required).
- A built Parabix tokenizer binary. See the [main README](../README.md) for build
  instructions.

## Setup

Create a Python virtual environment and install the HuggingFace library:

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

## Updating the binary path

Each script hardcodes the path to the Parabix tokenizer binary and the default
input file. If your build directory differs from `build19`, update these two lines
near the top of each script:

```python
DEFAULT_INPUT     = "/path/to/your/parabix-devel/build/test.txt"
DEFAULT_TOKENIZER = "/path/to/your/parabix-devel/build/bin/tokenizer"
```

## compare_normalizers.py

Compares Parabix `--normalize=X` against the equivalent HuggingFace normalizer.
No pre-tokenizer is used — normalization is a pure string transformation.

Supported modes: `nfd`, `stripaccents`, `stripleft`, `stripright`, `strip`,
`bytelevel`, `lowercase`.

```bash
python compare_normalizers.py                        # run all modes
python compare_normalizers.py lowercase nfd strip    # specific modes only
python compare_normalizers.py --hf-only              # HuggingFace output only
python compare_normalizers.py --parabix-only         # Parabix output only
python compare_normalizers.py --input file.txt       # custom input file
```

Output is written to `compare_normalizers_output.txt` and also printed to the
terminal.

## compare_pretokenizers.py

Compares Parabix `--pretokenizer=X` against HuggingFace pre-tokenizers. Also
runs behavior tests defined in `tokenizertest.xml`.

```bash
python compare_pretokenizers.py                      # run all modes
python compare_pretokenizers.py whitespace bert      # specific modes only
python compare_pretokenizers.py --hf-only            # HuggingFace output only
python compare_pretokenizers.py --parabix-only       # Parabix output only
```

Output is written to `compare_pretokenizers_output.txt`.

## Reading the output

Each test prints a block like the following:

```
=================================================================
Test 1: lowercase  →  MATCH
=================================================================
  Input:   'ПРИВЕТ Hello WORLD'
  Parabix: 'привет hello world'
  HF:      'привет hello world'
  Match:   YES
```

The summary at the end shows PASS, FAIL, or CRASH for each mode tested.

## License

Parabix is governed by the Open Software License 3.0. See `OSL3.0.txt` in the
repository root.
