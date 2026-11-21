#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: MultipleCaptureMultipleReplacement rules whose captures are fixed
#  sequences of sets, planned as string rules keeping the characters of their
#  captures (--capture-strings, StringRule::kept), matched in the fused kernels
#  rather than one regular expression pipeline for each rule (as am_FONIPA-am's
#  gemination rules, (t͡ʃʼ) ː (j $IPA_VOWEL) ː → $1 ፟ $2 ፞ ;).  A rule with
#  an optional set within its capture, ([hkmn] q w? $V q) → $1 4 ;, stays a
#  MultipleCaptureMultipleReplacement rule: a kept string rule only takes an
#  optional set at the end of its text (before, its absent form had the after
#  context of the optional set, so that nqiqw gave nqiq5w).  The rules must be
#  planned that way (xlit --plan), and their results must be ICU's (as given
#  by its createFromRules on the same rules, each line a text), with and
#  without --capture-strings, also on a line longer than a block.
#
#  xlit_fused_captures_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

RULES = r"""
$V = [aeiou] ;
(t) q (j $V) q → $1 1 $2 2 ;
([ps]) qwa } [^q] → $1 wa3 ;
([hkmn] q w? $V q) → $1 4 ;
([hkmn] qw $V) } [^q] → $1 6 ;
($V q) → $1 5 ;
"""

CASES = [
    ("tqjaq", "t1ja2"), ("pqwa", "pwa3"), ("pqwaq", "pqwaq5"), ("nqiq", "nqiq4"), ("nqwiq", "nqwiq4"),
    ("nqiqw", "nqiq4w"), ("nqiqwa", "nqiq4wa"), ("aq", "aq5"), ("kqaqtqjeq", "kqaq4t1je2"), ("sqwab", "swa3b"),
    ("mqwe", "mqwe6"), ("mqweq", "mqweq4"), ("hqoqpqwaxnqwuqiq", "hqoq4pwa3xnqwuq4iq5"),
    ("kqaqtqjeq" * 80, "kqaq4t1je2" * 80),
]

PLANNED = ["string rule: tq (j $V) q → t1 $1 2 ;", "string rule: ([ps]) qwa } [^q] → $1 wa3 ;",
           "SingleCaptureDoubleReplacement rules"]


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        plan = subprocess.run([xlit, "--plan", "--rules-file=" + rules], capture_output=True).stdout.decode("utf-8", "replace")
        if ": implementable" not in plan or any(p not in plan for p in PLANNED):
            failures += 1
            print("FAIL: not planned with the captures as string rules\n%s" % plan)
        for config in [[], ["--capture-strings=0"]]:
            run = subprocess.run([xlit, "--use-ICU=0"] + config + ["--lines", "--rules-file=" + rules, input], capture_output=True)
            if run.returncode != 0:
                failures += 1
                print("FAIL %s: exit status %d\n%s" % (" ".join(config), run.returncode, run.stderr.decode("utf-8", "replace")))
                continue
            got = run.stdout.decode("utf-8", "replace").split("\n")
            for i, (line, want) in enumerate(CASES):
                if i >= len(got) or got[i] != want:
                    failures += 1
                    print("FAIL %s with %r:\n  want %r\n  got  %r" % (" ".join(config), line[:40], want[:60],
                                                                  got[i][:60] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
