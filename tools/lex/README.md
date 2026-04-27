# Parabix Tokenizer

A Unicode tokenizer built on the Parabix bit-parallel processing
framework. Text is processed as parallel bitstreams — all characters are transformed
simultaneously rather than one at a time.

---

## Building

From the repo root:

```bash
mkdir build && cd build
cmake ..
make tokenizer
```

The binary is at `build/bin/tokenizer`.

---

## Tokenizer — entry point

`tokenizer.cpp` is the CLI that ties everything together. It reads an input file,
runs optional normalization, then runs optional pre-tokenization, and writes the
result to stdout.

```
input file → [normalize] → [pre-tokenize] → stdout
```

**Basic usage:**

```bash
bin/tokenizer input.txt
bin/tokenizer --normalize=lowercase input.txt
bin/tokenizer --normalize=nfd --pretokenizer=bert input.txt
bin/tokenizer --normalize=lowercase --pretokenizer=whitespace input.txt
```

If only `--normalize` is given (no `--pretokenizer`), the tokenizer outputs the
raw normalized text and stops — normalization alone is not tokenization.

---

## Normalizer — stage 1

`normalize.h` / `normalize.cpp`

Normalization runs first. It transforms the input text before any splitting
happens. Each mode is a Parabix pipeline that operates on the UTF-8 bitstreams
directly.

| Flag | What it does |
|---|---|
| `none` | No normalization (default) |
| `nfd` | Unicode NFD canonical decomposition |
| `nfc` | Unicode NFC canonical composition |
| `lowercase` | Map all uppercase codepoints to lowercase (Unicode SLC) |
| `stripaccents` | Remove combining accent marks — apply after `nfd` |
| `stripleft` | Remove leading Unicode whitespace |
| `stripright` | Remove trailing Unicode whitespace |
| `strip` | Remove both leading and trailing whitespace |
| `bytelevel` | GPT-2 byte alphabet: remap every byte to a printable Unicode char |

**Examples:**

```bash
bin/tokenizer --normalize=lowercase input.txt
bin/tokenizer --normalize=nfd input.txt
bin/tokenizer --normalize=strip input.txt
```

Modes can be stacked by chaining tokenizer invocations through pipes, or by
combining normalization with pre-tokenization in one command.

---

## Pre-tokenizer — stage 2

`pretokenizer.h` / `pretokenizer.cpp`

Pre-tokenization runs after normalization. It splits the (possibly normalized)
text into tokens by inserting newline separators at token boundaries.

| Flag | What it does |
|---|---|
| `uax29` | Unicode UAX#29 word boundaries (default) |
| `whitespace` | Split on whitespace characters |
| `whitespacesplit` | Split on whitespace, output delimiters as separate tokens |
| `digits` | Split on digit sequences |
| `punctuation` | Split on punctuation characters |
| `simplewordboundaries` | Boundaries between `\w` and `\W` characters |
| `bytelevel` | Split on whitespace with GPT-2 byte remapping |
| `chardelimiter` | Split on a specific character (set with `--delimiter`) |
| `bert` | BERT pre-tokenizer: separates punctuation and words |
| `sequence_whitespace_punctuation` | Whitespace then punctuation splitting |

**Examples:**

```bash
bin/tokenizer --pretokenizer=whitespace input.txt
bin/tokenizer --pretokenizer=bert input.txt
bin/tokenizer --pretokenizer=chardelimiter --delimiter=, input.txt
```

### Split behavior

The `--behavior` flag controls what happens to the delimiter (whitespace or
punctuation) at each split boundary:

| Flag | What it does |
|---|---|
| `isolated` | Keep token boundaries and add space boundaries (default) |
| `removed` | Only keep word/punctuation boundaries, exclude whitespace |
| `mergedwithprevious` | Attach whitespace to the previous word |
| `mergedwithnext` | Attach whitespace to the next word |
| `contiguous` | Keep punctuation with words, separate spaces |

**Example:**

```bash
bin/tokenizer --pretokenizer=whitespace --behavior=removed input.txt
```

---

## Full pipeline examples

```bash
# BERT-style: lowercase + strip accents + bert pre-tokenizer
bin/tokenizer --normalize=lowercase --pretokenizer=bert input.txt

# GPT-2 style: bytelevel normalization + bytelevel pre-tokenizer
bin/tokenizer --normalize=bytelevel --pretokenizer=bytelevel input.txt

# Strip leading whitespace then split on word boundaries
bin/tokenizer --normalize=stripleft --pretokenizer=uax29 input.txt
```

---

## Testing

The `tokenizer-test/` directory contains Python scripts that compare Parabix
output against the HuggingFace `tokenizers` library. See
[tokenizer-test/README.md](tokenizer-test/README.md) for setup and usage.
