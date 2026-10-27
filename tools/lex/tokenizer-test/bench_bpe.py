#!/usr/bin/env python3
"""
BPE throughput benchmark: Parabix vs HuggingFace `tokenizers` vs tiktoken.

This is the PERFORMANCE counterpart to compare_bpe.py (which checks
correctness, including tiktoken as a third independent reference). It mirrors
HuggingFace's own criterion methodology (tokenizers/benches/*.rs): a fixed
corpus, N warm iterations, and the primary metric is **throughput in MB/s** —
comparable across inputs and machines — not raw milliseconds.

tiktoken is OpenAI's own Rust BPE implementation (its built-in "gpt2"
encoding) — a genuinely separate codebase from both Parabix and HF, not just
another config of HF's tokenizer, and worth benchmarking against because it is
the fastest widely-used BPE implementation available.

Fairness
--------
HuggingFace times an in-process `encode()` loop.  A naive subprocess timing
of the Parabix binary would also pay OS process spawn + merges.txt load +
Pablo pipeline build + (cold) JIT compile on every call — none of which is
tokenization.  To measure the same thing on both sides, Parabix is invoked
with `--bench-loop=N`, a flag that builds the pipeline ONCE and then runs it
N times inside the one process, suppressing token output, and prints a
`BENCH_RESULT ...` line to stderr.  We parse the best (min) iteration from
that line — the steady-state peak, exactly what criterion reports.

Usage
-----
    python bench_bpe.py                       # default corpus list (12 KB → 12 MB)
    python bench_bpe.py --input file.txt      # one file
    python bench_bpe.py --sweep               # 4 KB → 12 MB prefix sweep + line fit
    python bench_bpe.py --sweep --sweep-max-mb 1   # cheap sub-MB sweep
    python bench_bpe.py --iters 20            # timed iterations per side
    python bench_bpe.py --no-adaptive-iters   # same --iters at every size
    python bench_bpe.py --openwebtext         # add a streamed openwebtext sample
    python bench_bpe.py --sweep --openwebtext # full 4 KB → 20 MB sweep (needs the
                                             #   stream: on-disk corpora stop at 12.4 MB)
    python bench_bpe.py --openwebtext --owt-bytes 50000000  # bigger sample
    python bench_bpe.py --parabix-only
    python bench_bpe.py --hf-only
    python bench_bpe.py --tiktoken-only
    python bench_bpe.py --no-tiktoken         # Parabix vs HF only (original 2-way)
    python bench_bpe.py --svg bench.svg       # also emit a MB/s bar chart
"""

import argparse
import os
import re
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
from tokenizers import Tokenizer
import tiktoken

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --tokenizer/--merges/--tokenizer-json override.
REPO_ROOT      = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER      = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
MERGES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")
VOCAB          = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/vocab.json")
TOKENIZER_JSON = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/tokenizer.json")
TFILES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files")
OUTPUT_FILE    = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "bench_bpe_output.txt")

# Default corpus sweep — existing repo files, small → medium.
DEFAULT_SWEEP = [
    os.path.join(TFILES, "webtext_10.txt"),
    os.path.join(TFILES, "webtext_100.txt"),
    os.path.join(TFILES, "webtext_cases.txt"),
]
BIG_FILE = os.path.join(TFILES, "webtext_cases.txt")   # ~13 MB, --big only

# --sweep: cut increasing-size prefixes from a source corpus so we can separate
# Parabix's fixed per-call floor (intercept) from its true per-byte throughput
# (slope). Sizes span 4 KB → 20 MB (~3.7 orders of magnitude). The multi-MB tail
# is what makes the fit trustworthy: at 1 MB the fixed dispatch floor is still
# ~24% of min_ms, so the slope is poorly conditioned; past 8 MB it is a few
# percent, and the large-x points carry most of the leverage. Cap a run with
# --sweep-max-mb.
#
# Sizes here are BINARY MB (MiB), so the top entry needs 20,971,520 source bytes
# — keep OWT_BYTES comfortably above it or the point is silently skipped.
#
# The top sizes exceed every corpus on disk: webtext_cases.txt is 12,996,805
# bytes and already holds ALL 5000 documents of webtext.test.jsonl, so there is
# no more real text in the repo. Anything past ~12 MB needs --openwebtext, which
# streams a fresh sample (see ensure_openwebtext) and replaces the sweep source.
# Without it, sizes above the source are skipped with a reason on stderr.
SWEEP_SOURCE = BIG_FILE if os.path.isfile(BIG_FILE) else \
               os.path.join(TFILES, "webtext_100.txt")
