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

## Build

Clone the repository and build the tokenizer:

```bash
git clone https://cs-git-research.cs.sfu.ca/cameron/parabix-devel.git
cd parabix-devel
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make tokenizer
```

All compiled tools will be found under `build/bin/`. The tokenizer binary is at
`build/bin/tokenizer`.

## Tokenizer

`tokenizer.cpp` is the CLI entry point. It reads an input file, runs optional
normalization, then runs optional pre-tokenization, and writes the result to
stdout.

```
input file → [normalize] → [pre-tokenize] → stdout
```

Basic usage:

```bash
bin/tokenizer input.txt
bin/tokenizer --normalize=lowercase input.txt
bin/tokenizer --normalize=nfd --pretokenizer=bert input.txt
bin/tokenizer --normalize=lowercase --pretokenizer=whitespace input.txt
```

If only `--normalize` is given with no `--pretokenizer`, the tokenizer outputs
the raw normalized text and stops — normalization alone is not tokenization.

## Normalizer

`normalize.h` / `normalize.cpp`

Normalization is stage 1 of the pipeline. It transforms the input text before
any splitting happens. Each mode is a Parabix pipeline that operates directly on
the UTF-8 bitstreams. The following modes are available via `--normalize=`:

- `none` — No normalization (default).
- `nfd` — Unicode NFD canonical decomposition.
- `nfc` — Unicode NFC canonical composition.
- `lowercase` — Map all uppercase codepoints to lowercase (Unicode SLC, 1-to-1).
- `stripaccents` — Remove combining accent marks. Should be applied after `nfd`
  so accents are isolated codepoints.
- `stripleft` — Remove leading Unicode whitespace.
- `stripright` — Remove trailing Unicode whitespace.
- `strip` — Remove both leading and trailing Unicode whitespace.
- `bytelevel` — GPT-2 byte alphabet: remap every input byte to a unique printable
  Unicode character.

Examples:

```bash
bin/tokenizer --normalize=lowercase input.txt
bin/tokenizer --normalize=nfd input.txt
bin/tokenizer --normalize=strip input.txt
```

## Pre-tokenizer

`pretokenizer.h` / `pretokenizer.cpp`

Pre-tokenization is stage 2 of the pipeline. It splits the (possibly normalized)
text into tokens by inserting newline separators at token boundaries. The
following modes are available via `--pretokenizer=`:

- `uax29` — Unicode UAX#29 word boundaries (default).
- `whitespace` — Split on whitespace characters.
- `whitespacesplit` — Split on whitespace, output delimiters as separate tokens.
- `digits` — Split on digit sequences.
- `punctuation` — Split on punctuation characters.
- `simplewordboundaries` — Boundaries between `\w` and `\W` characters.
- `bytelevel` — Split on whitespace with GPT-2 byte remapping.
- `chardelimiter` — Split on a specific character, set with `--delimiter`.
- `bert` — BERT pre-tokenizer: separates punctuation and words.
- `sequence_whitespace_punctuation` — Whitespace then punctuation splitting.

The `--behavior` flag controls what happens to the delimiter at each split
boundary:

- `isolated` — Keep token boundaries and add space boundaries (default).
- `removed` — Only keep word/punctuation boundaries, exclude whitespace.
- `mergedwithprevious` — Attach whitespace to the previous word.
- `mergedwithnext` — Attach whitespace to the next word.
- `contiguous` — Keep punctuation with words, separate spaces.

Examples:

```bash
bin/tokenizer --pretokenizer=whitespace input.txt
bin/tokenizer --pretokenizer=bert input.txt
bin/tokenizer --pretokenizer=chardelimiter --delimiter=, input.txt
bin/tokenizer --pretokenizer=whitespace --behavior=removed input.txt
```

Full pipeline examples:

```bash
# BERT-style: lowercase then split on word and punctuation boundaries
bin/tokenizer --normalize=lowercase --pretokenizer=bert input.txt

# GPT-2 style: bytelevel normalization then bytelevel pre-tokenization
bin/tokenizer --normalize=bytelevel --pretokenizer=bytelevel input.txt

# Strip leading whitespace then split on Unicode word boundaries
bin/tokenizer --normalize=stripleft --pretokenizer=uax29 input.txt
```

## Testing

The `tokenizer-test/` directory contains Python scripts that compare Parabix
output against the HuggingFace `tokenizers` library. See
[tokenizer-test/README.md](tokenizer-test/README.md) for setup and usage.

## License

Parabix is governed by the Open Software License 3.0. See `OSL3.0.txt` in the
repository root.
