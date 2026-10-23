#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The regular expression engine over the codes of a multiplexed alphabet,
#  compared with the engine over the 21-bit Unicode basis (see
#  multiplexed_engine.cpp): forward matching, starts and lookaheads by
#  reversal, lookbehinds matched forward, anchors, string classes, lookahead
#  chains and the walks of capture boundaries.  The results must agree, and
#  each pattern must have results (so that the comparison is not vacuous).

import os
import random
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# Groups of up to 4 patterns (kind: e ends, s starts, cN capture N), and
# whether the text is without line regions.
GROUPS = [
    (["e:[a-c]+d", "e:(?i)café", "e:x(?=(?:ab|c)+d)", "e:foo(?![a-z]*bar)"], False),
    (["s:(<[^>]*>)+[a-z]", "s:(?:a*b)*c", "s:(?:[a-z]+@)+[a-z]+$", "s:(?<=c(?:a*b)*)(?:a*b)*d"], False),
    (["s:[a-z]{2,4}é", "s:x(?:ab|cd)*y", "s:(?=[a-z]*q)[a-z]+", "s:日[^ ]*"], False),
    (["c1:([a-z0-9]*[a-z])[0-9]+;", "c2:([a-z]+)@([a-z]+);", "c1:x[a-z0-9]*([a-z][0-9]+);"], False),
    (["s:(?:a*b)*c$", "e:x(?=(?:a*b)*(?<!a)c)", "e:[a-c]+$", "s:^(?:a*b)*c"], True),
]

FRAGMENTS = ["ab", "a", "b", "c", "d", "cd", "x", "y", "q", "<p>", "<b>", "é", "café", "CAFÉ", "foo",
             "bar", "@", ";", "1", "23", "日", "本", " ", " ", "z"]


def text(noregions):
    # Without regions, the text is one region, which begins and ends with
    # matches of the anchored patterns.
    rng = random.Random(22)
    lines = []
    for _ in range(300):
        lines.append("".join(rng.choice(FRAGMENTS) for _ in range(rng.randint(0, 40))))
    if noregions:
        return "ababc" + "\n".join(lines) + "abc"
    return "\n".join(lines) + "\n"


def run(binary, paths, group):
    patterns, noregions = group
    cmd = [binary, paths[noregions]] + [a for p in patterns for a in ("-e", p)]
    if noregions:
        cmd.append("-noregions")
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        return [f"{patterns}: exit {proc.returncode}: {proc.stderr.decode(errors='replace')}"]
    header, sep, marks = proc.stdout.partition(b"--\n")
    if not sep:
        return [f"{patterns}: no output"]
    errors = []
    for i, p in enumerate(patterns):
        diffs = [k for k, b in enumerate(marks) if b & (1 << i)]
        if diffs:
            errors.append(f"{p}{' (noregions)' if noregions else ''}: {len(diffs)} positions differ, "
                          f"the first at character {diffs[0]}")
        if not any(b & (1 << (4 + i)) for b in marks):
            errors.append(f"{p}: no results to compare")
    return errors


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        paths = {}
        for noregions in (False, True):
            paths[noregions] = os.path.join(tmp, f"text{int(noregions)}.txt")
            with open(paths[noregions], "w", encoding="utf-8") as f:
                f.write(text(noregions))
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda g: run(binary, paths, g), GROUPS))
    errors = [e for r in results for e in r]
    for e in errors:
        print("FAIL", e)
    if errors:
        sys.exit(1)
    print(f"multiplexed_engine: {sum(len(g[0]) for g in GROUPS)} patterns agree")


if __name__ == "__main__":
    main()
