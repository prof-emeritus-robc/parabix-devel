#!/usr/bin/env python3
#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
# Compares the output of provisional_lookahead with and without -ProvisionalLookAheadStride over
# random inputs whose lengths fall around block and segment boundaries.
#
# usage: provisional_lookahead_test.py <path to provisional_lookahead> [-v]

import os
import random
import subprocess
import sys
import tempfile

LOOKAHEAD_CHAINS = ["3", "1,5,7", "2,20,9", "31,32", "60,60,60", "200,100"]
SEGMENT_SIZES = [None, "1", "2", "3"]
LENGTHS = sorted({0, 1, 2, 63, 64, 65, 127, 128, 129, 255, 256, 257, 383, 511, 512, 513,
                  761, 777, 1892, 10253, 33023, 100003})


def run(exe, path, chain, seg, provisional):
    cmd = [exe, path, "-la=" + chain, "-object-cache-salt=provisional_test"]
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
            with open(path, "wb") as f:
                f.write(bytes(rng.getrandbits(8) for _ in range(n)))
            files.append(path)
        for chain in LOOKAHEAD_CHAINS:
            for seg in SEGMENT_SIZES:
                for path in files:
                    cases += 1
                    rc0, out0, err0, cmd0 = run(exe, path, chain, seg, False)
                    rc1, out1, err1, cmd1 = run(exe, path, chain, seg, True)
                    if rc0 != 0 or rc1 != 0 or out0 != out1:
                        failures += 1
                        print("FAIL: %s" % cmd1)
                        if rc0 != 0:
                            print("  reference exited with %d: %s" % (rc0, err0.strip()[:400]))
                        if rc1 != 0:
                            print("  provisional exited with %d: %s" % (rc1, err1.strip()[:400]))
                        if rc0 == 0 and rc1 == 0:
                            first = next((i for i, (a, b) in enumerate(zip(out0, out1)) if a != b),
                                         min(len(out0), len(out1)))
                            print("  outputs differ at byte %d (lengths %d, %d)" % (first, len(out0), len(out1)))
                    elif verbose:
                        print("ok: %s" % cmd1)
    print("%d of %d cases failed" % (failures, cases))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
