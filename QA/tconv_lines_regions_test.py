#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv --lines with region scope (see regionScopeProblem in
#  tools/ldml/tconv.cpp and doc/masked-transforms-plan.md, step 3): a transform
#  whose components all support it is applied in one run over the whole input,
#  each line a region of a mask, and must give the output of one run per line
#  (--lines-regions=0).  Each case checks the plan's verdict (--plan --lines)
#  and, either way, the equality of the two outputs, on inputs with contexts
#  reaching for a neighbouring line at both edges, empty lines and a last line
#  without a line feed.
#
#  tconv_lines_regions_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

LINES = "b\nd\nbd\ndb\n\nax\ny\naxy\na\ne\nab\nxa\nbx\nt\n-t\n\n"

# (rules file text or transform name, input, whether in one run)
CASES = [
    # Rules without contexts, with a match that would span a line feed
    ("a b \u2192 X ;\nc \u2192 yy ;\nb \u2192 ;\n", LINES + "a", True),
    # Within a global filter
    (":: [a-c] ;\na b \u2192 X ;\n", LINES, True),
    # Complements, as contexts: they match beyond the line as on the line feed
    ("b } [^d] \u2192 Z ;\nb \u2192 Y ;\n", LINES + "b", True),
    ("[^d] { b \u2192 Z ;\nb \u2192 Y ;\n", "b" + LINES, True),
    # An inner item without the line feed
    ("a } x y \u2192 Q ;\na \u2192 R ;\n", LINES, True),
    # Not line-bound: the end of the text without the line feed, an inner
    # complement, and the start of the text with a class
    ("b } [$ x] \u2192 Z ;\nb \u2192 Y ;\n", LINES, False),
    ("a } [^d] [^e] \u2192 Q ;\na \u2192 R ;\n", LINES, False),
    ("[$ \\-] { t \u2192 T ;\n", LINES, False),
    # Insertions keep the per-line runs
    ("b { } d \u2192 I ;\n", LINES, False),
    # Built-ins
    ("Any-Hex", "a\n\nb", True),
    ("Any-Upper", "a\u00df\nb", True),
    ("Any-Lower", "\u0391\u03a3\n\u03a3\u0391\n\u0391\u03a3 \u0392\n\u03a3", True),
    ("Any-Title", "hello world\nfoo\n\nbar baz", True),
    ("Any-NFC", "e\u0301\n\u0301x\ne\n\u0301", True),
    ("Any-NFD", "\u00e9\n\u0301\u00e9", True),
    ("Any-Remove", "ab\n\nc", True),
    ("Any-FCD", "a\u0301\nb", False),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for spec, text, oneRun in CASES:
            path = os.path.join(tmp, "input.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            if "\u2192" in spec:
                rules = os.path.join(tmp, "rules.txt")
                with open(rules, "w", encoding="utf-8") as f:
                    f.write(spec)
                transform = ["--rules-file=" + rules]
            else:
                transform = [spec]
            plan = subprocess.run([tconv, "--use-ICU=0", "--plan", "--lines"] + transform, capture_output=True)
            verdict = [l for l in plan.stdout.decode("utf-8", "replace").split("\n") if "region scope" in l]
            got = bool(verdict) and "one run" in verdict[0]
            if got != oneRun:
                failures += 1
                print("FAIL %r: region scope %r, expected %s" % (spec, verdict, "one run" if oneRun else "per-line runs"))
            outputs = []
            for regions in ("1", "0"):
                run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--lines-regions=" + regions] + transform + [path],
                                     capture_output=True)
                outputs.append((run.returncode, run.stdout))
            if outputs[0] != outputs[1]:
                failures += 1
                print("FAIL %r on %r:\n  one run    %r\n  line runs  %r" % (spec, text, outputs[0], outputs[1]))
            elif outputs[0][0] != 0:
                failures += 1
                print("FAIL %r: exit status %d" % (spec, outputs[0][0]))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
