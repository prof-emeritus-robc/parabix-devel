#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: rules keeping the characters of their captures, whose before contexts
#  must be matched against the converted text (as blt-blt_FONIPA's tone rules),
#  planned as string rules keeping those characters (see StringRule::kept in
#  include/ldml/transform_plan.h): $L { ($V $F) → $1 1 ; inserts 1 after a final
#  consonant, which may be the $L that the next syllable's before context sees
#  (aobub → aob1ub, as ICU), and $L $V { 9 ($F) → $1 2 ; deletes the 9.  The
#  next group's {($L $V $F?)} [^$T] → $1 3 ; matches $F? possessively (no 3
#  where $F is there, followed by a 1).  The rules must be planned that way
#  (tconv --plan), and their result must be ICU's (as given by its
#  createFromRules on the same rules), including on a line longer than a block.
#
#  tconv_kept_captures_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

LONG = 300

RULES = r"""
$L = [a b] ;
$V = [o u] ;
$F = [a b k] ;
$T = [1 2 3] ;
$L { ($V $F) → $1 1 ;
$L $V { 9 ($F) → $1 2 ;
::Null ;
{($L $V $F?)} [^$T] → $1 3 ;
::Null ;
"""

TEXT = ("aobub aok bo9k ao9bo ouk aobo\n"
        + "aob" * LONG + "\n")

WANT = ("aob1ub aok1 bok2 aob2o ouk aob1o\n"
        + "aob1" * LONG + "\n")


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
        if (out.count("before context matched against the converted text") != 2 or "walk branches" not in out
                or ": implementable" not in out):
            failures += 1
            print("FAIL: not planned with kept captures and converted before contexts\n%s" % out)
        run = subprocess.run([tconv, "--rules-file=" + rules, input], capture_output=True)
        got = run.stdout.decode("utf-8", "replace")
        if run.returncode != 0 or got != WANT:
            failures += 1
            print("FAIL:\n  want %r\n  got  %r\n%s" % (WANT[:120], got[:120], run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
