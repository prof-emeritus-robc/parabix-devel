#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: before contexts with a repeated set of marks ($ignore
#  [:Uppercase_Letter:] { $rough, $afterLetter { σ), matched against the
#  converted text, whose after contexts (also with a repetition) are matched
#  by the regular expression engine: the rough breathing after a deleted
#  smooth breathing, after Χ and Θ written Ch and Th, after another rough
#  breathing written H; the iota subscript after Ch; σ and ς after a rough
#  breathing written h.  The rules are from Greek-Latin, reduced.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_converted_star_contexts_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = (
    ":: NFD (NFC) ;\n"
    "$smooth = \u0313 ;\n"
    "$rough = \u0314 ;\n"
    "$iotasub = \u0345 ;\n"
    "$upper = [[:Latin:][:Greek:] & [:Lu:]] ;\n"
    "$lower = [[:Latin:][:Greek:] & [:Ll:]] ;\n"
    "$accent = [:M:] ;\n"
    "$ignore = [[:Mark:]\\'] * ;\n"
    "$beforeLower = $accent * $lower ;\n"
    "$afterLetter = [:L:] [[:M:]\\']* ;\n"
    "$beforeLetter = [[:M:]\\']* [:L:] ;\n"
    "$upper $accent * { $iotasub \u2192 I ;\n"
    "$iotasub \u2192 i ;\n"
    "$smooth \u2192 ;\n"
    "\u0398 } $beforeLower \u2192 Th ;\n"
    "\u0398 \u2192 TH ;\n"
    "\u03a7 } $beforeLower \u2192 Ch ;\n"
    "\u03a7 \u2192 CH ;\n"
    "\u03a1 $rough } $beforeLower \u2192 Rh ;\n"
    "\u03a1 $rough \u2192 RH ;\n"
    "\u03c2 } $beforeLetter \u2192 s\u0331 ;\n"
    "\u03c3 } $beforeLetter \u2192 s ;\n"
    "$afterLetter { \u03c3 \u2192 s\u0331 ;\n"
    "$afterLetter { \u03c2 \u2192 s ;\n"
    "\u03c2 \u2192 s\u0331 ;\n"
    "\u03c3 \u2192 s ;\n"
    "\u03b1 \u2192 a ; \u03b5 \u2192 e ; \u03b7 \u2192 \u0113 ; \u03bf \u2192 o ; \u03c5 \u2192 y ; \u0391 \u2192 A ; \u039f \u2192 O ; \u03a5 \u2192 Y ; \u0399 \u2192 I ; \u03c1 \u2192 r ; \u03a1 \u2192 R ; \u03ba \u2192 k ;\n"
    "$rough } $ignore [:Uppercase_Letter:] \u2192 H ;\n"
    "$ignore [:Uppercase_Letter:] { $rough \u2192 H ;\n"
    "$rough \u2192 h ;\n"
    ":: NFC (NFD) ;\n"
)

# (line, expected result)
CASES = [
    ("a\u03a5\u0313\u0314", "aYH"),
    ("\u039f\u0313\u0314\u03c5\u03c5\u03b5", "OHyye"),
    ("\u0399\u0313\u0314\u03ba\u039f", "IHkO"),
    ("\u03a7\u0314\u0314\u03b5", "Chhhe"),
    ("\u0398\u0314\u0314\u03b7\u0391", "Thhh\u0113A"),
    ("\u03a7\u0314\u0314\u03c1\u03c9H", "Chhhr\u03c9H"),
    ("\u0398\u0314\u0308\u03b5", "Th\u1e27e"),
    ("\u03a7\u0314\u0345\u03b1\u03bf", "Chhiao"),
    ("\u03a7\u0314", "CHH"),
    ("H\u0314\u0314", "HHH"),
    ("\u03baH\u0314\u0314\u03bf", "kHHHo"),
    ("\u03a1\u0314\u0314\u0304", "RHH\u0304"),
    ("\u03a1\u0313\u0314\u0301\u03b1", "RH\u0301a"),
    ("\u0314\u03c3", "hs\u0331"),
    ("\u0314\u03c2", "hs"),
    ("\u0314\u03c3 ", "hs\u0331 "),
    ("\u03b1 \u0345\u03c3", "a is\u0331"),
    ("\u0314\u03c2\u0314", "hsh"),
    ("\u03b1\u03c3", "as\u0331"),
    ("\u03b1\u03c2", "as"),
    ("\u03c3\u03b1", "sa"),
    ("\u03a5\u0314", "YH"),
    ("\u0314", "h"),
    ("\u03b1\u0314", "ah"),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        plan = subprocess.run([tconv, "--plan", "--rules-file=" + rules, input], capture_output=True)
        if b"implementable" not in plan.stdout or b"warning" in plan.stdout:
            failures += 1
            print("FAIL: the rules are not planned without warnings\n%s" % plan.stdout.decode("utf-8", "replace"))
        run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)
        if run.returncode != 0:
            failures += 1
            print("FAIL: exit status %d\n%s" % (run.returncode, run.stderr.decode("utf-8", "replace")))
        else:
            got = run.stdout.decode("utf-8", "replace").split("\n")
            for i, (line, want) in enumerate(CASES):
                if i >= len(got) or got[i] != want:
                    failures += 1
                    print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
