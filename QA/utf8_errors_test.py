#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Ill-formed UTF-8 (--utf8-errors): the lines that icgrep selects in random
#  text with ill-formed sequences, compared with Python's re on the lines as
#  Python decodes them (each maximal ill-formed subpart one U+FFFD with
#  use-replacement-char, the default, or dropped with drop-bad-characters;
#  with abort-on-error, the lines before the first error), in each mode of
#  the regular expression engine: as it chooses, with full Unicode indexing
#  (-UnicodeIndexing), and over multiplexed codes (--re-multiplex=force).
#
#  utf8_errors_test.py <icgrep binary>

import os
import random
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# Each pattern is given to icgrep and to Python, or as (icgrep, Python).
PATTERNS = [
    "qd",
    "^.qd",
    ".(?=..d)",
    "x[^q]{2}d",
    ("\\x{FFFD}", "�"),
    ("[\\x{FFF0}-\\x{FFFF}]q", "[￰-￿]q"),
    ("[^\\x{0}-\\x{7F}]d", "[^\x00-\x7F]d"),
    "x(?=(?:ab|c)+d)",
]

FRAGMENTS = [b"a", b"b", b"c", b"d", b"q", b"x", b"ab", b" ", b"\xc3\xa9", b"\xe6\x97\xa5",
             b"\xf0\x9f\x98\x80", b"\xef\xbf\xbd",
             # ill-formed: lone bytes, truncated sequences, invalid second bytes
             b"\xff", b"\x80", b"\xc3", b"\xe6\x97", b"\xf0\x9f\x98", b"\xed\xa0\x80", b"\xe0\x80",
             b"\xf4\x90", b"\xc0\xaf", b"\xf5"]

MODES = {
    "use-replacement-char": "replace",
    "drop-bad-characters": "ignore",
}

CONFIGURATIONS = [[], ["-UnicodeIndexing"], ["-UnicodeIndexing", "--re-multiplex=force"]]


def random_lines():
    rng = random.Random(23)
    return [b"".join(rng.choice(FRAGMENTS) for _ in range(rng.randint(0, 12))) for _ in range(400)]


def expected(lines, pattern, errors):
    regex = re.compile(pattern)
    return [k + 1 for k, line in enumerate(lines) if regex.search(line.decode("utf-8", errors))]


def selected(icgrep, path, pattern, flags):
    proc = subprocess.run([icgrep, "-n"] + flags + [pattern, path], capture_output=True)
    if proc.returncode not in (0, 1):
        return f"exit {proc.returncode}: {proc.stderr.decode(errors='replace')}"
    return [int(line.split(b":", 1)[0]) for line in proc.stdout.split(b"\n") if line]


def abort_lines():
    # Well-formed lines, then from line 251 the random lines with ill-formed
    # input (the first of which is in its first few lines).
    good = [l for l in random_lines() if is_well_formed(l)]
    return (good * 2)[:250] + random_lines()


def is_well_formed(line):
    try:
        line.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def abort_runs(icgrep, tmp):
    # (failures, runs) for abort-on-error: the lines before the line of the
    # first error, both as lines (-n) and counted (-c), and the error reported.
    lines = abort_lines()
    first = next(k for k, l in enumerate(lines) if not is_well_formed(l))
    path = os.path.join(tmp, "abort.txt")
    with open(path, "wb") as f:
        f.write(b"".join(line + b"\n" for line in lines))
    failures = []
    runs = 0
    for p in ["qd", "^.qd", "x(?=(?:ab|c)+d)"]:
        want = [n for n in expected(lines[:first], p, "strict")]
        for flags in (["-n"], ["-c"]):
            proc = subprocess.run([icgrep, "--utf8-errors=abort-on-error"] + flags + [p, path], capture_output=True)
            runs += 1
            out = proc.stdout.split(b"\n")
            got = ([int(l.split(b":", 1)[0]) for l in out if l] if flags == ["-n"] else int(out[0] or b"0"))
            if got != (want if flags == ["-n"] else len(want)):
                failures.append(f"{p} abort-on-error {flags[0]}: got {got if flags == ['-c'] else len(got)} lines, "
                                f"expected {len(want)}")
            if b"ill-formed UTF-8" not in proc.stderr:
                failures.append(f"{p} abort-on-error {flags[0]}: the error is not reported")
    return failures, runs


def main():
    icgrep = sys.argv[1]
    lines = random_lines()
    jobs = []
    for mode, errors in MODES.items():
        for p in PATTERNS:
            ours, theirs = (p, p) if isinstance(p, str) else p
            want = expected(lines, theirs, errors)
            for flags in CONFIGURATIONS:
                jobs.append((ours, ["--utf8-errors=" + mode] + flags, want))
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "text.txt")
        with open(path, "wb") as f:
            f.write(b"".join(line + b"\n" for line in lines))
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda j: selected(icgrep, path, j[0], j[1]), jobs))
        aborted, abortRuns = abort_runs(icgrep, tmp)
        failures.extend(aborted)
    for (pattern, flags, want), got in zip(jobs, results):
        if got != want:
            if isinstance(got, str):
                failures.append(f"{pattern} {' '.join(flags)}: {got}")
            else:
                extra = sorted(set(got) - set(want))[:5]
                missing = sorted(set(want) - set(got))[:5]
                failures.append(f"{pattern} {' '.join(flags)}: extra lines {extra}, missing lines {missing}")
    for f in failures:
        print("FAIL", f)
    if failures:
        sys.exit(1)
    print(f"utf8_errors_test: {len(jobs) + abortRuns} runs agree with Python")


if __name__ == "__main__":
    main()
