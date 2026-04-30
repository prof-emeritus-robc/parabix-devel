#!/usr/bin/env python3
"""
BPE tokenizer test suite.

Compares Parabix BPE output against HuggingFace's tokenizer.
Each non-empty line in the input file is treated as a separate test case.

Parabix uses a two-step pipeline:
  Step 1: tokenizer --pretokenizer bytelevel <input>              → pre-tokens
  Step 2: tokenizer --vocab vocab.json --merges merges.txt <pre>  → token IDs

HuggingFace runs the same GPT-2 BPE model via the tokenizers library,
loaded from the local tokenizer.json so both sides use identical vocab/merges.

Usage:
    python compare_bpe.py                              # default input file
    python compare_bpe.py --input path/to/cases.txt   # custom input file
    python compare_bpe.py --verbose                    # full token-by-token table always
    python compare_bpe.py --parabix-only               # skip HF, show Parabix output only
    python compare_bpe.py --hf-only                    # skip Parabix, show HF output only
"""

import sys
import subprocess
import tempfile
import os
import argparse
from tokenizers import Tokenizer

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT      = "/Users/munizahashim/parabix-devel"
TOKENIZER      = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
VOCAB          = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/vocab.json")
MERGES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")
TOKENIZER_JSON = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/tokenizer.json")
DEFAULT_INPUT  = os.path.join(REPO_ROOT, "build19/test.txt")
OUTPUT_FILE    = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "compare_bpe_output.txt")

SEP  = "=" * 70
DASH = "-" * 70

# ---------------------------------------------------------------------------
# Tee: write to terminal and file simultaneously
# ---------------------------------------------------------------------------

class Tee:
    def __init__(self, file):
        self.file = file

    def write(self, msg):
        sys.stdout.write(msg)
        self.file.write(msg)

    def flush(self):
        sys.stdout.flush()
        self.file.flush()


# ---------------------------------------------------------------------------
# Runners
# ---------------------------------------------------------------------------

def run_parabix_bpe(text: str) -> tuple[list[int], list[str]]:
    """Two-step Parabix BPE: bytelevel pretokenizer then BPE merge + vocab lookup."""

    # Step 1: bytelevel pretokenizer → byte-encoded pre-tokens (one per line)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        input_path = f.name
    try:
        r1 = subprocess.run(
            [TOKENIZER, "--pretokenizer=bytelevel", input_path],
            capture_output=True, encoding="utf-8"
        )
        pretokens_text = r1.stdout
    finally:
        os.unlink(input_path)

    # Step 2: BPE merge + vocab lookup → integer IDs
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(pretokens_text)
        pretokens_path = f.name
    try:
        r_ids = subprocess.run(
            [TOKENIZER, f"--vocab={VOCAB}", f"--merges={MERGES}", pretokens_path],
            capture_output=True, encoding="utf-8"
        )
        r_strs = subprocess.run(
            [TOKENIZER, f"--vocab={VOCAB}", f"--merges={MERGES}",
             "--strings", pretokens_path],
            capture_output=True, encoding="utf-8"
        )
    finally:
        os.unlink(pretokens_path)

    ids     = [int(x) for x in r_ids.stdout.split() if x.lstrip("-").isdigit()]
    strings = [s for s in r_strs.stdout.split("\n") if s]
    return ids, strings


def run_hf_bpe(tokenizer_obj, text: str) -> tuple[list[int], list[str]]:
    """Run HuggingFace tokenizer (loaded from local tokenizer.json)."""
    output = tokenizer_obj.encode(text)
    return output.ids, output.tokens


# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def write_token_table(out, parabix_ids, parabix_strs, hf_ids, hf_strs) -> None:
    w = 22
    out.write(f"    {'#':<5} {'Parabix ID':<12} {'Parabix token':<{w}} "
              f"{'HF ID':<12} {'HF token':<{w}} Match\n")
    out.write(f"    {'-'*5} {'-'*12} {'-'*w} {'-'*12} {'-'*w} -----\n")
    max_len = max(len(parabix_ids), len(hf_ids))
    for i in range(max_len):
        pid  = str(parabix_ids[i])   if i < len(parabix_ids)  else "<miss>"
        pstr = repr(parabix_strs[i]) if i < len(parabix_strs) else "<miss>"
        hid  = str(hf_ids[i])        if i < len(hf_ids)        else "<miss>"
        hstr = repr(hf_strs[i])      if i < len(hf_strs)       else "<miss>"
        mark = "YES" if pid == hid else "NO "
        out.write(f"    {i+1:<5} {pid:<12} {pstr:<{w}} {hid:<12} {hstr:<{w}} {mark}\n")


def write_diff_detail(out, parabix_ids, hf_ids) -> None:
    if len(parabix_ids) != len(hf_ids):
        out.write(f"  → Length mismatch: Parabix={len(parabix_ids)}, HF={len(hf_ids)}\n")
    first = next(
        (i for i in range(min(len(parabix_ids), len(hf_ids)))
         if parabix_ids[i] != hf_ids[i]),
        None
    )
    if first is not None:
        out.write(f"  → First mismatch at position {first + 1}: "
                  f"Parabix={parabix_ids[first]}, HF={hf_ids[first]}\n")


