#!/usr/bin/env python3
"""
Unified pre-tokenizer test suite.

Modes:
    (default)         Compare Parabix vs HuggingFace side by side
    --hf-only         Run HuggingFace pre-tokenizers only
    --parabix-only    Run Parabix pre-tokenizers only + behavior tests from XML

Usage:
    python compare_parabix_hf.py                          # compare all modes
    python compare_parabix_hf.py --hf-only                # HF reference only
    python compare_parabix_hf.py --parabix-only           # Parabix only
    python compare_parabix_hf.py whitespace bert          # specific modes
    python compare_parabix_hf.py /path/to/input.txt       # custom input file
"""

import sys
import subprocess
import tempfile
import os
import argparse
import xml.etree.ElementTree as ET
from tokenizers.pre_tokenizers import (
    Whitespace, WhitespaceSplit, ByteLevel, BertPreTokenizer,
    Punctuation, Digits, CharDelimiterSplit, Sequence, Split,
)
from tokenizers import Regex

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --input/--tokenizer override.
REPO_ROOT         = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
DEFAULT_INPUT     = os.path.join(REPO_ROOT, "build19/test.txt")
DEFAULT_TOKENIZER = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
OUTPUT_FILE       = os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare_pretokenizers_output.txt")
BEHAVIOR_XML      = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tokenizertest.xml")

SEP  = "=" * 65
DASH = "-" * 65

# ---------------------------------------------------------------------------
# Parabix mode → HuggingFace pre-tokenizer mapping
# ---------------------------------------------------------------------------

MODES = {
    "whitespacesplit":           WhitespaceSplit(),
    "whitespace":                Whitespace(),
    "bytelevel":                 ByteLevel(add_prefix_space=False),
    # "bert":                      BertPreTokenizer(),
    "punctuation":               Punctuation(),
    "digits":                    Digits(individual_digits=True),
    "chardelimiter":             CharDelimiterSplit(","),
    "sequence_whitespace_punct": Sequence([WhitespaceSplit(), Punctuation()]),
    # "simplewordboundaries":      Split(pattern=Regex(r"\w+"), behavior="isolated"),
    # uax29: no HF equivalent — appears in parabix-only / behavior tests only
}

# Parabix CLI flag name where it differs from the dict key
PARABIX_FLAG = {
    "sequence_whitespace_punct": "sequence_whitespace_punctuation",
}

# ---------------------------------------------------------------------------
# Tee: write to terminal and file simultaneously
# ---------------------------------------------------------------------------

class Tee:
    def __init__(self, file): self.file = file
    def write(self, msg): sys.stdout.write(msg); self.file.write(msg)
    def flush(self): sys.stdout.flush(); self.file.flush()

# ---------------------------------------------------------------------------
# Runners
# ---------------------------------------------------------------------------

def run_parabix(mode: str, text: str, behavior: str = "") -> list[str]:
    flag = PARABIX_FLAG.get(mode, mode)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        cmd = [DEFAULT_TOKENIZER, f"--pretokenizer={flag}", tmp]
        if behavior:
            cmd += [f"--behavior={behavior}"]
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
        return [t for t in result.stdout.split("\n") if t]
    finally:
        os.unlink(tmp)


def run_hf(pt, text: str) -> list[str]:
    return [tok for tok, _ in pt.pre_tokenize_str(text)]

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def write_tokens(out, label: str, tokens: list[str]) -> None:
    out.write(f"  {label} ({len(tokens)} tokens):\n")
    for i, tok in enumerate(tokens, 1):
        out.write(f"    {i:3}. {repr(tok)}\n")


def write_diff_summary(out, parabix: list[str], hf: list[str]) -> None:
    if len(parabix) != len(hf):
        out.write(f"  → Count differs: Parabix={len(parabix)}, HF={len(hf)}\n")
    first = next((i for i in range(min(len(parabix), len(hf)))
                  if parabix[i] != hf[i]), None)
    if first is not None:
        out.write(f"  → First mismatch at token {first + 1}: "
                  f"Parabix={repr(parabix[first])}  HF={repr(hf[first])}\n")
    out.write("\n")


