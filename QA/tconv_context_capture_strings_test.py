#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a before context that is a capture of a set with strings, copied
#  (W5, fa-fa_FONIPA's ([$IPA_CONSONANT|$VOWEL]) { U+0651 → $1 with {t͡ʃ} and
#  {d͡ʒ}): expanded into a rule for each string, the longest first, then one
#  for the single characters (see ldml::ExpandContextCaptureStrings), as ICU
#  takes the longest string ending before the text; copies chain over
#  consecutive U+0651.  The expected results are ICU's (createFromRules, each
#  line a text).
#
#  tconv_context_capture_strings_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$VOWEL = [ \u064e \u0650  \u064f  \u0653 \u0627 \u0648 \u06cc];\n"
     "$IPA_CONSONANT = [ m n p b t d k \u0261 \u0294 f v s z \u0283 \u0292 \u0281 \u0262 h \u03c7 {t\u0361\u0283} {d\u0361\u0292} l \u027e ];\n"
     "([$IPA_CONSONANT|$VOWEL]){\u0651} \u2192 $1;\n"
     "[ \u0651 \u0654 \u064b \u0670 ] \u2192 ;\n",
     [("t\u0361\u0283\u0651", "t\u0361\u0283t\u0361\u0283"), ("\u0283\u0651", "\u0283\u0283"), ("d\u0361\u0292\u0651a", "d\u0361\u0292d\u0361\u0292a"),
      ("\u0651", ""), ("a\u0651", "a"), ("m\u0651\u0651", "mmm"),
      ("|\u0651", "||"), ("t\u0651", "tt"), ("t\u0361\u0283\u0651\u0651", "t\u0361\u0283t\u0361\u0283t\u0361\u0283"),
      ("xd\u0361\u0292\u0651", "xd\u0361\u0292d\u0361\u0292"), ("\u064e\u0651", "\u064e\u064e"), ("\u0361\u0283\u0651", "\u0361\u0283\u0283")]),
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
