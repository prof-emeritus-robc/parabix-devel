#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The MatchStarts mode of the regular expression engine (see match_starts.cpp),
#  compared with Python's re: within each line, a match starts at the
#  positions p at which re.match(line, p) succeeds.  For a lookbehind of
#  variable length, which Python does not support, the Python pattern is a
#  triple (B, sense, R): a match of (?<=B)R or (?<!B)R starts at p if B does
#  (or does not) match the line up to p and R matches at p.

import os
import re
import subprocess
import sys
import tempfile

TEXT = (
    "the quick brown fox jumps over the lazy dog\n"
    "queue quu qu qxu xabcx abd acd xab abcc abcd\n"
    "singing ringing ing 123abc 45x a@b foo@bar.com\n"
    "café éa éb Ψυχή ab é. done\n"
    "\n"
    "aaab bbbc abcabc xyz abab cc ababx\n"
    "cabd cbabd xcaabbd abd cab@d ab@cd x@y\n"
    "abbc ab abc\n"
    "c q@r abc@d\n"
)

# Groups of at most 8 patterns, each run as one pipeline: (Parabix, Python).
GROUPS = [
    [   # fixed length (including zero-width and anchored)
        ("abc", "abc"),
        ("a[bc]d", "a[bc]d"),
        ("(?<=x)ab", "(?<=x)ab"),
        ("ab(?=c)", "ab(?=c)"),
        ("(?=ab)", "(?=ab)"),
        ("^qu", "^qu"),
        ("ab$", "ab$"),
        ("é[ab.]", "é[ab.]"),
    ],
    [   # unique prefix, chains and alternations
        ("q[a-z]*u", "q[a-z]*u"),
        ("(?<=a)q[a-z]*u", "(?<=a)q[a-z]*u"),
        ("[a-z]+ing", "[a-z]+ing"),
        ("b*c", "b*c"),
        ("[0-9]+[a-z]", "[0-9]+[a-z]"),
        ("[a-z]+@[a-z]+", "[a-z]+@[a-z]+"),
        ("abc|x+y", "abc|x+y"),
        ("(?:ab|c)d", "(?:ab|c)d"),
    ],
    [   # trailing lookaheads of variable length, Unicode, repetitions
        ("ab(?=c+d)", "ab(?=c+d)"),
        ("[a-z](?=[0-9]+[a-z])", "[a-z](?=[0-9]+[a-z])"),
        ("(?:ab)+x", "(?:ab)+x"),
        ("[ΨυχήΑ-ω]+", "[ΨυχήΑ-ω]+"),
        ("é+", "é+"),
        ("a+b", "a+b"),
    ],
    [   # length classes (bounded REs) and nullable suffixes
        ("[ab]{1,3}c", "[ab]{1,3}c"),
        ("a{2,4}", "a{2,4}"),
        ("é?a", "é?a"),
        ("(?:ab|b)c?", "(?:ab|b)c?"),
        ("ab*", "ab*"),
        ("x*", "x*"),
        ("(?:ab)+c*", "(?:ab)+c*"),
        ("b{2,}", "b{2,}"),
    ],
    [   # by reversal (no forward strategy): the regions between the characters
        # the RE cannot match, reversed in place
        ("q[a-z]*(?![a-z]*e)", "q[a-z]*(?![a-z]*e)"),
        ("(?:[a-z]+@)+[a-z]+", "(?:[a-z]+@)+[a-z]+"),
        ("(?:ab|c)+(?![a-z]*x)", "(?:ab|c)+(?![a-z]*x)"),
        ("(?<!x)(?:[a-z]+@)+[a-z]", "(?<!x)(?:[a-z]+@)+[a-z]"),      # extended text (negative lookahead reversed)
        ("(?:é|[a-z])+(?![a-zé]*e)", "(?:é|[a-z])+(?![a-zé]*e)"),
        ("(?:[0-9]+[a-z])+(?![a-z]*c)", "(?:[0-9]+[a-z])+(?![a-z]*c)"),
        # lookbehinds of variable length, matched forward (lookaheads without a
        # forward strategy in the reversed RE)
        ("(?<=c(?:a*b)*)(?:a*b)*d", ("c(?:a*b)*", True, "(?:a*b)*d")),
        ("(?<!c(?:a*b)*)(?:[a-z]+@)+[a-z]", ("c(?:a*b)*", False, "(?:[a-z]+@)+[a-z]")),
    ],
    [   # anchored, by reversal: the anchors of the lines, in the regions between
        # the barriers reversed
        ("(?:a*b)*c$", "(?:a*b)*c$"),
        ("^(?:a*b)*c", "^(?:a*b)*c"),
        ("(?:[a-z]+@)+[a-z]+$", "(?:[a-z]+@)+[a-z]+$"),
        ("^(?:[a-z]+@)+[a-z]", "^(?:[a-z]+@)+[a-z]"),
        ("(?:ab|c)+$", "(?:ab|c)+$"),
        ("^(?:ab|c)+(?![a-z]*x)", "^(?:ab|c)+(?![a-z]*x)"),
    ],
]


def reference(pattern, text, unicode_mode):
    if isinstance(pattern, tuple):
        behind, sense, rest = pattern
        bx = re.compile("(?:%s)\\Z" % behind)
        rx = re.compile(rest)
        matches = lambda line, p: (bx.search(line[:p]) is not None) == sense and rx.match(line, p)
    else:
        rx = re.compile(pattern)
        matches = lambda line, p: rx.match(line, p)
    data = text.encode("utf-8")
    marks = set()
    offset = 0
    for line in text.split("\n")[:-1]:
        positions = []      # byte position of each character, and of the line end
        pos = offset
        for ch in line:
            positions.append(pos)
            pos += len(ch.encode("utf-8"))
        positions.append(pos)
        for p in range(len(line) + 1):
            if matches(line, p):
                if unicode_mode and p < len(line):
                    marks.add(positions[p] + len(line[p].encode("utf-8")) - 1)
                else:
                    marks.add(positions[p])
        offset = pos + 1
    assert offset == len(data)
    return marks


def main():
    tool = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(TEXT)
        failures = 0
        for group in GROUPS:
            args = [tool]
            for parabix, _ in group:
                args += ["-e", parabix]
            args.append(path)
            run = subprocess.run(args, capture_output=True)
            if run.returncode != 0:
                print("FAIL: %s exited with %d\n%s" % (args, run.returncode, run.stderr.decode()))
                failures += 1
                continue
            out = run.stdout
            modes = []
            for _ in group:
                nl = out.index(b"\n")
                modes.append(out[:nl].decode())
                out = out[nl + 1:]
            for i, (parabix, python) in enumerate(group):
                got = {p for p, b in enumerate(out) if b & (1 << i)}
                expected = reference(python, TEXT, modes[i] == "unicode")
                if got != expected:
                    failures += 1
                    print("FAIL %s (%s): missing %s, extra %s"
                          % (parabix, modes[i], sorted(expected - got), sorted(got - expected)))
        if failures:
            sys.exit(1)
        print("match_starts_test: all tests passed")


if __name__ == "__main__":
    main()
