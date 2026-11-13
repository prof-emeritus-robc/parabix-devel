#!/usr/bin/env python3
"""
BPE throughput benchmark: Parabix vs HuggingFace tokenizers 1.0 vs tiktoken 0.14.0.

Same benchmark as bench_bpe.py (same flags, same corpora handling, same report),
but the HuggingFace side is the tokenizers 1.0 release candidate (1.0.0rc4, the
newest on PyPI): its "bitcannon" bitstream pre-tokenizer, word cache and
whole-word fold. tiktoken is pinned to 0.14.0, the version used for every thesis
measurement. compare_bpe_hf1.py is the matching correctness check.

tokenizers 1.0 and 0.23 share the package name, so 1.0 goes in a side folder
of the same venv, hf1-site, and this script puts that folder first on sys.path
before it imports tokenizers. bench_bpe.py, in the same venv, still gets 0.23.3:

    .venv/bin/pip install --no-deps --target .venv/hf1-site "tokenizers==1.0.0rc4"
    .venv/bin/python bench_bpe_hf1.py --input FILE [FILE ...]

The folder is <venv>/hf1-site by default, or $HF1_SITE if set. tokenizers 1.0
needs numpy and huggingface-hub in the venv.

How HF 1.0 differs from 0.23, and what this script does about it
-----------------------------------------------------------------
* Tokenizer.from_file() only loads; compile() builds the CompiledTokenizer that
  encodes. Both are one-time setup, excluded from timing.
* Caches. A CompiledTokenizer keeps a word cache, so repeated encodes of the same
  text hit it for every repeated word. When Parabix runs with --bench-cold-cache,
  HF 1.0 is cold too: a fresh compile() (not timed) before every timed run.
  --hf-cache=warm|cold overrides that match.
* Threads. HF 1.0's encode_batch runs on its own pool with one thread per core,
  and ignores RAYON_NUM_THREADS and TOKENIZERS_PARALLELISM (the pool size is set
  only from Rust; checked in tk-encode/src/utils/parallelism.rs, rc4). One encode()
  of plain text is serial: it splits a text only at special tokens. encode()
  releases the GIL, so with --baseline-threads N > 1 this script runs N Python
  threads, each encoding pieces of the text with encode(); the process CPU time
  is checked against N. --hf-native-batch times HF's own encode_batch instead
  (all cores, not an equal-threads comparison).

Usage
-----
    python bench_bpe_hf1.py --input file.txt      # one file
    python bench_bpe_hf1.py --input a.txt b.txt   # several files, one row each
    python bench_bpe_hf1.py --sweep --sweep-source file.txt    # 4 KB → 20 MB prefix sweep + line fit
    python bench_bpe_hf1.py --input FILE --iters 20            # timed iterations per side
    python bench_bpe_hf1.py --input FILE --no-adaptive-iters   # same --iters at every size
    python bench_bpe_hf1.py --input FILE --parabix-only
    python bench_bpe_hf1.py --input FILE --hf-only
    python bench_bpe_hf1.py --input FILE --tiktoken-only
    python bench_bpe_hf1.py --input FILE --no-tiktoken         # Parabix vs HF 1.0 only
    python bench_bpe_hf1.py --input FILE --baseline-threads 4 --parabix-args="... --thread-num=4"
                                              # equal threads: HF 1.0 / tiktoken on 4 threads
    python bench_bpe_hf1.py --input FILE --parabix-args="... --bench-cold-cache"
                                              # cold: HF 1.0 recompiled before every run
    python bench_bpe_hf1.py --input FILE --parabix-iters 50    # more Parabix samples
    python bench_bpe_hf1.py --input FILE --svg bench.svg       # also emit a MB/s bar chart
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
from concurrent.futures import ThreadPoolExecutor
from functools import partial

# tokenizers 1.0 from the venv's side folder (see the docstring), ahead of 0.23.
HF1_SITE = os.environ.get("HF1_SITE", os.path.join(sys.prefix, "hf1-site"))
if os.path.isdir(HF1_SITE):
    sys.path.insert(0, HF1_SITE)

import tokenizers
from tokenizers import Tokenizer
import tiktoken

# ---------------------------------------------------------------------------
# Versions
# ---------------------------------------------------------------------------

TIKTOKEN_VERSION = "0.14.0"          # the thesis reference version


def check_versions(allow_other_tiktoken: bool) -> None:
    """tokenizers must be 1.x (0.x has no compile() and would time the old
    encoder); tiktoken must be the thesis version unless overridden."""
    if tokenizers.__version__.split(".")[0] != "1":
        print(f"Error: found tokenizers {tokenizers.__version__}, and this script needs 1.0. "
              f"Install it with: {sys.executable} -m pip install --no-deps --target "
              f"{HF1_SITE} \"tokenizers==1.0.0rc4\". For tokenizers 0.x use bench_bpe.py.")
        sys.exit(1)
    if tiktoken.__version__ != TIKTOKEN_VERSION and not allow_other_tiktoken:
        print(f"Error: tiktoken {tiktoken.__version__} is installed; the thesis used "
              f"{TIKTOKEN_VERSION} (pip install \"tiktoken=={TIKTOKEN_VERSION}\"), "
              f"or pass --any-tiktoken.")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --tokenizer/--merges/--tokenizer-json override.
# Corpora have no default: they are always named on the command line.
REPO_ROOT      = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER      = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
MERGES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")
TOKENIZER_JSON = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/tokenizer.json")
TFILES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files")
OUTPUT_FILE    = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "bench_bpe_hf1_output.txt")

# --sweep: prefixes of increasing size cut from a source corpus, to separate the
# fixed per-call floor (intercept) from per-byte throughput (slope). Binary MB.
SWEEP_SIZES  = [4 * 1024, 16 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024,
                4 * 1024 * 1024, 8 * 1024 * 1024, 12 * 1024 * 1024,
                15 * 1024 * 1024, 20 * 1024 * 1024]

# --openwebtext: stream a Skylion007/openwebtext sample (HF datasets) and cache it
# here, shared with bench_bpe.py. See bench_bpe.py for why the caps are sized so.
OWT_CACHE = os.path.join(TFILES, "openwebtext_sample.txt")
OWT_BYTES = 24_000_000      # byte cap for the sample (~22.9 MiB)
OWT_DOCS  = 100_000         # doc cap (whichever hit first)

DEFAULT_ITERS = 10

# Above 1 MB, scale iterations down to hold roughly constant wall-clock per size,
# with a floor of 3 samples (criterion's own minimum). --no-adaptive-iters turns it off.
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
    Pipeline build + JIT happen once, before the timed loop."""
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
# --baseline-threads: split the text so HF / tiktoken can use N threads
# ---------------------------------------------------------------------------

