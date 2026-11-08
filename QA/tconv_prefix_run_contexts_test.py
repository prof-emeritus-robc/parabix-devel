#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a prefix run that only deletes (t \u0361 → | t, A empty; see
#  ldml::ComposeRevisits, prefixRunConflict), applied before its group although
#  the after context of another rule (^ { t } t → X) may see its P: the last
#  item of an after context sees the P that stays in place, as ICU does.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_prefix_run_contexts_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("^ { t } t \u2192 X ;\n"
         "t \\u0361 \u2192 | t ;\n"
         "t \u2192 T ;\n")

# (line, expected result)
CASES = [
    ('tt', 'XT'),
    ('t\u0361t', 'XT'),
    ('tt\u0361', 'XT'),
    ('t\u0361\u0361t', 'XT'),
    ('xt\u0361t', 'xTT'),
    ('tt\u0361a', 'XTa'),
    ('t\u0361', 'T'),
    ('at\u0361t', 'aTT'),
    ('a', 'a'),
    ('t', 'T'),
]


def main():
    tconv = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)
    if run.returncode != 0:
        print("FAIL: exit status %d" % run.returncode + "\n" + run.stderr.decode("utf-8", "replace"))
        return 1
    got = run.stdout.decode("utf-8", "replace").split("\n")
    failures = 0
    for i, (line, want) in enumerate(CASES):
        if i >= len(got) or got[i] != want:
            failures += 1
            print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
