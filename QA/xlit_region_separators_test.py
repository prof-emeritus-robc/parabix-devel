#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: region scope relies only on the mask (doc/masked-transforms-plan.md,
#  steps 10-16): a region is a run of positions of the mask, with at least one
#  position outside it between any two regions, which may hold any character.
#  With --region-separators (hidden, for testing), the regions of --lines are
#  separated by other characters than the line feed; where they are not line
#  feeds, line feeds are inserted (outside the mask) at the edges of the
#  regions and deleted afterwards (RulePipelineBuilder::pushRegions).  Each
#  case is a transform (or rules file) and separators taken from its own
#  alphabet (a cased letter for Final_Sigma, a combining mark for the
#  normalizations, the run characters of prefix runs and chain revisits, a
#  mark of a mark run edit, the characters of an escape for Hex-Any); the
#  output of one run must equal that of the same pieces separated by line
#  feeds (each line a region, the semantics of --lines), with the separators
#  put back, unchanged.
#
#  xlit_region_separators_test.py <xlit binary> [--extended-testing]

import os
import random
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

# (transform name or rules file text, separators (hex), alphabet of the pieces)
CASES = [
    ("Any-Lower", "3A3,61", "\u03a3\u03c3\u0391\u0392aB \u03c2x"),
    ("Hex-Any", "5C,75", "\\u0041x"),
    # An insertion rule, its contexts bounded
    ("a { } b \u2192 X ;\nx \u2192 y ;\n", "62,61", "abxab"),
    # A prefix run (it-am's form: a consonant revisited, the apostrophes deleted)
    ("$c = [bcdfg] ;\n($c) \\' \u2192 | $1 ;\n$c \u2192 C ;\n", "27,62", "bcdfga'"),
    # A chain revisit
    ("(a b*) x \u2192 | $1 b ;\n", "62,78", "abxxxcb"),
    # A string rule with an after context, and a converted before context
    ("ab } c \u2192 X ;\nx \u2192 a ;\na { b \u2192 Y ;\n", "63,61", "abcxab"),
]
if EXTENDED:
    CASES += [
        ("Any-Upper", "61", "ab\u00dfi\u03c2"),
        ("Any-NFC", "301,65", "e\u00e9a\u0301\u0323o"),
        ("Any-NFD", "301", "\u00e9\u00e8a\u0229\u0323"),
        ("Any-Hex", "5C", "ab \u00e9"),
        # The deferred rules: runs of iteration marks within the regions (their
        # rounds' engine matches bounded by the regions in their positions)
        ("Kana-Latn", "30FC,30C3", "\u30ab\u30cf\u30bf\u30e3\u30fd\u30fd\u30fe\u30fe\u30fc\u30c3"),
        ("Latn-Cyrl", "6A,20", "abc\u010d\u0107d\u0111efghijklmnoprs\u0161tuvz\u017e'"),
        ("Latn-Arab", "323,61", "tttab\u0323\u0324\u0331\u0308\u0308"),
        ("Kana-Latn", "30FD,30FC", "\u30ab\u30ad\u30b5\u30bf\u30cf\u30e3\u30c3\u30fd\u30fe\u30fc"),
    ]


def run(xlit, spec, extra, path, tmp):
    args = [xlit, "--use-ICU=0", "--lines"] + extra
    if "\u2192" in spec:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(spec)
        args.append("--rules-file=" + rules)
    else:
        args.append(spec)
    r = subprocess.run(args + [path], capture_output=True)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")[-300:]


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for spec, seps, alpha in CASES:
            sepchars = [chr(int(x, 16)) for x in seps.split(",")]
            pieces_alpha = "".join(c for c in alpha if c not in sepchars)
            rnd = random.Random(7)
            pieces = ["".join(rnd.choice(pieces_alpha) for _ in range(rnd.randint(1, 6))) for _ in range(300)]
            joins = [rnd.choice(sepchars) for _ in range(len(pieces) - 1)]
            text = pieces[0] + "".join(j + p for j, p in zip(joins, pieces[1:]))
            path, lines = os.path.join(tmp, "input.txt"), os.path.join(tmp, "lines.txt")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            with open(lines, "w", encoding="utf-8") as f:
                f.write("\n".join(pieces))
            one = run(xlit, spec, ["--region-separators=" + seps], path, tmp)
            ref = run(xlit, spec, [], lines, tmp)
            name = spec.split("\n")[0]
            if one[0] or ref[0]:
                failures += 1
                print("FAIL %r: exit status %d / %d\n%s%s" % (name, one[0], ref[0], one[2], ref[2]))
                continue
            parts = ref[1].split("\n")
            want = parts[0] + "".join(j + p for j, p in zip(joins, parts[1:])) if len(parts) == len(pieces) else None
            if one[1] != want:
                i = next((k for k in range(min(len(one[1]), len(want or ""))) if one[1][k] != want[k]), 0)
                failures += 1
                print("FAIL %r with separators %s at %d:\n  want %r\n  got  %r"
                      % (name, seps, i, (want or "")[max(0, i - 12):i + 12], one[1][max(0, i - 12):i + 12]))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
