#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: blocking marks (see ldml::DeletedRunBlockMark).  Prefix runs that
#  only delete, ($c) ' → | $1 ;, are applied before the rest of their group,
#  whose matches a deleted run then blocks where ICU would still have found it:
#  within a match or its after context, other than after its first character
#  (or after a character at which ICU revisits).  The rules that DisambiguateOrder
#  cannot keep apart from an earlier marked rule apply only where it does not
#  match (TransformStep::stringPriority, subgroupPriority).  Prefix runs of P of
#  several characters (tsu~ → | ts) are applied after the others.  A revisit
#  that is a prefix run after one step of composition is split with a marker
#  (Composer::prefixRunStep).  The expected
#  results are ICU's (createFromRules, each line a text).
#
#  tconv_blocking_marks_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    # A run deleted after the first character of a match does not block it; within it, or seen by an after context, it does; revisits
    ("$c = [bcdfghlmnpqrstvz] ;\n($c) \\' \u2192 | $1 ;\n\\' \u2192 ;\ncc \u2192 | c ;\ncha \u2192 K ;\nca \u2192 A ;\nc } $c \u2192 C ;\nh \u2192 H ;\na \u2192 a ;\ns } [aeiou] \u2192 S ;\ntzu \u2192 T ;\ntz \u2192 | zz ;\nzz \u2192 | \\~z ;\n",
     [("c'ha", 'K'), ("ch'a", 'CHa'), ("c'h'a", 'CHa'), ("ch''a", 'CHa'), ("ch'", 'CH'), ("cc'a", 'A'), ("c'ca", 'A'), ("cc'ha", 'K'), ("c'c'a", 'A'), ("tz'u", '~zu'), ("t'zu", 'T'), ('tzu', 'T'), ("bs'a", 'bSa'), ("s'a", 'Sa'), ("c''", 'c'), ("'c", 'c'), ("h'c'a", 'HA')]),
    # A rule after a blocked rule applies where it is blocked
    ("$c = [bcdfghlmnpqrstvz] ;\n($c) \\' \u2192 | $1 ;\n\\' \u2192 ;\ngnr \u2192 N ;\ng \u2192 G ;\nn \u2192 X ;\nr \u2192 R ;\n",
     [("gn'r", 'GXR'), ('gnr', 'N'), ("g'nr", 'N')]),
    # An after context blocked; the single-character rules then apply
    ("$c = [bcdfghlmnpqrstvz] ;\n($c) \\' \u2192 | $1 ;\n\\' \u2192 ;\ngn } $c \u2192 N ;\nga \u2192 A ;\ng \u2192 G ;\nn \u2192 X ;\nr \u2192 R ;\n",
     [("gn'r", 'GXR'), ('gnr', 'NR'), ("gn'a", 'GXa'), ("ign'r", 'iGXR')]),
    # Composed alternatives left to priority (ss → | ~s)
    ("$c = [bcdfghlmnpqrstvz] ;\n($c) \\' \u2192 | $1 ;\n\\' \u2192 ;\nss \u2192 | \\~s ;\n\\~sa \u2192 S ;\n[:^Letter:] { sa \u2192 Q ;\ns \u2192 Z ;\na \u2192 A ;\n",
     [("ss'a", '~Q'), ('ssa', 'S'), ('sa', 'Q'), ("s'a", 'Q'), ("sss'a", '~~Q'), ("ss's'a", '~~Q')]),
    # Prefix runs of P of several characters (tsu~ → | ts), after those of one (te~ → | t)
    ("te\\~ \u2192 | t ;\nto\\~ \u2192 | t ;\ntsu\\~ \u2192 | ts ;\nts \u2192 C ;\nt \u2192 T ;\n\\~tsu \u2192 | tsu ;\ns \u2192 S ;\nu \u2192 U ;\ne \u2192 E ;\no \u2192 O ;\na \u2192 A ;\n",
     [('tsu~a', 'CA'), ('tsu~u~a', 'CA'), ('te~su~a', 'CA'), ('te~tsu~a', 'TCA'), ('~tsu~a', 'CA'), ('~tsa', '~CA'), ('~te~su', '~CU'), ('tsu~', 'C'), ('~tsu~', 'C'), ('tsu~e~a', 'CE~A'), ('~tsu~u~', 'C'), ('te~to~a', 'TTA'), ('tsu', 'CU')]),
    # A revisit composed one step into a prefix run, split with a marker (i $m → | i i as i } $m → M, M { $m → | i)
    ("$m = [\\u0304] ;\ni $m \u2192 | i i ;\n$m \u2192 ;\ni\\~e \u2192 | ye ;\ni \u2192 I ;\nk \u2192 K ;\nyi \u2192 Y ;\nye \u2192 E ;\ny \u2192 X ;\na \u2192 A ;\n",
     [('i\u0304', 'II'), ('i\u0304\u0304', 'III'), ('i\u0304\u0304a', 'IIIA'), ('yi\u0304', 'Y'), ('ki\u0304\u0304', 'KIII'), ('i\u0304~e', 'IE'), ('y\u0304i', 'XI'), ('i', 'I')]),
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
