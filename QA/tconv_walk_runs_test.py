#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: MultipleCaptureMultipleReplacement rules whose matches may end within
#  another, selected by possessive walks of several runs (from Zawgyi-my:
#  ($umedial*) (U+1031+) ($umedial*) → $1 $3 $2, whose variants walk a run of
#  medials, of U+1031, then of medials), and the regions of two sets moved
#  after their captures together (as one move).  The expected results are
#  ICU's (createFromRules, each line a text).
#
#  tconv_walk_runs_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$umedial = [\u103b-\u103e];\n"
     "($umedial*) ([\u1031]+) ($umedial*) \u2192 $1 $3 $2;\n",
     [("\u103c\u1031\u1031\u103e", "\u103c\u103e\u1031\u1031"), ("\u103e\u1031\u1031\u1031\u103d", "\u103e\u103d\u1031\u1031\u1031"), ("\u103b\u1031\u103d\u102d\u1032\u1039\u1031\u103e", "\u103b\u103d\u1031\u102d\u1032\u1039\u103e\u1031"),
      ("\u103c\u1031\u103e\u1031\u103e", "\u103c\u103e\u1031\u103e\u1031"), ("\u1031\u103e", "\u103e\u1031"), ("\u103e\u1031", "\u103e\u1031"),
      ("\u1031", "\u1031"), ("\u103c\u103d\u1031\u103b\u103e\u1000\u1031\u103b", "\u103c\u103d\u103b\u103e\u1031\u1000\u103b\u1031"), ("\u1031\u1031\u103b\u103c\u1031\u103d", "\u103b\u103c\u1031\u1031\u103d\u1031")]),
    ("$umedial = [\u103b-\u103e];\n"
     "$vowelsign = [\u102b-\u1030\u1032];\n"
     "$consonant = [\u1000-\u1021];\n"
     "($vowelsign+) \u1039 ($consonant) \u2192 \u1039 $2 $1;\n"
     "($umedial*) ([\u1031]+) ($umedial*) \u2192 $1 $3 $2;\n",
     [("\u102d\u1039\u1000", "\u1039\u1000\u102d"), ("\u102d\u102f\u1039\u1010\u103c\u1031\u103e", "\u1039\u1010\u102d\u102f\u103c\u103e\u1031"), ("\u103e\u1031\u102d\u1039\u1000", "\u103e\u1031\u1039\u1000\u102d"),
      ("\u103b\u1031\u103d\u102d\u1039\u1010\u1031\u103e", "\u103b\u103d\u1031\u1039\u1010\u102d\u103e\u1031"), ("\u1031\u103b\u102f\u1039a", "\u103b\u1031\u102f\u1039a"), ("\u102d\u1039\u1000\u103c\u1031\u1031\u103d\u103e", "\u1039\u1000\u102d\u103c\u103d\u103e\u1031\u1031")]),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for rules_text, cases in GROUPS:
            rules = os.path.join(tmp, "rules.txt")
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rules_text)
            input = os.path.join(tmp, "input.txt")
            with open(input, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in cases))
            run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)
            if run.returncode != 0:
                failures += 1
                print("FAIL %r: exit status %d\n%s" % (rules_text, run.returncode, run.stderr.decode("utf-8", "replace")))
                continue
            got = run.stdout.decode("utf-8", "replace").split("\n")
            for i, (line, want) in enumerate(cases):
                if i >= len(got) or got[i] != want:
                    failures += 1
                    print("FAIL %r with %r:\n  want %r\n  got  %r" % (line, rules_text, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
