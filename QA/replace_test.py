#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  icgrep --replace (the search and replace of the regular expression engine),
#  compared with Python's re: each line with a match, with re.sub applied.  The
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

TEXT = (
    "the cat sat on the mat\n"
    "no match here\n"
    "catcat concat 123 de45f\n"
    "café crème brûlée naïve\n"
    "foo@bar.com and x@y\n"
    "ababa abcd cdab\n"
    "\n"
    "last line without a line break cat"
)

# (icgrep pattern, icgrep replacement, Python pattern, Python replacement text)
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
]

# (icgrep pattern, icgrep replacement, a part of the expected error)
REJECTED = [
    ("\\d+", "N", "may end within another"),
    ("ab|abc", "X", "may end within another"),
    ("x*", "X", "empty string"),
    ("(a)b", "$2", "No capture 2"),
    ("([a-z]+)@", "$1", "not yet supported"),
    ("cat", "a\\nb", "line break"),
]


def expected(pattern, replacement):
    rx = re.compile(pattern)
    lines = []
    for line in TEXT.split("\n"):
        if rx.search(line):
            lines.append(rx.sub(lambda m: replacement, line))
    return "".join(l + "\n" for l in lines)


def main():
    icgrep = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(TEXT)
        for pattern, replacement, py_pattern, py_replacement in CASES:
            run = subprocess.run([icgrep, "--colors=never", "--replace=" + replacement, pattern, path],
                                 capture_output=True)
            got = run.stdout.decode("utf-8", "replace")
            want = expected(py_pattern, py_replacement)
            if run.returncode not in (0, 1) or got != want:
                failures += 1
                print("FAIL s/%s/%s/: exit %d\n  got:      %r\n  expected: %r\n%s"
                      % (pattern, replacement, run.returncode, got, want, run.stderr.decode()[:300]))
        for pattern, replacement, message in REJECTED:
            run = subprocess.run([icgrep, "--replace=" + replacement, pattern, path], capture_output=True)
            if run.returncode in (0, 1) or message not in run.stderr.decode("utf-8", "replace"):
                failures += 1
                print("FAIL s/%s/%s/: expected an error with %r, got exit %d: %s"
                      % (pattern, replacement, message, run.returncode, run.stderr.decode()[:300]))
        # Line numbers and file names (one file at a time).
        run = subprocess.run([icgrep, "--colors=never", "-n", "-H", "--replace=Q", "cat", path, path],
                             capture_output=True)
        lines = run.stdout.decode("utf-8").split("\n")
        want = [path + ":1:the Q sat on the mat", path + ":3:QQ conQ 123 de45f",
                path + ":8:last line without a line break Q"]
        if lines[:3] != want or lines[3:6] != want:
            failures += 1
            print("FAIL -n -H: got %r" % lines)
    if failures:
        sys.exit(1)
    print("replace_test: all tests passed")


if __name__ == "__main__":
    main()
