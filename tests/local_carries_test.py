#!/usr/bin/env python3
#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
# Compares the output of local_carries with and without -LocalCarries over random inputs
# whose lengths fall around block and segment boundaries, and checks both against a reference
# computed here.  The advance chains cover short advances, long advances (of 64 or more) and
# advances of more than a block, which rebuild their carries from several blocks.  By default,
# a small set of cases completes in under a second; --extended-testing runs all the
# combinations of the advance chains, lengths and segment sizes below.
#
# usage: local_carries_test.py <path to local_carries> [--extended-testing] [-v]

import os
import random
import subprocess
import sys
import tempfile

ADVANCE_CHAINS = ["1", "3,70", "2,200", "126,127,128", "300", "5,600,9"]
SEGMENT_SIZES = [None, "1", "2", "3"]
LENGTHS = sorted({0, 1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 257, 383, 511, 512, 513,
                  761, 777, 1892, 10253, 33023, 100003})

# The default cases: a chain with a short, a long and a multi-block advance, the empty input,
# a length just past a block boundary and one of several blocks and segments, and the default
# segment size and segments of one stride.
QUICK_ADVANCE_CHAINS = ["3,70,200"]
QUICK_SEGMENT_SIZES = [None, "1"]
QUICK_LENGTHS = [0, 257, 10253]


def reference(data, chain):
    """The output of local_carries, computing each stream as an integer whose bit k is
    the stream bit at position k.  Basis stream i holds bit i of each byte."""
    n = len(data)
    mask = (1 << n) - 1
    streams = []
    for i in range(8):
        bits = "".join("1" if (c >> i) & 1 else "0" for c in reversed(data))
        streams.append(int(bits, 2) if bits else 0)
    for amount in (int(x) for x in chain.split(",")):
        out = []
        for i in range(8):
            adv = (streams[(i + 2) % 8] << amount) & mask
            follows = ((streams[(i + 1) % 8] << 1) & mask) & adv
            inner = (follows << 2) & mask
            out.append(streams[i] ^ adv ^ inner)
        streams = out
    result = bytearray(n)
    for i in range(8):
        bits = bin(streams[i])[2:].zfill(n)[::-1] if n else ""
        for k, bit in enumerate(bits):
            if bit == "1":
                result[k] |= 1 << i
    return bytes(result)


def run(exe, path, chain, seg, rebuild):
    cmd = [exe, path, "-adv=" + chain, "-object-cache-salt=local_carries_test"]
    if seg is not None:
        cmd.append("-segment-size=" + seg)
    if not rebuild:
        cmd.append("-LocalCarries=false")
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
    if "--extended-testing" in sys.argv[2:]:
        chains, segment_sizes, lengths = ADVANCE_CHAINS, SEGMENT_SIZES, LENGTHS
    else:
        chains, segment_sizes, lengths = QUICK_ADVANCE_CHAINS, QUICK_SEGMENT_SIZES, QUICK_LENGTHS
    rng = random.Random(20261024)
    failures = 0
    cases = 0
    with tempfile.TemporaryDirectory() as tmp:
        files = []
        for n in lengths:
            path = os.path.join(tmp, "in%d.bin" % n)
            data = bytes(rng.getrandbits(8) for _ in range(n))
            with open(path, "wb") as f:
                f.write(data)
            files.append((path, data))
        for chain in chains:
            for path, data in files:
                expected = reference(data, chain)
                for seg in segment_sizes:
                    cases += 1
                    rc0, out0, err0, cmd0 = run(exe, path, chain, seg, False)
                    rc1, out1, err1, cmd1 = run(exe, path, chain, seg, True)
                    if rc0 != 0 or rc1 != 0 or out0 != expected or out1 != expected:
                        failures += 1
                        print("FAIL: %s" % cmd1)
                        if rc0 != 0:
                            print("  with stored carries exited with %d: %s" % (rc0, err0.strip()[:400]))
                        if rc1 != 0:
                            print("  with rebuilt carries exited with %d: %s" % (rc1, err1.strip()[:400]))
                        for name, out in (("with stored carries", out0), ("with rebuilt carries", out1)):
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
