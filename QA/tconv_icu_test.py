#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv --use-ICU (see tools/ldml/icu_transform.h): a transform tconv does
#  not implement (the built-in Any-Name, a rules file whose run is followed by
#  an after context) is applied by ICU by default, and with --use-ICU=0 is an
#  error (exit status 2); a transform tconv implements (Any-Upper) is applied
#  by ICU where --use-ICU matches its name, with the same result.  Skipped
#  (passing) if tconv is built without ICU.
#
#  tconv_icu_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = "a b+ } c → X ;\nb → Y ;\n"

# (arguments, input, expected output, or None for exit status 2)
CASES = [
    (["Any-Name"], "a\u00df", "\\N{LATIN SMALL LETTER A}\\N{LATIN SMALL LETTER SHARP S}"),
    (["--use-ICU=0", "Any-Name"], "a\u00df", None),
    (["Any-Upper"], "a\u00df", "ASS"),
    (["--use-ICU=Any-Upp.*", "Any-Upper"], "a\u00df", "ASS"),
    (["--rules-file={rules}"], "abbc abd", "Xc aYd"),
    (["--use-ICU=0", "--rules-file={rules}"], "abbc abd", None),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        probe = subprocess.run([tconv, "Any-Name", os.devnull], capture_output=True)
        if b"built without ICU" in probe.stderr:
            print("SKIP: tconv is built without ICU")
            return 0
        for args, text, want in CASES:
            with open(input, "w", encoding="utf-8") as f:
                f.write(text)
            argv = [tconv] + [a.format(rules=rules) for a in args] + [input]
            run = subprocess.run(argv, capture_output=True)
            got = run.stdout.decode("utf-8", "replace")
            if want is None:
                if run.returncode != 2:
                    failures += 1
                    print("FAIL %s: want exit status 2, got %d (%r)" % (" ".join(args), run.returncode, got))
            elif run.returncode != 0 or got != want:
                failures += 1
                print("FAIL %s:\n  want %r\n  got  %r\n%s" % (" ".join(args), want, got, run.stderr.decode("utf-8", "replace")))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
