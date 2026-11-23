#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: a conversion group split by its independent components
#  (ldml::SplitIndependentComponents, --split-components): the components with
#  before contexts (a { b, a, and x { y, whose context is the result of a; pq
#  and q, which change the length, so they stay with the contexts), then the
#  others as a group of their own (d, e, m).  d → e and e → f stay together
#  (one pass: de gives ef, not ff).  Split (--split-components=2), checked in
#  the plan, and not split (=0); the expected results are ICU's
#  (createFromRules, each line a text).
#
#  xlit_split_components_test.py <xlit binary>

import os
import subprocess
import sys
import tempfile

RULES = ("a { b \u2192 c ;\na \u2192 x ;\nx { y \u2192 z ;\nd \u2192 e ;\ne \u2192 f ;\nm \u2192 n ;\n"
         "pq \u2192 r ;\nq \u2192 s ;\n")
CASES = [("ab", "xb"), ("xyab", "xzxb"), ("de", "ef"), ("ede", "fef"), ("mnpq", "nnr"),
         ("qab", "sxb"), ("xy", "xz"), ("axy", "xxz"), ("bde", "bef")]
CONFIGS = [["--split-components=2"], ["--split-components=0"], ["--split-components=2", "-thread-num=4", "-segment-size=1"]]


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
        plan = subprocess.run([xlit, "--plan", "--split-components=2", "--rules-file=" + rules], capture_output=True)
        groups = [l for l in plan.stdout.decode("utf-8", "replace").split("\n") if l.startswith("  conversion group")]
        if "  transform Null" not in plan.stdout.decode("utf-8", "replace").split("\n") or len(groups) != 2:
            failures += 1
            print("FAIL: the group is not split in the plan\n%s" % plan.stdout.decode("utf-8", "replace"))
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
