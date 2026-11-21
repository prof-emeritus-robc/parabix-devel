#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv --lines with the transforms or steps that ICU applies
#  (doc/masked-transforms-plan.md, step 11): in one run, without a pipeline
#  run (or a pipe) for each line.  A transform that ICU applies as a whole
#  (Latn-Grek, a rules file tconv does not implement), and the built-ins ICU
#  applies (Any-Name, Name-Any, Any-BreakInternal), are applied by ICU to each
#  line as a text of its own, in the process (icuLinesRunner, as uconv -x
#  does); a transform with ICU stages (Thai-Latn) has its tconv stages applied
#  once to all the lines (as regions) and its ICU stages line by line.  The
#  expected results are ICU's (each line a text); the plan must say one run.
#  Skipped if tconv is built without ICU.
#
#  tconv_icu_lines_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

# (transform name or rules file text, input, expected output)
CASES = [
    ("Any-Name", "a\u00e9\n\n\u4e2d b",
     "\\N{LATIN SMALL LETTER A}\\N{LATIN SMALL LETTER E WITH ACUTE}\n\n"
     "\\N{CJK UNIFIED IDEOGRAPH-4E2D}\\N{SPACE}\\N{LATIN SMALL LETTER B}"),
    ("Name-Any", "\\N{LATIN SMALL LETTER A}x\n\\N{SPACE}\n\\N{GREEK CAPITAL LETTER SIGMA}", "ax\n \n\u03a3"),
    ("Any-BreakInternal", "\u0e01\u0e02\u0e04 \u0e07\u0e08\n\u0e09\u0e0a\n", "\u0e01\u0e02\u0e04 \u0e07\u0e08\n\u0e09\u0e0a\n"),
    ("Latn-Grek", "abc\nkosmos\n", "\u1f00\u03b2\u03ba\n\u03ba\u03bf\u03c3\u03bc\u03bf\u03c2\n"),
    ("ab \u2192 | ba ;\nc \u2192 C ;\n", "abc\nca\nab", "baC\nCa\nba"),
]
if EXTENDED:
    CASES += [
        ("Thai-Latn", "\u0e01\u0e02\u0e04 \u0e07\u0e08\nab \u0e09\u0e0a\n\n", "kk\u0304hkh ngc\nab c\u0304hch\n\n"),
    ]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        probe = subprocess.run([tconv, "Any-Name", os.devnull], capture_output=True)
        if probe.returncode != 0:
            print("skipped: tconv is built without ICU")
            return 0
        for spec, text, want in CASES:
            path = os.path.join(tmp, "input.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            args = []
            if "\u2192" in spec:
                rules = os.path.join(tmp, "rules.txt")
                with open(rules, "w", encoding="utf-8") as f:
                    f.write(spec)
                args.append("--rules-file=" + rules)
            else:
                args.append(spec)
            plan = subprocess.run([tconv, "--plan", "--lines"] + args, capture_output=True).stdout.decode("utf-8", "replace")
            verdict = [l for l in plan.split("\n") if "region scope" in l]
            if not verdict or "one run" not in verdict[0] or "ICU line by line" not in verdict[0]:
                failures += 1
                print("FAIL %r: %s" % (spec.split("\n")[0], verdict))
            run = subprocess.run([tconv, "--lines"] + args + [path], capture_output=True)
            got = run.stdout.decode("utf-8", "replace")
            if run.returncode != 0 or got != want:
                failures += 1
                print("FAIL %r:\n  want %r\n  got  %r\n%s" % (spec.split("\n")[0], want, got, run.stderr.decode("utf-8", "replace")[-300:]))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
