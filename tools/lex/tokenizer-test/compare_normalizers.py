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
    --sweep           Codepoint sweep instead of --input: every assigned codepoint
                      of Python's unicodedata (not Cn, not Cs), each in five
                      contexts (alone, twice, after 'a', before U+0301, between
                      two 'a's).  Lists each differing codepoint with its Unicode
                      age, and for nfc/nfd/nfkc/nfkd says which side equals
                      Python's unicodedata.normalize.

Usage:
    python compare_normalizers.py                           # compare all modes
    python compare_normalizers.py --hf-only                 # HF reference only
    python compare_normalizers.py --parabix-only            # Parabix only
    python compare_normalizers.py nfd nfc nfkd nfkc            # specific modes
    python compare_normalizers.py --input /path/to/file.txt # custom input file
    python compare_normalizers.py --sweep                   # codepoint sweep, 10 modes
    python compare_normalizers.py --sweep nfc nfkc          # sweep some modes
"""

import sys
import subprocess
import tempfile
import os
import argparse
import re
import unicodedata

from tokenizers.normalizers import NFD, NFC, NFKD, NFKC, StripAccents, Strip, Sequence, ByteLevel, Lowercase, Nmt, BertNormalizer

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --input/--tokenizer override.
REPO_ROOT         = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
DEFAULT_INPUT     = os.path.join(REPO_ROOT, "build19/test.txt")
DEFAULT_TOKENIZER = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
OUTPUT_FILE       = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "compare_normalizers_output.txt")
SWEEP_OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "compare_normalizers_sweep_output.txt")
# Unicode age of each codepoint: the ranges in the comments of the generated
# Parabix UCD table (the same ranges as the Unicode file DerivedAge.txt).
DERIVED_AGE       = os.path.join(REPO_ROOT, "lib/ucd/data/DerivedAge.cpp")

SEP = "=" * 65

# ---------------------------------------------------------------------------
# Parabix --normalize flag → HuggingFace normalizer
# ---------------------------------------------------------------------------

MODES = {
    "nfd":          NFD(),
    "nfc":          NFC(),
    "nfkd":         NFKD(),
    "nfkc":         NFKC(),
    "stripaccents": Sequence([NFD(), StripAccents()]),
    "stripleft":    Strip(left=True,  right=False),
    "stripright":   Strip(left=False, right=True),
    "strip":        Strip(left=True,  right=True),
    "bytelevel":    ByteLevel(),
    "lowercase":    Lowercase(),
    "nmt":          Nmt(),
    "bertchinesechars":  BertNormalizer(clean_text=False,
                                        handle_chinese_chars=True,
                                        strip_accents=False,
                                        lowercase=False),
    "bertcleantext":     BertNormalizer(clean_text=True,
                                        handle_chinese_chars=False,
                                        strip_accents=False,
                                        lowercase=False),
    # Built as a literal chain (Sequence), not a single BertNormalizer(strip_accents=True,...)
    # call: HF's own BertNormalizer.strip_accents uses BERT's original Mn-only accent
    # rule internally, which differs from the standalone StripAccents() class (Mn+Mc+Me,
    # see "stripaccents" above) -- confirmed by comparing the two directly in pure HF code,
    # no Parabix involved. Parabix's --normalize=X,Y,Z is a literal chain (cl::CommaSeparated),
    # so the chain is the correct HF counterpart here, not the single composite object.
    # NFD is required before StripAccents(): it only removes marks already split out as
    # their own codepoint (e.g. 'e'+'́'), not a precomposed char like 'e' -- matching
    # Parabix's own stripaccents step, which always NFD-decomposes internally first
    # (normalize.cpp: applyStripAccents(P, applyNFD(P, ...))).
    "bertcleantext,stripaccents,lowercase":
        Sequence([BertNormalizer(clean_text=True, handle_chinese_chars=False,
                                  strip_accents=False, lowercase=False),
                   NFD(), StripAccents(), Lowercase()]),
    "bertcleantext,bertchinesechars,stripaccents,lowercase":
        Sequence([BertNormalizer(clean_text=True, handle_chinese_chars=True,
                                  strip_accents=False, lowercase=False),
                   NFD(), StripAccents(), Lowercase()]),
    "bytelevel,lowercase":              Sequence([ByteLevel(), Lowercase()]),
    "lowercase,strip":                  Sequence([Lowercase(), Strip()]),
    "bytelevel,strip":                  Sequence([ByteLevel(), Strip()]),
    "bytelevel,strip,lowercase":        Sequence([ByteLevel(), Strip(), Lowercase()]),
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
# Codepoint sweep
# ---------------------------------------------------------------------------

# Written after every context.  Digits are kept unchanged by every mode and
# nothing composes with them, so both outputs can be split here.
SWEEP_SEP = "0123456789"
# The neighbours normalization depends on: alone; twice (a mark that composes
# with itself); after a base letter (a mark composing with it); before a
# combining acute (the codepoint as a base); between letters (word-internal
# rules such as final sigma).
SWEEP_CONTEXTS = {"alone": "{}", "twice": "{0}{0}", "after_a": "a{}",
                  "before_acute": "{}\u0301", "between_a": "a{}a"}
# Single modes only; strip* change only the ends of the whole text.
SWEEP_MODES = ["nfc", "nfd", "nfkc", "nfkd", "stripaccents", "bytelevel",
               "lowercase", "nmt", "bertchinesechars", "bertcleantext"]
UNICODEDATA_FORMS = {"nfc": "NFC", "nfd": "NFD", "nfkc": "NFKC", "nfkd": "NFKD"}


def unicode_ages() -> dict[int, str]:
    with open(DERIVED_AGE, encoding="utf-8") as f:
        src = f.read()
    ages = {}
    for name, body in re.findall(r"Code Point Ranges for (V\w+)(.*?)\*/", src, re.S):
        age = name[1:].replace("_", ".")
        for lo, hi in re.findall(r"\[([0-9a-f]+), ([0-9a-f]+)\]", body):
            for cp in range(int(lo, 16), int(hi, 16) + 1):
                ages[cp] = age
    return ages


def run_sweep(requested: list[str]) -> None:
    ages = unicode_ages()
    cps = [cp for cp in range(0x110000)
           if unicodedata.category(chr(cp)) not in ("Cn", "Cs")]
    missing = [cp for cp in cps if cp not in ages]
    if missing:
        sys.exit(f"Error: no Unicode age in {DERIVED_AGE} for U+{missing[0]:04X} "
                 f"and {len(missing) - 1} more")
    n = len(SWEEP_CONTEXTS)
    text = "".join(c.format(chr(cp)) + SWEEP_SEP
                   for cp in cps for c in SWEEP_CONTEXTS.values())
    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as f:
        f.write(text.encode("utf-8"))
        tmp = f.name
    try:
        with open(SWEEP_OUTPUT_FILE, "w", encoding="utf-8") as f:
            out = Tee(f)
            out.write(f"Codepoint sweep: {len(cps):,} assigned codepoints of Unicode "
                      f"{unicodedata.unidata_version} (Python unicodedata), "
                      f"contexts {', '.join(SWEEP_CONTEXTS)}\n")
            out.write(f"Tokenizer: {DEFAULT_TOKENIZER}\n\n")
            for mode in requested:
                # Bytes, not text: universal newlines would turn \r into \n.
                r = subprocess.run([DEFAULT_TOKENIZER, f"--normalize={mode}", tmp],
                                   capture_output=True)
                if r.returncode != 0:
                    out.write(f"{mode:18} CRASH: {ParabixCrash(r.returncode, r.stderr.decode(errors='replace'))}\n")
                    continue
                pbx = r.stdout.decode("utf-8").split(SWEEP_SEP)
                hf = run_hf(MODES[mode], text).split(SWEEP_SEP)
                if not len(pbx) == len(hf) == n * len(cps) + 1:
                    out.write(f"{mode:18} CANNOT ALIGN: {len(pbx)} Parabix pieces, "
                              f"{len(hf)} HF pieces, {n * len(cps) + 1} expected\n")
                    continue
                form = UNICODEDATA_FORMS.get(mode)
                rows, both, pbx_py, hf_py = [], 0, 0, 0
                for i, cp in enumerate(cps):
                    ctx = []
                    for j, name in enumerate(SWEEP_CONTEXTS):
                        k = n * i + j
                        if pbx[k] == hf[k]:
                            continue
                        ctx.append(name)
                        if form:
                            py = unicodedata.normalize(form, SWEEP_CONTEXTS[name].format(chr(cp)))
                            both += 1
                            pbx_py += pbx[k] == py
                            hf_py += hf[k] == py
                    if ctx:
                        rows.append(f"  U+{cp:04X}\t{ages[cp]}\t"
                                    f"{unicodedata.name(chr(cp), '')}\t{','.join(ctx)}\n")
                span = sorted({r.split("\t")[1] for r in rows},
                              key=lambda v: tuple(map(int, v.split("."))))
                line = f"{mode:18} {len(rows):5} differing codepoints"
                if rows:
                    line += f"  (Unicode ages {', '.join(span)})"
                if form and both:
                    line += (f"; of {both} differing contexts, Parabix = unicodedata "
                             f"in {pbx_py}, HF in {hf_py}")
                out.write(line + "\n")
                f.write("".join(rows))
    finally:
        os.unlink(tmp)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    global DEFAULT_TOKENIZER
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
    parser.add_argument("--tokenizer",    default=DEFAULT_TOKENIZER,
                        help=f"Parabix tokenizer binary (default: {DEFAULT_TOKENIZER})")
    parser.add_argument("--sweep",        action="store_true",
                        help="Codepoint sweep instead of --input (default modes: "
                             f"{SWEEP_MODES}); output in {SWEEP_OUTPUT_FILE}")
    args = parser.parse_args()
    DEFAULT_TOKENIZER = args.tokenizer
    if args.sweep and (args.hf_only or args.parabix_only):
        parser.error("--sweep compares both sides; drop --hf-only/--parabix-only")
    if not args.hf_only and not os.path.isfile(DEFAULT_TOKENIZER):
        print(f"Error: tokenizer binary not found: {DEFAULT_TOKENIZER}")
        sys.exit(1)

    if args.sweep:
        unknown = [m for m in args.modes if m not in all_modes]
        if unknown:
            parser.error(f"unknown mode(s): {unknown}")
        run_sweep(args.modes or SWEEP_MODES)
        print(f"\nOutput written to: {SWEEP_OUTPUT_FILE}")
        return

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