SWEEP_SIZES  = [4 * 1024, 16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024,
                4 * 1024 * 1024, 8 * 1024 * 1024, 12 * 1024 * 1024,
                15 * 1024 * 1024, 20 * 1024 * 1024]

# --openwebtext: stream a real-world sample from Skylion007/openwebtext (HF datasets),
# cache it here, and use it as a corpus / sweep source. Streaming = no 40 GB download.
# The dataset is script-based upstream, but the Hub serves an auto-converted
# parquet revision, so this works on datasets 4.x (which dropped loading scripts).
#
# Defaults are sized to feed the largest SWEEP_SIZES entry with headroom. Note
# SWEEP_SIZES is in binary MB: the 20 MB point needs 20,971,520 bytes, so a
# 20_000_000 cap (19.07 MiB) would fall just short and get it skipped — hence
# 24 MB here.
#
# Both caps apply and the FIRST one hit wins, so OWT_DOCS must stay high enough
# not to bind before the byte cap: openwebtext averages ~7.3 KB/doc, so 24 MB
# needs ~3300 docs. A low doc cap silently yields a short sample and the big
# sweep sizes get skipped.
OWT_CACHE = os.path.join(TFILES, "openwebtext_sample.txt")
OWT_BYTES = 24_000_000      # byte cap for the sample (~22.9 MiB)
OWT_DOCS  = 100_000         # doc cap (whichever hit first)

DEFAULT_ITERS = 10

# Multi-MB inputs cost seconds per iteration, so a flat --iters would make the
# sweep run for many minutes. Above 1 MB, scale iterations down to hold roughly
# constant wall-clock per size, with a floor of 3 samples (criterion's own
# minimum). The reported statistic is min_ms, whose relative noise shrinks as
# the input grows, so fewer samples on big inputs costs almost no precision.
ITER_SCALE_THRESHOLD = 1 << 20
ITER_FLOOR           = 3

SEP  = "=" * 78
DASH = "-" * 78


def iters_for(nbytes: int, base_iters: int, adaptive: bool = True) -> int:
    """Iterations to run for an input of `nbytes` (see ITER_SCALE_THRESHOLD)."""
    if not adaptive or nbytes <= ITER_SCALE_THRESHOLD:
        return base_iters
    return max(ITER_FLOOR, int(base_iters * ITER_SCALE_THRESHOLD / nbytes))


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
# Parabix side — one subprocess, N in-process iterations via --bench-loop
# ---------------------------------------------------------------------------

_BENCH_RE = re.compile(
    r"BENCH_RESULT bytes=(\d+) iters=(\d+) min_ms=([\d.]+) "
    r"median_ms=([\d.]+) mean_ms=([\d.]+) mbps=([\d.]+)"
)


def run_parabix(path: str, iters: int, extra: list | None = None) -> dict | None:
    """Invoke the tokenizer with --bench-loop and parse its BENCH_RESULT line.

    Pipeline build + JIT happen once, before the timed loop, so the parsed
    numbers are pure tokenization (matches HF's in-process criterion loop).
    `extra` are raw tokenizer flags (e.g. --geometric-compaction) from
    --parabix-args, appended before the input path."""
    cmd = [TOKENIZER, f"--merges={MERGES}", f"--bench-loop={iters}", *(extra or []), path]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8")
    m = _BENCH_RE.search(r.stderr)
    if not m:
        sys.stderr.write(f"  [parabix] no BENCH_RESULT for {path}\n{r.stderr[-500:]}\n")
        return None
    nbytes, n, minv, median, mean, mbps = m.groups()
    return {
        "bytes":     int(nbytes),
        "iters":     int(n),
        "min_ms":    float(minv),
        "median_ms": float(median),
        "mean_ms":   float(mean),
        "mbps":      float(mbps),
    }


# ---------------------------------------------------------------------------
# HuggingFace side — in-process encode loop, min/median of N iterations
# ---------------------------------------------------------------------------

def run_hf(tok, text: str, iters: int) -> dict:
    """Time HF encode() on the whole-file string, N warm iterations + 1 discard.

    Whole-file encode mirrors how Parabix processes the input as a single byte
    stream (rather than encode_batch over pre-split lines)."""
    nbytes = len(text.encode("utf-8"))
    tok.encode(text)                       # warm-up (discarded)
    times = []
    for _ in range(iters):
        t0 = time.perf_counter()
        tok.encode(text)
        times.append((time.perf_counter() - t0) * 1e3)   # ms
    times.sort()
    minv   = times[0]
    median = statistics.median(times)
    mean   = statistics.fmean(times)
    mbps   = (nbytes / (minv / 1e3)) / 1e6 if minv > 0 else 0.0
    return {
        "bytes":     nbytes,
        "iters":     iters,
        "min_ms":    minv,
        "median_ms": median,
        "mean_ms":   mean,
        "mbps":      mbps,
    }


