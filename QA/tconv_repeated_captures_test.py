#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: rules referring more than once to a capture of a character of a set
#  (([bcd]) \u02d0 \u2192 $1 $1, as sk_FONIPA-ja's length mark doubling a
#  consonant), replaced by a rule for each character (see
#  ldml::EnumerateRepeatedCaptures), also with two captures.  The expected
#  results are ICU's (createFromRules, each line a text).
#
#  tconv_repeated_captures_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("([bcd]) \u02d0 \u2192 $1 $1 ;\n"
         "([xy]) ([pq]) w \u2192 $2 $1 $2 ;\n"
         "dd \u2192 D ;\n"
         "\u02d0 \u2192 L ;\n")

# (line, expected result)
CASES = [
    ('bː', 'bb'),
    ('cːa', 'cca'),
    ('dːː', 'ddL'),
    ('ː', 'L'),
    ('abː', 'abb'),
    ('xpw', 'pxp'),
    ('yqw', 'qyq'),
    ('xqwz', 'qxqz'),
    ('dd', 'D'),
    ('bːcː', 'bbcc'),
    ('xpwyqw', 'pxpqyq'),
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
        print("FAIL: exit status %d\n%s" % (run.returncode, run.stderr.decode("utf-8", "replace")))
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
