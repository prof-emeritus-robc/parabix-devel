#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  icgrep --replace (the search and replace of the regular expression engine),
#  compared with Python's re: each line with a match, with re.sub applied (the
#  Python replacement is a template, with \1 for a capture).  The
#  search patterns accepted are those whose replaced matches do not depend on
#  the matching conventions, for which Python's agree.  Patterns that are not
#  supported must be reported as errors.
#
#  replace_test.py <icgrep binary>

import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

TEXT = (
    "the cat sat on the mat\n"
    "no match here\n"
    "catcat concat 123 de45f\n"
    "café crème brûlée naïve\n"
    "foo@bar.com and x@y\n"
    "ababa abcd cdab\n"
    "\n"
    "a@ b@c @@x @ab@\n"
    "@. a@. @b. #ab12; #9; #x7;\n"
    "ab12; x9z77; abc1;; 7a7; q1a22;b3; é5é12; a1-bc; zz9q12-ab; x7a3;\n"
    "last line without a line break cat"
)

# (icgrep pattern, icgrep replacement, Python pattern, Python replacement template)
CASES = [
    ("cat", "DOG", "cat", "DOG"),
    ("at", "", "at", ""),
    ("é", "E", "é", "E"),
    ("cat", "ü✓", "cat", "ü✓"),
    ("cr[eè]me", "cream", "cr[eè]me", "cream"),
    ("[a-z]+@", "<at>", "[a-z]+@", "<at>"),
    ("\\d+(?!\\d)", "$$#", "[0-9]+(?![0-9])", "$#"),
    ("[a-z]+(?![a-z])", "W", "[a-z]+(?![a-z])", "W"),
    ("ab|cd", "\\x{2192}", "ab|cd", "→"),
    ("e+$", "E", "e+$", "E"),
    ("@[a-z]*(?![a-z])", "AT", "@[a-z]*(?![a-z])", "AT"),      # adjacent matches, one of one character
    # References to captures (kept in place, in their order).
    ("([a-z]+)@", "<$1>", "([a-z]+)@", "<\\1>"),
    ("([a-z]+)@([a-z]+)\\.", "[$1|$2]", "([a-z]+)@([a-z]+)\\.", "[\\1|\\2]"),
    ("([a-zé]+)@([a-zè]+)\\.", "$1/$2", "([a-zé]+)@([a-zè]+)\\.", "\\1/\\2"),
    ("(c)a(t)", "$1-$2", "(c)a(t)", "\\1-\\2"),
    ("([a-z]*)@", "{$1}", "([a-z]*)@", "{\\1}"),
    ("@([a-z]*)(?![a-z])", "<$1>", "@([a-z]*)(?![a-z])", "<\\1>"),
    ("([a-z]*)@([a-z]*)\\.", "x$1y$2z", "([a-z]*)@([a-z]*)\\.", "x\\1y\\2z"),
    ("#([a-z0-9]*)([0-9]);", "$1.${2}", "#([a-z0-9]*)([0-9]);", "\\1.\\2"),
    ("c(a)(t)", "$1$1", "c(a)(t)", "\\1\\1"),
    ("([a-z])(?![x])", "${1}q", "([a-z])(?![x])", "\\1q"),       # adjacent matches, each a capture
    # Boundaries found by a walk back from the end of the match (by reversal).
    ("([a-z0-9]*[a-z])\\d+;", "<$1>", "([a-z0-9]*[a-z])[0-9]+;", "<\\1>"),
    ("x[a-z0-9]*([a-z]\\d+);", "[$1]", "x[a-z0-9]*([a-z][0-9]+);", "[\\1]"),
    ("([a-z0-9]*[a-z])(\\d+)-([a-z]+);", "$1.$2.$3", "([a-z0-9]*[a-z])([0-9]+)-([a-z]+);", "\\1.\\2.\\3"),
    ("([a-z0-9é]*é)\\d+;", "<$1>", "([a-z0-9é]*é)[0-9]+;", "<\\1>"),
]

# (icgrep pattern, icgrep replacement, a part of the expected error)
REJECTED = [
    ("\\d+", "N", "may end within another"),
    ("ab|abc", "X", "may end within another"),
    ("x*", "X", "empty string"),
    ("(a)b", "$2", "No capture 2"),
    ("([a-z]+)@([a-z]+)\\.", "$2$1", "in their order"),
    ("([a-z]+)@", "$1$1", "in their order"),
    ("(?:(a)|b)c", "$1", "no unique binding"),
    ("([a-z])+@", "$1", "no unique binding"),
    ("#([a-z0-9]*)([0-9]+);", "$1", "is not found"),
    ("([a-z0-9]*[a-z])[a-z0-9]+;", "$1", "is not found"),     # not maximal backward either
    ("cat", "a\\nb", "line break"),
]


def expected(pattern, replacement):
    rx = re.compile(pattern)
    lines = []
    for line in TEXT.split("\n"):
        if rx.search(line):
            lines.append(rx.sub(replacement, line))
    return "".join(l + "\n" for l in lines)


def main():
    icgrep = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(TEXT)
        # The runs of icgrep, three at a time.
        def run_all(argvs):
            with ThreadPoolExecutor(max_workers=3) as pool:
                return list(pool.map(lambda argv: subprocess.run(argv, capture_output=True), argvs))
        runs = run_all([[icgrep, "--colors=never", "--replace=" + c[1], c[0], path] for c in CASES])
        for (pattern, replacement, py_pattern, py_replacement), run in zip(CASES, runs):
            got = run.stdout.decode("utf-8", "replace")
            want = expected(py_pattern, py_replacement)
            if run.returncode not in (0, 1) or got != want:
                failures += 1
                print("FAIL s/%s/%s/: exit %d\n  got:      %r\n  expected: %r\n%s"
                      % (pattern, replacement, run.returncode, got, want, run.stderr.decode()[:300]))
        runs = run_all([[icgrep, "--replace=" + r[1], r[0], path] for r in REJECTED])
        for (pattern, replacement, message), run in zip(REJECTED, runs):
            if run.returncode in (0, 1) or message not in run.stderr.decode("utf-8", "replace"):
                failures += 1
                print("FAIL s/%s/%s/: expected an error with %r, got exit %d: %s"
                      % (pattern, replacement, message, run.returncode, run.stderr.decode()[:300]))
        # Line numbers and file names (one file at a time).
        run = subprocess.run([icgrep, "--colors=never", "-n", "-H", "--replace=Q", "cat", path, path],
                             capture_output=True)
        lines = run.stdout.decode("utf-8").split("\n")
        want = [path + ":1:the Q sat on the mat", path + ":3:QQ conQ 123 de45f",
                path + ":11:last line without a line break Q"]
        if lines[:3] != want or lines[3:6] != want:
            failures += 1
            print("FAIL -n -H: got %r" % lines)
    if failures:
        sys.exit(1)
    print("replace_test: all tests passed")


if __name__ == "__main__":
    main()