# ---------------------------------------------------------------------------
# Run modes
# ---------------------------------------------------------------------------

def run_compare(out, tokenizer_obj, cases: list[tuple[int, str]], verbose: bool) -> dict:
    results = {}
    for num, (lineno, text) in enumerate(cases, 1):
        parabix_ids, parabix_strs = run_parabix_bpe(text)
        hf_ids, hf_strs           = run_hf_bpe(tokenizer_obj, text)

        match  = (parabix_ids == hf_ids)
        status = "MATCH" if match else "MISMATCH"
        label  = f"line {lineno}"
        results[label] = status

        out.write(SEP + "\n")
        out.write(f"Test {num} (line {lineno})  →  {status}\n")
        out.write(SEP + "\n")
        out.write(f"  Input:          {repr(text)}\n")
        out.write(f"  Parabix tokens: {len(parabix_ids)}\n")
        out.write(f"  HF tokens:      {len(hf_ids)}\n\n")

        if not match:
            write_diff_detail(out, parabix_ids, hf_ids)
            out.write("\n")

        if verbose or not match:
            write_token_table(out, parabix_ids, parabix_strs, hf_ids, hf_strs)
        else:
            out.write(f"  IDs: {parabix_ids[:12]}{'...' if len(parabix_ids) > 12 else ''}\n")

        out.write("\n")

    return results


def run_hf_only(out, tokenizer_obj, cases: list[tuple[int, str]]) -> None:
    for num, (lineno, text) in enumerate(cases, 1):
        ids, tokens = run_hf_bpe(tokenizer_obj, text)
        out.write(SEP + "\n")
        out.write(f"Test {num} (line {lineno}) — HuggingFace\n")
        out.write(SEP + "\n")
        out.write(f"  Input: {repr(text)}\n")
        out.write(f"  Tokens ({len(ids)}):\n")
        for i, (tid, tok) in enumerate(zip(ids, tokens), 1):
            out.write(f"    {i:3}. {tid:<7} {repr(tok)}\n")
        out.write("\n")


def run_parabix_only(out, cases: list[tuple[int, str]]) -> None:
    for num, (lineno, text) in enumerate(cases, 1):
        ids, strings = run_parabix_bpe(text)
        out.write(SEP + "\n")
        out.write(f"Test {num} (line {lineno}) — Parabix\n")
        out.write(SEP + "\n")
        out.write(f"  Input: {repr(text)}\n")
        out.write(f"  Tokens ({len(ids)}):\n")
        for i, (tid, tok) in enumerate(zip(ids, strings), 1):
            out.write(f"    {i:3}. {tid:<7} {repr(tok)}\n")
        out.write("\n")


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def write_summary(out, results: dict) -> None:
    passed = sum(1 for v in results.values() if v == "MATCH")
    failed = sum(1 for v in results.values() if v != "MATCH")
    out.write(SEP + "\n")
    out.write("SUMMARY\n")
    out.write(SEP + "\n")
    for name, status in results.items():
        mark = "PASS" if status == "MATCH" else "FAIL"
        out.write(f"  [{mark}]  {name}\n")
    out.write(DASH + "\n")
    out.write(f"  Total: {len(results)}   Passed: {passed}   Failed: {failed}\n")
    out.write(SEP + "\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="BPE tokenizer comparison test suite.")
    parser.add_argument("--input",        default=DEFAULT_INPUT,
                        help=f"Input file (entire file is one test case, default: {DEFAULT_INPUT})")
    parser.add_argument("--hf-only",      action="store_true",
                        help="Run HuggingFace tokenizer only")
    parser.add_argument("--parabix-only", action="store_true",
                        help="Run Parabix tokenizer only")
    parser.add_argument("--verbose",      action="store_true",
                        help="Always show full token-by-token table (default: only on mismatch)")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Error: input file not found: {args.input}")
        sys.exit(1)

    with open(args.input, "r", encoding="utf-8") as f:
        # Treat the entire file as a single input string (matches HuggingFace's
        # API: tokenizer.encode(text) takes one string, returns one list of IDs).
        full_text = f.read()

    if not full_text.strip():
        print("Error: input file is empty.")
        sys.exit(1)

    cases = [(1, full_text)]

    tokenizer_obj = None
    if not args.parabix_only:
        if not os.path.isfile(TOKENIZER_JSON):
            print(f"Error: tokenizer.json not found at {TOKENIZER_JSON}")
            sys.exit(1)
        tokenizer_obj = Tokenizer.from_file(TOKENIZER_JSON)

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        out = Tee(f)
        out.write(f"Parabix BPE vs HuggingFace BPE comparison\n")
        out.write(f"Input:  {args.input}\n")
        out.write(f"Vocab:  {VOCAB}\n")
        out.write(f"Merges: {MERGES}\n\n")

        if args.hf_only:
            run_hf_only(out, tokenizer_obj, cases)
        elif args.parabix_only:
            run_parabix_only(out, cases)
        else:
            results = run_compare(out, tokenizer_obj, cases, args.verbose)
            write_summary(out, results)

    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