# ---------------------------------------------------------------------------
# tiktoken side — in-process encode loop, same shape as run_hf
# ---------------------------------------------------------------------------

def run_tiktoken(enc, text: str, iters: int) -> dict:
    """Time tiktoken's encode() on the whole-file string, same methodology as run_hf."""
    nbytes = len(text.encode("utf-8"))
    enc.encode(text)                       # warm-up (discarded)
    times = []
    for _ in range(iters):
        t0 = time.perf_counter()
        enc.encode(text)
        times.append((time.perf_counter() - t0) * 1e3)   # ms
    times.sort()
    minv   = times[0]
    median = statistics.median(times)
    mean   = statistics.fmean(times)
    mbps   = (nbytes / (minv / 1e3)) / 1e6 if minv > 0 else 0.0
    return {
        "bytes":     nbytes,
        "iters":     iters,
        "min_ms":    minv,
        "median_ms": median,
        "mean_ms":   mean,
        "mbps":      mbps,
    }


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

ENGINE_ORDER = ["Parabix", "HF", "tiktoken"]

def _fmt_bytes(n: int) -> str:
    if n >= 1 << 20:
        return f"{n / (1 << 20):.2f} MB"
    if n >= 1 << 10:
        return f"{n / (1 << 10):.1f} KB"
    return f"{n} B"


def write_row(out, name, results: dict):
    """One corpus file: results maps engine label -> result dict (or None/absent)."""
    present = {k: v for k, v in results.items() if v}
    any_r = next(iter(present.values()))
    nbytes = any_r["bytes"]
    niters = any_r["iters"]
    out.write(f"\n{name}   ({_fmt_bytes(nbytes)}, {nbytes} bytes, {niters} iters)\n")
    out.write(f"  {'engine':<10} {'min ms':>12} {'median ms':>12} "
              f"{'mean ms':>12} {'MB/s (peak)':>14}\n")
    out.write(f"  {'-'*10} {'-'*12} {'-'*12} {'-'*12} {'-'*14}\n")

    def line(label, r):
        if r is None:
            out.write(f"  {label:<10} {'—':>12} {'—':>12} {'—':>12} {'—':>14}\n")
            return
        out.write(f"  {label:<10} {r['min_ms']:>12.3f} {r['median_ms']:>12.3f} "
                  f"{r['mean_ms']:>12.3f} {r['mbps']:>14.2f}\n")

    for label in ENGINE_ORDER:
        if label in results:
            line(label, results[label])

    # Ranked throughput summary: fastest first (highest MB/s), each one's
    # slowdown vs the fastest.
    ranked = sorted(present.items(), key=lambda kv: -kv[1]["mbps"])
    if len(ranked) >= 2 and ranked[0][1]["mbps"] > 0:
        fastest_label, fastest = ranked[0]
        for label, r in ranked[1:]:
            ratio = fastest["mbps"] / r["mbps"] if r["mbps"] > 0 else float("inf")
            out.write(f"  → {fastest_label} {ratio:.2f}x faster than {label} (throughput)\n")


SVG_ENGINE_STYLE = [("Parabix", "#58a6ff", "PBX"), ("HF", "#f0883e", "HF"),
                    ("tiktoken", "#3fb950", "TIK")]


