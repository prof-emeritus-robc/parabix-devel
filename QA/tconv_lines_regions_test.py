#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv --lines with region scope (see regionScope in tools/ldml/tconv.cpp and
#  doc/masked-transforms-plan.md, steps 3 and 4): a transform whose components
#  all support it is applied in one run over the whole input, each line a
#  region of a mask, and must give the output of one run per line
#  (--lines-regions=0).  Contexts that are not line-bound are bounded by the
#  lines (step 4), in the fused kernels (also the before contexts matched
#  against the converted text, by the correction kernels) and by the engine,
#  which also matches the capture replacements, bounded by the lines.
#  Each case checks the plan's verdict (--plan --lines) and, either way, the
#  equality of the two outputs, on inputs with contexts reaching for a
#  neighbouring line at both edges, empty lines and a last line without a line
#  feed.
#
#  tconv_lines_regions_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

LINES = "b\nd\nbd\ndb\n\nax\ny\naxy\na\ne\nab\nxa\nbx\nt\n-t\n\n"
CONVERTED = "xb\nx\nb\nab\na\nb\nxxb\nx\nxb\nyb\ny\nbb\n\nb\nbx\nqb\nq\nb"

ONE, BOUNDED, PER_LINE = "one run", "bounded", "per-line runs"

# (rules file text or transform name, input, the plan's verdict)
CASES = [
    # Rules without contexts, with a match that would span a line feed
    ("a b \u2192 X ;\nc \u2192 yy ;\nb \u2192 ;\n", LINES + "a", ONE),
    # Within a global filter
    (":: [a-c] ;\na b \u2192 X ;\n", LINES, ONE),
    # Complements, as contexts: they match beyond the line as on the line feed
    ("b } [^d] \u2192 Z ;\nb \u2192 Y ;\n", LINES + "b", ONE),
    ("[^d] { b \u2192 Z ;\nb \u2192 Y ;\n", "b" + LINES, ONE),
    # An inner item without the line feed
    ("a } x y \u2192 Q ;\na \u2192 R ;\n", LINES, ONE),
    # Not line-bound, bounded by the lines: the end of the text without the line
    # feed, an inner complement, and the start of the text with a class
    ("b } [$ x] \u2192 Z ;\nb \u2192 Y ;\n", LINES, BOUNDED),
    ("a } [^d] [^e] \u2192 Q ;\na \u2192 R ;\n", "a\nd\nad\na\ndx\na\nb\n", BOUNDED),
    ("[$ \\-] { t \u2192 T ;\n", LINES, BOUNDED),
    # Before contexts matched against the converted text, bounded (correction
    # kernels): items that would see the previous line's converted text
    ("x \u2192 a ;\n[^a] [^a] { b \u2192 N ;\n", CONVERTED, BOUNDED),
    ("x \u2192 a ;\n[$ y] { b \u2192 B ;\ny \u2192 x ;\n", CONVERTED, BOUNDED),
    ("x \u2192 a ;\n[$ a] [$ a] { b \u2192 B ;\n", CONVERTED, BOUNDED),
    ("x \u2192 a ;\n[^a]* c { b \u2192 B ;\n", "xcb\nc\nb\nab\ncb\nc\nxb", BOUNDED),
    # Contexts seen by the engine (an unbounded repetition), bounded
    ("a } [^x]* y \u2192 Q ;\n", "a\ny\nay\naxy\na b\ny", BOUNDED),
    # Capture replacements, matched by the engine, bounded: a capture whose set
    # has the line feed, and one of variable length that stays one (not a
    # string rule, --capture-strings)
    ("(a [c\\u000A]*) b \u2192 $1 B ;\n", "a\ncb\nacb\nab\na\nb", BOUNDED),
    ("(a b*) c \u2192 $1 C ;\n", "ab\nbc\nabbc\na\nc\nabc", BOUNDED),
    # Insertions, their contexts bounded (step 10)
    ("b { } d \u2192 I ;\n", LINES, BOUNDED),
    # Built-ins
    ("Any-Hex", "a\n\nb", ONE),
    ("Any-Upper", "a\u00df\nb", ONE),
    ("Any-Lower", "\u0391\u03a3\n\u03a3\u0391\n\u0391\u03a3 \u0392\n\u03a3", ONE),
    ("Any-Title", "hello world\nfoo\n\nbar baz", ONE),
    ("Any-NFC", "e\u0301\n\u0301x\ne\n\u0301", ONE),
    ("Any-NFD", "\u00e9\n\u0301\u00e9", ONE),
    ("Any-Remove", "ab\n\nc", ONE),
    ("Any-FCD", "a\u0301\nb\u0323\u0301\n\u0301\u0323\n\u0344\u0323", ONE),
    ("Any-FCC", "a\u0301\n\u0301\ne\u0323\u0302\n\u0302\u0323", ONE),
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for spec, text, expected in CASES:
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
            got = None
            if verdict:
                got = PER_LINE if "per-line runs" in verdict[0] else BOUNDED if "bounded" in verdict[0] else ONE
            if got != expected:
                failures += 1
                print("FAIL %r: region scope %r, expected %s" % (spec, verdict, expected))
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
