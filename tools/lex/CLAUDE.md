# CLAUDE.md — BPE tokenizer (tools/lex)

`tokenizer.cpp` (binary `tokenizer`) implements GPT-2 byte-level BPE as a Parabix pipeline. The merge logic is in `bpe.cpp`: merges are partitioned into groups, one `BPEMergeKernel` (a Pablo kernel) is generated per group, and a token-id BixNum stream plus a live-token-start mask are threaded from kernel to kernel. `README.md` in this directory is the detailed design document. It describes the partitioning schemes, the correctness constraints and the full flag table; each flag also documents its cache-name tag there.

- Run: `bin/tokenizer --merges=tools/lex/tokenizer_files/merges.txt --pretokenizer=bytelevel [flags] <input>`, which prints one token id per line. `--merges-limit=N` uses only the first N merges, so kernels JIT much faster during development; a cold full-vocabulary compile takes about a minute or more.
- `BPE_GROUPS=1` / `BPE_RULES=1` dump the partition to stderr before compiling.
- Every BPE optimization must be **byte-identical** to the baseline. Check against HuggingFace with `tokenizer-test/compare_bpe.py`. That script needs Python `tokenizers` and has `REPO_ROOT`/`build19` hardcoded at the top, so adjust it before use. Alternatively, diff a run with the new flags against a baseline run on the same input.
- Any parameter that changes a merge kernel's body must appear in the `BPEMergeKernel` name, or in `hashRuleSet` for per-rule data, because of the object cache.