def write_svg(path, rows):
    """Grouped MB/s bar chart: one bar per present engine, per corpus file."""
    rows = [(n, res) for (n, res) in rows if any(res.values())]
    if not rows:
        return
    present_engines = [(lab, col, short) for lab, col, short in SVG_ENGINE_STYLE
                       if any(res.get(lab) for _, res in rows)]
    if not present_engines:
        return
    ROW_H, PAD, LABEL_W, BAR_MAX = 16 * len(present_engines) + 14, 20, 200, 420
    W = PAD * 2 + LABEL_W + BAR_MAX + 90
    H = PAD * 2 + 40 + len(rows) * ROW_H
    peak = max(r["mbps"] for _, res in rows for r in res.values() if r) or 1.0
    engines_txt = " vs ".join(lab for lab, _, _ in present_engines)
    L = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="ui-monospace,Menlo,monospace" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="#0d1117" rx="8"/>',
         f'<text x="{W//2}" y="26" fill="#e6edf3" font-size="15" font-weight="bold" '
         f'text-anchor="middle">BPE throughput — {engines_txt} (MB/s, peak)</text>']
    for i, (name, res) in enumerate(rows):
        y = PAD + 40 + i * ROW_H
        L.append(f'<text x="{PAD}" y="{y+14}" fill="#8b949e">{os.path.basename(name)}</text>')
        for j, (label, col, lab) in enumerate(present_engines):
            r = res.get(label)
            if not r:
                continue
            bw = int(r["mbps"] / peak * BAR_MAX)
            by = y + j * 16
            L.append(f'<rect x="{PAD+LABEL_W}" y="{by}" width="{max(bw,1)}" height="14" '
                     f'fill="{col}" rx="2"/>')
            L.append(f'<text x="{PAD+LABEL_W+bw+6}" y="{by+12}" fill="#e6edf3" '
                     f'font-size="11">{lab} {r["mbps"]:.1f}</text>')
    L.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(L))
    print(f"Wrote chart {path}")


# ---------------------------------------------------------------------------
# Marginal throughput — separate the fixed per-call floor from per-byte cost
# ---------------------------------------------------------------------------

def _linfit(xs, ys):
    """Least-squares y = a + b*x. Returns (a_intercept, b_slope, r2).

    x = bytes, y = min_ms.  a = fixed per-call floor (ms), b = ms per byte."""
    n = len(xs)
    sx, sy = sum(xs), sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    denom = n * sxx - sx * sx
    if denom == 0:
        return ys[0], 0.0, 0.0
    b = (n * sxy - sx * sy) / denom
    a = (sy - b * sx) / n
    ybar = sy / n
    ss_tot = sum((y - ybar) ** 2 for y in ys)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1 - ss_res / ss_tot if ss_tot > 0 else 1.0
    return a, b, r2


def _marginal_mbps(b_slope_ms_per_byte):
    """Slope (ms/byte) → steady-state throughput (MB/s), floor excluded."""
    if b_slope_ms_per_byte <= 0:
        return float("inf")
    return 1e-3 / b_slope_ms_per_byte          # (1 byte / (slope/1000) s) / 1e6


def write_marginal(out, rows):
    """Fit min_ms vs bytes per engine → report fixed floor + marginal MB/s.

    Peak MB/s (single-file) is contaminated by the fixed per-call pipeline
    dispatch floor. The slope isolates the real steady-state per-byte rate —
    what the SIMD data-parallelism actually delivers once the floor amortizes."""
    pts = []
    for _, res in rows:
        ok = next((r for r in res.values() if r), None)
        if ok:
            pts.append((ok["bytes"], res))
    if len({b for b, _ in pts}) < 2:
        return   # need ≥2 distinct sizes to fit a line

    out.write("\n" + SEP + "\n")
    out.write("MARGINAL THROUGHPUT  (min_ms = floor + slope·bytes; floor = fixed\n")
    out.write("per-call pipeline-dispatch cost, excluded from the per-byte rate)\n")
    out.write(SEP + "\n")

    def fit_line(label):
        xs, ys = [], []
        for b, res in pts:
            r = res.get(label)
            if r:
                xs.append(b); ys.append(r["min_ms"])
        if len(xs) < 2:
            return
        a, slope, r2 = _linfit(xs, ys)
        mbps = _marginal_mbps(slope)
        mtxt = "∞ (below noise)" if mbps == float("inf") else f"{mbps:.2f} MB/s"
        out.write(f"  {label:<9} floor {a:8.2f} ms   marginal {mtxt:<16} "
                  f"(R²={r2:.3f}, n={len(xs)})\n")

    for label in ENGINE_ORDER:
        fit_line(label)
    out.write("\n  Peak MB/s (per file above) includes the floor; marginal MB/s is\n")
    out.write("  the floor-free steady-state rate. On large inputs peak → marginal.\n")
    out.write(SEP + "\n")


# ---------------------------------------------------------------------------
# openwebtext sample — stream + cache
# ---------------------------------------------------------------------------

