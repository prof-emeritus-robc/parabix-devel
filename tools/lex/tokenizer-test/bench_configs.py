#!/usr/bin/env python3
"""
Parabix-only BPE configuration benchmark — no HuggingFace involved.

compare_bpe.py answers "is Parabix CORRECT vs HF" and bench_bpe.py answers "is
Parabix FASTER than HF". Neither answers the question this script exists for:

    given N Parabix flag combinations, which one is fastest, and by how much?

Everything here is head-to-head between Parabix configs, so no `tokenizers`
import, no vocab.json, no HF at all — just build19/bin/tokenizer against itself.

Why a dedicated script instead of running bench_bpe.py --parabix-only twice
-------------------------------------------------------------------------------
1. Interleaving. Two sequential benchmark runs are minutes apart, and laptop
   clocks drift (thermal throttle, background load). This script round-robins
   the configs every round and keeps each config's MIN across rounds, so drift
   hits every config equally instead of penalising whichever ran last.
2. Floor vs marginal rate. The BPE pipeline pays a large FIXED per-call
   dispatch cost (one invocation per merge kernel — hundreds of them), which
   dominates small inputs. A single MB/s number at one size therefore mostly
   measures the floor. With --sizes, each config is timed at several input
   sizes and min_ms = floor + slope*bytes is fitted, separating
      floor_ms       = fixed per-call dispatch cost (kernel-count driven)
      marginal MB/s  = floor-free steady-state rate (per-byte work)
   A flag that cuts kernel count moves the floor; a flag that cuts per-byte
   Pablo ops moves the marginal rate. Reporting one number hides which.
3. Correctness guard. --verify re-runs each config WITHOUT --bench-loop and
   hashes the emitted token ids against the baseline config. A config that is
   faster but changes output is a regression, not a win, and gets flagged.

Usage
-----
    # default: baseline vs --level-partition on val_2MB
    python bench_configs.py

    # explicit configs, NAME=FLAGS (first one is the baseline for speedups)
    python bench_configs.py \
        --config 'baseline=' \
        --config 'level=--level-partition' \
        --config 'level+compact=--level-partition --compact-base=40'

    # floor / marginal-rate split, plus an output-identity check
    python bench_configs.py --sizes 0.25,1,4,8 --verify

    # more interleaved rounds (noisy machine), fewer in-process iterations
    python bench_configs.py --rounds 5 --iters 3

    # every config gets --pretokenizer=bytelevel by default; to time the raw-byte
    # path instead (no pretoken boundary gating, NOT what compare_bpe.py verifies)
    python bench_configs.py --common-flags=''

Output is teed to bench_configs_output.txt next to this script.
"""

import argparse
import hashlib
import os
import re
import shlex
import statistics
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

# Defaults only: the repo root is three levels above this script
# (tools/lex/tokenizer-test/); --input/--tokenizer/--merges override.
REPO_ROOT   = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER   = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
MERGES      = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")
TFILES      = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files")
DEFAULT_INPUT = os.path.join(TFILES, "val_2MB.txt")
OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "bench_configs_output.txt")

# Default comparison when the user names no configs: the current default
# partition against the level schedule.
DEFAULT_CONFIGS = [("baseline", ""), ("level-partition", "--level-partition")]

DEFAULT_ITERS  = 5
DEFAULT_ROUNDS = 3

SEP  = "=" * 78
DASH = "-" * 78

# BENCH_RESULT bytes=N iters=N min_ms=.. median_ms=.. mean_ms=.. mbps=..
_BENCH_RE = re.compile(
    r"BENCH_RESULT\s+bytes=(\d+)\s+iters=(\d+)\s+min_ms=([\d.]+)\s+"
    r"median_ms=([\d.]+)\s+mean_ms=([\d.]+)\s+mbps=([\d.]+)"
)
# [BPE] 1123 merge-range kernels (contiguous clean ranges)
_KERNELS_RE = re.compile(r"\[BPE\]\s+(\d+)\s+merge-range kernels(?:\s+\(([^)]*)\))?")


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
# Tokenizer invocation
# ---------------------------------------------------------------------------

def bench_once(path: str, iters: int, extra: list) -> dict | None:
    """One --bench-loop run: pipeline built once, then `iters` timed in-process
    runs over an mmap'd buffer. Excludes spawn, merges load, JIT and file I/O,
    so the numbers are pure tokenization. Returns None if no BENCH_RESULT."""
    cmd = [TOKENIZER, f"--merges={MERGES}", f"--bench-loop={iters}", *extra, path]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8")
    m = _BENCH_RE.search(r.stderr or "")
    if not m:
        sys.stderr.write(f"  [!] no BENCH_RESULT ({' '.join(extra) or 'baseline'}) "
                         f"on {os.path.basename(path)}\n{(r.stderr or '')[-400:]}\n")
        return None
    nbytes, n, minv, median, mean, mbps = m.groups()
    out = {"bytes": int(nbytes), "iters": int(n), "min_ms": float(minv),
           "median_ms": float(median), "mean_ms": float(mean), "mbps": float(mbps)}
    k = _KERNELS_RE.search(r.stderr or "")
    if k:
        out["kernels"] = int(k.group(1))
        out["partition"] = k.group(2) or ""
    return out


