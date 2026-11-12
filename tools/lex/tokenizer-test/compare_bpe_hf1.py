#!/usr/bin/env python3
"""
BPE tokenizer test suite against HuggingFace tokenizers 1.0 (bitcannon).

Same checks and timing as compare_bpe.py, but the HuggingFace side is the
tokenizers 1.0 release candidate (1.0.0rc4, the newest on PyPI): its
"bitcannon" bitstream pre-tokenizer, per-thread word cache and whole-word fold.
tiktoken is pinned to 0.14.0, the version used for every thesis measurement.

tokenizers 1.0 and 0.23 share the package name, so 1.0 goes in a side folder
of the same venv, hf1-site, and this script puts that folder first on sys.path
before it imports tokenizers. compare_bpe.py, in the same venv, still gets 0.23.3:

    .venv/bin/pip install --no-deps --target .venv/hf1-site "tokenizers==1.0.0rc4"
    .venv/bin/python compare_bpe_hf1.py --input FILE

The folder is <venv>/hf1-site by default, or $HF1_SITE if set. tokenizers 1.0
needs numpy and huggingface-hub in the venv.

API differences from 0.23 handled here:
  * Tokenizer.from_file(...) only loads; .compile() builds the CompiledTokenizer
    that encodes. One-time setup, excluded from timing like from_file() is.
  * Encoding has ids but no tokens; token strings come from id_to_token(),
    outside any timed region.

Usage (--input is required: no input file is built into the script):
    python compare_bpe_hf1.py --input path/to/cases.txt
    python compare_bpe_hf1.py --input FILE --verbose         # full token-by-token table always
    python compare_bpe_hf1.py --input FILE --parabix-only    # skip HF + tiktoken
    python compare_bpe_hf1.py --input FILE --hf-only         # skip Parabix + tiktoken
    python compare_bpe_hf1.py --input FILE --tiktoken-only   # skip Parabix + HF
    python compare_bpe_hf1.py --input FILE --no-tiktoken     # Parabix vs HF 1.0 only
    python compare_bpe_hf1.py --input FILE --runs 10         # timed runs per side (default 5)
    python compare_bpe_hf1.py --input FILE --no-timing       # skip the timing section
    python compare_bpe_hf1.py --input FILE --cold            # every timed run starts with empty caches
    python compare_bpe_hf1.py --input FILE --parabix-args="--len-scan=15 --pair-cuts=3"
"""

import sys
import subprocess
import tempfile
import os
import re
import argparse
import shlex
import time

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
    """tokenizers must be 1.x (0.x has no compile() and would silently test the
    old encoder); tiktoken must be the thesis version unless overridden."""
    hf_major = tokenizers.__version__.split(".")[0]
    if hf_major != "1":
        print(f"Error: found tokenizers {tokenizers.__version__}, and this script needs 1.0. "
              f"Install it with: {sys.executable} -m pip install --no-deps --target "
              f"{HF1_SITE} \"tokenizers==1.0.0rc4\". For tokenizers 0.x use compare_bpe.py.")
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
# (tools/lex/tokenizer-test/); --tokenizer/--merges/--vocab/--tokenizer-json override.
# The input file has no default: it is always given with --input.
REPO_ROOT      = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER      = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
VOCAB          = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/vocab.json")
MERGES         = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")
TOKENIZER_JSON = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/tokenizer.json")
OUTPUT_FILE    = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "compare_bpe_hf1_output.txt")

DEFAULT_RUNS = 5

# Extra flags forwarded verbatim to the tokenizer binary on every invocation
# (both the correctness run and the --bench-loop timing run). Set by --parabix-args.
EXTRA_ARGS: list[str] = []

# --cold: every timed run starts with empty caches on every side.
COLD = False

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


def _pretokenizer_arg() -> list[str]:
    """--pretokenizer=bytelevel, unless --parabix-args already names a pretokenizer:
    the tokenizer rejects the option given twice."""
    if any(a.startswith("--pretokenizer") for a in EXTRA_ARGS):
        return []
    return ["--pretokenizer=bytelevel"]


