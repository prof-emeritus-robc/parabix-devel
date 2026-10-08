#!/usr/bin/env python3
#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
# Compares the output of provisional_lookahead with and without -ProvisionalLookAheadStride over
# random inputs whose lengths fall around block and segment boundaries, and checks both against
# a reference computed here.
#
# usage: provisional_lookahead_test.py <path to provisional_lookahead> [-v]

import os
import random
import subprocess
import sys
import tempfile

LOOKAHEAD_CHAINS = ["3", "1,5,7", "2,20,9", "31,32", "60,60,60", "200,100"]
SEGMENT_SIZES = [None, "1", "2", "3"]
INVERT = [False, True]
LENGTHS = sorted({0, 1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 257, 383, 511, 512, 513,
                  761, 777, 1892, 10253, 33023, 100003})


def reference(data, chain, invert):
    """The output of provisional_lookahead, computing each stream as an integer whose bit k is
    the stream bit at position k.  Basis stream i holds bit i of each byte."""
    n = len(data)
    mask = (1 << n) - 1
    streams = []
    for i in range(8):
        bits = "".join("1" if (c >> i) & 1 else "0" for c in reversed(data))
        streams.append(int(bits, 2) if bits else 0)
    for la in (int(x) for x in chain.split(",")):
        out = []
        for i in range(8):
            lookahead = streams[(i + 1) % 8] >> la
            advance = (streams[(i + 2) % 8] << 3) & mask
            m, c = streams[i], streams[(i + 3) % 8]
            star = ((((m & c) + c) ^ c) | m) & mask
            r = lookahead ^ advance ^ star
            if invert:
                r = ~r & mask
            out.append(r)
        streams = out
    result = bytearray(n)
    for i in range(8):
        bits = bin(streams[i])[2:].zfill(n)[::-1] if n else ""
        for k, bit in enumerate(bits):
            if bit == "1":
                result[k] |= 1 << i
    return bytes(result)


def run(exe, path, chain, seg, invert, provisional):
    cmd = [exe, path, "-la=" + chain, "-object-cache-salt=provisional_test"]
    if invert:
        cmd.append("-invert")
    if seg is not None:
        cmd.append("-segment-size=" + seg)
    if not provisional:
        cmd.append("-ProvisionalLookAheadStride=false")
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except subprocess.TimeoutExpired:
        return -1, b"", "timed out", " ".join(cmd)
    return proc.returncode, proc.stdout, proc.stderr.decode(errors="replace"), " ".join(cmd)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    verbose = "-v" in sys.argv[2:]
    rng = random.Random(20261007)
    failures = 0
    cases = 0
    with tempfile.TemporaryDirectory() as tmp:
        files = []
        for n in LENGTHS:
            path = os.path.join(tmp, "in%d.bin" % n)
            data = bytes(rng.getrandbits(8) for _ in range(n))
            with open(path, "wb") as f:
                f.write(data)
            files.append((path, data))
        for chain in LOOKAHEAD_CHAINS:
            for invert in INVERT:
                for path, data in files:
                    expected = reference(data, chain, invert)
                    for seg in SEGMENT_SIZES:
                        cases += 1
                        rc0, out0, err0, cmd0 = run(exe, path, chain, seg, invert, False)
                        rc1, out1, err1, cmd1 = run(exe, path, chain, seg, invert, True)
                        if rc0 != 0 or rc1 != 0 or out0 != expected or out1 != expected:
                            failures += 1
                            print("FAIL: %s" % cmd1)
                            if rc0 != 0:
                                print("  without provisional strides exited with %d: %s" % (rc0, err0.strip()[:400]))
                            if rc1 != 0:
                                print("  provisional exited with %d: %s" % (rc1, err1.strip()[:400]))
                            for name, out in (("without provisional strides", out0), ("provisional", out1)):
                                if out != expected:
                                    first = next((i for i, (a, b) in enumerate(zip(expected, out)) if a != b),
                                                 min(len(expected), len(out)))
                                    print("  %s output differs from reference at byte %d (lengths %d, %d)"
                                          % (name, first, len(expected), len(out)))
                        elif verbose:
                            print("ok: %s" % cmd1)
    print("%d of %d cases failed" % (failures, cases))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
