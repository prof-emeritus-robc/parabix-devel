#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: after contexts repeating a set with strings, matched by the regular
#  expression engine in lookaheads with ICU's possessive matching (each
#  repetition takes the longest string of the set; see
#  ldml::repeatsStringsOrSequences and re::resolvePossessiveQuantifiers).
#  $cons* takes m̥ whole, so that yw } $cons* [x \u0325 \u0361] does not match
#  ywm̥ (as the greedy form would, by m then \u0325); $v+ takes ü (u \u0308)
#  before [b-d]+.  The expected results are ICU's.
#
#  tconv_possessive_lookahead_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("$cons = [{m\\u0325}{d\\u0361\\u0292} b d m] ;\n"
         "yw } $cons* [x \\u0325 \\u0361] → X ;\n"
         "$v = [{u\\u0308} u a] ;\n"
         "qq } $v+ [b-d]+ 5 → Q ;\n")

# (line, expected result)
CASES = [
    ("ywm\u0325", "ywm\u0325"),
    ("ywm\u0325x", "Xm\u0325x"),
    ("ywd\u0361x", "Xd\u0361x"),
    ("ywd\u0361\u0292", "ywd\u0361\u0292"),
    ("ywd\u0361\u0292x", "Xd\u0361\u0292x"),
    ("qqu\u0308b5", "Qu\u0308b5"),
    ("qqu\u0308\u0308b5", "qqu\u0308\u0308b5"),
    ("qqub5", "Qub5"),
    ("qqa5", "qqa5"),
]


def main():
    tconv = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        run = subprocess.run([tconv, "--use-ICU=0", "--rules-file=" + rules, input], capture_output=True)
    if run.returncode != 0:
        print("FAIL: exit status %d\n%s" % (run.returncode, run.stderr.decode("utf-8", "replace")))
        return 1
    got = run.stdout.decode("utf-8", "replace").split("\n")
    failures = 0
    for i, (line, want) in enumerate(CASES):
        if i >= len(got) or got[i] != want:
            failures += 1
            print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