# Cut only between a non-space character and a following space (same rule as
# bench_bpe.py: the GPT-2 pretoken before the space ends there, and the piece
# before the cut ends in a non-space). Each engine still checks once that the
# pieces' ids, joined, equal one whole-text encode.
_SAFE_CUT = re.compile(r"(?<=\S)(?= )")


def split_for_threads(text: str, pieces: int) -> list[str]:
    """Cut `text` into about `pieces` chunks at safe pretoken boundaries."""
    cuts, step = [0], max(1, len(text) // pieces)
    for i in range(1, pieces):
        m = _SAFE_CUT.search(text, max(cuts[-1] + 1, i * step))
        if not m:
            break
        cuts.append(m.start())
    cuts.append(len(text))
    return [text[a:b] for a, b in zip(cuts, cuts[1:]) if b > a]


def _summary(times: list[float], nbytes: int) -> dict:
    """min/median/mean ms + peak MB/s of a list of per-run times (ms)."""
    times = sorted(times)
    minv  = times[0]
    return {
        "bytes":     nbytes,
        "iters":     len(times),
        "min_ms":    minv,
        "median_ms": statistics.median(times),
        "mean_ms":   statistics.fmean(times),
        "mbps":      (nbytes / (minv / 1e3)) / 1e6 if minv > 0 else 0.0,
    }


def _timed(fn, nbytes: int, iters: int) -> dict:
    """Run fn() `iters` times (already warmed up)."""
    times = []
    for _ in range(iters):
        t0 = time.perf_counter()
        fn()
        times.append((time.perf_counter() - t0) * 1e3)   # ms
    return _summary(times, nbytes)


# ---------------------------------------------------------------------------
# HuggingFace 1.0 side — in-process encode loop on a CompiledTokenizer
# ---------------------------------------------------------------------------

def run_hf(hf, text: str, iters: int, chunks: list[str] | None = None,
           threads: int = 1, cold: bool = False, native_batch: bool = False) -> dict | None:
    """Time HF 1.0 on the whole-file string, N iterations + 1 discarded warm-up.

    hf = (loaded Tokenizer, CompiledTokenizer). cold: compile a fresh
    CompiledTokenizer before every timed run (outside the timer), so no run sees
    an earlier run's word cache. With `chunks`: N Python threads each encode()
    pieces (encode releases the GIL and is serial on plain text), or with
    native_batch HF's own encode_batch on all cores. Either is first checked to
    give the same ids as one encode() of the whole text."""
    loaded, compiled = hf
    nbytes = len(text.encode("utf-8"))
    pool = None
    if chunks and native_batch:
        run = lambda c: c.encode_batch(chunks)
    elif chunks:
        pool = ThreadPoolExecutor(max_workers=threads)
        run = lambda c: list(pool.map(c.encode, chunks))
    else:
        run = lambda c: c.encode(text)
    try:
        if chunks:
            joined = [i for e in run(compiled) for i in e.ids]          # also the warm-up
            if joined != compiled.encode(text).ids:
                sys.stderr.write("  [HF 1.0] split encode differs from one encode: not timed\n")
                return None
        else:
            run(compiled)                                               # warm-up (discarded)
        times, cpu_ratios = [], []
        for _ in range(iters):
            c = loaded.compile() if cold else compiled
            c0 = time.process_time()
            t0 = time.perf_counter()
            run(c)
            wall = time.perf_counter() - t0
            cpu_ratios.append((time.process_time() - c0) / wall if wall > 0 else 0.0)
            times.append(wall * 1e3)
    finally:
        if pool:
            pool.shutdown()
    r = _summary(times, nbytes)
    r["cpu_x"] = statistics.median(cpu_ratios)        # cores busy on average
    if not native_batch and r["cpu_x"] > threads + 0.5:
        sys.stderr.write(f"  [HF 1.0] warning: used {r['cpu_x']:.1f} cores on average, "
                         f"more than the {threads} asked for (special tokens in the text?)\n")
    return r


# ---------------------------------------------------------------------------
# tiktoken side — in-process encode loop, same shape as run_hf
# ---------------------------------------------------------------------------

def run_tiktoken(enc, text: str, iters: int, chunks: list[str] | None = None,
                 threads: int = 1, ordinary: bool = False) -> dict | None:
    """Time tiktoken's encode() on the whole-file string, same methodology as run_hf.
    With `chunks`, encode_batch over them on `threads` threads. `ordinary`: the
    encode_ordinary forms, which skip the scan of the text for special tokens.
    tiktoken has no cache, so warm and cold are the same."""
    nbytes = len(text.encode("utf-8"))
    # <|endoftext|> is the special token, as in HF and Parabix (the default raises on it)
    one   = enc.encode_ordinary if ordinary else partial(enc.encode, allowed_special="all")
    batch = enc.encode_ordinary_batch if ordinary else partial(enc.encode_batch, allowed_special="all")
    if chunks:
        joined = [i for ids in batch(chunks, num_threads=threads) for i in ids]
        if joined != one(text):
            sys.stderr.write("  [tiktoken] split encode differs from one encode: not timed\n")
            return None
        return _timed(lambda: batch(chunks, num_threads=threads), nbytes, iters)
    one(text)                              # warm-up (discarded)
    return _timed(lambda: one(text), nbytes, iters)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

ENGINE_ORDER = ["Parabix", "HF 1.0", "tiktoken"]

def _fmt_bytes(n: int) -> str:
    if n >= 1 << 20:
        return f"{n / (1 << 20):.2f} MB"
    if n >= 1 << 10:
        return f"{n / (1 << 10):.1f} KB"
    return f"{n} B"


def write_row(out, name, results: dict):
    """One corpus file: results maps engine label -> result dict (or None/absent)."""
    present = {k: v for k, v in results.items() if v}
    if not present:
        out.write(f"\n{name}   (no engine produced a result)\n")
        return
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
        cpu = f"   (CPU/wall {r['cpu_x']:.1f})" if "cpu_x" in r else ""
        out.write(f"  {label:<10} {r['min_ms']:>12.3f} {r['median_ms']:>12.3f} "
                  f"{r['mean_ms']:>12.3f} {r['mbps']:>14.2f}{cpu}\n")

    for label in ENGINE_ORDER:
        if label in results:
            line(label, results[label])

    # Ranked summary from medians: fastest first, each one's slowdown vs the fastest.
    ranked = sorted(present.items(), key=lambda kv: kv[1]["median_ms"])
    if len(ranked) >= 2 and ranked[0][1]["median_ms"] > 0:
        fastest_label, fastest = ranked[0]
        for label, r in ranked[1:]:
            ratio = r["median_ms"] / fastest["median_ms"]
            out.write(f"  → {fastest_label} {ratio:.2f}x faster than {label} (median)\n")


SVG_ENGINE_STYLE = [("Parabix", "#58a6ff", "PBX"), ("HF 1.0", "#f0883e", "HF1"),
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
    return 1e-3 / b_slope_ms_per_byte


def write_marginal(out, rows):
    """Fit min_ms vs bytes per engine → report fixed floor + marginal MB/s."""
    pts = []
    for _, res in rows:
        ok = next((r for r in res.values() if r), None)
        if ok:
            pts.append((ok["bytes"], res))
    sizes = [b for b, _ in pts]
    if len(set(sizes)) < 2 or max(sizes) < 2 * min(sizes):
        return   # a line through inputs of about the same size says nothing

    out.write("\n" + SEP + "\n")
    out.write("MARGINAL THROUGHPUT  (min_ms = floor + slope·bytes; floor = fixed\n")
    out.write("per-call cost, excluded from the per-byte rate)\n")
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
    """Materialize a Skylion007/openwebtext sample to `path` (cached). `datasets`
    is imported lazily. Returns path or None."""
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
    # Write to a temp path, promote only on success, so an interrupted fetch is
    # never mistaken for a complete cache.
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
    ap = argparse.ArgumentParser(
        description="Parabix vs HuggingFace tokenizers 1.0 vs tiktoken 0.14.0 BPE throughput benchmark.")
    ap.add_argument("--input", nargs="+", metavar="FILE",
                    help="Corpus file(s) to bench, one row each. Required unless --sweep "
                         "or --openwebtext gives the input.")
    ap.add_argument("--iters", type=int, default=DEFAULT_ITERS,
                    help=f"Timed iterations per side (default {DEFAULT_ITERS})")
    ap.add_argument("--parabix-only", action="store_true")
    ap.add_argument("--hf-only", action="store_true")
    ap.add_argument("--tiktoken-only", action="store_true")
    ap.add_argument("--no-tiktoken", action="store_true",
                    help="Parabix vs HF 1.0 only, skip tiktoken")
    ap.add_argument("--tiktoken-encoding", default="gpt2",
                    help="tiktoken encoding name (default: gpt2, same vocab as this "
                         "project's vocab.json/merges.txt)")
    ap.add_argument("--any-tiktoken", action="store_true",
                    help=f"Allow a tiktoken version other than {TIKTOKEN_VERSION}")
    ap.add_argument("--svg", metavar="PATH", help="Also write a MB/s bar chart SVG")
    ap.add_argument("--sweep", action="store_true",
                    help="Input-size sweep: cut prefixes of increasing size from a "
                         "corpus, fit min_ms vs bytes → report fixed floor + marginal MB/s")
    ap.add_argument("--sweep-max-mb", type=float, metavar="MB",
                    help="Drop sweep sizes above MB megabytes. Default: all sizes up to 20 MB.")
    ap.add_argument("--sweep-source", metavar="PATH",
                    help="Slice the sweep prefixes from PATH. --sweep needs this or "
                         "--openwebtext; this takes precedence over --openwebtext.")
    ap.add_argument("--sweep-sizes", metavar="MB[,MB...]",
                    help="Replace the default size ladder with this comma-separated "
                         "list, in MiB (fractions allowed: 0.004 = 4 KB).")
    ap.add_argument("--no-adaptive-iters", action="store_true",
                    help="Use --iters for every size instead of scaling iterations "
                         f"down above {ITER_SCALE_THRESHOLD >> 20} MB (floor {ITER_FLOOR})")
    ap.add_argument("--openwebtext", action="store_true",
                    help="Stream a Skylion007/openwebtext sample (cached in tokenizer_files) "
                         "and bench it after any --input files, or use it as the --sweep source")
    ap.add_argument("--owt-bytes", type=int, default=OWT_BYTES,
                    help=f"openwebtext sample byte cap (default {OWT_BYTES})")
    ap.add_argument("--owt-docs", type=int, default=OWT_DOCS,
                    help=f"openwebtext sample doc cap (default {OWT_DOCS})")
    ap.add_argument("--parabix-args", default="",
                    help="Extra flags forwarded verbatim to the tokenizer binary. "
                         "--pretokenizer=bytelevel is added unless a --pretokenizer is given.")
    ap.add_argument("--parabix-iters", type=int, metavar="N",
                    help="Parabix --bench-loop iterations (default: same as HF/tiktoken).")
    ap.add_argument("--baseline-threads", type=int, default=1, metavar="N",
                    help="Threads for HF 1.0 and tiktoken (default 1: one encode() of the "
                         "whole text). N > 1 splits the text at safe pretoken boundaries "
                         "and encodes the pieces on N threads; match Parabix's --thread-num "
                         "for an equal-threads comparison.")
    ap.add_argument("--hf-native-batch", action="store_true",
                    help="With --baseline-threads > 1, time HF 1.0's own encode_batch, "
                         "which uses every core whatever N is")
    ap.add_argument("--hf-cache", choices=["match", "warm", "cold"], default="match",
                    help="HF 1.0 word cache between timed runs: 'match' (default) = cold "
                         "when --parabix-args has --bench-cold-cache, else warm; 'cold' = "
                         "fresh compile() before every run (not timed)")
    ap.add_argument("--tiktoken-ordinary", action="store_true",
                    help="Time tiktoken's encode_ordinary(_batch), which skips the "
                         "special-token scan, instead of encode(_batch)")
    ap.add_argument("--tokenizer", default=TOKENIZER,
                    help=f"Parabix tokenizer binary (default: {TOKENIZER})")
    ap.add_argument("--merges", default=MERGES,
                    help=f"merges.txt (default: {MERGES})")
    ap.add_argument("--tokenizer-json", default=TOKENIZER_JSON,
                    help=f"HuggingFace tokenizer.json (default: {TOKENIZER_JSON})")
    args = ap.parse_args()
    check_versions(args.any_tiktoken)
    TOKENIZER, MERGES, TOKENIZER_JSON = args.tokenizer, args.merges, args.tokenizer_json
    if args.sweep and not (args.sweep_source or args.openwebtext):
        ap.error("--sweep needs --sweep-source FILE (or --openwebtext)")
    if not (args.input or args.sweep or args.openwebtext):
        ap.error("no input: give --input FILE [FILE ...], --sweep --sweep-source FILE, "
                 "or --openwebtext")
    only_flags = [args.parabix_only, args.hf_only, args.tiktoken_only]
    if sum(only_flags) > 1:
        print("Error: --parabix-only / --hf-only / --tiktoken-only are mutually exclusive.")
        sys.exit(1)
    if not (args.hf_only or args.tiktoken_only) and not os.path.isfile(TOKENIZER):
        print(f"Error: tokenizer binary not found: {TOKENIZER}")
        sys.exit(1)
    parabix_extra = shlex.split(args.parabix_args)
    # Time the pipeline compare_bpe_hf1.py validates.
    if not any(a.startswith("--pretokenizer") for a in parabix_extra):
        parabix_extra.insert(0, "--pretokenizer=bytelevel")
    parabix_threads = next((a.split("=", 1)[1] for a in parabix_extra
                            if a.startswith("--thread-num=")), "default")
    parabix_cold = "--bench-cold-cache" in parabix_extra
    hf_cold = parabix_cold if args.hf_cache == "match" else args.hf_cache == "cold"

    owt_path = None
    if args.openwebtext:
        owt_path = ensure_openwebtext(args.owt_bytes, args.owt_docs, OWT_CACHE)
        if owt_path is None:
            sys.exit(1)

    tmpdir = None
    if args.sweep:
        sweep_src = args.sweep_source or owt_path
        if not os.path.isfile(sweep_src):
            print(f"No sweep source corpus: {sweep_src}")
            sys.exit(1)
        sizes = SWEEP_SIZES
        if args.sweep_sizes:
            sizes = sorted(int(float(s) * (1 << 20)) for s in args.sweep_sizes.split(","))
        cap = int(args.sweep_max_mb * (1 << 20)) if args.sweep_max_mb else None
        wanted = [s for s in sizes if cap is None or s <= cap]
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
            # Trim to the last newline: a mid-line cut corrupts timing and token count.
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
    else:
        files = list(args.input or [])
        if owt_path:
            files.append(owt_path)
        missing = [f for f in files if not os.path.isfile(f)]
        if missing:
            print(f"Error: input file not found: {', '.join(missing)}")
            sys.exit(1)
        files = list(dict.fromkeys(files))   # a file named twice is benched once

    want_parabix  = not (args.hf_only or args.tiktoken_only)
    want_hf       = not (args.parabix_only or args.tiktoken_only)
    want_tiktoken = args.tiktoken_only or not (args.parabix_only or args.hf_only or args.no_tiktoken)

    hf = None
    if want_hf:
        if not os.path.isfile(TOKENIZER_JSON):
            print(f"Error: tokenizer.json not found at {TOKENIZER_JSON}")
            sys.exit(1)
        loaded = Tokenizer.from_file(TOKENIZER_JSON)
        hf = (loaded, loaded.compile())

    tiktoken_enc = None
    if want_tiktoken:
        tiktoken_enc = tiktoken.get_encoding(args.tiktoken_encoding)

    rows = []
    with open(OUTPUT_FILE, "w", encoding="utf-8") as fh:
        out = Tee(fh)
        out.write(SEP + "\n")
        out.write("Parabix vs HuggingFace tokenizers 1.0 vs tiktoken — BPE throughput benchmark\n")
        out.write(f"Binary: {TOKENIZER}\n")
        out.write(f"Merges: {MERGES}\n")
        if hf is not None:
            out.write(f"HuggingFace tokenizers: {tokenizers.__version__} (compiled), "
                      f"word cache {'cold (fresh compile per run)' if hf_cold else 'warm'}\n")
        if tiktoken_enc is not None:
            out.write(f"tiktoken: {tiktoken.__version__}, encoding {args.tiktoken_encoding}\n")
        out.write(f"Python: {sys.version.split()[0]}\n")
        out.write(f"Iters:  {args.iters} (per side; +1 discarded warm-up)")
        if not args.no_adaptive_iters:
            out.write(f", scaled down above {ITER_SCALE_THRESHOLD >> 20} MB "
                      f"(floor {ITER_FLOOR})")
        if args.parabix_iters:
            out.write(f"; Parabix {args.parabix_iters}")
        out.write(f"\nCaches: Parabix {'cold (--bench-cold-cache)' if parabix_cold else 'warm'}")
        out.write(f"\nThreads: Parabix --thread-num={parabix_threads}, "
                  f"HF 1.0/tiktoken {args.baseline_threads}")
        if args.baseline_threads > 1:
            out.write(" (text split at safe pretoken boundaries)")
            if args.hf_native_batch:
                out.write(f"\n         HF 1.0 via its own encode_batch: all {os.cpu_count()} cores, "
                          f"NOT equal threads")
        out.write("\n" + SEP + "\n")

        for path in files:
            it = iters_for(os.path.getsize(path), args.iters,
                           adaptive=not args.no_adaptive_iters)
            p = run_parabix(path, args.parabix_iters or it, parabix_extra) if want_parabix else None
            text = chunks = None
            if want_hf or want_tiktoken:
                # newline="": keep "\r\n" as it is in the file, so every engine reads
                # the same bytes as Parabix.
                with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
                    text = f.read()
                if args.baseline_threads > 1:
                    chunks = split_for_threads(text, 4 * args.baseline_threads)
            h = run_hf(hf, text, it, chunks, args.baseline_threads, hf_cold,
                       args.hf_native_batch) if want_hf else None
            t = run_tiktoken(tiktoken_enc, text, it, chunks, args.baseline_threads,
                             args.tiktoken_ordinary) if want_tiktoken else None
            results = {"Parabix": p, "HF 1.0": h, "tiktoken": t}
            sizes = {r["bytes"] for r in results.values() if r}
            if len(sizes) > 1:
                sys.stderr.write(f"  warning: engines read different byte counts {sorted(sizes)} "
                                 f"for {path} (invalid UTF-8?)\n")
            write_row(out, os.path.basename(path), results)
            rows.append((path, results))

        write_marginal(out, rows)

        out.write("\n" + DASH + "\n")
        out.write("Note: Parabix numbers come from --bench-loop (pipeline built once;\n")
        out.write("      excludes process spawn + merges load + JIT). HF 1.0 and tiktoken\n")
        out.write("      via preloaded in-process encode() loops (load + compile excluded).\n")
        out.write("      Cold: HF 1.0 recompiled before every run (not timed), Parabix\n")
        out.write("      --bench-cold-cache; tiktoken has no cache. CPU/wall = cores HF 1.0\n")
        out.write("      kept busy on average. Speedups are from medians; peak MB/s from\n")
        out.write("      the best run. All engines read the same bytes (CRLF kept).\n")
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
