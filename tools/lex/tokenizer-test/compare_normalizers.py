#!/usr/bin/env python3
"""
Normalizer comparison: Parabix vs HuggingFace.

Compares the raw normalized string output of each Parabix --normalize=X mode
against the equivalent HuggingFace normalizer.  No pre-tokenizer is used —
normalization is a string transformation, not a splitting operation.

Modes:
    (default)         Compare Parabix vs HuggingFace side by side
    --hf-only         Run HuggingFace normalizers only
    --parabix-only    Run Parabix normalizers only

Usage:
    python compare_normalizers.py                           # compare all modes
    python compare_normalizers.py --hf-only                 # HF reference only
    python compare_normalizers.py --parabix-only            # Parabix only
    python compare_normalizers.py nfd strip                 # specific modes
    python compare_normalizers.py --input /path/to/file.txt # custom input file
"""

import sys
import subprocess
import tempfile
import os
import argparse

from tokenizers.normalizers import NFD, StripAccents, Strip, Sequence, ByteLevel, Lowercase

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

DEFAULT_INPUT     = "/Users/munizahashim/parabix-devel/build19/test.txt"
DEFAULT_TOKENIZER = "/Users/munizahashim/parabix-devel/build19/bin/tokenizer"
OUTPUT_FILE       = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "compare_normalizers_output.txt")

SEP = "=" * 65

# ---------------------------------------------------------------------------
# Parabix --normalize flag → HuggingFace normalizer
# ---------------------------------------------------------------------------

MODES = {
    "nfd":          NFD(),
    "stripaccents": Sequence([NFD(), StripAccents()]),
    "stripleft":    Strip(left=True,  right=False),
    "stripright":   Strip(left=False, right=True),
    "strip":        Strip(left=True,  right=True),
    "bytelevel":    ByteLevel(),
    "lowercase":    Lowercase(),
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

class ParabixCrash(Exception):
    def __init__(self, returncode: int, stderr: str):
        self.returncode = returncode
        self.stderr = stderr.strip()
    def __str__(self):
        return f"tokenizer crashed (exit {self.returncode}): {self.stderr}"


def run_parabix(normalize: str, text: str) -> str:
    """Run Parabix with --normalize=X only (no pre-tokenizer) and return the
    raw normalized string."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        tmp = f.name
    try:
        cmd = [DEFAULT_TOKENIZER, f"--normalize={normalize}", tmp]
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
        if result.returncode != 0:
            raise ParabixCrash(result.returncode, result.stderr)
        return result.stdout
    finally:
        os.unlink(tmp)


def run_hf(normalizer, text: str) -> str:
    """Return the raw normalized string from HuggingFace."""
    return normalizer.normalize_str(text)

# ---------------------------------------------------------------------------
# Three run modes
# ---------------------------------------------------------------------------

def run_compare(out, text: str, requested: list[str]) -> dict[str, str]:
    results = {}
    num     = 0

    for mode in [m for m in requested if m in MODES]:
        num += 1
        hf_result = run_hf(MODES[mode], text)

        try:
            pbx_result = run_parabix(mode, text)
        except ParabixCrash as e:
            results[mode] = "CRASH"
            out.write(SEP + "\n")
            out.write(f"Test {num}: {mode}  →  CRASH\n")
            out.write(SEP + "\n")
            out.write(f"  Input:   {repr(text)}\n")
            out.write(f"  HF:      {repr(hf_result)}\n")
            out.write(f"  ERROR:   {e}\n\n")
            continue

        match  = pbx_result == hf_result
        status = "MATCH" if match else "MISMATCH"
        results[mode] = status

        out.write(SEP + "\n")
        out.write(f"Test {num}: {mode}  →  {status}\n")
        out.write(SEP + "\n")
        out.write(f"  Input:   {repr(text)}\n")
        out.write(f"  Parabix: {repr(pbx_result)}\n")
        out.write(f"  HF:      {repr(hf_result)}\n")
        out.write(f"  Match:   {'YES' if match else 'NO'}\n\n")

    return results


def run_hf_only(out, text: str, requested: list[str]) -> None:
    for num, mode in enumerate([m for m in requested if m in MODES], 1):
        hf_result = run_hf(MODES[mode], text)
        out.write(SEP + "\n")
        out.write(f"Test {num}: {mode} (HuggingFace)\n")
        out.write(SEP + "\n")
        out.write(f"  Input:  {repr(text)}\n")
        out.write(f"  Output: {repr(hf_result)}\n\n")


def run_parabix_only(out, text: str, requested: list[str]) -> dict[str, str]:
    results = {}
    for num, mode in enumerate([m for m in requested if m in MODES], 1):
        try:
            pbx_result = run_parabix(mode, text)
        except ParabixCrash as e:
            out.write(SEP + "\n")
            out.write(f"Test {num}: {mode} (Parabix)\n")
            out.write(SEP + "\n")
            out.write(f"  Input:  {repr(text)}\n")
            out.write(f"  ERROR:  {e}\n\n")
            continue

        out.write(SEP + "\n")
        out.write(f"Test {num}: {mode} (Parabix)\n")
        out.write(SEP + "\n")
        out.write(f"  Input:  {repr(text)}\n")
        out.write(f"  Output: {repr(pbx_result)}\n\n")

    return results

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def write_summary(out, results: dict[str, str]) -> None:
    out.write(SEP + "\n")
    out.write("SUMMARY\n")
    out.write(SEP + "\n")
    for name, status in results.items():
        if status == "MATCH":
            mark = "PASS"
        elif status == "CRASH":
            mark = "CRASH"
        else:
            mark = "FAIL"
        out.write(f"  [{mark}]  {name}\n")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    all_modes = list(MODES.keys())

    parser = argparse.ArgumentParser(description="Normalizer comparison: Parabix vs HuggingFace.")
    parser.add_argument("--input",        default=DEFAULT_INPUT,
                        help="Path to input text file")
    parser.add_argument("--hf-only",      action="store_true",
                        help="Run HuggingFace normalizers only")
    parser.add_argument("--parabix-only", action="store_true",
                        help="Run Parabix normalizers only")
    parser.add_argument("modes",          nargs="*",
                        help=f"Specific modes to test (default: all). "
                             f"Available: {all_modes}")
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        text = f.read()

    requested = args.modes if args.modes else all_modes

    unknown = [m for m in requested if m not in all_modes]
    if unknown:
        print(f"Unknown mode(s): {unknown}")
        print(f"Available: {all_modes}")
        sys.exit(1)

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        out = Tee(f)
        out.write(f"Input file: {args.input}\n")
        out.write(f"Input:      {repr(text)}\n\n")

        if args.hf_only:
            run_hf_only(out, text, requested)
        elif args.parabix_only:
            results = run_parabix_only(out, text, requested)
            write_summary(out, results)
        else:
            results = run_compare(out, text, requested)
            write_summary(out, results)

    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