def _parabix_ids_cmd(input_path: str, strings: bool = False):
    """Spawn the tokenizer binary on a pre-written input file. One subprocess."""
    cmd = [TOKENIZER, *_pretokenizer_arg(), f"--vocab={VOCAB}", f"--merges={MERGES}"]
    cmd += EXTRA_ARGS
    if strings:
        cmd.append("--strings")
    cmd.append(input_path)
    return subprocess.run(cmd, capture_output=True, encoding="utf-8")


def run_parabix_bpe(text: str) -> tuple[list[int], list[str]]:
    """Parabix BPE: --pretokenizer=bytelevel + BPE merge stage in one pipeline
    (the same configuration compare_bpe.py checks)."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        input_path = f.name
    try:
        r_ids  = _parabix_ids_cmd(input_path)
        r_strs = _parabix_ids_cmd(input_path, strings=True)
    finally:
        os.unlink(input_path)

    ids     = [int(x) for x in r_ids.stdout.split() if x.lstrip("-").isdigit()]
    strings = [s.split("\t", 1)[-1] for s in r_strs.stdout.split("\n") if s]
    return ids, strings


def load_hf(tokenizer_json: str):
    """Load tokenizer.json and compile it. Returns (loaded, compiled): the loaded
    Tokenizer is kept so --cold can compile a fresh instance per timed run."""
    loaded = Tokenizer.from_file(tokenizer_json)
    return loaded, loaded.compile()


def run_hf_bpe(compiled, text: str) -> tuple[list[int], list[str]]:
    """Run HuggingFace tokenizers 1.0 (CompiledTokenizer from the local
    tokenizer.json). Encoding carries ids only; strings via id_to_token()."""
    ids = list(compiled.encode(text).ids)
    return ids, [compiled.id_to_token(i) for i in ids]


def run_tiktoken_bpe(enc, text: str) -> tuple[list[int], list[str]]:
    """Run tiktoken's built-in "gpt2" encoding -- its own Rust implementation,
    not loaded from this project's vocab.json/merges.txt at all. <|endoftext|> in
    the text is the special token 50256, as in HF and Parabix (tiktoken's default
    raises on it)."""
    ids = enc.encode(text, allowed_special="all")
    toks = [enc.decode_single_token_bytes(i).decode("utf-8", errors="replace") for i in ids]
    return ids, toks


# ---------------------------------------------------------------------------
# Timing — FAIR: pure tokenization, one-time setup excluded on every side.
#
#   Parabix : tokenizer --bench-loop=N runs the already-built, JIT-compiled
#             pipeline N times inside ONE process, token output suppressed.
#             The binary prints a BENCH_RESULT stderr line we parse.
#   HF 1.0  : compiled.encode(text) in a best-of-N in-process loop; from_file()
#             + compile() ran once before timing.
#   tiktoken: enc.encode(text) in a best-of-N in-process loop.
#
# Warm (default): HF 1.0 keeps a word cache inside the CompiledTokenizer, so
#   runs after the first hit it for every repeated word; Parabix's --bench-loop
#   keeps its segment caches the same way.
# Cold (--cold): HF 1.0 gets a freshly compiled instance before every timed run
#   (compile not timed) and Parabix runs with --bench-cold-cache. tiktoken has
#   no cache, so it is timed the same way in both modes.
# ---------------------------------------------------------------------------

# BENCH_RESULT bytes=848 iters=50 min_ms=.. median_ms=.. mean_ms=.. mbps=..
BENCH_RE = re.compile(
    r"BENCH_RESULT\s+bytes=(\d+)\s+iters=(\d+)\s+min_ms=([\d.]+)\s+"
    r"median_ms=([\d.]+)\s+mean_ms=([\d.]+)\s+mbps=([\d.]+)"
)


def stats(times_ms: list[float], nbytes: int) -> dict:
    times_ms = sorted(times_ms)
    minv   = times_ms[0]
    median = times_ms[len(times_ms) // 2]
    mean   = sum(times_ms) / len(times_ms)
    mbps   = (nbytes / (minv / 1e3)) / 1e6 if minv > 0 else 0.0
    return {"bytes": nbytes, "iters": len(times_ms), "min_ms": minv,
            "median_ms": median, "mean_ms": mean, "mbps": mbps}


def bench_parabix(input_path: str, runs: int):
    """Parabix tokenize time via in-process --bench-loop. Returns a stats dict
    (min/median/mean ms + mbps) or None if no BENCH_RESULT was produced."""
    cmd = [TOKENIZER, *_pretokenizer_arg(), f"--bench-loop={runs}",
           f"--vocab={VOCAB}", f"--merges={MERGES}", *EXTRA_ARGS]
    if COLD and "--bench-cold-cache" not in EXTRA_ARGS:
        cmd.append("--bench-cold-cache")
    cmd.append(input_path)
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8")
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


def bench_hf(hf, text: str, runs: int) -> dict:
    """HF 1.0 tokenize time: best-of-N in-process encode(). Warm reuses one
    CompiledTokenizer; --cold compiles a fresh one (untimed) before each run."""
    loaded, compiled = hf
    compiled.encode(text)                            # one discarded warm-up
    times_ms = []
    for _ in range(runs):
        c = loaded.compile() if COLD else compiled
        t0 = time.perf_counter()
        c.encode(text)
        times_ms.append((time.perf_counter() - t0) * 1e3)
    return stats(times_ms, len(text.encode("utf-8")))


def bench_tiktoken(enc, text: str, runs: int) -> dict:
    """tiktoken tokenize time: best-of-N in-process encode() (encoding already
    loaded by tiktoken.get_encoding() before timing)."""
    enc.encode(text, allowed_special="all")           # one discarded warm-up
    times_ms = []
    for _ in range(runs):
        t0 = time.perf_counter()
        enc.encode(text, allowed_special="all")
        times_ms.append((time.perf_counter() - t0) * 1e3)
    return stats(times_ms, len(text.encode("utf-8")))


def measure_timing(hf, tiktoken_enc, text: str, runs: int) -> dict:
    """Tokenize-vs-tokenize timing (setup excluded on every side)."""
    result = {}

    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                     encoding="utf-8", delete=False) as f:
        f.write(text)
        ppath = f.name
    try:
        result["parabix"] = bench_parabix(ppath, runs)
    finally:
        os.unlink(ppath)

    if hf is not None:
        result["hf"] = bench_hf(hf, text, runs)
    if tiktoken_enc is not None:
        result["tiktoken"] = bench_tiktoken(tiktoken_enc, text, runs)

    return result


# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def write_token_table(out, engines: list[tuple[str, list[int], list[str]]]) -> None:
    """engines: list of (label, ids, strs). Match column is YES iff every
    engine's id agrees at that position."""
    w = 20
    header = "    {:<5} ".format("#")
    rule   = "    {:<5} ".format("-" * 5)
    for label, _, _ in engines:
        header += f"{label + ' ID':<10} {label + ' token':<{w}} "
        rule   += f"{'-'*10} {'-'*w} "
    out.write(header + "Match\n")
    out.write(rule + "-----\n")
    max_len = max((len(ids) for _, ids, _ in engines), default=0)
    for i in range(max_len):
        row_ids = []
        row = f"    {i+1:<5} "
        for _, ids, strs in engines:
            tid  = ids[i]  if i < len(ids)  else None
            tstr = strs[i] if i < len(strs) else "<miss>"
            row_ids.append(tid)
            row += f"{str(tid) if tid is not None else '<miss>':<10} {repr(tstr):<{w}} "
        mark = "YES" if len(set(row_ids)) == 1 else "NO "
        out.write(row + mark + "\n")


