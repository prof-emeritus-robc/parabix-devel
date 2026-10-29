#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: prefix runs, (P) S → A | $1 ; and p S → A | p ; which ICU applies
#  to each run P S^n to leave A^n P (see ComposeRevisits in
#  include/ldml/transform_rules.h).  Each rules file must be planned with its
#  prefix runs in a group of their own (tconv --plan), and its result must be
#  ICU's (as given by uconv -x on the same rules), including on a run longer
#  than a block.
#
#  tconv_prefix_runs_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

LONG = 600

# (LDML rules, text, ICU's result)
CASES = [
    # A capture revisited, a replacement of two characters, stray S deleted.
    (r"([ab]) \~ → xy | $1 ; a → A ; b → B ; \~ → ;",
     "a~~b c~ ~a b~~~ ab~a~\na" + "~" * LONG + "b\n",
     "xyxyAB c A xyxyxyB AxyBxyA\n" + "xy" * LONG + "AB\n"),
    # A character revisited, an empty replacement (the run deleted).
    ("t \\u0361 → | t ; t → T ; s → S ;",
     "t\u0361\u0361ts t\u0361 st\u0361\u0361\u0361 \u0361t\n",
     "TTS T ST \u0361T\n"),
    # A replacement of one character.
    (r"([xy]) \- → \= | $1 ; x → X ; y → Y ;",
     "x--y x- -x y---\n",
     "==XY =X -X ===Y\n"),
    # Replacements of three, zero and one characters together, within a filter.
    (r":: [a-f\~] ; ([ab]) \~ → xyz | $1 ; ([cd]) \~ → | $1 ; ([ef]) \~ → q | $1 ; a → A ; c → C ; e → E ;",
     "a~~c~~e~~ b~d~f~ g~a~ c~~~~X ~e\n",
     "xyzxyzACqqE xyzbdqf g~xyzA CX ~E\n"),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        argvs = []
        for i, (rule, text, _) in enumerate(CASES):
            rules = os.path.join(tmp, "rules%d.txt" % i)
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rule + "\n")
            input = os.path.join(tmp, "input%d.txt" % i)
            with open(input, "w", encoding="utf-8") as f:
                f.write(text)
            argvs.append([tconv, "--plan", "--rules-file=" + rules])
            argvs.append([tconv, "--rules-file=" + rules, input])
        # The runs of tconv, three at a time.
        with ThreadPoolExecutor(max_workers=3) as pool:
            runs = list(pool.map(lambda argv: subprocess.run(argv, capture_output=True), argvs))
        for i, (rule, _, want) in enumerate(CASES):
            plan, run = runs[2 * i: 2 * i + 2]
            if b"prefix runs: " not in plan.stdout or b": implementable" not in plan.stdout:
                failures += 1
                print("FAIL %s: not planned as a prefix run\n%s" % (rule, plan.stdout.decode("utf-8", "replace")))
            got = run.stdout.decode("utf-8", "replace")
            if run.returncode != 0 or got != want:
                failures += 1
                print("FAIL %s:\n  want %r\n  got  %r\n%s" % (rule, want[:80], got[:80], run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
