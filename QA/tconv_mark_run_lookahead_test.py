#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: mark run edits left in their group with a lookahead and a mark (see
#  ldml::RunEditMark).  A mark run edit P R* T → C | $1 that cannot be applied
#  before the rest of its group (here h, which k̄h and kh cover, and the h of
#  h → | k may begin; s, which z → | s revisits) stays in the group as
#  P } R* T → C M, and a group after it deletes M with T.  The other edits (a)
#  are still applied before the group.  The expected results are ICU's
#  (createFromRules, each line a text).
#
#  tconv_mark_run_lookahead_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$notAbove = [^\\p{ccc=0}\\p{ccc=Above}] ;\n$notHAccent0 = [^\\u0325\\u0304\\u0323] ;\n"
     "h ($notAbove*) \\u0304 \u2192 H | $1 ;\n"
     "k\\u0304h \u2192 Q ;\nkh } $notHAccent0 \u2192 K ;\nk \u2192 k ;\n"
     "s \\u0323 ($notAbove*) \\u0304 \u2192 C | $1 ;\ns ($notAbove*) \\u0304 \u2192 S | $1 ;\ns \u2192 s ;\n"
     "a ($notAbove*) \\u0304 \u2192 A | $1 ;\n"
     "z \u2192 | s ;\nh \u2192 | k ;\n",
     [('h\u0316\u0304', 'H\u0316'), ('hh\u0316\u0304', 'K\u0316\u0304'), ('hhh\u0316\u0304', 'KH\u0316'),
      ('kh\u0316\u0304', 'K\u0316\u0304'), ('k\u0304h\u0304', 'Q\u0304'), ('kh\u0304', 'kH'), ('z\u0304', 'S'),
      ('z\u0323\u0316\u0304', 'C\u0316'), ('zh\u0316\u0304', 'sH\u0316'), ('s\u0316\u0323\u0304', 'S\u0316\u0323'),
      ('s\u0323\u0304', 'C'), ('a\u0316\u0304', 'A\u0316'), ('ha\u0304', 'kA'), ('h', 'k'), ('h\u0316', 'k\u0316'),
      ('h\u0304\u0304', 'H\u0304'), ('kha', 'Ka')]),
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