def write_diff_detail(out, engines: list[tuple[str, list[int]]]) -> None:
    """engines: list of (label, ids). Reports length + first mismatch of every
    engine against the first (the one under test -- Parabix)."""
    ref_label, ref_ids = engines[0]
    for label, ids in engines[1:]:
        if len(ids) != len(ref_ids):
            out.write(f"  → Length mismatch: {ref_label}={len(ref_ids)}, {label}={len(ids)}\n")
        first = next(
            (i for i in range(min(len(ref_ids), len(ids))) if ref_ids[i] != ids[i]),
            None
        )
        if first is not None:
            out.write(f"  → First mismatch vs {label} at position {first + 1}: "
                      f"{ref_label}={ref_ids[first]}, {label}={ids[first]}\n")


ENGINE_ORDER = ["parabix", "hf", "tiktoken"]
ENGINE_LABEL = {"parabix": "Parabix", "hf": "HF 1.0", "tiktoken": "tiktoken"}


def write_timing(out, timing: dict, text: str, runs: int, ntokens: int) -> None:
    nbytes = len(text.encode("utf-8"))
    mode = "COLD — empty caches every run" if COLD else "WARM — caches kept across runs"
    out.write(SEP + "\n")
    out.write(f"TIMING  (pure tokenize, setup excluded every side; {mode}; N={runs})\n")
    out.write(SEP + "\n")
    out.write(f"  Input size:  {nbytes} bytes, {ntokens} tokens\n\n")

    def row(label, s):
        out.write(f"  {label:<9}  min {s['min_ms']:9.4f} ms   "
                  f"median {s['median_ms']:9.4f} ms   mean {s['mean_ms']:9.4f} ms   "
                  f"{s['mbps']:8.3f} MB/s\n")

    present = [(name, timing[name]) for name in ENGINE_ORDER if timing.get(name)]
    if not timing.get("parabix"):
        out.write("  Parabix    <no BENCH_RESULT — rebuild tokenizer with --bench-loop support>\n")
    for name, s in present:
        row(ENGINE_LABEL[name], s)

    # Ranked ratio summary from medians: fastest first, each one's slowdown vs the fastest.
    ranked = sorted(present, key=lambda ns: ns[1]["median_ms"])
    if len(ranked) >= 2:
        fastest_name, fastest = ranked[0]
        out.write(f"\n  Speedup (median), fastest first:\n")
        out.write(f"    {ENGINE_LABEL[fastest_name]:<9} baseline\n")
        for name, s in ranked[1:]:
            ratio = s["median_ms"] / fastest["median_ms"] if fastest["median_ms"] > 0 else float("inf")
            out.write(f"    {ENGINE_LABEL[name]:<9} {ratio:.2f}x slower than {ENGINE_LABEL[fastest_name]}\n")

    out.write("\n  Parabix via --bench-loop (N iters in ONE process, output suppressed);\n"
              "  HF 1.0 and tiktoken via preloaded in-process encode() loops.\n"
              "  Warm: HF 1.0 word cache and Parabix segment caches are kept between runs.\n"
              "  Cold: fresh HF compile() per run (untimed) and Parabix --bench-cold-cache;\n"
              "        tiktoken has no cache. Parabix 'run' includes the input file read.\n")
    out.write(SEP + "\n\n")


