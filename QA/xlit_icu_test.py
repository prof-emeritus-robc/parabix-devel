#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit --use-ICU (see tools/ldml/icu_transform.h): a transform xlit does
#  not implement (the built-in Any-Name, a rules file with a run within its
#  text, in a group with a before context) is applied by ICU by default, and with --use-ICU=0 is an
#  error (exit status 2); a transform xlit implements (Any-Upper) is applied
#  by ICU where --use-ICU matches its name, with the same result.  A stage
#  ICU applies within a global filter (Any-BreakInternal) sees the text around
#  each run, as ICU does: no break after a mark that begins a run after a
#  character outside the filter.  The built-ins ICU applies whenever they are
#  called for (Any-Name, Name-Any, Any-BreakInternal) are planned as
#  implementable, unless --use-ICU=0.  Skipped (passing) if xlit is built
#  without ICU.
#
#  xlit_icu_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

RULES = "a b+ c → X ;\nq { b → Z ;\nb → Y ;\n"
BREAKS = ":: [\u0e01-\u0e5b] ;\n:: Any-BreakInternal ;\n\u0e1e → p ;\n"

# (arguments, input, expected output, or None for exit status 2)
CASES = [
    (["Any-Name"], "a\u00df", "\\N{LATIN SMALL LETTER A}\\N{LATIN SMALL LETTER SHARP S}"),
    (["--use-ICU=0", "Any-Name"], "a\u00df", None),
    (["Any-Upper"], "a\u00df", "ASS"),
    (["--use-ICU=Any-Upp.*", "Any-Upper"], "a\u00df", "ASS"),
    (["--rules-file={rules}"], "abbc abd", "X aYd"),
    (["--use-ICU=0", "--rules-file={rules}"], "abbc abd", None),
    (["--plan", "Any-Name"], "", "Any-Name (built-in, applied by ICU): implementable\n"
     "  mode: Unicode: the built-in Any-Name (applied by ICU)\n"),
    (["--plan", "Name-Any"], "", "Name-Any (built-in, applied by ICU): implementable\n"
     "  mode: Unicode: the built-in Name-Any (applied by ICU)\n"),
    (["--use-ICU=0", "--plan", "Any-BreakInternal"], "", None),
    (["Name-Any"], "\\N{LATIN SMALL LETTER SHARP S}", "\u00df"),
    (["--rules-file={breaks}"], "a\u0e35\u0e1e", "a\u0e35p"),
    (["--rules-file={breaks}"], "\u0e35\u0e1e", "\u0e35 p"),
    (["--rules-file={breaks}"], "1\u0e35\u0e1e \u0e35\u0e1e", "1\u0e35p \u0e35 p"),
]


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        breaks = os.path.join(tmp, "breaks.txt")
        with open(breaks, "w", encoding="utf-8") as f:
            f.write(BREAKS)
        input = os.path.join(tmp, "input.txt")
        probe = subprocess.run([xlit, "Any-Name", os.devnull], capture_output=True)
        if b"built without ICU" in probe.stderr:
            print("SKIP: xlit is built without ICU")
            return 0
        for args, text, want in CASES:
            with open(input, "w", encoding="utf-8") as f:
                f.write(text)
            argv = [xlit] + [a.format(rules=rules, breaks=breaks) for a in args] + [input]
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
