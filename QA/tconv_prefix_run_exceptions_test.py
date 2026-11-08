#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a prefix run that only deletes (t \u0361 \u2192 | t), with rules before it
#  that apply at its P S (t \u0361 s a \u2192 A, t \u0361 s } [\\u0020 $] \u2192 W): ICU
#  deletes S until P S X remains, to which it applies the rule, so the prefix
#  run is applied first except for the last S where X (and the rule's after
#  context) follows (see ldml::ComposeRevisits, prefixRunConflict).  The expected
#  results are ICU's (createFromRules, each line a text).
#
#  tconv_prefix_run_exceptions_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("t \\u0361 s a \u2192 A ;\n"
         "t \\u0361 s i \u2192 I ;\n"
         "t \\u0361 s } [\\u0020 $] \u2192 W ;\n"
         "t \\u0361 \u2192 | t ;\n"
         "t \u2192 T ;\n"
         "s \u2192 S ;\n")

# (line, expected result)
CASES = [
    ('t\u0361sa', 'A'),
    ('t\u0361\u0361sa', 'A'),
    ('t\u0361\u0361\u0361si', 'I'),
    ('t\u0361se', 'TSe'),
    ('t\u0361\u0361se', 'TSe'),
    ('tsa', 'TSa'),
    ('t\u0361x', 'Tx'),
    ('at\u0361si', 'aI'),
    ('t\u0361s', 'W'),
    ('t\u0361\u0361s', 'W'),
    ('t\u0361s b', 'W b'),
    ('t\u0361\u0361s b', 'W b'),
    ('t\u0361\u0361sat\u0361u', 'ATu'),
    ('t\u0361', 'T'),
    ('a', 'a'),
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