def ensure_openwebtext(cap_bytes: int, max_docs: int, path: str):
    """Materialize a Skylion007/openwebtext sample to `path` (cached).

    Streams the dataset (no full download); reuses the cache if it already holds
    >= cap_bytes. `datasets` is imported lazily so the benchmark still runs when
    it is not installed and --openwebtext was not requested. Returns path or None."""
    if os.path.isfile(path) and os.path.getsize(path) >= cap_bytes:
        sys.stderr.write(f"  [owt] reusing cached sample {path} "
                         f"({os.path.getsize(path)} bytes)\n")
        return path
    try:
        from datasets import load_dataset
    except ImportError:
        sys.stderr.write("  [owt] `datasets` not installed — pip install datasets\n")
        return None
    sys.stderr.write(f"  [owt] streaming openwebtext → {path} "
                     f"(cap {cap_bytes} bytes / {max_docs} docs)…\n")
    ds = load_dataset("Skylion007/openwebtext", split="train", streaming=True)
    total = ndocs = 0
    # Write to a temp path, promote only on success. The cache-validity test above
    # is "size >= cap_bytes", so a fetch interrupted partway would otherwise leave
    # a short file that a later run with a smaller --owt-bytes accepts as complete.
    tmp = path + ".partial"
    with open(tmp, "w", encoding="utf-8") as f:
        for ex in ds:
            t = ex["text"]
            if not t:
                continue
            f.write(t if t.endswith("\n") else t + "\n")
            total += len(t.encode("utf-8")) + (0 if t.endswith("\n") else 1)
            ndocs += 1
            if ndocs % 200 == 0:
                sys.stderr.write(f"  [owt] {ndocs} docs, {total / 1e6:.1f} MB\n")
                sys.stderr.flush()
            if total >= cap_bytes or ndocs >= max_docs:
                break
    os.replace(tmp, path)
    sys.stderr.write(f"  [owt] wrote {ndocs} docs, {total} bytes "
                     f"({total / 1048576:.2f} MB)\n")
    if total < cap_bytes:
        sys.stderr.write(f"  [owt] warning: short of the {cap_bytes}-byte cap — "
                         f"the {max_docs}-doc cap bound first; raise --owt-docs\n")
    return path


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    global TOKENIZER, MERGES, TOKENIZER_JSON
    ap = argparse.ArgumentParser(description="Parabix vs HuggingFace vs tiktoken BPE throughput benchmark.")
    ap.add_argument("--input", help="Single corpus file (overrides the default sweep)")
    ap.add_argument("--big", action="store_true", help="Append the ~13 MB corpus to the sweep")
    ap.add_argument("--iters", type=int, default=DEFAULT_ITERS,
                    help=f"Timed iterations per side (default {DEFAULT_ITERS})")
    ap.add_argument("--parabix-only", action="store_true")
    ap.add_argument("--hf-only", action="store_true")
    ap.add_argument("--tiktoken-only", action="store_true")
    ap.add_argument("--no-tiktoken", action="store_true",
                    help="Parabix vs HF only, skip tiktoken (original 2-way behavior)")
    ap.add_argument("--tiktoken-encoding", default="gpt2",
                    help="tiktoken encoding name (default: gpt2, same vocab as this "
                         "project's vocab.json/merges.txt)")
    ap.add_argument("--svg", metavar="PATH", help="Also write a MB/s bar chart SVG")
    ap.add_argument("--sweep", action="store_true",
                    help="Input-size sweep: cut prefixes of increasing size from a "
                         "corpus, fit min_ms vs bytes → report fixed floor + marginal MB/s")
    ap.add_argument("--sweep-max-mb", type=float, metavar="MB",
                    help="Drop sweep sizes above MB megabytes (e.g. 1 for the old "
                         "sub-MB sweep). Default: all sizes up to 20 MB.")
    ap.add_argument("--sweep-source", metavar="PATH",
                    help="Slice the sweep prefixes from PATH instead of the built-in "
                         "corpus. Needed to sweep past ~13 MB (e.g. val_1gb.txt). "
                         "Takes precedence over --openwebtext.")
    ap.add_argument("--sweep-sizes", metavar="MB[,MB...]",
                    help="Replace the default size ladder with this comma-separated "
                         "list, in MiB (fractions allowed: 0.004 = 4 KB). Example: "
                         "'1,4,16,64,256,1024' sweeps 1 MB → 1 GB.")
    ap.add_argument("--no-adaptive-iters", action="store_true",
                    help="Use --iters for every size instead of scaling iterations "
                         f"down above {ITER_SCALE_THRESHOLD >> 20} MB (floor {ITER_FLOOR})")
    ap.add_argument("--openwebtext", action="store_true",
                    help="Stream a Skylion007/openwebtext sample (cached in tokenizer_files) "
                         "and add it to the corpus, or use as the --sweep source")
    ap.add_argument("--owt-bytes", type=int, default=OWT_BYTES,
                    help=f"openwebtext sample byte cap (default {OWT_BYTES})")
    ap.add_argument("--owt-docs", type=int, default=OWT_DOCS,
                    help=f"openwebtext sample doc cap (default {OWT_DOCS})")
    ap.add_argument("--parabix-args", default="",
                    help="Extra flags forwarded verbatim to the tokenizer binary, "
                         "e.g. --parabix-args=\"--geometric-compaction --compact-base=15\"")
    ap.add_argument("--tokenizer", default=TOKENIZER,
                    help=f"Parabix tokenizer binary (default: {TOKENIZER})")
    ap.add_argument("--merges", default=MERGES,
                    help=f"merges.txt (default: {MERGES})")
    ap.add_argument("--tokenizer-json", default=TOKENIZER_JSON,
                    help=f"HuggingFace tokenizer.json (default: {TOKENIZER_JSON})")
    args = ap.parse_args()
    TOKENIZER, MERGES, TOKENIZER_JSON = args.tokenizer, args.merges, args.tokenizer_json
    only_flags = [args.parabix_only, args.hf_only, args.tiktoken_only]
    if sum(only_flags) > 1:
        print("Error: --parabix-only / --hf-only / --tiktoken-only are mutually exclusive.")
        sys.exit(1)
    if not (args.hf_only or args.tiktoken_only) and not os.path.isfile(TOKENIZER):
        print(f"Error: tokenizer binary not found: {TOKENIZER}")
        sys.exit(1)
    parabix_extra = shlex.split(args.parabix_args)

    owt_path = None
    if args.openwebtext:
        owt_path = ensure_openwebtext(args.owt_bytes, args.owt_docs, OWT_CACHE)
        if owt_path is None:
            sys.exit(1)

    tmpdir = None
    if args.sweep:
        # --sweep-source beats --openwebtext beats the built-in corpus.
        sweep_src = args.sweep_source or owt_path or SWEEP_SOURCE
        if not os.path.isfile(sweep_src):
            print(f"No sweep source corpus: {sweep_src}")
            sys.exit(1)
        sizes = SWEEP_SIZES
        if args.sweep_sizes:
            sizes = sorted(int(float(s) * (1 << 20)) for s in args.sweep_sizes.split(","))
        cap = int(args.sweep_max_mb * (1 << 20)) if args.sweep_max_mb else None
        wanted = [s for s in sizes if cap is None or s <= cap]
        # Read only as much as the largest requested size: a GB-scale sweep source
        # would otherwise be pulled into RAM in full.
        src_bytes = os.path.getsize(sweep_src)
        with open(sweep_src, "rb") as fh:
            src = fh.read(max(wanted) if wanted else 0)
        tmpdir = tempfile.mkdtemp(prefix="bench_sweep_")
        files = []
        skipped = []
        for sz in sizes:
            if cap is not None and sz > cap:
                skipped.append((sz, "--sweep-max-mb"))
                continue
            if sz > src_bytes:
                skipped.append((sz, f"source is only {src_bytes} bytes"))
                continue
            # Trim to the last newline: a mid-line cut makes the tokenizer emit junk
            # rows past end-of-input, which would corrupt the timing AND the token count.
            chunk = src[:sz]
            nl = chunk.rfind(b"\n")
            if nl >= 0:
                chunk = chunk[:nl + 1]
            fp = os.path.join(tmpdir, f"sweep_{sz}.txt")
            with open(fp, "wb") as f:
                f.write(chunk)
            files.append(fp)
        for sz, why in skipped:
            sys.stderr.write(f"  [sweep] skipping {_fmt_bytes(sz)} ({why})\n")
    elif args.input:
        files = [args.input]
    else:
        files = list(DEFAULT_SWEEP)
        if args.big:
            files.append(BIG_FILE)
        if owt_path:
            files.append(owt_path)
    # BIG_FILE is also a DEFAULT_SWEEP entry, so --big would otherwise bench the
    # same 12 MB corpus twice. Dedupe, order-preserving.
    files = list(dict.fromkeys(f for f in files if os.path.isfile(f)))
    if not files:
        print("No corpus files found.")
        sys.exit(1)

    want_parabix  = not (args.hf_only or args.tiktoken_only)
    want_hf       = not (args.parabix_only or args.tiktoken_only)
    want_tiktoken = args.tiktoken_only or not (args.parabix_only or args.hf_only or args.no_tiktoken)

    tok = None
    if want_hf:
        if not os.path.isfile(TOKENIZER_JSON):
            print(f"Error: tokenizer.json not found at {TOKENIZER_JSON}")
            sys.exit(1)
        tok = Tokenizer.from_file(TOKENIZER_JSON)

    tiktoken_enc = None
    if want_tiktoken:
        tiktoken_enc = tiktoken.get_encoding(args.tiktoken_encoding)

    rows = []
    with open(OUTPUT_FILE, "w", encoding="utf-8") as fh:
        out = Tee(fh)
        out.write(SEP + "\n")
        out.write("Parabix vs HuggingFace vs tiktoken — BPE throughput benchmark\n")
        out.write(f"Binary: {TOKENIZER}\n")
        out.write(f"Merges: {MERGES}\n")
        if tiktoken_enc is not None:
            out.write(f"tiktoken encoding: {args.tiktoken_encoding}\n")
        out.write(f"Iters:  {args.iters} (per side, warm; +1 discarded warm-up)")
        if not args.no_adaptive_iters:
            out.write(f", scaled down above {ITER_SCALE_THRESHOLD >> 20} MB "
                      f"(floor {ITER_FLOOR})")
        out.write("\n" + SEP + "\n")

        for path in files:
            it = iters_for(os.path.getsize(path), args.iters,
                           adaptive=not args.no_adaptive_iters)
            p = run_parabix(path, it, parabix_extra) if want_parabix else None
            text = None
            if want_hf or want_tiktoken:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            h = run_hf(tok, text, it) if want_hf else None
            t = run_tiktoken(tiktoken_enc, text, it) if want_tiktoken else None
            results = {"Parabix": p, "HF": h, "tiktoken": t}
            write_row(out, os.path.basename(path), results)
            rows.append((path, results))

        write_marginal(out, rows)

        out.write("\n" + DASH + "\n")
        out.write("Note: Parabix numbers come from --bench-loop (pipeline built once,\n")
        out.write("      excludes process spawn + merges load + JIT + file I/O — input\n")
        out.write("      is mmap'd once and reused, matching HF's in-memory encode).\n")
        out.write("      HF and tiktoken via preloaded in-process encode() loops (model/\n")
        out.write("      encoding load excluded). Peak MB/s is best-iteration; marginal\n")
        out.write("      MB/s removes the fixed per-call floor (see --sweep).\n")
        out.write(SEP + "\n")

    if args.svg:
        write_svg(args.svg, rows)
    if tmpdir:
        for f in os.listdir(tmpdir):
            os.unlink(os.path.join(tmpdir, f))
        os.rmdir(tmpdir)
    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()


