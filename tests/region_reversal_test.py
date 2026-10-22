#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Regions reversed in place (kernel::RegionReversal, see region_reversal.cpp),
#  compared with Python: each maximal run of characters other than barriers
#  (line breaks, unless -noregions, and the characters given by -b) reversed.
#  Regions longer than --reverse-region-limit must be reported as errors.

import os
import random
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

SHORT = (
    "the cat sat on the mat\n"
    "café crème, brûlée; naïve\n"
    "\n"
    "ψ@ω. 日本語のテキスト 𝔘𝔫𝔦𝔠𝔬𝔡𝔢\n"
    "a\n"
    "no final line break"
)


def long_text():
    # Lines of up to 900 characters (many blocks, regions across the strides
    # and segments of the pipeline), with words of up to 30 characters.
    rng = random.Random(9)
    alphabet = "abcdefghijklmnopqrstuvwxyzéψ日𝔘"
    lines = []
    for _ in range(400):
        words, n = [], rng.randint(0, 900)
        while sum(len(w) + 1 for w in words) < n:
            words.append("".join(rng.choice(alphabet) for _ in range(rng.randint(1, 30))))
        lines.append(" ".join(words)[:n])
    return "\n".join(lines) + "\n"


def expected(text, barriers):
    out, region = [], []
    for c in text:
        if c in barriers:
            out.extend(reversed(region))
            region = []
            out.append(c)
        else:
            region.append(c)
    out.extend(reversed(region))
    return "".join(out)


def main():
    binary = sys.argv[1]
    texts = {"short": SHORT, "long": long_text(), "toolong": "x" * 1200 + "\nab\n"}
    # (text, -b, -noregions, limit, an expected error)
    cases = [
        ("short", "", False, None, None),
        ("short", " ,;", False, None, None),
        ("short", " ", True, None, None),
        ("long", "", False, None, None),
        ("long", " ", False, 40, None),
        ("long", " ", True, 64, None),        # words joined across line breaks
        ("toolong", "", False, None, "reverse-region-limit"),
        ("toolong", "", False, 1200, None),
        ("long", "", False, 500, "reverse-region-limit"),
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        paths = {}
        for name, text in texts.items():
            paths[name] = os.path.join(tmp, name + ".txt")
            with open(paths[name], "w", encoding="utf-8") as f:
                f.write(text)
        argvs = []
        for name, b, noregions, limit, _ in cases:
            argv = [binary, paths[name]]
            if b:
                argv.append("-b=" + b)
            if noregions:
                argv.append("-noregions")
            if limit is not None:
                argv.append("--reverse-region-limit=%d" % limit)
            argvs.append(argv)
        with ThreadPoolExecutor(max_workers=3) as pool:
            runs = list(pool.map(lambda argv: subprocess.run(argv, capture_output=True), argvs))
        for (name, b, noregions, limit, error), run in zip(cases, runs):
            label = "%s -b=%r%s limit %s" % (name, b, " -noregions" if noregions else "", limit)
            if error:
                if run.returncode == 0 or error not in run.stderr.decode("utf-8", "replace"):
                    failures += 1
                    print("FAIL %s: expected an error with %r, got exit %d" % (label, error, run.returncode))
                continue
            barriers = set(b) | (set() if noregions else {"\n"})
            want = expected(texts[name], barriers)
            got = run.stdout.decode("utf-8", "replace")
            if run.returncode != 0 or got != want:
                failures += 1
                i = next((k for k in range(min(len(got), len(want))) if got[k] != want[k]), min(len(got), len(want)))
                print("FAIL %s: exit %d, first difference at character %d: got %r, expected %r\n%s"
                      % (label, run.returncode, i, got[i:i + 20], want[i:i + 20], run.stderr.decode()[:300]))
    if failures:
        sys.exit(1)
    print("region_reversal_test: all tests passed")


if __name__ == "__main__":
    main()
