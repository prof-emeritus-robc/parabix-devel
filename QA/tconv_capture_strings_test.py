#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: capture rules whose captures repeat a set with strings (from
#  cy-cy_FONIPA's stress placement).  Their exact starts are found with the
#  captures matched possessively in a lookahead chain (engineStarts), and
#  their spans by a chain ending with (?=$); a before context the insertions
#  of the group cannot change is matched against the text (every match ends
#  at the end of the text); a group of one such rule, by the engine's search
#  and replace, keeps no position after the text (an empty text too).  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_capture_strings_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$end = [$ ];\n"
     "$vowel = [aeiouwy\u00e2\u00ea\u00ee\u00f4\u00fb\u0175\u0177\u0251\u0268\u0259\u025b\u026a\u0254\u028a];\n"
     "$cons = [ m {m\u0325} n {n\u0325} \u014b {\u014b\u030a} p b t d k \u0261 f v \u03b8 \u00f0 s \u0283 h \u03c7 l \u026c r {r\u0325} {d\u0361\u0292} g W w j ];\n"
     "{($vowel+ $cons+ $vowel+ $cons*)} $end \u2192 \u02c8 $1;\n"
     "$end $cons* {($vowel+ $cons*)} $end \u2192 \u02c8 $1;\n",
     [("a", "\u02c8a"), ("gan", "g\u02c8an"), ("hawl", "h\u02c8awl"), ("mae", "m\u02c8ae"),
      ("bara", "b\u02c8ara"), ("www", "\u02c8www"), ("m\u0325am\u0325", "m\u0325\u02c8am\u0325"), ("d\u0361\u0292ad\u0361\u0292", "d\u0361\u0292\u02c8ad\u0361\u0292"),
      ("awa", "\u02c8awa"), ("gwlad", "g\u02c8wlad"), ("wlad", "\u02c8wlad"), ("bwlad", "b\u02c8wlad"),
      ("gwl", "g\u02c8wl"), ("ym\u0325ad", "\u02c8ym\u0325ad")]),
    ("$vowel = [aeiouy];\n"
     "$cons = [ m {m\u0325} n p b t d l r ];\n"
     "si } $vowel \u2192 \u0283;\n"
     "::Null;\n"
     "($vowel+ $cons*) } [$] \u2192 \u02c8 $1;\n",
     [("a", "\u02c8a"), ("ban", "b\u02c8an"), ("sia", "\u0283\u02c8a"), ("bam\u0325", "b\u02c8am\u0325"),
      ("", ""), ("sim\u0325o", "sim\u0325\u02c8o")]),
    ("[:P:] \u2192 ;\n"
     "::Null;\n"
     "([b] [l]?) x \u2192 x $1;\n",
     [("-", ""), ("blx", "xbl"), ("-a", "a"), ("", ""),
      ("b-lx", "xbl")]),
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
