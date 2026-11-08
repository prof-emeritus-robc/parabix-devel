#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: the ends of the runs of a global filter (see ldml::MoveTextAnchors),
#  as ICU matches them: each stage of a compound transform with a global filter
#  transforms each run of filtered characters as a text of its own, so a set
#  of the text with the end of the text ([$], [^c]) also matches nothing at
#  the end of a run, where the after context is a character outside the filter
#  (ldml::FilterRunEndMark), as am_Ethi-d0_Morse deletes the space after the
#  last character of a run.  The expected results are ICU's (createFromRules,
#  each line a text).
#
#  tconv_filter_run_end_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    # Filtered runs before a Null stage: [$] at the end of a run
    (":: [a-z] ;\n:: Null ;\nb a [$] \u2192 X ;\n",
     [('ba', 'X'), ('ba1', 'X1'), ('bab', 'bab'), ('ba1ba', 'X1X')]),
    # A complement contains the end of a run
    (":: [a-z] ;\n:: Null ;\nb [^c] \u2192 X ;\n",
     [('b', 'X'), ('b1', 'X1'), ('bc', 'bc'), ('bd', 'X'), ('b1b', 'X1X'), ('b11', 'X11')]),
    # The rule in the first stage
    (":: [a-z] ;\nb a [$] \u2192 X ;\n:: Null ;\n",
     [('ba', 'X'), ('ba1', 'X1')]),
    # An after context of a complement
    (":: [a-z] ;\n:: Null ;\nx } [^c] \u2192 X ;\n",
     [('x', 'X'), ('xc', 'xc'), ('x1', 'X1'), ('xd', 'Xd'), ('x1c', 'X1c')]),
    # A capture of a complement revisited
    (":: [a-z] ;\n:: Null ;\n$nd = [^c] ;\n^ q ($nd) \u2192 | W $1 ;\nW \u2192 U ;\n",
     [('q', 'U'), ('qa', 'Ua'), ('q1', 'U1'), ('qc', 'qc'), ('qa1', 'Ua1')]),
    # As am_Ethi-d0_Morse: a space before the end of a run deleted
    (":: [a-z\\u0020\\-] ;\n:: Null ;\na \u2192 a ;\n:: Null ;\n([b\\-]) \\u0020 [$] \u2192 $1 ;\n",
     [('b ', 'b'), ('b 1', 'b1'), ('b b', 'b b'), ('- 1x', '-1x'), ('b', 'b'), ('- ', '-'), ('b  1', 'b  1')]),
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
