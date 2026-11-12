#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: runs split by ldml::SplitLeadingRuns where an earlier rule matches
#  at the last character of the run ($wspace U+1037: the rule of the run's
#  last two characters placed before it), and a run within a text split by
#  ldml::SplitMiddleRuns with a marker (U+1031 U+1037+ ($consonant) → $1
#  U+1031 U+1037), from Zawgyi-my's third group.  The expected results are
#  ICU's (createFromRules, each line a text).
#
#  tconv_split_runs_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$wspace = [ \u00a0\u200b];\n"
     "$consonant = [\u1000-\u1021];\n"
     "($wspace) \u1037 > \u1037 $1;\n"
     "($wspace+) ([\u102b-\u1030\u1032-\u103b\u103d\u103e]) \u2192 $2;\n"
     "\u1037+ \u2192 \u1037;\n"
     "\u1031+ \u1037+ ($consonant) > $1 \u1031 \u1037 ;\n"
     "\u1031+ ($consonant) > $1 \u1031;\n",
     [(" \u1037", " \u1037"), (" \u1037\u1037", " \u1037"), ("  \u1037", "  \u1037"),
      (" \u102d", " \u102d"), ("  \u102d", "  \u102d"), ("\u00a0\u200b \u102f", "\u00a0\u200b \u102f"),
      ("\u1031\u1037\u1000", "\u1000\u1031\u1037"), ("\u1031\u1031\u1037\u1037\u1010", "\u1010\u1031\u1037"), (" \u1031\u1037\u1037\u1037\u1001", " \u1001\u1031\u1037"),
      ("\u1031\u1037x", "\u1031\u1037x"), ("\u1037\u1037\u1037", "\u1037"), ("\u1031\u1000", "\u1000\u1031"),
      ("\u1031\u1031\u1000", "\u1000\u1031"), (" \u1031\u1037\u1000 \u1037", " \u1000\u1031\u1037 \u1037"), ("x  \u103b", "x  \u103b")]),
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
