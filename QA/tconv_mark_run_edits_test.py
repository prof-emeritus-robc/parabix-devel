#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: mark run edits, P ($R*) T → C | $1 ; which replace P by C, delete T
#  and revisit the run of marks between (see ComposeRevisits in
#  include/ldml/transform_rules.h).  The rules file must be planned with its
#  mark run edits in a group of their own (tconv --plan), with the rule
#  before them that ICU applies instead (t ̣ → T ;) kept there, and in two
#  families (runs ending with ̈ or ̄, and with ̉), and its result must be
#  ICU's (as given by its createFromRules on the same rules), including on a
#  run longer than a block and with a mark outside the filter.
#
#  tconv_mark_run_edits_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

LONG = 700

RULES = r"""
:: [a-z ̀-̰] ;
$m = [̖-̰] ;
t ̣ → T ;
t ($m+) ̈ → Q | $1 ;
s ̣ ($m*) ̄ → S | $1 ;
s ($m*) ̄ → X | $1 ;
a ($m*) ̉ → Y | $1 ;
t → τ ;
s → σ ;
a → α ;
̣ → d ;
̰ → u ;
"""

TEXT = ("ẗ̰ ṭ̈ ẗ ṣ̰̄ s̰̄ s̄ ạ̉ ả̄ "
        "ţ̰̈ ẗ̰̱\n"
        "t" + "̰" * LONG + "̈ ṣ" + "̰" * 300 + "̄a\n")

WANT = ("Qu T̈ τ̈ Su Xu X Yd Ȳ Qu̧ τü̱\n"
        "Q" + "u" * LONG + " S" + "u" * 300 + "α\n")


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
        if "mark run edits: 4 rules, 1 kept" not in out or ": implementable" not in out:
            failures += 1
            print("FAIL: not planned as mark run edits\n%s" % out)
        run = subprocess.run([tconv, "--rules-file=" + rules, input], capture_output=True)
        got = run.stdout.decode("utf-8", "replace")
        if run.returncode != 0 or got != WANT:
            failures += 1
            print("FAIL:\n  want %r\n  got  %r\n%s" % (WANT[:120], got[:120], run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
