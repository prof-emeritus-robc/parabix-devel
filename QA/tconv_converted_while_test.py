#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: before contexts matched against the converted text, resolved in the
#  regular plan's correction kernel by a While loop (doc/round-replacement.md
#  §1, --converted-context-while) instead of rounds: single-character rules
#  whose before contexts have two or three items, in chains longer than the
#  default 8 rounds; a deletion seen through, and the first character of an
#  expansion seen at distance 2; negated sets at the start of the text, and
#  the rules of one character made exclusive by order disambiguation.  The
#  first two cases (all with --extended-testing) also with one thread and with
#  four threads and segments of one stride.
#  The expected results are ICU's (createFromRules, each line a text).
#
#  tconv_converted_while_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

# (rules, [(line, expected result)])
CASES = [
    ("[ab] a { b → a ;", [
        ("aabbbbbbbbbbbbbbbbbbbbbbbbb", "aaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        ("babbbb", "baaaaa"),
        ("xaabbb abb", "xaaaaa abb"),
        ("b", "b"),
    ]),
    ("x { y → ;\na x { b → c ;\nc x { b → a ;", [
        ("axyyyb", "axc"),
        ("axbxyb", "axcxa"),
        ("xyb", "xb"),
        ("axyb xb", "axc xb"),
        ("axbxbxbxbxbxbxbxbxbxbxbxb", "axcxaxcxaxcxaxcxaxcxaxcxa"),
    ]),
    ("q → pq ;\np q { r → s ;\nq s { r → q ;\ns { r → p ;", [
        ("qrrrrrrrrrrrr", "pqsqrrrrrrrrrr"),
        ("rqr", "rpqs"),
        ("pqr", "ppqs"),
        ("qqrr", "pqpqsq"),
    ]),
    ("[^a] b { c → a ;\na [^b] { c → b ;\nc → d ;", [
        ("c", "d"),
        ("bc", "ba"),
        ("acc", "adb"),
        ("bccccccccccccccc", "badbadbadbadbadb"),
        ("abc", "abd"),
    ]),
    ("a [ab] a { b → a ;\nb b a { a → b ;", [
        ("aaabbbbbbbbbbbbbbbbbbbb", "aaaaaaaaaaaaaaaaaaaaaaa"),
        ("bbaaaaaaaaaaaa", "bbabaaaaaaaaaa"),
        ("abab", "abaa"),
    ]),
]

CONFIGS = [[], ["-thread-num=1"], ["-thread-num=4", "-segment-size=1"]]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n, (rules, lines) in enumerate(CASES):
            rules_path = os.path.join(tmp, "rules%d.txt" % n)
            with open(rules_path, "w", encoding="utf-8") as f:
                f.write(rules + "\n")
            input_path = os.path.join(tmp, "input%d.txt" % n)
            with open(input_path, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in lines))
            expected = "".join(result + "\n" for _, result in lines)
            for config in (CONFIGS if EXTENDED or n < 2 else CONFIGS[:1]):
                args = [tconv, "--use-ICU=0", "--apply-shape-groups=0", "--divcode-groups=0", "--utf8-mode=0",
                        "--lines", "--rules-file=" + rules_path] + config + [input_path]
                r = subprocess.run(args, capture_output=True)
                what = "rules %d %s" % (n, " ".join(config) or "default")
                if r.returncode != 0:
                    failures += 1
                    print("FAIL %s: exit %d: %s" % (what, r.returncode, r.stderr.decode("utf-8", "replace")[:300]))
                    continue
                got = r.stdout.decode("utf-8")
                if got != expected:
                    failures += 1
                    for (line, want), have in zip(lines, got.split("\n")):
                        if have != want:
                            print("FAIL %s: %r gives %r, expected %r" % (what, line, have, want))
    print("tconv_converted_while_test: %s" % ("%d failures" % failures if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
