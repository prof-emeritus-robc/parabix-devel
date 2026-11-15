#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Smoke test of the Div32Code / Div64Code kernels (doc/ldml-divcode-plan.md):
#  tconv --divcode-codes=G replaces each character by U+4000 | (the external
#  classes it is in << 10) | its code in the alphabet of conversion group G,
#  and describes the alphabet (shift, blocks, external sets) on the standard
#  error.  For conversion groups with small and large alphabets (up to 9 code
#  bits), with and without external sets, for div32 and div64, the codes and
#  classes of random text (characters of each block, of the external sets,
#  and others, of 1 to 4 UTF-8 bytes) must be those the description gives.
#  (Skipped without the CLDR transforms.)
#
#  tconv_divcode_test.py <tconv binary>

import json
import os
import random
import subprocess
import sys
import tempfile

GROUPS = [
    ("Bopo-Latn", 1),                       # 3 blocks
    ("Halfwidth-Fullwidth", 1),             # 10 blocks
    ("und_FONIPA-und_FONXSAMP", 1),         # 22 blocks (5 bits)
    ("byn_Ethi-byn_Latn/Xaleget", 1),       # an external set
    ("tk_Cyrl-tk/BGN", 1),                  # two external sets
    ("de-ASCII", 1),                        # an external set
    ("Hant-Hans", 1),                       # about 450 blocks (9 bits)
]
OTHER = "aZ 09.,é€中😀𐍈́\t"


def valid(cp):
    return not (0xD800 <= cp <= 0xDFFF) and cp != 0 and cp <= 0x10FFFF


def sample(rnd, ranges):
    lo, hi = rnd.choice(ranges)
    return rnd.randint(lo, hi)


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, group in GROUPS:
            for shift in (5, 6):
                rnd = random.Random("%s %d %d" % (name, group, shift))
                path = os.path.join(tmp, "input.txt")
                # (The alphabet first, from a run on a small input.)
                with open(path, "w", encoding="utf-8") as f:
                    f.write("a\n")
                args = [tconv, "--divcode-codes=%d" % group, "--divcode-shift=%d" % shift, name, path]
                run = subprocess.run(args, capture_output=True)
                if b"unknown transform" in run.stderr:
                    print("tconv_divcode_test: skipped (no CLDR transforms directory)")
                    return
                if run.returncode != 0:
                    failures += 1
                    print("FAIL %s#%d s=%d: exit %d %s" % (name, group, shift, run.returncode, run.stderr[-300:]))
                    continue
                desc = json.loads(run.stderr.decode("utf-8").splitlines()[0])
                blocks = desc["blocks"]
                code_of = {b: i + 1 for i, b in enumerate(blocks)}
                externals = desc["externals"]
                # The text: characters of each block, of each external set, and others.
                chars = []
                for b in blocks:
                    for _ in range(4):
                        cp = (b << shift) | rnd.randrange(1 << shift)
                        if valid(cp):
                            chars.append(cp)
                for e in externals:
                    for _ in range(16):
                        cp = sample(rnd, e)
                        if valid(cp):
                            chars.append(cp)
                chars += [ord(c) for c in OTHER]
                for _ in range(64):
                    cp = rnd.choice([rnd.randint(0x20, 0x7FF), rnd.randint(0x800, 0xFFFF), rnd.randint(0x10000, 0x10FFFF)])
                    if valid(cp):
                        chars.append(cp)
                text = []
                for _ in range(20000):
                    text.append(rnd.choice(chars))
                    if rnd.random() < 0.02:
                        text.append(0x0A)
                text = "".join(chr(c) for c in text)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
                run = subprocess.run(args, capture_output=True)
                if run.returncode != 0:
                    failures += 1
                    print("FAIL %s#%d s=%d: exit %d %s" % (name, group, shift, run.returncode, run.stderr[-300:]))
                    continue
                out = run.stdout.decode("utf-8")
                if len(out) != len(text):
                    failures += 1
                    print("FAIL %s#%d s=%d: %d characters out, %d in" % (name, group, shift, len(out), len(text)))
                    continue
                bad = 0
                for i, (c, d) in enumerate(zip(text, out)):
                    cp = ord(c)
                    code = code_of.get(cp >> shift, 0)
                    ext = 0
                    for k, e in enumerate(externals):
                        if any(lo <= cp <= hi for lo, hi in e):
                            ext |= 1 << k
                    expected = 0x4000 | (ext << 10) | code
                    if ord(d) != expected:
                        if bad < 5:
                            print("FAIL %s#%d s=%d: U+%04X at %d: code %d, externals %d; expected %d, %d"
                                  % (name, group, shift, cp, i, (ord(d) - 0x4000) & 0x3FF, (ord(d) - 0x4000) >> 10, code, ext))
                        bad += 1
                if bad:
                    failures += 1
                else:
                    print("ok %s#%d s=%d: %d blocks, %d external sets (%s)" % (name, group, shift, len(blocks), len(externals),
                                                                              desc["kernel"]))
    if failures:
        print("tconv_divcode_test: %d failures" % failures)
        sys.exit(1)
    print("tconv_divcode_test: passed")


if __name__ == "__main__":
    main()
