#!/usr/bin/env python3
"""
Pre-tokenizer throughput benchmark: Parabix vs HuggingFace `tokenizers`.

The performance counterpart to compare_pretokenizers.py. Each pre-tokenizer runs
on its own (no normalizer, no BPE), one thread on both sides.

What is timed
-------------
Parabix: `tokenizer --pretokenizer=MODE --bench-loop=N --thread-num=1`. The
pipeline is built and compiled once and the input is loaded once, outside the
timer; one untimed run, then N timed runs. Each run finds the boundaries, builds
the pre-token strings with their separators and writes them to /dev/null.

HuggingFace: per call, a fresh `PreTokenizedString(text)` and an in-place
`pre_tokenize`. The strings and offsets stay in native storage; they are not
converted to Python lists. One untimed call, then N timed calls.

So the two sides are complete pipelines with different outputs (serialized text
vs native strings with offsets), not boundary finding alone.

Correctness first
-----------------
Before timing, each case runs the tokenizer once with
`--pretok-separator=0x10FFFF` and compares the complete pre-token list (strings
and order, not offsets) with HuggingFace's `pre_tokenize_str`. U+10FFFF must not
occur in the input, so unlike LF it cannot be confused with a line break inside
a pre-token. A case that does not match is reported and not timed.

Rounds
------
--rounds R (default 3) rounds of --iters N (default 5) timed calls per side; the
side that goes first alternates between rounds. Reported: the median of the
round medians, the range of the round medians, and HF median / Parabix median
(above 1: Parabix faster). Each case runs in a fresh process, so HuggingFace
allocations do not pile up from one case to the next. Keep the machine idle
while it runs.

Usage
-----
No corpus is built in: name the inputs (UTF-8 text) with --input.

    python bench_pretokenizers.py --input val_4MB.txt code_4MB.txt multi_4MB.txt
    python bench_pretokenizers.py --input FILE --modes whitespace bytelevel_frontend
    python bench_pretokenizers.py --input FILE --rounds 1 --iters 1      # quick check
    python bench_pretokenizers.py --input FILE --json results.json        # all samples too
"""

import os

# One thread on the HuggingFace side; set before `tokenizers` is imported.
os.environ["TOKENIZERS_PARALLELISM"] = "false"
os.environ["RAYON_NUM_THREADS"] = "1"

import argparse
import json
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime

import tokenizers
from tokenizers import PreTokenizedString, Regex
from tokenizers.pre_tokenizers import (
    BertPreTokenizer, ByteLevel, CharDelimiterSplit, Digits, Punctuation,
    Sequence, Split, Whitespace, WhitespaceSplit,
)

# ---------------------------------------------------------------------------
# Paths and settings
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --tokenizer overrides.
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "bench_pretokenizers_output.txt")

SEPARATOR = "\U0010FFFF"   # correctness run only; never timed
TIMEOUT_S = 600            # one tokenizer call (compile + all runs)

# The BPE front end's boundary builder (buildGPT2ByteBoundaries), as the best BPE configuration
# runs it, on its own.
FRONTEND_FLAGS = ["--pretokenizer=bytelevel", "--pretok-compress-classes", "--pretok-class-code",
                  "--pretok-byte-domain", "--pretok-utf8-classes"]

# name: (label, Parabix flags, HuggingFace pre-tokenizer). Defaults as in
# compare_pretokenizers.py: isolated behavior, Digits one digit at a time,
# CharDelimiterSplit on ',', ByteLevel without a prefix space.
MODES = {
    "whitespace": ("Whitespace", ["--pretokenizer=whitespace"], lambda: Whitespace()),
    "whitespacesplit": ("WhitespaceSplit", ["--pretokenizer=whitespacesplit"], lambda: WhitespaceSplit()),
    "digits": ("Digits", ["--pretokenizer=digits"], lambda: Digits(individual_digits=True)),
    "punctuation": ("Punctuation", ["--pretokenizer=punctuation"], lambda: Punctuation()),
    "simplewordboundaries": ("Split (\\w+)", ["--pretokenizer=simplewordboundaries"],
                             lambda: Split(Regex(r"\w+"), behavior="isolated")),
    "chardelimiter": ("CharDelimiterSplit", ["--pretokenizer=chardelimiter"],
                      lambda: CharDelimiterSplit(",")),
    "bert": ("BertPreTokenizer", ["--pretokenizer=bert"], lambda: BertPreTokenizer()),
    "sequence_whitespace_punctuation": ("WhitespaceSplit + Punctuation",
                                        ["--pretokenizer=sequence_whitespace_punctuation"],
                                        lambda: Sequence([WhitespaceSplit(), Punctuation()])),
    "bytelevel": ("ByteLevel", ["--pretokenizer=bytelevel"], lambda: ByteLevel(add_prefix_space=False)),
    "bytelevel_frontend": ("ByteLevel (BPE front end)", FRONTEND_FLAGS,
                           lambda: ByteLevel(add_prefix_space=False)),
}


