#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: run rules (a text followed by the longest run of a set, deleted)
#  with an after context, which holds after the run (S* A from the end of the
#  text): ^ { a+ } [^bd], a+ $, and Zawgyi-my's last group (the U+200B runs at
#  the start and end of a text, among the runs of spaces).  The expected
#  results are ICU's (createFromRules, each line a text).
#
#  tconv_run_contexts_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("^ { a+ } [^bd] \u2192 ;\n"
     "a+ $ \u2192 ;\n"
     "a b+ } c \u2192 X ;\n"
     "a \u2192 q ;\n"
     "b \u2192 y ;\n",
     [("aab", "qqy"), ("aa", ""), ("ba", "y"), ("baa", "y"),
      ("abbc", "Xc"), ("abc", "Xc"), ("abbd", "qyyd"), ("aad", "qqd"),
      ("a", ""), ("bb", "yy"), ("", "")]),
    ("$nondigits = [^\\u1040-\\u1049];\n"
     "$consonant = [\\u1000-\\u1021];\n"
     "$vowelsign = [\\u102B-\\u1030\\u1032];  # Unicode vowel signs except E (1031)\n"
     "$vowelsAndConsonants = [\\u1000-\\u102a];\n"
     "$umedial = [\\u103B-\\u103E];    # Medial codepoints in Unicode\n"
     "$vowelmedial = [\\u102B-\\u1030\\u1032\\1u36\\u1037\\u103A-\\u103F];  # Union of vowel signs and medials\n"
     "$ukinzi = \\u1004\\u103A\\u1039;  # Codepoints representing kinzi in Unicode\n"
     "$zmedialra = [\\u103B\\u107E-\\u1084];\n"
     "$wspace = [\\u0020\\u00a0\\u1680\\u2000-\\u200d\\u2060\\u202f\\u205f\\u3000\\ufeff];\n"
     "::Null;\n"
     "\\u102D \\u102D+ \u2192 \\u102D;\n"
     "\\u102E \\u102E+ \u2192 \\u102E;\n"
     "\\u102F \\u102F+ \u2192 \\u102F;\n"
     "\\u1030 \\u1030+ \u2192 \\u1030;\n"
     "\\u1032 \\u1032+ \u2192 \\u1032;\n"
     "\\u1036 \\u1036+ \u2192 \\u1036;\n"
     "\\u1037 \\u1037+ \u2192 \\u1037;\n"
     "\\u1039 \\u1039+ \u2192 \\u1039;\n"
     "\\u103a \\u103a+ \u2192 \\u103a;\n"
     "\\u103b \\u103b+ \u2192 \\u103b;\n"
     "\\u103c \\u103c+ \u2192 \\u103c;\n"
     "\\u103d \\u103d+ \u2192 \\u103d;\n"
     "\\u103e \\u103e+ \u2192 \\u103e; # http://unicode.org/cldr/trac/ticket/10386\n"
     "\\u102F [\\u1030\\u103a] \u2192 \\u102F;\n"
     "\\u102D \\u102E \u2192 \\u102E;\n"
     "($wspace)+ ([\\u102b-\\u1032\\u1036-\\u103e]) \u2192 $2;\n"
     "^ \\u200b+ \u2192 ;\n"
     "\\u200b+ $ \u2192 ;\n"
     "$wspace* \\u200b $wspace* \u2192 \\u200b;\n",
     [("\u200b\u200bab", "ab"), ("\u200ba", "a"), ("a\u200b\u200b", "a"), ("\u200b \u200ba", " \u200ba"),
      ("a \u200b b", "a \u200b b"), ("\u200b\u102d", "\u102d"), ("  \u102d", "\u102d"), ("\u200b", ""),
      ("\u200b\u200b", ""), (" \u200b ", " \u200b "), ("a\u200b", "a"), ("\u102d\u102d\u102d ", "\u102d ")]),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for rules_text, cases in GROUPS:
            rules = os.path.join(tmp, "rules.txt")
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rules_text)
            input = os.path.join(tmp, "input.txt")
            with open(input, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in cases))
            run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)
            if run.returncode != 0:
                failures += 1
                print("FAIL %r: exit status %d\n%s" % (rules_text, run.returncode, run.stderr.decode("utf-8", "replace")))
                continue
            got = run.stdout.decode("utf-8", "replace").split("\n")
            for i, (line, want) in enumerate(cases):
                if i >= len(got) or got[i] != want:
                    failures += 1
                    print("FAIL %r with %r:\n  want %r\n  got  %r" % (line, rules_text, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