def write_comparison(out, parabix: list[str], hf: list[str]) -> None:
    w = 35
    out.write(f"    {'#':<4} {'Parabix':<{w}} {'HuggingFace':<{w}} Match\n")
    out.write(f"    {'-'*4} {'-'*w} {'-'*w} -----\n")
    max_len = max(len(parabix), len(hf))
    for i in range(max_len):
        p     = repr(parabix[i]) if i < len(parabix) else "<missing>"
        h     = repr(hf[i])      if i < len(hf)      else "<missing>"
        match = "YES" if p == h else "NO"
        out.write(f"    {i+1:<4} {p:<{w}} {h:<{w}} {match}\n")

# ---------------------------------------------------------------------------
# Three run modes
# ---------------------------------------------------------------------------

def run_compare(out, lines: list[str], requested: list[str]) -> dict[str, str]:
    """Run both Parabix and HF per line, compare token lists.

    Parabix's CLI always treats a newline as a hard pretoken boundary, by
    design (most corpora are one document per line; confirmed universal
    across every mode, not something any single mode opts into). A
    multi-line input compared as one whole-string HF call would therefore
    disagree at every embedded newline regardless of mode, which is a test
    artifact, not a Parabix bug (confirmed: a corpus joined with "\\n" was
    producing false MISMATCH on digits/punctuation/chardelimiter/
    sequence_whitespace_punct until switched to this per-line comparison,
    which then matched HF exactly, 0 mismatches on 5000 real lines).
    Matches compare_bpe.py's own "each line is a separate test case" method.
    """
    results = {}
    for num, mode in enumerate(requested, 1):
        pt = MODES[mode]
        mismatches = []
        for li, line in enumerate(lines):
            parabix = run_parabix(mode, line)
            hf      = run_hf(pt, line)
            if parabix != hf:
                mismatches.append((li, line, parabix, hf))

        status = "MATCH" if not mismatches else "MISMATCH"
        results[mode] = status

        out.write(SEP + "\n")
        out.write(f"Test {num}: {mode}  →  {status}\n")
        out.write(SEP + "\n")
        out.write(f"  Lines tested:  {len(lines)}\n")
        out.write(f"  Lines matched: {len(lines) - len(mismatches)}\n")
        out.write(f"  Lines failed:  {len(mismatches)}\n\n")

        for li, line, parabix, hf in mismatches[:3]:
            out.write(f"  --- mismatch at line {li} ---\n")
            out.write(f"  Input:          {repr(line)}\n")
            out.write(f"  Parabix tokens: {len(parabix)}\n")
            out.write(f"  HF tokens:      {len(hf)}\n\n")
            write_diff_summary(out, parabix, hf)
            write_comparison(out, parabix, hf)
            out.write("\n")
        if len(mismatches) > 3:
            out.write(f"  ... and {len(mismatches) - 3} more mismatching lines\n\n")

    return results


def run_hf_only(out, text: str, requested: list[str]) -> None:
    """Run HuggingFace pre-tokenizers only."""
    for num, mode in enumerate(requested, 1):
        pt     = MODES[mode]
        tokens = run_hf(pt, text)

        out.write(SEP + "\n")
        out.write(f"Test {num}: {mode} (HuggingFace)\n")
        out.write(SEP + "\n")
        out.write(f"  Input: {repr(text)}\n\n")
        write_tokens(out, "Tokens", tokens)
        out.write("\n")