def token_digest(path: str, extra: list) -> tuple[str, int] | None:
    """Run WITHOUT --bench-loop so token ids actually print, and hash them.
    Two configs that tokenize identically must produce byte-identical stdout —
    a differing digest means the flag changed the result, not just the speed."""
    cmd = [TOKENIZER, f"--merges={MERGES}", *extra, path]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(f"  [!] verify run failed ({' '.join(extra) or 'baseline'}): "
                         f"{(r.stderr or b'')[-300:].decode(errors='replace')}\n")
        return None
    body = r.stdout
    return hashlib.sha256(body).hexdigest()[:16], body.count(b"\n")


# ---------------------------------------------------------------------------
# Size ladder
# ---------------------------------------------------------------------------

def make_prefixes(src: str, sizes_mb: list[float], tmpdir: str) -> list[tuple[int, str]]:
    """Cut increasing-size prefixes of `src`, each trimmed back to the last
    newline (a mid-line cut would tokenize a partial word and skew the fit)."""
    want = sorted(int(mb * (1 << 20)) for mb in sizes_mb)
    src_bytes = os.path.getsize(src)
    with open(src, "rb") as fh:
        blob = fh.read(min(max(want), src_bytes))
    files = []
    for sz in want:
        if sz > src_bytes:
            sys.stderr.write(f"  [skip] {sz} bytes > source ({src_bytes})\n")
            continue
        chunk = blob[:sz]
        cut = chunk.rfind(b"\n")
        if cut > 0:
            chunk = chunk[:cut + 1]
        p = os.path.join(tmpdir, f"prefix_{len(chunk)}.txt")
        with open(p, "wb") as fh:
            fh.write(chunk)
        files.append((len(chunk), p))
    return files


def linfit(xs: list[float], ys: list[float]) -> tuple[float, float] | None:
    """Least-squares y = a + b*x. Returns (a, b), or None if degenerate."""
    n = len(xs)
    if n < 2:
        return None
    mx = statistics.fmean(xs)
    my = statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    return my - b * mx, b


# ---------------------------------------------------------------------------
# Measurement
# ---------------------------------------------------------------------------

def measure(configs, files, iters, rounds, out) -> dict:
    """Interleaved measurement: every round times every (config, size) pair, and
    each pair keeps its MIN across rounds. Interleaving is the point — it makes
    thermal drift and background load hit all configs alike."""
    best: dict = {(name, nbytes): None for name, _ in configs for nbytes, _ in files}

    out.write("Warm-up (JIT + objcache fill, untimed)\n")
    for name, flags in configs:
        for nbytes, path in files:
            bench_once(path, 1, flags)
        out.write(f"  {name:<24} ready\n")
    out.write("\n")

    for rnd in range(1, rounds + 1):
        out.write(f"Round {rnd}/{rounds}\n")
        for name, flags in configs:
            for nbytes, path in files:
                r = bench_once(path, iters, flags)
                if r is None:
                    continue
                key = (name, nbytes)
                prev = best[key]
                if prev is None or r["min_ms"] < prev["min_ms"]:
                    best[key] = r
                out.write(f"  {name:<24} {nbytes/1e6:7.2f} MB  "
                          f"min {r['min_ms']:9.2f} ms  {r['mbps']:7.3f} MB/s\n")
        out.write("\n")
    return best


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def report_per_size(configs, files, best, out) -> None:
    # The speedup column is relative to the FIRST NAMED config, looked up by name.
    # Taking rows[0] instead would silently promote whichever config survived when
    # the real baseline produced no BENCH_RESULT at this size, and every "vs base"
    # figure in the table would then be against the wrong denominator, unflagged.
    base_name = configs[0][0]
    for nbytes, _ in files:
        rows = [(name, best[(name, nbytes)]) for name, _ in configs]
        rows = [(n, r) for n, r in rows if r]
        if not rows:
            continue
        base_row = best.get((base_name, nbytes))
        base_ms = base_row["min_ms"] if base_row and base_row["min_ms"] else None
        out.write(f"{SEP}\nSIZE {nbytes/1e6:.2f} MB  ({nbytes} bytes)\n{DASH}\n")
        if base_ms is None:
            out.write(f"baseline '{base_name}' produced no result at this size — "
                      f"speedups omitted\n")
        out.write(f"{'config':<24}{'kernels':>9}{'min ms':>11}{'median ms':>12}"
                  f"{'MB/s':>9}{'vs base':>10}\n")
        for name, r in rows:
            if base_ms is None or not r["min_ms"]:
                spd_txt = f"{'-':>10}"
            else:
                spd_txt = f"{base_ms / r['min_ms']:>9.2f}x"
            out.write(f"{name:<24}{r.get('kernels', '-'):>9}{r['min_ms']:>11.2f}"
                      f"{r['median_ms']:>12.2f}{r['mbps']:>9.3f}{spd_txt}\n")
        out.write(f"{SEP}\n\n")


