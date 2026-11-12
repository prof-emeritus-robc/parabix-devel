#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a rule inserting a string between its captures and revisiting all,
#  ($c) ($c) → | $1 a $2, applied as an insertion rule before its group (see
#  ldml::ComposeRevisits, insertionRevisit), also where it is not the first rule
#  of its group: the rules before it that begin with a character of the first
#  capture continue with a character not in the second (c y → Z), and a rule
#  with a character of the first capture within its text is followed there by
#  a character not in the second (q c e → Q).  With a rule before it that may
#  apply where it matches (c → K), it is not applied so: the rules are then
#  either not implementable (exit status 2) or as ICU applies them.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_insertion_revisit_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("$c = [bcdfg] ;\n"
         "x → X ;\n"
         "c y → Z ;\n"
         "($c) ($c) → | $1 a $2 ;\n"
         "b → B ;\nc → C ;\nd → D ;\nf → F ;\ng → G ;\n"
         "q c e → Q ;\n")

# (line, expected result)
CASES = [
    ('bc', 'BaC'),
    ('bcd', 'BaCaD'),
    ('cy', 'Z'),
    ('cyb', 'ZB'),
    ('xbc', 'XBaC'),
    ('qce', 'Q'),
    ('qceb', 'QB'),
    ('bqce', 'BQ'),
    ('bb', 'BaB'),
    ('b', 'B'),
    ('cyc', 'ZC'),
    ('gfdcb', 'GaFaDaCaB'),
]

BLOCKED = "$c = [bcdfg] ;\nc → K ;\n($c) ($c) → | $1 a $2 ;\nb → B ;\nd → D ;\n"
BLOCKED_CASES = [('bc', 'BaK'), ('cb', 'KB'), ('bcd', 'BaKD'), ('cc', 'KK')]


def run(tconv, rules_text, cases):
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(rules_text)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in cases))
        return subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)


def compare(r, cases):
    got = r.stdout.decode("utf-8", "replace").split("\n")
    failures = 0
    for i, (line, want) in enumerate(cases):
        if i >= len(got) or got[i] != want:
            failures += 1
            print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    return failures


def main():
    tconv = sys.argv[1]
    r = run(tconv, RULES, CASES)
    if r.returncode != 0:
        print("FAIL: exit status %d\n%s" % (r.returncode, r.stderr.decode("utf-8", "replace")))
        return 1
    failures = compare(r, CASES)
    r = run(tconv, BLOCKED, BLOCKED_CASES)
    if r.returncode == 0:
        failures += compare(r, BLOCKED_CASES)
    elif r.returncode != 2:
        print("FAIL (blocked): exit status %d\n%s" % (r.returncode, r.stderr.decode("utf-8", "replace")))
        failures += 1
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
