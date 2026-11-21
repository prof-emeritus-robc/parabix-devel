#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a whole group whose string matches are selected sequentially (two-way
#  or cyclic overlaps, TransformStep::sequentialSelection), with string rules
#  whose before contexts are matched against the converted text, selected in
#  the correction kernel's While loop (FusedRulesConfig::sequential,
#  --sequential-while; doc/round-replacement.md step 6) instead of staged
#  rounds of matching, LeftmostMatches and applying kernels.  The loop is exact
#  at any chain length: chains of conversions longer than the rounds, also
#  across blocks.  The expected results are ICU's (createFromRules, each line
#  a text).  With --sequential-while=0 the chains of 9 or more fail.
#
#  tconv_sequential_while_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

GROUPS = [
    # Two-way overlaps (bc, cb), a longer rule over both, and a converted
    # context that a rule's own result continues ([aQ] { bc).
    ("x \u2192 a ;\n[aQ] { bc \u2192 Q ;\ncb \u2192 R ;\nbcb \u2192 S ;\n",
     [("x" + "bc" * 3, "aQQQ"),
      ("x" + "bc" * 9, "a" + "Q" * 9),
      ("x" + "bc" * 15, "a" + "Q" * 15),
      ("x" + "bc" * 12 + "b", "a" + "Q" * 12 + "b"),
      ("cb" * 5 + "xbcbcb", "RRRRRaQQb"),
      ("xbcbc abcb xcbcb bcbcbc", "aQQ aQb aRR SRc"),
      ("x" + "bc" * 700 + "b", "a" + "Q" * 700 + "b")]),
    # A cycle of three rules (pq, qr, rp), with a converted chain before it.
    ("x \u2192 a ;\n[aT] { st \u2192 T ;\npq \u2192 P ;\nqr \u2192 Q ;\nrp \u2192 R ;\n",
     [("xpqrpqrp", "aPRQp"),
      ("xststst" + "pqr" * 5, "aTTTPRQPRQPr"),
      ("qrpqrpq", "QPRq"),
      ("x" + "st" * 11 + "rpq", "a" + "T" * 11 + "Rq"),
      ("rpqx" + "st" * 10 + "pq", "Rqa" + "T" * 10 + "P"),
      ("x" + "st" * 700 + "pqr", "a" + "T" * 700 + "Pr")]),
]

CONFIGS = [[], ["-thread-num=4", "-segment-size=1"]] + ([["-thread-num=1"]] if EXTENDED else [])


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        input = os.path.join(tmp, "input.txt")
        for rules_text, cases in GROUPS:
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rules_text)
            with open(input, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in cases))
            for config in CONFIGS:
                run = subprocess.run([tconv, "--use-ICU=0"] + config + ["--lines", "--rules-file=" + rules, input],
                                     capture_output=True)
                if run.returncode != 0:
                    failures += 1
                    print("FAIL %r %s: exit status %d\n%s" % (rules_text, " ".join(config), run.returncode,
                                                              run.stderr.decode("utf-8", "replace")))
                    continue
                got = run.stdout.decode("utf-8", "replace").split("\n")
                for i, (line, want) in enumerate(cases):
                    if i >= len(got) or got[i] != want:
                        failures += 1
                        print("FAIL %r %s with %r:\n  want %r\n  got  %r"
                              % (rules_text, " ".join(config), line[:40], want[:60], got[i][:60] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
