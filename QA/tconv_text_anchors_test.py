#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: the anchors of the texts of rules (see ldml::MoveTextAnchors), as ICU
#  matches them: ^ at the start of a text and $ at its end are its contexts,
#  and a set including the text boundary as the last item of a text ([^b],
#  [x $]) matches a character of the set or, matching nothing, the end of the
#  text (a [^b] replaces a final a).  The expected results are ICU's
#  (createFromRules, each line a text).
#
#  tconv_text_anchors_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("a [^b] \u2192 X ;\n"
         "^ k } g \u2192 Y ;\n"
         "n $ \u2192 N ;\n"
         "$B = [\\u0020 $] ;\n"
         "q $B \u2192 Z ;\n"
         "([\\-\\.]) \\u0020 [$] \u2192 $1 ;\n")

# (line, expected result)
CASES = [
    ('a', 'X'),
    ('ac', 'X'),
    ('ab', 'ab'),
    ('kg', 'Yg'),
    ('xkg', 'xkg'),
    ('k', 'k'),
    ('en', 'eN'),
    ('ene', 'ene'),
    ('xq', 'xZ'),
    ('xq y', 'xZy'),
    ('xqy', 'xqy'),
    ('o- ', 'o-'),
    ('o- p', 'o- p'),
    ('o.', 'o.'),
    ('o. ', 'o.'),
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
