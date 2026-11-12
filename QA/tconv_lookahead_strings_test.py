#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: after contexts that repeat a set with strings, matched by lookahead
#  chains (W10, from cy-cy_FONIPA).  ICU repeats $cons possessively, taking
#  the longest string at each step, which re::resolvePossessive makes
#  unambiguous (m(?!U+0325) | m U+0325): the chain's string class takes such
#  guarded strings (an occurrence counts where no guard follows), and the
#  assertions before the end of the text that hold there are dropped.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_lookahead_strings_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$end = [$ ];\n"
     "$cons = [ m {m\u0325} n t d {d\u0361\u0292} s ];\n"
     "y } $cons* $end \u2192 \u0268 ;\n"
     "y \u2192 Y ;\n",
     [("ym", "\u0268m"), ("ym ", "Ym "), ("ym\u0325", "\u0268m\u0325"), ("ym\u0325 ", "Ym\u0325 "),
      ("ymx", "Ymx"), ("ym\u0325x", "Ym\u0325x"), ("ym\u0325\u0325", "Ym\u0325\u0325"), ("yd\u0361\u0292", "\u0268d\u0361\u0292"),
      ("yd\u0361\u0292a", "Yd\u0361\u0292a"), ("yd\u0361", "Yd\u0361"), ("yd\u0361a", "Yd\u0361a"), ("yd\u0292", "Yd\u0292"),
      ("ynst ", "Ynst "), ("y", "\u0268"), ("y ", "Y "), ("ya", "Ya"),
      ("yy", "Y\u0268"), ("ym\u0325m\u0325d", "\u0268m\u0325m\u0325d"), ("yd\u0361\u0292\u0325", "Yd\u0361\u0292\u0325"), ("ym\u0361\u0292", "Ym\u0361\u0292")]),
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
