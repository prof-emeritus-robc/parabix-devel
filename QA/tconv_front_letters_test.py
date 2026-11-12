#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: front-letter revisits (ldml::FrontLetterRevisits), Greek-Latin's
#  rough breathing: the rules writing H or h before a run of vowels and
#  revisiting it, applied again while they match (all the letters before the
#  run), with the rules marking a vowel before the iota subscript and the
#  characters revisited as vowels (ϵ, ϒ), and rules whose contexts see the
#  letters (Χ before a lowercase letter, a rough breathing before an
#  uppercase letter).  The rules are Greek-Latin's main group, reduced.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_front_letters_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = (
    ":: NFD (NFC) ;\n"
    "$lower = [[:Latin:][:Greek:] & [:Ll:]] ;\n"
    "$glower = [[:Greek:] & [:Ll:]] ;\n"
    "$accent = [:M:] ;\n"
    "$macron = \u0304 ;\n"
    "$ddot = \u0308 ;\n"
    "$ddotmac = [$ddot$macron] ;\n"
    "$lcgvowel = [\u03b1\u03b5\u03b7\u03b9\u03bf\u03c5\u03c9] ;\n"
    "$ucgvowel = [\u0391\u0395\u0397\u0399\u039f\u03a5\u03a9] ;\n"
    "$gvowel = [$lcgvowel $ucgvowel] ;\n"
    "$vowel = [aeiouyAEIOUY $gvowel] ;\n"
    "$rough = \u0314 ;\n"
    "$iotasub = \u0345 ;\n"
    "$ignore = [[:Mark:]\\'] * ;\n"
    "$beforeLower = $accent * $lower ;\n"
    "$accent_minus = [[$accent]-[$iotasub$macron]] ;\n"
    "\u0391 } $accent_minus * $iotasub \u2192 | \u0391 $macron ;\n"
    "\u03b1 } $accent_minus * $iotasub \u2192 | \u03b1 $macron ;\n"
    "$iotasub \u2192 i ;\n"
    "\u0391 ($macron?) $rough } $beforeLower \u2192 H | \u03b1 $1 ;\n"
    "\u0395 $rough } $beforeLower \u2192 H | \u03b5 ;\n"
    "\u0397 $rough } $beforeLower \u2192 H | \u03b7 ;\n"
    "\u0399 ($ddot?) $rough } $beforeLower \u2192 H | \u03b9 $1 ;\n"
    "\u039f $rough } $beforeLower \u2192 H | \u03bf ;\n"
    "\u03a5 $rough } $beforeLower \u2192 H | \u03c5 ;\n"
    "\u03a9 ($ddot?) $rough } $beforeLower \u2192 H | \u03c9 $1 ;\n"
    "\u0391 ($glower $macron?) $rough \u2192 H | \u03b1 $1 ;\n"
    "\u0395 ($glower) $rough \u2192 H | \u03b5 $1 ;\n"
    "\u0397 ($glower) $rough \u2192 H | \u03b7 $1 ;\n"
    "\u0399 ($glower $ddot?) $rough \u2192 H | \u03b9 $1 ;\n"
    "\u039f ($glower) $rough \u2192 H | \u03bf $1 ;\n"
    "\u03a5 ($glower) $rough \u2192 H | \u03c5 $1 ;\n"
    "\u03a9 ($glower $ddot?) $rough \u2192 H | \u03c9 $1 ;\n"
    "($lcgvowel + $ddotmac?) $rough \u2192 h | $1 ;\n"
    "($gvowel + $ddotmac?) $rough \u2192 H | $1 ;\n"
    "$vowel { \u03c5 \u2192 u ;\n"
    "$vowel { \u03a5 \u2192 U ;\n"
    "\u03b1 \u2192 a ; \u03b5 \u2192 e ; \u03b7 \u2192 \u0113 ; \u03b9 \u2192 i ; \u03bf \u2192 o ; \u03c5 \u2192 y ; \u03c9 \u2192 \u014d ;\n"
    "\u0391 \u2192 A ; \u0395 \u2192 E ; \u0397 \u2192 \u0112 ; \u0399 \u2192 I ; \u039f \u2192 O ; \u03a5 \u2192 Y ; \u03a9 \u2192 \u014c ;\n"
    "\u03a7 } $beforeLower \u2192 Ch ;\n"
    "\u03a7 \u2192 CH ;\n"
    "\u03c1 $rough \u2192 rh ;\n"
    "\u03a1 $rough } $beforeLower \u2192 Rh ;\n"
    "\u03a1 $rough \u2192 RH ;\n"
    "\u03c1 \u2192 r ; \u03a1 \u2192 R ; \u03bb \u2192 l ; \u03bd \u2192 n ; \u03c2 \u2192 s ; \u03c3 \u2192 s ; \u03d0 \u2192 b ; \u03bc \u2192 m ; \u03ba \u2192 k ; \u03c0 \u2192 p ;\n"
    "$rough } $ignore [:Uppercase_Letter:] \u2192 H ;\n"
    "$ignore [:Uppercase_Letter:] { $rough \u2192 H ;\n"
    "$rough \u2192 h ;\n"
    "\u03d2 \u2192 | \u03a5 ;\n"
    "\u03f5 \u2192 | \u03b5 ;\n"
    ":: NFC (NFD) ;\n"
)