class Tee:
    def __init__(self, f):
        self.file = f

    def write(self, msg):
        sys.stdout.write(msg)
        self.file.write(msg)

    def flush(self):
        sys.stdout.flush()
        self.file.flush()


# ---------------------------------------------------------------------------
# One case (one mode on one input), in its own process
# ---------------------------------------------------------------------------

def parabix_cmd(args, mode: str, path: str, extra: list) -> list:
    return [args.tokenizer, *MODES[mode][1], "--thread-num=1",
            f"--segment-size={args.segment_size}", *extra, path]


def run_tokenizer(cmd: list) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_S)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} exited {r.returncode}:\n"
                           f"{r.stderr.decode(errors='replace')[-2000:]}")
    return r


def check_case(args, mode: str, path: str, text: str) -> dict:
    """Compare the complete pre-token lists. Returns the check record."""
    out = run_tokenizer(parabix_cmd(args, mode, path, [f"--pretok-separator=0x{ord(SEPARATOR):X}"])).stdout
    actual = [s for s in out.decode("utf-8").split(SEPARATOR) if s]
    expected = [s for s, _ in MODES[mode][2]().pre_tokenize_str(text)]
    check = {"match": actual == expected, "parabix_pretokens": len(actual),
             "hf_pretokens": len(expected)}
    if not check["match"]:
        i = next((i for i, (a, e) in enumerate(zip(actual, expected)) if a != e),
                 min(len(actual), len(expected)))
        check.update(first_difference=i, parabix=actual[max(0, i - 2):i + 3],
                     hf=expected[max(0, i - 2):i + 3])
    return check


def time_parabix(args, mode: str, path: str, nbytes: int) -> list:
    r = run_tokenizer(parabix_cmd(args, mode, path, [f"--bench-loop={args.iters}"]))
    stderr = r.stderr.decode(errors="replace")
    line = next((l for l in stderr.splitlines() if l.startswith("BENCH_SAMPLES_MS ")), None)
    result = next((l for l in stderr.splitlines() if l.startswith("BENCH_RESULT ")), "")
    if line is None or f"bytes={nbytes} " not in result:
        raise RuntimeError(f"no timing for {mode} on {path}:\n{stderr[-2000:]}")
    return [float(x) for x in line.split()[1].split(",")]


def time_hf(pt, text: str, iters: int) -> list:
    def call():
        pts = PreTokenizedString(text)
        pt.pre_tokenize(pts)
    call()   # untimed
    samples = []
    for _ in range(iters):
        t0 = time.perf_counter_ns()
        call()
        samples.append((time.perf_counter_ns() - t0) / 1e6)
    return samples