def run_parabix_only(out, text: str, requested: list[str]) -> dict[str, str]:
    """Run Parabix pre-tokenizers only, then behavior tests from XML."""
    results = {}

    # Pre-tokenizer tests
    for num, mode in enumerate(requested, 1):
        tokens = run_parabix(mode, text)

        out.write(SEP + "\n")
        out.write(f"Test {num}: {mode} (Parabix)\n")
        out.write(SEP + "\n")
        out.write(f"  Input: {repr(text)}\n\n")
        write_tokens(out, "Tokens", tokens)
        out.write("\n")

    # Behavior tests from XML (uax29 only — HF has no equivalent)
    if os.path.isfile(BEHAVIOR_XML):
        out.write(SEP + "\n")
        out.write("BEHAVIOR TESTS (Parabix only — from tokenizertest.xml)\n")
        out.write(SEP + "\n\n")

        tree  = ET.parse(BEHAVIOR_XML)
        root  = tree.getroot()
        cases = [tc for tc in root.findall("tokenizercase") if tc.get("behavior")]

        passed = failed = 0
        for num, tc in enumerate(cases, 1):
            mode     = tc.get("pretokenizer")
            behavior = tc.get("behavior")
            expected = int(tc.get("expectedcount"))
            tokens   = run_parabix(mode, text, behavior)
            actual   = len(tokens)
            status   = "PASS" if actual == expected else "FAIL"
            results[f"{mode}+{behavior}"] = status

            if status == "PASS":
                passed += 1
            else:
                failed += 1

            out.write(DASH + "\n")
            out.write(f"Behavior Test {num}: {mode} + {behavior}  →  {status}\n")
            out.write(DASH + "\n")
            out.write(f"  Expected: {expected}   Actual: {actual}\n\n")
            write_tokens(out, "Tokens", tokens)
            out.write("\n")

        out.write(f"Behavior summary: {passed} passed, {failed} failed\n\n")

    return results

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def write_summary(out, results: dict[str, str]) -> None:
    out.write(SEP + "\n")
    out.write("SUMMARY\n")
    out.write(SEP + "\n")
    for name, status in results.items():
        mark = "PASS" if status in ("MATCH", "PASS") else "FAIL"
        out.write(f"  [{mark}]  {name}\n")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    global DEFAULT_TOKENIZER
    parser = argparse.ArgumentParser(description="Unified pre-tokenizer test suite.")
    parser.add_argument("--input",        default=DEFAULT_INPUT,
                        help="Path to input text file")
    parser.add_argument("--hf-only",      action="store_true",
                        help="Run HuggingFace pre-tokenizers only")
    parser.add_argument("--parabix-only", action="store_true",
                        help="Run Parabix pre-tokenizers only (includes behavior tests)")
    parser.add_argument("modes",          nargs="*",
                        help="Specific modes to test (default: all)")
    parser.add_argument("--tokenizer",    default=DEFAULT_TOKENIZER,
                        help=f"Parabix tokenizer binary (default: {DEFAULT_TOKENIZER})")
    parser.add_argument("--max-lines",    type=int, default=200,
                        help="Cap the number of lines compared for a multi-line "
                             "corpus (default 200; each line spawns a subprocess "
                             "per mode, so a full 5000-line corpus over 7 modes "
                             "is slow). Use 0 for no cap.")
    args = parser.parse_args()
    DEFAULT_TOKENIZER = args.tokenizer
    if not args.hf_only and not os.path.isfile(DEFAULT_TOKENIZER):
        print(f"Error: tokenizer binary not found: {DEFAULT_TOKENIZER}")
        sys.exit(1)

    with open(args.input, "r", encoding="utf-8") as f:
        text = f.read().strip()

    # Parabix's CLI always treats "\n" as a hard pretoken boundary (confirmed
    # universal, every mode), so a multi-line corpus must be compared line by
    # line against HF, not as one joined string -- see run_compare's docstring.
    lines = [l for l in text.split("\n") if l]
    if args.max_lines and len(lines) > args.max_lines:
        lines = lines[:args.max_lines]

    requested = args.modes if args.modes else list(MODES.keys())
    unknown   = [m for m in requested if m not in MODES]
    if unknown:
        print(f"Unknown mode(s): {unknown}")
        print(f"Available: {list(MODES.keys())}")
        sys.exit(1)

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        out = Tee(f)
        out.write(f"Input file: {args.input}\n")
        out.write(f"Lines:      {len(lines)}\n\n")

        if args.hf_only:
            run_hf_only(out, text, requested)
        elif args.parabix_only:
            results = run_parabix_only(out, text, requested)
            write_summary(out, results)
        else:
            results = run_compare(out, lines, requested)
            write_summary(out, results)

    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