# (line, expected result)
CASES = [
    ("\u1f01\u1f11", "hhae"),
    ("\u1f41\u0314", "hho"),
    ("\u03b1\u0308\u0314", "h\u00e4"),
    ("\u03b1\u0308\u0314\u1f11", "h\u00e4he"),
    ("\u03ac\u1f11", "\u00e1he"),
    ("\u1f49", "HO"),
    ("\u1f49\u03bb", "Hol"),
    ("\u0391\u1fe5", "Har"),
    ("\u03b5\u0391\u1fe5", "eHar"),
    ("\u03b1\u1f19", "HaE"),
    ("\u0395\u1f31", "Hei"),
    ("\u03b5\u0391\u1f31", "HeAi"),
    ("\u1f81", "h\u0101i"),
    ("\u1fb3\u0308\u0314", "\u0101\u0308hi"),
    ("\u03bc\u0314", "mh"),
    ("\u1fe5", "rh"),
    ("\u1f01 \u1f11", "ha he"),
    ("\u1f31\u1f5d", "hHi\u00da"),
    ("\u03b1\u03c9\u03c9\u1f11\u1f01\u03bd", "hha\u014d\u014dean"),
    (" \u0314\u03c5\u1f39", " hHyI"),
    ("\u03a7\u1f01", "Chha"),
    ("\u03a7\u0391\u0314\u03b5", "CHHae"),
    ("\u0301\u03b7\u03b7\u1f39\u0314\u03d0\u0314", "\u0301HH\u0113\u0113Hib"),
    ("\u03d0\u0395\u03f5\u1f49\u03c0\u0314", "bEHeHop"),
    ("\u03a9\u03f5\u0314\u0314\u03a9\u03c5", "H\u014dhe\u014cy"),
    ("\u03b7\u1f39\u03c2\u0314\u03a1", "H\u0113HisR"),
    ("\u0397\u03f5\u03b9\u1f11\u0314", "\u0112hheie"),
    ("\u03f5\u0301\u03a9\u03b7\u03a5\u03f5\u1f11", "\u00e9\u014c\u0113Yhee"),
    ("\u03c1\u03c5\u1f59\u03b8\u0314\u0304\u03bb\u0314", "rHyHy\u03b8\u0304lh"),
    ("\u03b9\u03b5\u0304\u0314\u0399\u03f5\u0308\u0314\u0308", "hi\u0113Hi\u00eb\u0308"),
    ("\u0391\u1f30\u03a9\u03f5\u1f21\u03c5", "Ai\u0313\u014che\u0113y"),
    ("\u03d2\u0314\u03b5", "Hye"),
    ("\u039f\u0314\u03b5\u0314", "Hhoe"),
    ("\u0314\u03c5\u1f4b\u03c5\u0345\u03bf", "hHy\u00d2yio"),
    ("\u03b1\u03b1\u03a7\u03b5\u0397\u03b5\u1f41a", "aaChHe\u0112eoa"),
    ("\u0395\u03c1\u03c5\u1f39\u03bc\u0314\u03bf", "ErHyHimo"),
    ("\u1f00\u1f01\u1f02", "a\u0313haa\u0313\u0300"),
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
        if b"front-letter revisits" not in plan.stdout:
            failures += 1
            print("FAIL: the rules are not planned as front-letter revisits\n%s" % plan.stdout.decode("utf-8", "replace"))
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
