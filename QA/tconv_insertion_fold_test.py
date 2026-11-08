#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: an insertion rule folded into the rules before it, and before
#  contexts matched in the converted text next to capture rules (from
#  Grek-Latn/UNGEGN).
#    - B { } A → I in a group with string rules, folded into the rules whose
#      results end with a character of B (π → p, the deletion of ̓), with
#      A extended by ϲ → | σ, and, after the characters of B outside the
#      filter (Latin p), applied in a group before.
#    - A string rule whose before context is matched against the converted
#      text (μπ after the deleted ͺ), in a group with a capture rule (applied
#      before the rest of the group).
#    - Capture rules whose before contexts are translated to the text itself,
#      and υ → y, whose contexts, exclusive of theirs in the converted text,
#      are matched against the text itself: not where the capture rules apply.
#  The expected results are ICU's (createFromRules, each line a text).
#
#  tconv_insertion_fold_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    (":: [[:Greek:][:Mn:]\\.] ;\n"
     "\u03c0 \u2192 p ;\n"
     "\u0313 \u2192 ;\n"
     "[Pp] { } [\u03a3\u03c3\u03f2] \u2192 \\' ;\n"
     "\u03a3 \u2192 S ;\n"
     "\u03c3 \u2192 s ;\n"
     "\u03f2 \u2192 | \u03c3 ;\n",
     [("\u03c0\u03a3", "p'S"), ("p\u03a3", "p'S"), ("P\u03c3", "P's"), ("\u03c0\u0313\u03c3", "p's"), ("p\u0313\u03c3", "p's"), ("p\u03f2", "p's"), ("\u03c0\u03f2", "p's"), ("a\u03a3", "aS"), (".p\u03c3", ".p's"), ("\u03c0", "p")]),
    (":: [[:Greek:][:Mn:]\\.] ;\n"
     "\u037a \u2192 ;\n"
     "[^[:L:][:Mn:]] { \u03bc\u03c0 \u2192 B ;\n"
     "\u03bc \u2192 m ;\n"
     "\u03c0 \u2192 p ;\n"
     "a { \u03c5 (\u0301*) } \u2192 $1 f ;\n",
     [(".\u037a\u03bc\u03c0", ".B"), ("\u037a\u03bc\u03c0", "B"), ("\u03bc\u03c0", "B"), (".\u03bc\u03c0", ".B"), ("\u03b1\u03bc\u03c0", "\u03b1mp"), ("\u03bc", "m")]),
    (":: [[:Greek:][:Mn:]] ;\n"
     "$fmaker = [aeiAEI] \u0331 ? ;\n"
     "$marks = [[:Mn:]-[\u0308]] ;\n"
     "$fmaker { \u03c5 ( $marks* ) } [\u03b2\u03b3] \u2192 $1 v \u0331 ;\n"
     "$fmaker { \u03c5 ( $marks* ) } \u2192 $1 f \u0331 ;\n"
     "\u03c5 \u2192 y ;\n"
     "\u03b1 \u2192 a ;\n"
     "\u03b9 \u2192 i ;\n"
     "\u03b2 \u2192 v ;\n",
     [("\u03b1\u03c5\u0300\u0301", "a\u0300\u0301f\u0331"), ("\u03b1\u03c5\u0300\u0301\u03b2", "a\u0300\u0301v\u0331v"), ("\u03b1\u03c5\u0300", "a\u0300f\u0331"), ("\u03b1\u03c5\u0314", "a\u0314f\u0331"), ("\u03b1\u03c5\u0301", "a\u0301f\u0331"), ("\u03b9\u03c5\u0331\u0301\u03b2", "i\u0331\u0301v\u0331v"), ("\u03c5\u03c5\u0300\u0301", "yy\u0300\u0301"), ("\u03b1\u03c5", "af\u0331"), ("\u03b2\u03c5\u0300\u0301", "vy\u0300\u0301")]),
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
