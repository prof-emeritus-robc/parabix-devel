#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: a group in the chunked staged rounds (--staged-chunk-rules) whose
#  string matches are selected in one correction kernel's While loop instead
#  (--static-loop, doc/round-replacement.md, remaining rounds, step 3): the
#  string rules whose before contexts are matched against the converted text
#  (the core) decided in the loop with the subgroups, the others (static)
#  matched once in chunks and decided in the loop as classes by the lengths of
#  their texts, their results read from StartResults; StaticReplay applies the
#  static matches selected.  The groups here are small, so the chunked path is
#  forced (--staged-chunk-rules=0, and --whole-group-rules with only the core
#  and the subgroups within the limit) in Unicode mode (--utf8-mode=0; the loop is not used with the
#  absolute results of UTF-8 mode).  Static results that a converted context
#  reads: a static rule writing a character of the context's set (zz -> Q), one
#  inserting positions whose last character is in it (mn -> MNQ), one deleting
#  its text (pq), and two-way overlaps among the static rules (cb, bcb) and
#  with the core (bc); chains longer than the rounds, also across blocks.  The
#  expected results are ICU4C's and ICU4J's (identical).  The staged rounds
#  (--static-loop=0) fail the chains of 9 or more.
#
#  xlit_static_loop_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

GROUPS = [
    ("x \u2192 a ;\n[aQ] { bc \u2192 Q ;\ncb \u2192 R ;\nbcb \u2192 S ;\nmn \u2192 MNQ ;\nzz \u2192 Q ;\npq \u2192 ;\n", 2,
     [("x" + "bcpq" * 12, "a" + "Q" * 12),
      ("mn" + "bc" * 12, "MNQ" + "Q" * 12),
      ("zz" + "bczz" * 12, "Q" * 25),
      ("x" + "bcpq" * 700 + "bc", "a" + "Q" * 701),
      ("mn" + "bcmn" * 300, "MNQ" + "QMNQ" * 300),
      ("ybcbc", "ySc"),
      ("zbcbc", "zSc"),
      ("zzbcbcb", "QQQb"),
      ("mnbcbcb", "MNQQQb"),
      ("x" + "bc" * 5 + "pq" + "bc" * 5 + "mn" + "bc" * 5 + "zz" + "bc" * 5, "aQQQQQQQQQQMNQQQQQQQQQQQQ"),
      ("pqbc", "bc"),
      ("xpqpqpqbc", "aQ"),
      ("cbcbcbmnbc", "RRRMNQQ")]),
    # (The group of xlit_staged_chunks_test.py, with a subgroup matched against
    # the converted text and an insertion count among the static rules.)
    ("x \u2192 a ;\n[aQ] { bc \u2192 Q ;\ncb \u2192 R ;\nbcb \u2192 S ;\nmn \u2192 MNO ;\n"
     "k \u2192 kh ;\n[Q] { d \u2192 D ;\nd \u2192 e ;\n", 3,
     [("xbcbcd", "aQQD"),
      ("cbcbxbcd", "RRaQD"),
      ("x" + "bc" * 12 + "d", "a" + "Q" * 12 + "D"),
      ("xbcmnbcd", "aQMNObce"),
      ("x" + "bc" * 700 + "d", "a" + "Q" * 700 + "D")]),
]

OPTIONS = ["--use-ICU=0", "--utf8-mode=0", "--staged-chunk-rules=0"]
CONFIGS = [[], ["-thread-num=4", "-segment-size=1"]] + ([["-thread-num=1"]] if EXTENDED else [])


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        input = os.path.join(tmp, "input.txt")
        for rules_text, limit, cases in GROUPS:
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rules_text)
            with open(input, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in cases))
            for config in CONFIGS:
                run = subprocess.run([xlit] + OPTIONS + ["--whole-group-rules=%d" % limit] + config
                                     + ["--lines", "--rules-file=" + rules, input],
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
