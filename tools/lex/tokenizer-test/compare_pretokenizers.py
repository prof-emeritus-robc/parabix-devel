#!/usr/bin/env python3
"""
Unified pre-tokenizer test suite.

Modes:
    (default)         Compare Parabix vs HuggingFace side by side
    --hf-only         Run HuggingFace pre-tokenizers only
    --parabix-only    Run Parabix pre-tokenizers only + behavior tests from XML
    --sweep           Codepoint sweep instead of --input: every assigned codepoint
                      of Python's unicodedata (not Cn, not Cs) in six contexts,
                      each on its own line; see run_sweep.

Usage:
    python compare_parabix_hf.py                          # compare all modes
    python compare_parabix_hf.py --hf-only                # HF reference only
    python compare_parabix_hf.py --parabix-only           # Parabix only
    python compare_parabix_hf.py whitespace bert          # specific modes
    python compare_parabix_hf.py /path/to/input.txt       # custom input file
    python compare_pretokenizers.py --sweep               # codepoint sweep, 20 cases
    python compare_pretokenizers.py --sweep bytelevel bert
"""

import sys
import subprocess
import tempfile
import os
import argparse
import bisect
import re
import unicodedata
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
SWEEP_OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "compare_pretokenizers_sweep_output.txt")
# Unicode age of each codepoint: the ranges in the comments of the generated
# Parabix UCD table (the same ranges as the Unicode file DerivedAge.txt).
DERIVED_AGE       = os.path.join(REPO_ROOT, "lib/ucd/data/DerivedAge.cpp")

SEP  = "=" * 65
DASH = "-" * 65

# ---------------------------------------------------------------------------
# Parabix mode → HuggingFace pre-tokenizer mapping
# ---------------------------------------------------------------------------

MODES = {
    "whitespacesplit":           WhitespaceSplit(),
    "whitespace":                Whitespace(),
    "bytelevel":                 ByteLevel(add_prefix_space=False),
    "bert":                      BertPreTokenizer(),
    "punctuation":               Punctuation(),
    "digits":                    Digits(individual_digits=True),
    "chardelimiter":             CharDelimiterSplit(","),
    "sequence_whitespace_punct": Sequence([WhitespaceSplit(), Punctuation()]),
    "simplewordboundaries":      Split(pattern=Regex(r"\w+"), behavior="isolated"),
    # uax29: no HF equivalent — appears in parabix-only / behavior tests only
}

# Parabix CLI flag name where it differs from the dict key
PARABIX_FLAG = {
    "sequence_whitespace_punct": "sequence_whitespace_punctuation",
}

# Parabix --behavior= value -> HuggingFace Split(behavior=...) value. Both sides
# expose the same 5-way enum (confirmed via tokenizers.pre_tokenizers.Split),
# so this is a real differential check, not a Parabix-only concept.
BEHAVIOR_MAP = {
    "removed":            "removed",
    "isolated":           "isolated",
    "mergedwithprevious": "merged_with_previous",
    "mergedwithnext":     "merged_with_next",
    "contiguous":         "contiguous",
}
# Pattern shared by both sides: the same \w+ rule "simplewordboundaries" uses,
# confirmed correct against HF (see MODES["simplewordboundaries"] above) --
# so a mismatch here isolates a behavior-kernel bug, not a pattern bug.
BEHAVIOR_PATTERN = Regex(r"\w+")

# The two HF pre-tokenizers that take a behavior: Split(pattern, behavior) and
# Punctuation(behavior). Each Parabix mode is checked against the one it mirrors.
BEHAVIOR_MODES = [
    ("simplewordboundaries", lambda b: Split(pattern=BEHAVIOR_PATTERN, behavior=b),
     r'Split(Regex(r"\w+"))'),
    ("punctuation",          lambda b: Punctuation(behavior=b), "Punctuation()"),
]

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

    By default the tokenizer writes a line feed between pre-tokens, and
    run_parabix splits its output there. A line feed inside a pre-token
    (with punctuation, "a\\nb" is one pre-token in both tools) then looks
    like a boundary, so a multi-line text compared whole would show false
    mismatches (it did on digits/punctuation/chardelimiter/
    sequence_whitespace_punct). Each line is therefore run on its own, as
    compare_bpe.py does. run_sweep compares whole texts instead, with
    --pretok-separator=0x10FFFF.
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


