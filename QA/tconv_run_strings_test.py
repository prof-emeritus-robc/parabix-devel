#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: runs of sets with strings, [{i\u032f} i ɪ]+ → i (see
#  ldml::StringRule::runStrings), as ICU matches them: the longest string at
#  each step, each mark U+032F in the run only after its i (or u).  The run
#  rules are applied alone and selected with other string rules.  The expected
#  results are ICU's (createFromRules, each line a text).
#
#  tconv_run_strings_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("[{i\u032f} i ɪ]+ → i ;\n"
         "[{u\u032f} u w]+ → u ;\n"
         "ab → X ;\n"
         "b\u032f → Y ;\n")

# (line, expected result)
CASES = [
    ('i', 'i'),
    ('ii\u032f', 'i'),
    ('i\u032fi', 'i'),
    ('ii\u032fɪ', 'i'),
    ('\u032f', '\u032f'),
    ('i\u032f\u032f', 'i\u032f'),
    ('iɪa', 'ia'),
    ('ai\u032fb', 'aib'),
    ('ɪi\u032f', 'i'),
    ('i\u032fɪ\u032f', 'i\u032f'),
    ('u\u032fwu', 'u'),
    ('ab', 'X'),
    ('abi\u032f', 'Xi'),
    ('iu\u032fi', 'iui'),
    ('b\u032fi', 'Yi'),
]


def run(tconv, rules_text):
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(rules_text)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        return subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)


def main():
    tconv = sys.argv[1]
    failures = 0
    #  (The run rules alone, and with the other rules: their matches then
    #  selected together.)
    for rules_text, cases in ((RULES.split("\n")[0] + "\n", None), (RULES, CASES)):
        r = run(tconv, rules_text)
        if r.returncode != 0:
            print("FAIL: exit status %d\n%s" % (r.returncode, r.stderr.decode("utf-8", "replace")))
            return 1
        if cases is None:
            continue
        got = r.stdout.decode("utf-8", "replace").split("\n")
        for i, (line, want) in enumerate(cases):
            if i >= len(got) or got[i] != want:
                failures += 1
                print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