def report_fit(configs, files, best, out) -> None:
    """Split each config's cost into the fixed per-call floor (intercept) and
    the marginal per-byte rate (slope). Needs >= 2 sizes to be meaningful."""
    if len(files) < 2:
        out.write("Floor/marginal fit skipped: needs --sizes with 2+ entries.\n\n")
        return
    out.write(f"{SEP}\nFIXED FLOOR vs MARGINAL RATE   (min_ms = floor + slope*bytes)\n{DASH}\n")
    out.write(f"{'config':<24}{'kernels':>9}{'floor ms':>11}{'marginal MB/s':>16}"
              f"{'floor @ largest':>17}\n")
    # As in report_per_size: the relative column is against the FIRST NAMED config,
    # not "the first one that happened to fit".
    base_name = configs[0][0]
    base = None
    lines = []
    for name, _ in configs:
        pts = [(nbytes, best[(name, nbytes)]) for nbytes, _ in files]
        pts = [(b, r["min_ms"]) for b, r in pts if r]
        fit = linfit([float(b) for b, _ in pts], [ms for _, ms in pts])
        if fit is None:
            continue
        floor_ms, slope = fit
        marginal = (1.0 / slope) / 1e3 if slope > 0 else 0.0   # bytes/ms -> MB/s
        big_bytes, big_ms = max(pts, key=lambda p: p[0])
        share = 100.0 * floor_ms / big_ms if big_ms else 0.0
        if name == base_name:
            base = marginal
        lines.append((name, best[(name, max(b for b, _ in pts))].get("kernels", "-"),
                      floor_ms, marginal, share))
    if base is None:
        out.write(f"baseline '{base_name}' produced no usable fit — "
                  f"relative rates omitted\n")
    for name, kernels, floor_ms, marginal, share in lines:
        rel = f"  ({marginal/base:.2f}x)" if base else ""
        out.write(f"{name:<24}{kernels:>9}{floor_ms:>11.2f}"
                  f"{marginal:>12.3f}{rel:<8}{share:>12.1f}%\n")
    out.write(f"{DASH}\n"
              "floor ms       = fixed per-call pipeline-dispatch cost (kernel-count driven)\n"
              "marginal MB/s  = floor-free steady-state rate (per-byte Pablo work)\n"
              "floor @ largest= what fraction of the largest run is still the floor\n"
              f"{SEP}\n\n")


