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
    python compare_bpe.py --runs 10                    # timed runs per side (default 5)
    python compare_bpe.py --no-timing                  # skip the timing section
"""

import sys
import subprocess
import tempfile
import os
import re
import argparse
import time
from tokenizers import Tokenizer

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --tokenizer/--merges/--vocab/--tokenizer-json override.
REPO_ROOT      = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER      = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
VOCAB          = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/vocab.json")
MERGES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")
TOKENIZER_JSON = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/tokenizer.json")
DEFAULT_INPUT  = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/val_4MB.txt")
OUTPUT_FILE    = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "compare_bpe_output.txt")

DEFAULT_RUNS = 5

# Extra flags forwarded verbatim to the tokenizer binary on every invocation
# (both the correctness run and the --bench-loop timing run). Set by --tok-flag.
EXTRA_ARGS: list[str] = []

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

# Printing repr(text) for every case buries the result — one openwebtext line can
# be thousands of characters. Default to a size summary; --show-input restores the
# full text when a specific case needs eyeballing.
SHOW_INPUT = False


def input_line(text: str, pad: str = "") -> str:
    if SHOW_INPUT:
        return f"  Input:{pad} {repr(text)}\n"
    return f"  Input:{pad} <{len(text)} chars, {len(text.encode())} bytes>\n"


def _parabix_ids_cmd(input_path: str, strings: bool = False):
    """Spawn the tokenizer binary on a pre-written input file. One subprocess."""
    cmd = [TOKENIZER, "--pretokenizer=bytelevel", f"--vocab={VOCAB}", f"--merges={MERGES}"]
    cmd += EXTRA_ARGS
    if strings:
        cmd.append("--strings")
    cmd.append(input_path)
    return subprocess.run(cmd, capture_output=True, encoding="utf-8")


def run_parabix_bpe(text: str) -> tuple[list[int], list[str]]:
    """Parabix BPE: --pretokenizer=bytelevel + BPE merge stage in one pipeline.

    tokenizer.cpp's --pretokenizer=bytelevel path (buildBPEPipeline, PreTokenizer
    == bytelevel branch) feeds the merge stage the RAW input bytes as bpeBasis —
    NOT the Ġ-remapped pretokenizer output, which would double-encode (BPERangeSeed
    already applies GPT-2 bytes_to_unicode once) — and separately threads the
    regex pretoken-boundary mask through as bpeBoundary, so BPEMergeKernel blocks
    merges that would cross a pretoken boundary. This is the actual HF-equivalent
    configuration: verified byte-identical (ids + token count) against HF on a
    49MB / 15.75M-token openwebtext run (2026-09-03)."""

    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        input_path = f.name
    try:
        r_ids  = _parabix_ids_cmd(input_path)
        r_strs = _parabix_ids_cmd(input_path, strings=True)
    finally:
        os.unlink(input_path)

    # lists of typed values to be compared
    ids     = [int(x) for x in r_ids.stdout.split() if x.lstrip("-").isdigit()]
    strings = [s.split("\t", 1)[-1] for s in r_strs.stdout.split("\n") if s]
    return ids, strings


def run_hf_bpe(tokenizer_obj, text: str) -> tuple[list[int], list[str]]:
    """Run HuggingFace tokenizer (loaded from local tokenizer.json)."""
    output = tokenizer_obj.encode(text)
    return output.ids, output.tokens


# ---------------------------------------------------------------------------
# Timing — FAIR: pure tokenization, one-time setup excluded on BOTH sides.
#
#   Parabix : tokenizer --bench-loop=N runs the already-built, JIT-compiled
#             pipeline N times inside ONE process (no per-iter process spawn,
#             merges.txt load, or pipeline build/JIT), token output suppressed.
#             The binary prints a machine-readable BENCH_RESULT stderr line we
#             parse. This is Parabix's tokenize-only cost.
#   HF      : tokenizer.encode(text) in a warm best-of-N in-process loop; the
#             model was loaded ONCE by Tokenizer.from_file() before timing.
#
# HF's from_file() (load tokenizer.json + build vocab/merge tables) is the
# counterpart of Parabix's spawn + merges load + compile/link — one-time, and
# excluded on BOTH sides. So we compare tokenize-vs-tokenize, not process-vs-
# preloaded (the earlier version timed the whole Parabix subprocess = unfair).
# ---------------------------------------------------------------------------

# BENCH_RESULT bytes=848 iters=50 min_ms=.. median_ms=.. mean_ms=.. mbps=..
BENCH_RE = re.compile(
    r"BENCH_RESULT\s+bytes=(\d+)\s+iters=(\d+)\s+min_ms=([\d.]+)\s+"
    r"median_ms=([\d.]+)\s+mean_ms=([\d.]+)\s+mbps=([\d.]+)"
)


def bench_parabix(input_path: str, runs: int):
    """Fair Parabix tokenize time via in-process --bench-loop. Returns a stats
    dict (min/median/mean ms + mbps) or None if no BENCH_RESULT was produced.

    Carries --pretokenizer=bytelevel so this times the SAME pipeline shape that
    _parabix_ids_cmd checks for correctness. Without it the run takes the raw-byte
    path with no pretoken boundary gating, which is both cheaper (one fewer And per
    rule, across all 50,000) and not the configuration anything validates — so the
    reported MB/s would describe a pipeline nobody has checked."""
    r = subprocess.run(
        [TOKENIZER, "--pretokenizer=bytelevel", f"--bench-loop={runs}",
         f"--vocab={VOCAB}", f"--merges={MERGES}", *EXTRA_ARGS, input_path],
        capture_output=True, encoding="utf-8"
    )
    m = BENCH_RE.search(r.stderr or "")
    if not m:
        return None
    return {
        "bytes":     int(m.group(1)),
        "iters":     int(m.group(2)),
        "min_ms":    float(m.group(3)),
        "median_ms": float(m.group(4)),
        "mean_ms":   float(m.group(5)),
        "mbps":      float(m.group(6)),
    }


def bench_hf(tokenizer_obj, text: str, runs: int) -> dict:
    """Fair HF tokenize time: warm best-of-N in-process encode() (model preloaded)."""
    tokenizer_obj.encode(text)                       # one discarded warm-up
    times_ms = []
    for _ in range(runs):
        t0 = time.perf_counter()
        tokenizer_obj.encode(text)
        times_ms.append((time.perf_counter() - t0) * 1e3)
    times_ms.sort()
    nbytes = len(text.encode("utf-8"))
    minv   = times_ms[0]
    median = times_ms[len(times_ms) // 2]
    mean   = sum(times_ms) / len(times_ms)
    mbps   = (nbytes / (minv / 1e3)) / 1e6 if minv > 0 else 0.0
    return {"bytes": nbytes, "iters": runs, "min_ms": minv,
            "median_ms": median, "mean_ms": mean, "mbps": mbps}


def measure_timing(tokenizer_obj, text: str, runs: int) -> dict:
    """Fair tokenize-vs-tokenize timing (setup excluded both sides)."""
    result = {}

    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        ppath = f.name
    try:
        result["parabix"] = bench_parabix(ppath, runs)
    finally:
        os.unlink(ppath)

    if tokenizer_obj is not None:
        result["hf"] = bench_hf(tokenizer_obj, text, runs)

    return result


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


def write_timing(out, timing: dict, text: str, runs: int, ntokens: int) -> None:
    nbytes = len(text.encode("utf-8"))
    out.write(SEP + "\n")
    out.write(f"TIMING  (FAIR — pure tokenize, one-time setup excluded both sides; N={runs})\n")
    out.write(SEP + "\n")
    out.write(f"  Input size:  {nbytes} bytes, {ntokens} tokens\n\n")

    def row(label, s):
        out.write(f"  {label:<9}  min {s['min_ms']:9.4f} ms   "
                  f"median {s['median_ms']:9.4f} ms   mean {s['mean_ms']:9.4f} ms   "
                  f"{s['mbps']:8.3f} MB/s\n")

    p = timing.get("parabix")
    h = timing.get("hf")
    if p:
        row("Parabix", p)
    else:
        out.write("  Parabix    <no BENCH_RESULT — rebuild tokenizer with --bench-loop support>\n")
    if h:
        row("HF", h)

    if p and h and p["min_ms"] > 0 and h["min_ms"] > 0:
        ratio = p["min_ms"] / h["min_ms"]
        faster, slower = ("HF", "Parabix") if ratio >= 1 else ("Parabix", "HF")
        out.write(f"\n  Speedup (min): {faster} is {max(ratio, 1 / ratio):.1f}x "
                  f"faster than {slower}  (pure tokenize)\n")

    out.write("\n  Fair: Parabix via --bench-loop (N iters in ONE process — no per-iter\n"
              "        spawn / merges-load / compile, output suppressed); HF via preloaded\n"
              "        in-process encode() loop (from_file excluded). Setup excluded both.\n"
              "  Caveats: Parabix 'run' still includes the input file read; use MB-scale\n"
              "        input (tokenizer_files/webtext_100.txt) for a meaningful MB/s.\n")
    out.write(SEP + "\n\n")


# ---------------------------------------------------------------------------
# Run modes
# ---------------------------------------------------------------------------

def run_compare(out, tokenizer_obj, cases: list[tuple[int, str]], verbose: bool,
                runs: int = 0) -> dict:
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
        out.write(input_line(text, "         "))
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

        if runs > 0:
            timing = measure_timing(tokenizer_obj, text, runs)
            write_timing(out, timing, text, runs, len(hf_ids))

    return results


def run_hf_only(out, tokenizer_obj, cases: list[tuple[int, str]]) -> None:
    for num, (lineno, text) in enumerate(cases, 1):
        ids, tokens = run_hf_bpe(tokenizer_obj, text)
        out.write(SEP + "\n")
        out.write(f"Test {num} (line {lineno}) — HuggingFace\n")
        out.write(SEP + "\n")
        out.write(input_line(text))
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
        out.write(input_line(text))
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
    global SHOW_INPUT, EXTRA_ARGS, TOKENIZER, MERGES, VOCAB, TOKENIZER_JSON
    parser = argparse.ArgumentParser(description="BPE tokenizer comparison test suite.")
    parser.add_argument("--input",        default=DEFAULT_INPUT,
                        help=f"Input file (entire file is one test case, default: {DEFAULT_INPUT})")
    parser.add_argument("--hf-only",      action="store_true",
                        help="Run HuggingFace tokenizer only")
    parser.add_argument("--parabix-only", action="store_true",
                        help="Run Parabix tokenizer only")
    parser.add_argument("--show-input",   action="store_true",
                        help="Print each case's full input text (off by default: "
                             "only a char/byte count is shown)")
    parser.add_argument("--verbose",      action="store_true",
                        help="Always show full token-by-token table (default: only on mismatch)")
    parser.add_argument("--runs",         type=int, default=DEFAULT_RUNS,
                        help=f"Timed runs per side in compare mode (default: {DEFAULT_RUNS})")
    parser.add_argument("--no-timing",    action="store_true",
                        help="Skip the timing section")
    parser.add_argument("--tok-flag",     action="append", default=[], metavar="FLAG",
                        help="Extra flag passed straight to the tokenizer binary "
                             "(repeatable), e.g. --tok-flag=--level-partition")
    parser.add_argument("--tokenizer",      default=TOKENIZER,
                        help=f"Parabix tokenizer binary (default: {TOKENIZER})")
    parser.add_argument("--merges",         default=MERGES,
                        help=f"merges.txt (default: {MERGES})")
    parser.add_argument("--vocab",          default=VOCAB,
                        help=f"vocab.json (default: {VOCAB})")
    parser.add_argument("--tokenizer-json", default=TOKENIZER_JSON,
                        help=f"HuggingFace tokenizer.json (default: {TOKENIZER_JSON})")
    args = parser.parse_args()
    TOKENIZER, MERGES = args.tokenizer, args.merges
    VOCAB, TOKENIZER_JSON = args.vocab, args.tokenizer_json
    if not args.hf_only and not os.path.isfile(TOKENIZER):
        print(f"Error: tokenizer binary not found: {TOKENIZER}")
        sys.exit(1)
    SHOW_INPUT = args.show_input
    EXTRA_ARGS = list(args.tok_flag)
    if EXTRA_ARGS:
        print(f"Extra tokenizer flags: {' '.join(EXTRA_ARGS)}")

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
            runs = 0 if args.no_timing else args.runs
            results = run_compare(out, tokenizer_obj, cases, args.verbose, runs)
            write_summary(out, results)

    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()



