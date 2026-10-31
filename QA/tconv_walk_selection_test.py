#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: MultipleCaptureMultipleReplacement rules whose matches may end within
#  another (see MultipleCaptureMultipleReplacementSet::walkStarts in
#  include/ldml/transform_plan.h): their starts are found by the regular
#  expression engine, and those that apply are selected from the left by the
#  lengths of the branches of their possessive walks (an optional item, a set
#  with a string of two characters, a run followed by a suffix), together with
#  the matches of the other rules.  The rules file must be planned that way
#  (tconv --plan), and its result must be ICU's (as given by its
#  createFromRules on the same rules), including on runs longer than a block,
#  with a rule leaving its text unchanged before (\, ([0-9]) → \, $1 ;), and
#  at a character outside the filter (…, which ends ICU's text, where
#  [^0-9\.] matches; the rules are compound, as those of the CLDR transforms).
#
#  tconv_walk_selection_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

LONG = 700

RULES = r"""
:: [a-z \, \. 0-9] ;
\, ([0-9]) → \, $1 ;
\, ([^0-9]) → X $1 ;
\. ([0-9…]) → \. $1 ;
\. ([^0-9\.]) → Y $1 ;
(a b?) a → $1 Q ;
([xy] [{xy} z]) x → $1 W ;
(m n+) m → $1 R ;
::Null ;
"""

TEXT = (",5 ,a ,, x, .5 .… .a . aaa abaa aba xxyx xzx yxyxx mnm mnmnm mnnm mm\n"
        "m" + "n" * LONG + "mnm ,,5 " + "aa" * 300 + "a\n")

WANT = (",5 Xa X, xX .5 Y… Ya Y aQa abQa abQ xxyW xzW yxyWx mnR mnRnm mnnR mm\n"
        "m" + "n" * LONG + "Rnm X,5 " + "aQ" * 300 + "a\n")


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write(TEXT)
        plan = subprocess.run([tconv, "--plan", "--rules-file=" + rules], capture_output=True)
        out = plan.stdout.decode("utf-8", "replace")
        if out.count("walk branches") != 4 or ": implementable" not in out:
            failures += 1
            print("FAIL: not planned with selection by walks\n%s" % out)
        run = subprocess.run([tconv, "--rules-file=" + rules, input], capture_output=True)
        got = run.stdout.decode("utf-8", "replace")
        if run.returncode != 0 or got != WANT:
            failures += 1
            print("FAIL:\n  want %r\n  got  %r\n%s" % (WANT[:120], got[:120], run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
