#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: the ends of the runs of a global filter (see ldml::MoveTextAnchors),
#  as ICU matches them: each stage of a compound transform with a global filter
#  transforms each run of filtered characters as a text of its own, so a set
#  of the text with the end of the text ([$], [^c]) also matches nothing at
#  the end of a run, where the after context is a character outside the filter
#  (ldml::FilterRunEndMark), as am_Ethi-d0_Morse deletes the space after the
#  last character of a run.  The text of a rule ends with the run, but its
#  after context does not: a later rule ordered after an earlier one without
#  an after context (ldml::DisambiguateOrder, a } b [^c] for abc) applies
#  where the earlier one's text meets the end of a run (as Hira-Latn
#  transforms デ before ィ outside its filter).  The expected results are
#  ICU's (createFromRules, each line a text).
#
#  xlit_filter_run_end_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

# The implementations of a group whose rules match characters outside the
# filter only: the default, UTF-8 mode, without DivCode, and in four threads.
OUTSIDE = [[], ["--utf8-mode"], ["--divcode-groups=0"], ["-thread-num=4", "-segment-size=1"]]

# (rules, [(line, expected result)], [configurations])
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
    # The text of an earlier rule ends with the run: the negated set of the
    # later rule's order (a } b [^c]) also matches at the end of a run
    (":: [ab] ;\n:: Lower ;\nabc → X ;\na → A ;\nb → B ;\n",
     [('abc', 'ABc'), ('ab', 'AB'), ('xabcx', 'xABcx'), ('abd', 'ABd'), ('abC', 'ABC'), ('ab1abc', 'AB1ABc')]),
    # As Hira-Latn (Katakana-Latin): ディ with ィ outside the filter
    (":: [デ] ;\n:: NFD ;\nディ → di ;\nデ → de ;\n",
     [('ディ', 'deィ'), ('デ', 'de'), ('aディb', 'adeィb'), ('デデ', 'dede')]),
    # An earlier rule's after context sees beyond the run: not at its end
    (":: [ab] ;\n:: Lower ;\nab } c → X ;\na → A ;\nb → B ;\n",
     [('abc', 'Xc'), ('ab', 'AB'), ('abd', 'ABd'), ('xabcx', 'xXcx')]),
    # A later rule whose after context sees a character outside the filter
    # that only an earlier rule's text has: there, beyond the run, the
    # earlier rule fails (a } [c outside the filter])
    (":: [ab] ;\n:: Lower ;\nac \u2192 X ;\na } c \u2192 Y ;\na \u2192 A ;\n",
     [('ac', 'Yc'), ('ad', 'Ad'), ('a', 'A'), ('bac', 'bYc'), ('acc', 'Ycc'), ('aC', 'AC'), ('Ac', 'Ac')], OUTSIDE),
    # The same, a string rule (ab } [c outside the filter])
    (":: [ab] ;\n:: Lower ;\nabc \u2192 X ;\nab } c \u2192 Y ;\nab \u2192 Z ;\n",
     [('abc', 'Yc'), ('abd', 'Zd'), ('ab', 'Z'), ('xabcx', 'xYcx'), ('abcc', 'Ycc'), ('abab', 'ZZ')], OUTSIDE),
    # As am_Ethi-d0_Morse: a space before the end of a run deleted
    (":: [a-z\\u0020\\-] ;\n:: Null ;\na \u2192 a ;\n:: Null ;\n([b\\-]) \\u0020 [$] \u2192 $1 ;\n",
     [('b ', 'b'), ('b 1', 'b1'), ('b b', 'b b'), ('- 1x', '-1x'), ('b', 'b'), ('- ', '-'), ('b  1', 'b  1')]),
]
if EXTENDED:
    GROUPS += [
        # As Hira-Latn: ビ } $small_y → b | '~' ; with the small vowel outside
        # the filter, where ~ュ → yu (composed: ビュ → byu) does not apply
        (":: [\u30D3\u30C7\u30B8] ;\n:: Katakana-Latin ;\n",
         [('\u30D3\u30E5', 'b~\u30E5'), ('\u30C7\u30A3', 'de\u30A3'), ('\u30B8\u30A7', 'ji\u30A7'), ('\u30D3', 'bi'),
          ('\u30D3\u30E5\u30D3\u30E5', 'b~\u30E5b~\u30E5'), ('a\u30D3\u30E3b', 'ab~\u30E3b'), ('\u30D3\u30B9', 'bi\u30B9')]),
    ]


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for group in GROUPS:
            rules_text, cases = group[0], group[1]
            configs = group[2] if len(group) > 2 else [[]]
            rules = os.path.join(tmp, "rules.txt")
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rules_text)
            input = os.path.join(tmp, "input.txt")
            with open(input, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in cases))
            for config in configs:
                run = subprocess.run([xlit, "--use-ICU=0"] + config + ["--lines", "--rules-file=" + rules, input], capture_output=True)
                if run.returncode != 0:
                    failures += 1
                    print("FAIL %r %s: exit status %d\n%s" % (rules_text, " ".join(config), run.returncode,
                                                              run.stderr.decode("utf-8", "replace")))
                    continue
                got = run.stdout.decode("utf-8", "replace").split("\n")
                for i, (line, want) in enumerate(cases):
                    if i >= len(got) or got[i] != want:
                        failures += 1
                        print("FAIL %r with %r %s:\n  want %r\n  got  %r" % (line, rules_text, " ".join(config), want,
                                                                           got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