# ---------------------------------------------------------------------------
# Run modes
# ---------------------------------------------------------------------------

def run_compare(out, hf, tiktoken_enc, cases: list[tuple[int, str]],
                verbose: bool, runs: int = 0) -> dict:
    results = {}
    for num, (lineno, text) in enumerate(cases, 1):
        engines = [("Parabix", *run_parabix_bpe(text))]
        if hf is not None:
            engines.append(("HF 1.0", *run_hf_bpe(hf[1], text)))
        if tiktoken_enc is not None:
            engines.append(("tiktoken", *run_tiktoken_bpe(tiktoken_enc, text)))

        all_ids = [ids for _, ids, _ in engines]
        match   = all(ids == all_ids[0] for ids in all_ids[1:])
        status  = "MATCH" if match else "MISMATCH"
        label   = f"line {lineno}"
        results[label] = status

        out.write(SEP + "\n")
        out.write(f"Test {num} (line {lineno})  →  {status}\n")
        out.write(SEP + "\n")
        out.write(input_line(text, "         "))
        for label_e, ids, _ in engines:
            out.write(f"  {label_e + ' tokens:':<16}{len(ids)}\n")
        out.write("\n")

        if not match:
            write_diff_detail(out, [(label_e, ids) for label_e, ids, _ in engines])
            out.write("\n")

        if verbose or not match:
            write_token_table(out, engines)
        else:
            parabix_ids = engines[0][1]
            out.write(f"  IDs: {parabix_ids[:12]}{'...' if len(parabix_ids) > 12 else ''}\n")

        out.write("\n")

        if runs > 0:
            timing = measure_timing(hf, tiktoken_enc, text, runs)
            write_timing(out, timing, text, runs, len(engines[0][1]))

    return results


