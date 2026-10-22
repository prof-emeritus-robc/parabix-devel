#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The boundaries of captures in a search and replace (see capture_spans.cpp),
#  compared with Python's re: the matches of re.finditer within each line (or
#  the whole text, without match regions), whose captures are unique, so that
#  the matching conventions agree.

import os
import subprocess
import sys
import tempfile
import re

TEXT = (
    "foo@bar.com and x@y a@.b @c.\n"
    "café@crème.fr ψ@ω. @@\n"
    "#ab12; #9; #x7; #1a2b3; a1-bc; zz9q12-ab;\n"
    "\n"
    "k=v; key=value; =; k2=;\n"
    "last@line."
)

# Groups of at most 2 (pattern, capture number), each run as one pipeline.
GROUPS = [
    [("([a-z]+)@", 1), ("([a-z]*)@([a-z]*)\\.", 2)],
    [("([a-zéè]+)@([a-zéèω]+)\\.", 1), ("([^ @.\\n]*)@([^ @.\\n]*)\\.", 2)],     # Unicode characters
    [("#([a-z0-9]*)([0-9]);", 1), ("#([a-z0-9]*)([0-9]);", 2)],           # from the end of the match
    [("([a-z0-9]*)=([a-z]*);", 1), ("([a-z0-9]*)=([a-z]*);", 2)],         # empty captures
    [("@([a-z]*)(?![a-z])", 1), ("([a-z])(?!x)", 1)],                    # an empty capture at a follow; adjacent captures
    [("([a-z0-9]*[a-z])[0-9]+;", 1), ("([a-z0-9]*[a-z])([0-9]+)-([a-z]+);", 3)],   # by a walk back (reversal)
]


def byte_offsets(s):
    # The offset of the last byte of each character, and of the position after.
    offsets, n = [], 0
    for ch in s:
        n += len(ch.encode("utf-8"))
        offsets.append(n - 1)
    return offsets


def expected(group, regions):
    data = TEXT.encode("utf-8")
    size = len(data) + (0 if regions else 1)
    if regions and not TEXT.endswith("\n"):
        size += 1           # the line break added at the end of the file
    out = [0] * size
    # Character positions (the added line break or the extension as a character).
    text = TEXT if TEXT.endswith("\n") else TEXT + "\n"
    offsets = byte_offsets(text)
    pieces = text.split("\n")[:-1] if regions else [TEXT]
    base = 0
    for piece in pieces:
        for k, (pattern, capture) in enumerate(group):
            for m in re.finditer(pattern, piece):
                start, follow = m.start(capture), m.end(capture)
                out[offsets[base + start]] |= 1 << (3 * k)
                for p in range(start, follow):
                    out[offsets[base + p]] |= 1 << (3 * k + 1)
                out[offsets[base + follow]] |= 1 << (3 * k + 2)
        base += len(piece) + 1
    return bytes(out)


def main():
    binary = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(TEXT)
        for group in GROUPS:
            for regions in (True, False):
                args = [binary]
                for pattern, capture in group:
                    args += ["-e", pattern, "-c", str(capture)]
                if not regions:
                    args.append("-noregions")
                run = subprocess.run(args + [path], capture_output=True)
                want = expected(group, regions)
                if run.returncode != 0 or run.stdout != want:
                    failures += 1
                    print("FAIL %s%s: exit %d %s" % (group, "" if regions else " (no regions)", run.returncode,
                                                    run.stderr.decode()[:300]))
                    got = run.stdout
                    for i in range(max(len(got), len(want))):
                        g = got[i] if i < len(got) else None
                        w = want[i] if i < len(want) else None
                        if g != w:
                            print("  first difference at byte %d: got %r, expected %r" % (i, g, w))
                            break
    if failures:
        sys.exit(1)
    print("capture_spans_test: all tests passed")


if __name__ == "__main__":
    main()
