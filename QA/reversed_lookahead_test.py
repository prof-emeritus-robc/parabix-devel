#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Lookaheads without a forward strategy, matched by reversal of the regions
#  of the text (see ReversedLookaheadNamer and RE_PipelineBuilder::
#  reversedStarts): the lines that icgrep selects, compared with Python's re
#  on random lines.  Lookaheads that reversal does not support, and regions
#  longer than --reverse-region-limit, must be reported as errors.
#
#  reversed_lookahead_test.py <icgrep binary>

import os
import random
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

PATTERNS = [
    "foo(?![a-z]*bar)",
    "x(?=(?:ab|c)+d)",
    "x(?=(?:a*b)*c)",
    "x(?!(?:a*b)*c)",
    "[a-z]+(?=(?:ab)+ )",
    "(?:ab|c)(?=[a-z]*(?:ba|cc)+d)x?",
    "é(?=(?:a|é)*b+c)",
]

# (pattern, text, a part of the expected error)
REJECTED = [
    ("x(?=(?:a*b)*(?<!a)c)", "xabc\n", "not by reversal"),    # reversed: a negative lookahead
    ("x(?=(?:a*b)*c)", "x" + "ab" * 800 + "c\n", "reverse-region-limit"),
]


def random_text():
    rng = random.Random(7)
    words = ["a", "b", "c", "d", "x", "ab", "ba", "foo", "bar", "é", "cc", "abab"]
    lines = []
    for _ in range(300):
        n = rng.randint(0, 12)
        lines.append("".join(rng.choice(words) + (" " if rng.random() < 0.3 else "") for _ in range(n)))
    return "".join(l + "\n" for l in lines)


def main():
    icgrep = sys.argv[1]
    text = random_text()
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        argvs = [[icgrep, "--colors=never", p, path] for p in PATTERNS]
        rejected = []
        for i, (p, t, _) in enumerate(REJECTED):
            rpath = os.path.join(tmp, "rejected%d.txt" % i)
            with open(rpath, "w", encoding="utf-8") as f:
                f.write(t)
            argvs.append([icgrep, p, rpath])
        with ThreadPoolExecutor(max_workers=3) as pool:
            runs = list(pool.map(lambda argv: subprocess.run(argv, capture_output=True), argvs))
        for p, run in zip(PATTERNS, runs):
            rx = re.compile(p)
            want = "".join(l + "\n" for l in text.split("\n")[:-1] if rx.search(l))
            got = run.stdout.decode("utf-8", "replace")
            if run.returncode not in (0, 1) or got != want:
                failures += 1
                print("FAIL %s: exit %d, %d lines, expected %d\n%s"
                      % (p, run.returncode, got.count("\n"), want.count("\n"), run.stderr.decode()[:300]))
        for (p, _, message), run in zip(REJECTED, runs[len(PATTERNS):]):
            if run.returncode in (0, 1) or message not in run.stderr.decode("utf-8", "replace"):
                failures += 1
                print("FAIL %s: expected an error with %r, got exit %d: %s"
                      % (p, message, run.returncode, run.stderr.decode()[:300]))
    if failures:
        sys.exit(1)
    print("reversed_lookahead_test: all tests passed")


if __name__ == "__main__":
    main()
