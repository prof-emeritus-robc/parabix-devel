#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: chain revisits, (E M A*) X → | $1 Y ; with Y in A, which ICU applies
#  again on the capture extended by Y, through a run of A and X (see
#  ComposeRevisits in include/ldml/transform_rules.h).  The rules file must be
#  planned with its chain revisits in a group of their own (tconv --plan),
#  with E extended by the characters that the group revisits as one of E
#  (j → | i ;, and v → | u ; where v is not followed by a vowel), and its
#  result must be ICU's (as given by its createFromRules on the same rules),
#  including on a run longer than a block.
#
#  tconv_chain_revisits_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

LONG = 600

RULES = r"""
$e = [aeiouAEIOU] ;
$acc = [[̀-ͅ] - [̸]] ;
x → y ;
($e ̄ $acc*) i → | $1 ͅ ;
($e ̄ $acc*) I → | $1 ͅ ;
j → | i ;
v } $e → | b ;
v → | u ;
a ̄ → ā ;
a → α ;
i → ι ;
u → υ ;
b → β ;
ͅ → ι̲ ;
"""

TEXT = ("āīíI ai j̄i v̄I va x̄i ī̸i\n"
        "ā" + "́i" * LONG + " ēi\n")

WANT = ("āῑ̲ί̲ι̲ αι ῑι̲ "
        "ῡι̲ βα ȳι ῑ̸ι\n"
        "ā" + "́ι̲" * LONG + " ēι̲\n")


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
        if "chain revisits: 2 rules" not in out or ": implementable" not in out or "[AEIOUaeijouv]" not in out:
            failures += 1
            print("FAIL: not planned as chain revisits (with j and v in E)\n%s" % out)
        run = subprocess.run([tconv, "--rules-file=" + rules, input], capture_output=True)
        got = run.stdout.decode("utf-8", "replace")
        if run.returncode != 0 or got != WANT:
            failures += 1
            print("FAIL:\n  want %r\n  got  %r\n%s" % (WANT[:120], got[:120], run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