# ===========================================================================
# RUN INSTRUCTIONS — comparing tokenizer optimisation paths
# ===========================================================================
#
# The tokenizer binary carries several BPE merge implementations at once; env
# vars pick which one runs, so one build can benchmark all of them and you can
# diff them against each other without rebuilding.
#
#   BPE_INDEXED_SHIFT=N   Convert the FIRST N merge kernels from LookAhead to
#                         IndexedShiftBack. 0 = off (LookAhead everywhere).
#                         Full GPT-2 merges = 1123 kernels, so N=1123 = all.
#                         (tools/lex/merges.txt, the truncated dev copy, = 117.)
#                         A COUNT, not a flag: N=1 proves the mechanism on a
#                         single kernel, N=1123 tests full scale. That separates
#                         "the idea is wrong" from "the idea doesn't scale" —
#                         identical symptoms, different fixes.
#   BPE_COMPACT_EVERY=K   Insert a FilterByMask between kernels, after every K
#                         merge kernels: K=40 -> after kernel 40, 80, 120 ...
#                         1120 = 28 FilterByMask points. FilterByMask drops the
#                         positions already consumed by earlier merges, so later
#                         kernels walk a shorter stream.
#                         DEFAULT 40 — i.e. FilterByMask is ON unless you pass 0.
#                         K must be >= ~25: the pipeline's dataflow analysis
#                         aborts above ~44 chained PopcountOf rate changes.
#
# The two knobs are INDEPENDENT and compose — the FilterByMask schedule is the
# same whether a kernel uses LookAhead or IndexedShiftBack. Verified on the dev
# merges: all four combinations give byte-identical output.
#
# THE COMPARISON YOU NORMALLY WANT — LookAhead vs IndexedShiftBack, with
# FilterByMask left at its default 40 on both sides so only the lookahead
# mechanism changes. FilterByMask@40 is the DEFAULT, so the first run needs no
# env var at all:
#
#   # LookAhead + FilterByMask@40
#   python3 bench_bpe.py --sweep \
#       --sweep-source ~/parabix-devel/tools/lex/tokenizer_files/val_1gb.txt \
#       --sweep-sizes 1,4,16,64
#
#   # IndexedShiftBack + FilterByMask@40
#   BPE_INDEXED_SHIFT=1123 python3 bench_bpe.py --sweep \
#       --sweep-source ~/parabix-devel/tools/lex/tokenizer_files/val_1gb.txt \
#       --sweep-sizes 1,4,16,64
#
# BPE_COMPACT_EVERY=0 is a measurement tool, not a production setting — it is
# only needed to quantify what FilterByMask itself contributed (that is where
# the 2.0x below came from):
#
#   # neither optimisation
#   BPE_INDEXED_SHIFT=0 BPE_COMPACT_EVERY=0 python3 bench_bpe.py --sweep \
#       --sweep-source ~/parabix-devel/tools/lex/tokenizer_files/val_1gb.txt \
#       --sweep-sizes 1,4,16,64
#
#   # IndexedShiftBack alone
#   BPE_INDEXED_SHIFT=1123 BPE_COMPACT_EVERY=0 python3 bench_bpe.py --sweep \
#       --sweep-source ~/parabix-devel/tools/lex/tokenizer_files/val_1gb.txt \
#       --sweep-sizes 1,4,16,64
#
# Confirm which path actually ran — read stderr, not what you typed:
#   [BPE] BPE_INDEXED_SHIFT: first 1123 kernels via IndexedShiftBack (1 shift/kernel)
#   [BPE] compaction: BPE_COMPACT_EVERY=40 -> 28 FilterByMask points
# Both lines together = config 4. Missing line = that optimisation is off
# (BPE_COMPACT_EVERY=0 still prints, reporting 0 FilterByMask points).
#
# Compare the MARGINAL MB/s from --sweep, not peak MB/s. IndexedShiftBack adds
# one kernel per merge kernel (1123 -> 2246 stages), so it is expected to raise
# the fixed per-call dispatch floor while improving the per-byte rate. A single
# --input point blends the two and can hide both moving in opposite directions.
#
# ---------------------------------------------------------------------------
# PITFALLS (each of these produced hours of wrong numbers)
# ---------------------------------------------------------------------------
#
# 1. WIPE THE OBJECT CACHE before any differential run:
#        rm -rf ~/.parabix/objcache/
#    Kernel bodies are data-dependent and the cache is keyed by name. A stale
#    entry silently serves a kernel compiled for a DIFFERENT setting, which
#    fakes both passes and failures. Observed: identical output "proving" a
#    change worked when it was running the other path's cached kernels.
#
# 2. RUN ONE AT A TIME. All runs share ~/.parabix/objcache/, and two runs with
#    different merges files will cross-contaminate. Observed: out-of-vocab ids
#    (57391 in a 2255-id vocab) from a full-merges kernel body executing inside
#    a dev-merges pipeline. Check for orphans after Ctrl-C:
#        pgrep -fl "bin/tokenizer"
#    Concurrency also wrecks timing — two adjacent settings differed 2x purely
#    from a co-running job.
#
# 3. RUN EACH CONFIG TWICE, use the second. The first pays a cold JIT of 1123
#    kernels (2246 with IndexedShiftBack) at every size.
#
# 4. SANITY-CHECK IDS against the vocab ceiling (255 + number of merges). An
#    out-of-range id means a foreign kernel body, not a merge bug.
#
# 5. CORRECTNESS BEFORE SPEED. A faster wrong answer is worth nothing:
#        BPE_INDEXED_SHIFT=1123 python3 compare_offsets.py --limit 20000
#    Wants RESULT: FULL MATCH, 0 id and 0 offset mismatches.
#
# 6. HF LOADS THE WHOLE INPUT INTO PYTHON MEMORY and encodes in-process, so a
#    1 GB head-to-head means ~2 GB resident and minutes per iteration. Keep
#    HF comparisons at <= 64 MB; use --parabix-only for GB-scale points.
#
# ---------------------------------------------------------------------------
# VERIFICATION STATUS (2026-08-06)
# ---------------------------------------------------------------------------
#   FilterByMask K=30    FULL MATCH vs HF; 2.0x faster at 1 MB (1599 -> 791 ms)
#   IndexedShiftBack     dev merges only (117 kernels, 262 KB): N=1,2,5,20,117
#                        all byte-identical to the LookAhead baseline, no abort.
#                        All 4 IndexedShiftBack x FilterByMask combos identical too.
#                        Full GPT-2 merges NOT yet run clean; NO timing data.
# ===========================================================================
