#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a capture of a fixed string is replaced by that string, so that the
#  rule may be a string rule (see FixedCaptureElimination in
#  include/ldml/transform_rules.h): e { (̆) r → r $1 ; is e { ̆ r → r ̆ ;,
#  whose before context is matched against the converted text, as ICU matches
#  it.  And a capture of a set with strings of two characters takes the
#  longest string, as ICU does, by the walks of the matches (see
#  MultipleCaptureMultipleReplacementSet::walkStarts in
#  include/ldml/transform_plan.h): ̄on → on1, not o1n.  The rules (as those of
#  Latin-NumericPinyin) must be planned that way (tconv --plan), and their
#  result must be ICU's (as given by its createFromRules on the same rules),
#  including on a line longer than a block.
#
#  tconv_fixed_captures_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

LONG = 400

RULES = r"""
e { (̆) r → r $1 ;
(̄) ([{on} {ng} i n o u]) → $2 1 ;
̄ → 1 ;
̆ → 5 ;
::Null ;
"""

TEXT = ("ĕr ̄on ̄ong ̄o ̄n̄ ā ̄̄on ĕx\n"
        + "̄on" * LONG + "\n")

WANT = ("er̆ on1 on1g o1 n11 a1 1on1 e5x\n"
        + "on1" * LONG + "\n")


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
        if "string rule: e { ̆r → r̆ ;" not in out or "walk branches" not in out or ": implementable" not in out:
            failures += 1
            print("FAIL: not planned with the fixed capture as a string rule, and the strings by walks\n%s" % out)
        run = subprocess.run([tconv, "--rules-file=" + rules, input], capture_output=True)
        got = run.stdout.decode("utf-8", "replace")
        if run.returncode != 0 or got != WANT:
            failures += 1
            print("FAIL:\n  want %r\n  got  %r\n%s" % (WANT[:120], got[:120], run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
