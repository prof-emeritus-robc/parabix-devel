#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: rules whose texts begin with a run (see ldml::SplitLeadingRuns, from
#  Zawgyi-my): R* T → C (and R+ T → C) as R } R* T → ; then T → C (R T → C),
#  and a block of rules beginning with the same run as one deletion before
#  them, R } R+ (T1 | T2 | ...) → ; then R Ti → Ci in order.  An earlier rule
#  beginning with a character of R is allowed where it cannot match within such
#  a run (w U+1037).  The expected results are ICU's (createFromRules, each
#  line a text).
#
#  tconv_leading_runs_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

# (rules, [(line, expected result)])
GROUPS = [
    ("$zmedialra = [\u103b\u107e-\u1084];\n"
     "($zmedialra)+ \u2192 \u103c ;\n"
     "\u103c* \u108a \u2192 \u103d \u103e;\n"
     "\u103c \u2192 \u103d ;\n",
     [("\u103c\u108a", "\u103d\u103e"), ("\u108a", "\u103d\u103e"), ("\u103c\u103c\u108a", "\u103d\u103e"), ("\u103c", "\u103d"),
      ("\u103c\u103cx", "\u103d\u103dx"), ("\u103b\u103c\u108a", "\u103c\u103d\u103e"), ("\u107e\u108a", "\u103c\u103d\u103e"), ("x\u103c\u108ab", "x\u103d\u103eb")]),
    ("\u103c+ \u108a x \u2192 Q ;\n"
     "\u103c \u2192 \u103d ;\n"
     "x \u2192 y ;\n",
     [("\u103c\u108ax", "Q"), ("\u103c\u103c\u108ax", "Q"), ("\u108ax", "\u108ay"), ("\u103c\u103c", "\u103d\u103d"),
      ("\u103cx", "\u103dy"), ("\u103c\u108a", "\u103d\u108a")]),
    ("$c = [kgt];\n"
     "w \u1037 > \u1037 w ;\n"
     "\u1031+ x ($c) > x $1 \u1031 ;\n"
     "\u1031+ \u103c ($c) > $1 \u103c \u1031 ;\n"
     "\u1031+ ($c) ([\u103b\u103d]+) > $1 $2 \u1031 ;\n"
     "\u1031+ ([$c a]) > $1 \u1031 ;\n",
     [("\u1031k", "k\u1031"), ("\u1031\u1031k", "k\u1031"), ("\u1031\u1031\u1031xk", "xk\u1031"), ("\u1031xk", "xk\u1031"),
      ("\u1031\u103cg", "g\u103c\u1031"), ("\u1031\u1031\u103cg", "g\u103c\u1031"), ("\u1031t\u103b\u103d", "t\u103b\u103d\u1031"), ("\u1031\u1031t\u103b", "t\u103b\u1031"),
      ("\u1031\u1031a", "a\u1031"), ("\u1031\u1031", "\u1031\u1031"), ("\u1031b", "\u1031b"), ("a\u1031\u1031\u1031t", "at\u1031"),
      ("w\u1037", "\u1037w"), ("\u1031xa", "\u1031xa")]),
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
