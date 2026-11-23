#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: a whole-group correction kernel resolved by the While loop, without
#  sequential selection, with string rules that the loop does not decide
#  (static: before contexts not matched against the converted text, and not
#  beginning within a match of a rule the loop decides).  These are selected
#  (SelectStringMatches, by their predecessors) and applied in chunk kernels
#  before the kernel, their results given to it as its state, and the rules
#  it decides begin only outside their applied matches (--static-apply-chunks;
#  RulePipelineBuilder::conversionGroup).  The group has a one-way overlap
#  (bcd, cd), a converted string rule, a replacement longer than its text and
#  subgroups whose before contexts are matched against the converted text, so
#  their coverage by the static matches matters; one of two items (Q Q), which
#  DivCode does not take, so the group takes this path by default.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  xlit_static_apply_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

RULES = ("x \u2192 a ;\n[aQ] { b \u2192 Q ;\nbcd \u2192 V ;\ncd \u2192 W ;\nmn \u2192 MNO ;\nk \u2192 kh ;\n"
         "Q Q { d \u2192 D ;\nd \u2192 e ;\npq \u2192 P ;\nq \u2192 r ;\n")
CASES = [("xbbd", "aQQD"),
         ("xbcdbd", "aQWbe"),
         ("mnxbd", "MNOaQe"),
         ("kcdxbbbbbbbbbbbd", "khWaQQQQQQQQQQQD"),
         ("bcdcdpqq", "VWPr"),
         ("xbmnbbd", "aQMNObbe"),
         ("qpqxbcd", "rPaQW"),
         ("xbd", "aQe")]

CONFIGS = [[], ["--static-apply-chunks=0"], ["-thread-num=4", "-segment-size=1"]] + ([["-thread-num=1"]] if EXTENDED else [])


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        input = os.path.join(tmp, "input.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        for config in CONFIGS:
            run = subprocess.run([xlit, "--use-ICU=0", "--lines"] + config + ["--rules-file=" + rules, input],
                                 capture_output=True)
            if run.returncode != 0:
                failures += 1
                print("FAIL %s: exit status %d\n%s" % (" ".join(config), run.returncode, run.stderr.decode("utf-8", "replace")))
                continue
            got = run.stdout.decode("utf-8", "replace").split("\n")
            for i, (line, want) in enumerate(CASES):
                if i >= len(got) or got[i] != want:
                    failures += 1
                    print("FAIL %s with %r:\n  want %r\n  got  %r" % (" ".join(config), line, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
