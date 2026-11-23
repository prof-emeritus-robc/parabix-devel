#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  A fuzzer of icgrep's lookarounds with classes of characters of several
#  UTF-8 lengths (1 to 4 bytes): random patterns over random text, the counts
#  of matching lines of icgrep in its three modes (byte mode, the UTF-8 indexed
#  mode with -UnicodeIndexing, full Unicode indexing with -UnicodeIndexing
#  -UnicodeBasisMode) compared with those of GNU grep's PCRE mode.  Kinds:
#    nested     lookarounds within a lookbehind (the default);
#    lookahead  lookaheads of several characters, not within lookbehinds.
#  Mismatches and icgrep errors are printed with the pattern and the data file
#  (kept, in the directory given or a temporary one); the exit status is 1 if
#  there are any.
#
#  lookaround_fuzz.py <icgrep binary> [--kind=nested|lookahead] [--seed=N]
#                     [--count=N] [--ggrep=ggrep] [--dir=DIR]
#
#  (GNU grep is ggrep on macOS, grep elsewhere.)

import os
import random
import subprocess
import sys
import tempfile

CHARS = ["a", "b", "é", "ʃ", "中", "😀"]
MODES = ([], ["-UnicodeIndexing"], ["-UnicodeIndexing", "-UnicodeBasisMode"])


def main():
    if len(sys.argv) < 2:
        print("usage: lookaround_fuzz.py <icgrep binary> [--kind=nested|lookahead] [--seed=N] [--count=N] "
              "[--ggrep=ggrep] [--dir=DIR]")
        return 2
    icgrep = sys.argv[1]
    opts = {"kind": "nested", "seed": "1", "count": "150", "ggrep": "ggrep" if sys.platform == "darwin" else "grep",
            "dir": None}
    for arg in sys.argv[2:]:
        key, _, value = arg.lstrip("-").partition("=")
        if key not in opts:
            print("unknown option " + arg)
            return 2
        opts[key] = value
    seed, count = int(opts["seed"]), int(opts["count"])
    rnd = random.Random(seed)

    def item():
        k = rnd.choice([1, 1, 2, 3])
        s = rnd.sample(CHARS, k)
        return s[0] if k == 1 else "[" + "".join(s) + "]"

    def seq(n):
        return "".join(item() for _ in range(n))

    def inner():
        return "(" + rnd.choice(["?<!", "?<=", "?!", "?="]) + seq(rnd.choice([1, 1, 2])) + ")"

    def nested():
        body = rnd.choice([inner() + seq(rnd.choice([1, 2])), seq(1) + inner() + seq(1), seq(rnd.choice([1, 2])) + inner()])
        return "(" + rnd.choice(["?<=", "?<!"]) + body + ")" + seq(1)

    def lookahead():
        la = "(" + rnd.choice(["?=", "?!"]) + seq(rnd.choice([2, 2, 3])) + ")"
        return rnd.choice([seq(1) + la + seq(rnd.choice([0, 1])), seq(2) + la, la + seq(2),
                           seq(1) + la + seq(1) + "(" + rnd.choice(["?=", "?!"]) + seq(2) + ")"])

    pattern = {"nested": nested, "lookahead": lookahead}.get(opts["kind"])
    if pattern is None:
        print("unknown kind " + opts["kind"])
        return 2
    directory = opts["dir"] or tempfile.mkdtemp(prefix="lookaround_fuzz_")
    data = os.path.join(directory, "data_%s_%d.txt" % (opts["kind"], seed))
    lines = ["".join(rnd.choice(CHARS) for _ in range(rnd.randint(1, 12))) for _ in range(400)]
    with open(data, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    agree = mismatches = skipped = errors = 0
    for _ in range(count):
        p = pattern()
        g = subprocess.run([opts["ggrep"], "-P", "-c", p, data], capture_output=True, text=True)
        if g.returncode > 1:
            skipped += 1
            continue
        for flags in MODES:
            r = subprocess.run([icgrep] + flags + ["-c", p, data], capture_output=True, text=True)
            if r.returncode > 1 or not r.stdout.strip():
                errors += 1
                print("ERROR %s %r %s: %s" % (" ".join(flags) or "(default)", p, data, r.stderr.strip()[:200]))
            elif r.stdout.strip() == g.stdout.strip():
                agree += 1
            else:
                mismatches += 1
                print("MISMATCH %s %r %s: icgrep %s, GNU grep %s"
                      % (" ".join(flags) or "(default)", p, data, r.stdout.strip(), g.stdout.strip()))
    print("kind %s seed %d: %d agree, %d mismatches, %d icgrep errors, %d patterns GNU grep rejects"
          % (opts["kind"], seed, agree, mismatches, errors, skipped))
    return 1 if mismatches or errors else 0


if __name__ == "__main__":
    sys.exit(main())