def report_verify(configs, path, out) -> None:
    out.write(f"{SEP}\nOUTPUT IDENTITY  (no --bench-loop; token ids hashed)\n"
              f"input: {path}\n{DASH}\n")
    out.write(f"{'config':<24}{'tokens':>12}{'sha256[:16]':>20}{'vs base':>12}\n")
    base = None
    for name, flags in configs:
        d = token_digest(path, flags)
        if d is None:
            out.write(f"{name:<24}{'ERROR':>12}\n")
            continue
        digest, ntok = d
        if base is None:
            base = (digest, ntok)
            verdict = "baseline"
        else:
            verdict = "IDENTICAL" if digest == base[0] else "DIFFERS"
        out.write(f"{name:<24}{ntok:>12}{digest:>20}{verdict:>12}\n")
    out.write(f"{DASH}\nDIFFERS = that flag changed the tokenization, so its timing is not\n"
              "comparable; check correctness with compare_bpe.py before trusting a win.\n"
              f"{SEP}\n\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_config(spec: str) -> tuple[str, list]:
    """'name=--flag --flag2' -> ('name', ['--flag', '--flag2']). A bare flag
    string with no '=' is allowed and names itself."""
    if "=" in spec and not spec.startswith("--"):
        name, _, flags = spec.partition("=")
    else:
        name, flags = (spec or "baseline"), spec
    return name.strip() or "baseline", shlex.split(flags)


def main() -> None:
    global TOKENIZER, MERGES
    ap = argparse.ArgumentParser(
        description="Benchmark Parabix BPE configurations against each other (no HuggingFace).")
    ap.add_argument("--config", action="append", default=[], metavar="NAME=FLAGS",
                    help="Config to time, repeatable. The FIRST is the baseline for "
                         "speedup columns. Example: --config 'level=--level-partition'")
    ap.add_argument("--input", default=DEFAULT_INPUT,
                    help=f"Corpus (default {DEFAULT_INPUT})")
    ap.add_argument("--sizes", metavar="MB[,MB...]",
                    help="Time each config at these prefix sizes (MiB, fractions ok) "
                         "and fit floor + marginal MB/s. Example: --sizes 0.25,1,4,8")
    ap.add_argument("--iters", type=int, default=DEFAULT_ITERS,
                    help=f"In-process --bench-loop iterations (default {DEFAULT_ITERS})")
    ap.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS,
                    help=f"Interleaved rounds; per-pair min is kept (default {DEFAULT_ROUNDS})")
    ap.add_argument("--verify", action="store_true",
                    help="Also hash each config's token output against the baseline")
    ap.add_argument("--common-flags", default="--pretokenizer=bytelevel",
                    help="Flags prepended to EVERY config (default: "
                         "'--pretokenizer=bytelevel'). Without it the tokenizer runs the "
                         "raw-byte path with no pretoken boundary gating, which is both "
                         "cheaper and NOT the pipeline compare_bpe.py validates — so the "
                         "numbers would describe a config nobody checks for correctness. "
                         "Pass --common-flags='' to time that raw path deliberately.")
    ap.add_argument("--tokenizer", default=TOKENIZER,
                    help=f"Parabix tokenizer binary (default: {TOKENIZER})")
    ap.add_argument("--merges", default=MERGES,
                    help=f"merges.txt (default: {MERGES})")
    args = ap.parse_args()
    TOKENIZER, MERGES = args.tokenizer, args.merges

    if not os.path.isfile(TOKENIZER):
        sys.exit(f"No tokenizer binary at {TOKENIZER} — run: make -C build19 -j8 tokenizer")
    if not os.path.isfile(args.input):
        sys.exit(f"No input file: {args.input}")

    configs = [parse_config(c) for c in args.config] if args.config else \
              [(n, shlex.split(f)) for n, f in DEFAULT_CONFIGS]
    # Prepend --common-flags so every config shares the same pipeline shape and only
    # the flags under test differ. Applied here, after parsing, so the per-config
    # names stay clean in the report.
    common = shlex.split(args.common_flags)
    if common:
        configs = [(name, common + flags) for name, flags in configs]

    tmpdir = None
    try:
        if args.sizes:
            tmpdir = tempfile.mkdtemp(prefix="bench_configs_")
            files = make_prefixes(args.input,
                                  [float(s) for s in args.sizes.split(",")], tmpdir)
            if not files:
                sys.exit("No usable sizes — is --input smaller than every --sizes entry?")
        else:
            files = [(os.path.getsize(args.input), args.input)]

        with open(OUTPUT_FILE, "w") as fh:
            out = Tee(fh)
            out.write(f"{SEP}\nParabix BPE configuration benchmark (Parabix only)\n{DASH}\n")
            out.write(f"binary : {TOKENIZER}\nmerges : {MERGES}\ninput  : {args.input}\n")
            out.write(f"sizes  : {', '.join(f'{b/1e6:.2f} MB' for b, _ in files)}\n")
            out.write(f"iters  : {args.iters} per run, {args.rounds} interleaved rounds\n")
            out.write(f"common : {' '.join(common) or '(none)'}\n")
            for name, flags in configs:
                out.write(f"config : {name:<20} {' '.join(flags) or '(no extra flags)'}\n")
            out.write(f"{SEP}\n\n")

            best = measure(configs, files, args.iters, args.rounds, out)
            report_per_size(configs, files, best, out)
            report_fit(configs, files, best, out)
            if args.verify:
                # Verify on the SMALLEST prefix. --verify runs without --bench-loop, so
                # the tokenizer actually prints every token id: on a 44 MB corpus that is
                # ~15M lines per config, all captured in memory. It buys nothing — two
                # configs that tokenize identically do so on any input, and a divergence
                # shows up almost immediately (the compaction bug broke at token 3 of a
                # 12 KB file). Small input keeps the check cheap enough to leave on.
                vpath = min(files, key=lambda f: f[0])[1]
                report_verify(configs, vpath, out)

            out.write(f"Output written to: {OUTPUT_FILE}\n")
    finally:
        if tmpdir:
            for f in os.listdir(tmpdir):
                os.remove(os.path.join(tmpdir, f))
            os.rmdir(tmpdir)


if __name__ == "__main__":
    main()