def run_case(args, mode: str, path: str) -> dict:
    raw = open(path, "rb").read()
    text = raw.decode("utf-8")
    global SEPARATOR
    # A noncharacter absent from this input; U+10FFFF is used unless the input contains it.
    SEPARATOR = next((c for c in ("\U0010FFFF", "\U0010FFFE", "\U0010FFFD") if c not in text), None)
    if SEPARATOR is None:
        raise RuntimeError(f"{path} contains U+10FFFD..U+10FFFF, the correctness run's separators")
    case = {"mode": mode, "input": os.path.basename(path), "path": path, "bytes": len(raw)}
    print(f"  {mode} on {case['input']}: checking", file=sys.stderr, flush=True)
    case["check"] = check_case(args, mode, path, text)
    if not case["check"]["match"]:
        return case

    pt = MODES[mode][2]()
    rounds = []
    for r in range(args.rounds):
        order = ["parabix", "hf"] if r % 2 == 0 else ["hf", "parabix"]
        samples = {}
        for side in order:
            samples[side] = (time_parabix(args, mode, path, len(raw)) if side == "parabix"
                             else time_hf(pt, text, args.iters))
        rounds.append(samples)
        print(f"  {mode} on {case['input']}: round {r + 1}/{args.rounds}  "
              f"Parabix {statistics.median(samples['parabix']):.3f} ms  "
              f"HF {statistics.median(samples['hf']):.3f} ms", file=sys.stderr, flush=True)
    case["rounds"] = rounds
    for side in ("parabix", "hf"):
        medians = [statistics.median(r[side]) for r in rounds]
        case[side] = {"median_ms": statistics.median(medians),
                      "round_min_ms": min(medians), "round_max_ms": max(medians)}
    case["hf_over_parabix"] = case["hf"]["median_ms"] / case["parabix"]["median_ms"]
    return case


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def cpu_name() -> str:
    try:
        if sys.platform == "darwin":
            return subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                  capture_output=True, text=True).stdout.strip()
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def report(out, args, cases: list) -> None:
    inputs = list(dict.fromkeys(c["input"] for c in cases))
    by_key = {(c["mode"], c["input"]): c for c in cases}
    width = max(len(MODES[m][0]) for m in args.modes)

    out.write("\nCorrectness: complete pre-token lists, Parabix vs HF pre_tokenize_str\n")
    for c in cases:
        k = c["check"]
        status = (f"MATCH     {k['hf_pretokens']:>10,} pre-tokens" if k["match"] else
                  f"MISMATCH  first difference at pre-token {k['first_difference']}: "
                  f"Parabix {k['parabix']!r} HF {k['hf']!r}")
        out.write(f"  {MODES[c['mode']][0]:<{width}}  {c['input']:<16} {status}\n")

    timed = [c for c in cases if "rounds" in c]
    if not timed:
        return
    out.write("\nSpeed ratio: HF median time / Parabix median time (above 1: Parabix faster)\n")
    out.write(f"  {'Mode':<{width}}" + "".join(f"  {i:>14}" for i in inputs) + "\n")
    for m in args.modes:
        cells = []
        for i in inputs:
            c = by_key.get((m, i))
            cells.append(f"{c['hf_over_parabix']:.2f}x" if c and "rounds" in c else "-")
        out.write(f"  {MODES[m][0]:<{width}}" + "".join(f"  {x:>14}" for x in cells) + "\n")

    out.write("\nMedian time in ms (range of the round medians)\n")
    out.write(f"  {'Mode':<{width}}  {'Input':<16} {'Parabix':>26} {'HF':>28}\n")
    for c in timed:
        p, h = c["parabix"], c["hf"]
        out.write(f"  {MODES[c['mode']][0]:<{width}}  {c['input']:<16} "
                  f"{p['median_ms']:>9.3f} ({p['round_min_ms']:.3f}-{p['round_max_ms']:.3f})"
                  f"  {h['median_ms']:>9.3f} ({h['round_min_ms']:.3f}-{h['round_max_ms']:.3f})\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", nargs="+", metavar="FILE", help="UTF-8 input files (required)")
    ap.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES), metavar="MODE",
                    help="pre-tokenizers to run (default: all): " + ", ".join(MODES))
    ap.add_argument("--rounds", type=int, default=3, help="rounds per case (default 3)")
    ap.add_argument("--iters", type=int, default=5, help="timed calls per side per round (default 5)")
    ap.add_argument("--segment-size", type=int, default=16384,
                    help="Parabix --segment-size (default 16384)")
    ap.add_argument("--tokenizer", default=TOKENIZER, help=f"tokenizer binary (default {TOKENIZER})")
    ap.add_argument("--json", metavar="PATH", help="also write every case, with all samples, as JSON")
    ap.add_argument("--case", nargs=2, metavar=("MODE", "FILE"), help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.case:   # child process: one case, JSON on stdout
        if args.case[0] not in MODES:
            ap.error(f"unknown mode {args.case[0]}")
        print(json.dumps(run_case(args, *args.case)))
        return
    if not args.input:
        ap.error("--input FILE [FILE ...] is required")
    if args.rounds < 1 or args.iters < 1:
        ap.error("--rounds and --iters must be at least 1")
    if not os.access(args.tokenizer, os.X_OK):
        sys.exit(f"tokenizer binary not found: {args.tokenizer}  (make -C build19 -j8 tokenizer)")
    for path in args.input:
        if not os.path.isfile(path):
            sys.exit(f"input not found: {path}")

    cases = []
    with open(OUTPUT_FILE, "w", encoding="utf-8") as fh:
        out = Tee(fh)
        out.write("Pre-tokenizer benchmark: Parabix vs HuggingFace tokenizers\n")
        out.write(f"  date        {datetime.now().isoformat(timespec='seconds')}\n")
        out.write(f"  machine     {cpu_name()}, {platform.platform()}\n")
        out.write(f"  python      {platform.python_version()}, tokenizers {tokenizers.__version__}\n")
        out.write(f"  tokenizer   {args.tokenizer}\n")
        out.write(f"  method      1 thread each, Parabix --segment-size={args.segment_size}; "
                  f"{args.rounds} rounds x {args.iters} timed calls per side, 1 untimed call first\n")
        out.write(f"  inputs      " + ", ".join(f"{os.path.basename(p)} ({os.path.getsize(p):,} bytes)"
                                                for p in args.input) + "\n")
        out.flush()
        for mode in args.modes:
            for path in args.input:
                cmd = [sys.executable, os.path.abspath(__file__), "--case", mode, os.path.abspath(path),
                       "--rounds", str(args.rounds), "--iters", str(args.iters),
                       "--segment-size", str(args.segment_size), "--tokenizer", args.tokenizer]
                r = subprocess.run(cmd, stdout=subprocess.PIPE, text=True)
                if r.returncode != 0:
                    sys.exit(f"case {mode} on {path} failed (exit {r.returncode})")
                cases.append(json.loads(r.stdout))
        report(out, args, cases)
        out.write("\nThese are complete pipelines with different outputs: Parabix writes serialized text, "
                  "HF keeps\nnative strings and offsets. They do not time boundary finding alone.\n")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(cases, f, indent=1)
            f.write("\n")
    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