def run_behavior_compare(out, lines: list[str]) -> dict[str, str]:
    r"""Differential check of all 5 --behavior= values for each mode of
    BEHAVIOR_MODES: simplewordboundaries against HF's Split(Regex(r"\w+"),
    behavior=X), punctuation against HF's Punctuation(behavior=X). Both modes'
    default boundaries are already confirmed correct, so a mismatch here
    isolates a behavior-kernel bug rather than a boundary-detection bug.
    Per-line, same reason as run_compare (the line feed that separates
    Parabix's output pre-tokens)."""
    results = {}
    cases = [(mode, make_hf, hf_name, pbx_behavior, hf_behavior)
             for mode, make_hf, hf_name in BEHAVIOR_MODES
             for pbx_behavior, hf_behavior in BEHAVIOR_MAP.items()]
    for num, (mode, make_hf, hf_name, pbx_behavior, hf_behavior) in enumerate(cases, 1):
        hf_split = make_hf(hf_behavior)
        mismatches = []
        for li, line in enumerate(lines):
            parabix = run_parabix(mode, line, behavior=pbx_behavior)
            hf      = run_hf(hf_split, line)
            if parabix != hf:
                mismatches.append((li, line, parabix, hf))

        status = "MATCH" if not mismatches else "MISMATCH"
        results[f"{mode}+{pbx_behavior}"] = status

        out.write(SEP + "\n")
        out.write(f"Behavior Test {num}: {mode} + {pbx_behavior}  "
                  f"(HF: {hf_name}, {hf_behavior})  →  {status}\n")
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

        # Each case's own <inputdata id="..."> text, not the --input file --
        # these counts were calibrated against this fixture's short multi-script
        # string, not whatever corpus happens to be loaded for the other tests.
        inputs = {el.get("id"): el.text.strip() for el in root.findall("inputdata")}

        passed = failed = 0
        for num, tc in enumerate(cases, 1):
            mode     = tc.get("pretokenizer")
            behavior = tc.get("behavior")
            expected = int(tc.get("expectedcount"))
            case_text = inputs.get(tc.get("inputdata"), text)
            tokens   = run_parabix(mode, case_text, behavior)
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
# Codepoint sweep
# ---------------------------------------------------------------------------

# The neighbours a pre-tokenizer looks at: alone; twice (runs of one class);
# after a letter; before a combining mark; between letters (inside a word);
# after a space (the byte-level splitter attaches one space to the next piece).
SWEEP_CONTEXTS = {"alone": "{}", "twice": "{0}{0}", "after_a": "a{}",
                  "before_acute": "{}\u0301", "between_a": "a{}a",
                  "after_space": " {}"}
# Written before, between and after the cases.  Every mode splits inside it
# (line feed, comma, digit, punctuation, letter), so no pre-token covers two
# cases, and every case sits on its own line between the same neighbours.
SWEEP_SEP = "\n,0!a\n"
# Parabix writes this between pre-tokens; it is Cn, so never in the text.
SWEEP_OUT_SEP = "\U0010FFFF"
SWEEP_CHUNK = 100_000          # cases per tokenizer run
# The BPE front end's boundaries (as in bench_pretokenizers.py).
FRONTEND_FLAGS = ["--pretokenizer=bytelevel", "--pretok-compress-classes",
                  "--pretok-class-code", "--pretok-byte-domain",
                  "--pretok-utf8-classes"]


def sweep_cases() -> dict:
    """name -> (Parabix flags, HF pre-tokenizer, output is byte-level mapped)."""
    cases = {m: ([f"--pretokenizer={PARABIX_FLAG.get(m, m)}"], pt, m == "bytelevel")
             for m, pt in MODES.items()}
    cases["bytelevel_frontend"] = (FRONTEND_FLAGS, ByteLevel(add_prefix_space=False), True)
    for mode, make_hf, _ in BEHAVIOR_MODES:
        for pbx_b, hf_b in BEHAVIOR_MAP.items():
            cases[f"{mode}+{pbx_b}"] = ([f"--pretokenizer={mode}", f"--behavior={pbx_b}"],
                                        make_hf(hf_b), False)
    return cases


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


