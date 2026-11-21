#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: the hybrid of UTF-8 mode (--hybrid-groups; doc/masked-transforms-plan.md
#  step 16, option C): a conversion group decided one position per character,
#  on classes computed on the bytes, and applied to the bytes by selectors.  Its
#  output must equal Unicode mode's.  The cases (with DivCode off, so that the
#  hybrid applies the groups DivCode could also plan) are string rules with
#  replacements of one to four bytes and deletions, and before contexts matched
#  against the converted text (correction kernels, the selectors' strings as the
#  converted text): of string rules, of single-character rules (a group with no
#  string rules), with a repetition, with --lines.  The rule sets are applied
#  in turn as the groups of one transform (:: Null ; between them); with
#  --extended-testing also each on its own, and also as a text.
#
#  tconv_hybrid_test.py <tconv binary> [--extended-testing]

import os
import random
import subprocess
import sys
import tempfile

RULES = {
    "strings": "[ξx] { cd → 😀 ;\nab → ξξ ;\nb → x ;\nd → ;\n",
    "converted": "ab → ξ ;\nξ { c → ж ;\né → ξ ;\nq → 中 ;\n",
    "chain": "ab → x ;\nx { b → é ;\né { b → y ;\n",
    "singles": "[xж] [:L:]* { b → ξ ;\na → ж ;\nξ { ξ → 😀 ;\n",
}
ALPHABET = "abcdéqxξж中😀 "
RULE_SETS = {"all": "\n:: Null ;\n".join(RULES.values())}


def main():
    tconv = sys.argv[1]
    rnd = random.Random(7)
    text = "\n".join("".join(rnd.choice(ALPHABET) for _ in range(rnd.randint(0, 40))) for _ in range(80)) + "\n"
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        extended = "--extended-testing" in sys.argv
        rule_sets = dict(RULE_SETS, **RULES) if extended else RULE_SETS
        for name, rules in rule_sets.items():
            rp = os.path.join(tmp, name + ".rules")
            with open(rp, "w", encoding="utf-8") as f:
                f.write(rules)
            for lines in (([], ["--lines"]) if extended else (["--lines"],)):
                args = lines + ["--rules-file=" + rp, path]
                expected = subprocess.run([tconv, "--utf8-mode=0"] + args, capture_output=True)
                hybrid = subprocess.run([tconv, "--hybrid-groups", "--divcode-groups=0", "--check-utf8-masks"] + args,
                                        capture_output=True)
                label = name + (" --lines" if lines else "")
                if expected.returncode != 0 or hybrid.returncode != 0 or expected.stdout != hybrid.stdout:
                    failures += 1
                    print("FAIL %s: exit status %d (Unicode mode %d)\n%s"
                          % (label, hybrid.returncode, expected.returncode, hybrid.stderr.decode("utf-8", "replace")[-600:]))
                    want = expected.stdout.decode("utf-8", "replace").split("\n")
                    got = hybrid.stdout.decode("utf-8", "replace").split("\n")
                    for i, (w, g) in enumerate(zip(want, got)):
                        if w != g:
                            print("  line %d: %r: expected %r, got %r" % (i + 1, text.split("\n")[i], w, g))
                            break
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