def write_token_list(out, ids: list[int], tokens: list[str]) -> None:
    out.write(f"  Tokens ({len(ids)}):\n")
    for i, (tid, tok) in enumerate(zip(ids, tokens), 1):
        out.write(f"    {i:3}. {tid:<7} {repr(tok)}\n")
    out.write("\n")


def run_single(out, name: str, runner, cases: list[tuple[int, str]]) -> None:
    for num, (lineno, text) in enumerate(cases, 1):
        ids, tokens = runner(text)
        out.write(SEP + "\n")
        out.write(f"Test {num} (line {lineno}) — {name}\n")
        out.write(SEP + "\n")
        out.write(input_line(text))
        write_token_list(out, ids, tokens)


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
    global SHOW_INPUT, EXTRA_ARGS, COLD, TOKENIZER, MERGES, VOCAB, TOKENIZER_JSON
    parser = argparse.ArgumentParser(
        description="BPE tokenizer comparison: Parabix vs HuggingFace tokenizers 1.0 vs tiktoken 0.14.0.")
    parser.add_argument("--input",        required=True, metavar="FILE",
                        help="Input file (required; the entire file is one test case)")
    parser.add_argument("--hf-only",      action="store_true",
                        help="Run HuggingFace tokenizers 1.0 only")
    parser.add_argument("--parabix-only", action="store_true",
                        help="Run Parabix tokenizer only")
    parser.add_argument("--tiktoken-only", action="store_true",
                        help="Run tiktoken only")
    parser.add_argument("--no-tiktoken",  action="store_true",
                        help="Compare mode: Parabix vs HF 1.0 only, skip tiktoken")
    parser.add_argument("--tiktoken-encoding", default="gpt2",
                        help="tiktoken encoding name (default: gpt2, same vocab as this "
                             "project's vocab.json/merges.txt)")
    parser.add_argument("--any-tiktoken", action="store_true",
                        help=f"Allow a tiktoken version other than {TIKTOKEN_VERSION}")
    parser.add_argument("--show-input",   action="store_true",
                        help="Print each case's full input text (off by default: "
                             "only a char/byte count is shown)")
    parser.add_argument("--verbose",      action="store_true",
                        help="Always show full token-by-token table (default: only on mismatch)")
    parser.add_argument("--runs",         type=int, default=DEFAULT_RUNS,
                        help=f"Timed runs per side in compare mode (default: {DEFAULT_RUNS})")
    parser.add_argument("--no-timing",    action="store_true",
                        help="Skip the timing section")
    parser.add_argument("--cold",         action="store_true",
                        help="Empty caches before every timed run: fresh HF compile() "
                             "(untimed) and Parabix --bench-cold-cache")
    parser.add_argument("--parabix-args", default="",
                        help="Extra flags passed straight to the tokenizer binary, "
                             "e.g. --parabix-args=\"--len-scan=15 --pair-cuts=3\"")
    parser.add_argument("--tokenizer",      default=TOKENIZER,
                        help=f"Parabix tokenizer binary (default: {TOKENIZER})")
    parser.add_argument("--merges",         default=MERGES,
                        help=f"merges.txt (default: {MERGES})")
    parser.add_argument("--vocab",          default=VOCAB,
                        help=f"vocab.json (default: {VOCAB})")
    parser.add_argument("--tokenizer-json", default=TOKENIZER_JSON,
                        help=f"HuggingFace tokenizer.json (default: {TOKENIZER_JSON})")
    args = parser.parse_args()
    check_versions(args.any_tiktoken)
    TOKENIZER, MERGES = args.tokenizer, args.merges
    VOCAB, TOKENIZER_JSON = args.vocab, args.tokenizer_json
    only_flags = [args.hf_only, args.parabix_only, args.tiktoken_only]
    if sum(only_flags) > 1:
        print("Error: --hf-only / --parabix-only / --tiktoken-only are mutually exclusive.")
        sys.exit(1)
    if not args.hf_only and not args.tiktoken_only and not os.path.isfile(TOKENIZER):
        print(f"Error: tokenizer binary not found: {TOKENIZER}")
        sys.exit(1)
    SHOW_INPUT = args.show_input
    EXTRA_ARGS = shlex.split(args.parabix_args)
    # --bench-cold-cache in --parabix-args means cold for every engine, as --cold does.
    COLD = args.cold or "--bench-cold-cache" in EXTRA_ARGS
    if EXTRA_ARGS:
        print(f"Extra tokenizer flags: {' '.join(EXTRA_ARGS)}")

    if not os.path.isfile(args.input):
        print(f"Error: input file not found: {args.input}")
        sys.exit(1)

    with open(args.input, "r", encoding="utf-8") as f:
        # Treat the entire file as a single input string (one encode() call per side).
        full_text = f.read()

    if not full_text.strip():
        print("Error: input file is empty.")
        sys.exit(1)

    cases = [(1, full_text)]

    hf = None
    if not args.parabix_only and not args.tiktoken_only:
        if not os.path.isfile(TOKENIZER_JSON):
            print(f"Error: tokenizer.json not found at {TOKENIZER_JSON}")
            sys.exit(1)
        hf = load_hf(TOKENIZER_JSON)

    tiktoken_enc = None
    if args.tiktoken_only or not (args.parabix_only or args.hf_only or args.no_tiktoken):
        tiktoken_enc = tiktoken.get_encoding(args.tiktoken_encoding)

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        out = Tee(f)
        out.write("Parabix vs HuggingFace tokenizers 1.0 vs tiktoken BPE comparison\n")
        out.write(f"Input:  {args.input}\n")
        out.write(f"Vocab:  {VOCAB}\n")
        out.write(f"Merges: {MERGES}\n")
        if hf is not None:
            out.write(f"HuggingFace tokenizers: {tokenizers.__version__} (compiled)\n")
        if tiktoken_enc is not None:
            out.write(f"tiktoken: {tiktoken.__version__}, encoding {args.tiktoken_encoding}\n")
        out.write(f"Python: {sys.version.split()[0]}\n")
        out.write("\n")

        if args.hf_only:
            run_single(out, "HuggingFace 1.0", lambda t: run_hf_bpe(hf[1], t), cases)
        elif args.parabix_only:
            run_single(out, "Parabix", run_parabix_bpe, cases)
        elif args.tiktoken_only:
            run_single(out, "tiktoken", lambda t: run_tiktoken_bpe(tiktoken_enc, t), cases)
        else:
            runs = 0 if args.no_timing else args.runs
            results = run_compare(out, hf, tiktoken_enc, cases, args.verbose, runs)
            write_summary(out, results)

    print(f"\nOutput written to: {OUTPUT_FILE}")


if __name__ == "__main__":
    main()