def _byte_decoder() -> dict[str, int]:
    """Inverse of GPT-2's byte-to-character map."""
    bs = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    cs, n = bs[:], 0
    for b in range(256):
        if b not in bs:
            bs.append(b); cs.append(256 + n); n += 1
    return {chr(c): b for b, c in zip(bs, cs)}


BYTE_DECODER = _byte_decoder()


def locate(tokens: list[str], text: str, bytelevel: bool) -> list[tuple]:
    """(start, end, token) for Parabix's pre-tokens, which have no offsets: each
    is found in the text after the previous one (a mode may delete characters
    between them).  A token not found near there is kept at that position."""
    spans, pos = [], 0
    for t in tokens:
        raw = (bytes(BYTE_DECODER.get(c, 63) for c in t).decode("utf-8", "replace")
               if bytelevel else t)
        i = text.find(raw, pos, pos + len(raw) + 64)
        if i < 0:
            spans.append((pos, pos + len(raw), t))
        else:
            spans.append((i, i + len(raw), t))
            pos = i + len(raw)
    return spans


def run_sweep(requested: list[str]) -> None:
    """Every assigned codepoint in each of SWEEP_CONTEXTS, one case per line
    between SWEEP_SEP lines.  Parabix pre-tokenizes the whole text (pre-tokens
    written with SWEEP_OUT_SEP between them, so a line feed inside a pre-token
    stays visible) and HF pre_tokenize_str the same text; the two lists of
    (offsets, string) are compared, and every pre-token that differs is charged
    to the case it overlaps."""
    cases_by_name = sweep_cases()
    ages = unicode_ages()
    cps = [cp for cp in range(0x110000)
           if unicodedata.category(chr(cp)) not in ("Cn", "Cs")]
    missing = [cp for cp in cps if cp not in ages]
    if missing:
        sys.exit(f"Error: no Unicode age in {DERIVED_AGE} for U+{missing[0]:04X} "
                 f"and {len(missing) - 1} more")
    names = list(SWEEP_CONTEXTS)
    n = len(names)
    cases = [c.format(chr(cp)) for cp in cps for c in SWEEP_CONTEXTS.values()]
    chunks = []
    for lo in range(0, len(cases), SWEEP_CHUNK):
        part = cases[lo:lo + SWEEP_CHUNK]
        starts, pos = [], len(SWEEP_SEP)
        for c in part:
            starts.append(pos)
            pos += len(c) + len(SWEEP_SEP)
        chunks.append((lo, SWEEP_SEP + SWEEP_SEP.join(part) + SWEEP_SEP, starts))

    with open(SWEEP_OUTPUT_FILE, "w", encoding="utf-8") as f:
        out = Tee(f)
        out.write(f"Codepoint sweep: {len(cps):,} assigned codepoints of Unicode "
                  f"{unicodedata.unidata_version} (Python unicodedata), contexts "
                  f"{', '.join(names)}; {len(cases):,} cases, each on its own line "
                  f"between lines {SWEEP_SEP.strip()!r}\n")
        out.write(f"Tokenizer: {DEFAULT_TOKENIZER}\n\n")
        for name in requested:
            flags, pt, bytelevel = cases_by_name[name]
            bad, examples, stray, crash = {}, [], 0, None
            for lo, text, starts in chunks:
                with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as t:
                    t.write(text.encode("utf-8"))
                    tmp = t.name
                try:
                    r = subprocess.run([DEFAULT_TOKENIZER, *flags,
                                        f"--pretok-separator={ord(SWEEP_OUT_SEP):#x}", tmp],
                                       capture_output=True)
                finally:
                    os.unlink(tmp)
                if r.returncode != 0:
                    crash = f"exit {r.returncode}: {r.stderr.decode(errors='replace')[-300:]}"
                    break
                pbx = locate([s for s in r.stdout.decode("utf-8").split(SWEEP_OUT_SEP) if s],
                             text, bytelevel)
                hf = [(a, b, tok) for tok, (a, b) in pt.pre_tokenize_str(text)]
                diff = set(pbx).symmetric_difference(hf)
                hit = set()
                for a, b, _ in diff:
                    # cases overlapping [a, b); a token inside a separator line
                    # is charged to the cases on both sides of it
                    i = bisect.bisect_right(starts, a) - 1
                    over = [k for k in (i, i + 1) if 0 <= k < len(starts)
                            and starts[k] < max(b, a + 1)
                            and a < starts[k] + max(len(cases[lo + k]), 1)]
                    if not over:
                        stray += 1
                        over = [k for k in (i, i + 1) if 0 <= k < len(starts)]
                    hit.update(over)
                for k in sorted(hit):
                    g = lo + k
                    bad.setdefault(cps[g // n], []).append(names[g % n])
                    if len(examples) < 5:
                        a, b = starts[k], starts[k] + len(cases[g])
                        side = lambda toks: [x[2] for x in toks if x[0] < b + 1 and x[1] > a - 1]
                        examples.append((cases[g], side(pbx), side(hf)))
            if crash:
                out.write(f"{name:40} CRASH {crash}\n")
                continue
            rows = [f"  U+{cp:04X}\t{ages[cp]}\t{unicodedata.name(chr(cp), '')}\t"
                    f"{','.join(ctx)}\n" for cp, ctx in sorted(bad.items())]
            span = sorted({ages[cp] for cp in bad}, key=lambda v: tuple(map(int, v.split("."))))
            line = f"{name:40} {len(bad):6} differing codepoints"
            if bad:
                line += f"  (Unicode ages {', '.join(span)})"
            if stray:
                line += f"; {stray} differing pre-tokens inside separator lines"
            out.write(line + "\n")
            f.write("".join(rows))
            for case, p, h in examples:
                f.write(f"    e.g. {case!r}: Parabix {p!r}  HF {h!r}\n")

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
    parser.add_argument("--behaviors",    action="store_true",
                        help="Differential check of all 5 --behavior= values against "
                             "HF's Split(Regex(r'\\w+'), behavior=X), instead of the "
                             "per-mode comparison")
    parser.add_argument("modes",          nargs="*",
                        help="Specific modes to test (default: all)")
    parser.add_argument("--tokenizer",    default=DEFAULT_TOKENIZER,
                        help=f"Parabix tokenizer binary (default: {DEFAULT_TOKENIZER})")
    parser.add_argument("--max-lines",    type=int, default=200,
                        help="Cap the number of lines compared for a multi-line "
                             "corpus (default 200; each line spawns a subprocess "
                             "per mode, so a full 5000-line corpus over 7 modes "
                             "is slow). Use 0 for no cap.")
    parser.add_argument("--sweep",        action="store_true",
                        help="Codepoint sweep instead of --input (default: the 9 "
                             "modes, the BPE front end and the 10 behavior cases); "
                             f"output in {SWEEP_OUTPUT_FILE}")
    args = parser.parse_args()
    DEFAULT_TOKENIZER = args.tokenizer
    if args.sweep and (args.hf_only or args.parabix_only or args.behaviors):
        parser.error("--sweep runs both sides; drop --hf-only/--parabix-only/--behaviors")
    if not args.hf_only and not os.path.isfile(DEFAULT_TOKENIZER):
        print(f"Error: tokenizer binary not found: {DEFAULT_TOKENIZER}")
        sys.exit(1)

    if args.sweep:
        names = list(sweep_cases())
        unknown = [m for m in args.modes if m not in names]
        if unknown:
            parser.error(f"unknown sweep case(s): {unknown}; available: {names}")
        run_sweep(args.modes or names)
        print(f"\nOutput written to: {SWEEP_OUTPUT_FILE}")
        return

    with open(args.input, "r", encoding="utf-8") as f:
        text = f.read().strip()

    # Parabix's output has a line feed between pre-tokens, so a multi-line
    # corpus is compared line by line, not as one string -- see run_compare.
    # Text mode: "\r\n" and a lone "\r" are read as "\n".
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
        elif args.behaviors:
            results = run_behavior_compare(out, lines)
            write_summary(out, results)
        else:
            results = run_compare(out, lines, requested)
            write_summary(out, results)

    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
