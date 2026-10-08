#!/usr/bin/env python3
#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
# Checks the output of unwritten_wide_output against a reference over random inputs whose lengths
# fall around pack and block boundaries, for several block widths and numbers of byte streams.
#
# usage: unwritten_wide_output_test.py <path to unwritten_wide_output> [-v]

import os
import random
import subprocess
import sys
import tempfile

BLOCK_WIDTHS = [128, 256, 512]
STREAM_COUNTS = [1, 2, 3]
SEGMENT_SIZES = [None, "1"]


def lengths(block_width):
    pack = block_width // 8
    return sorted({0, 1, pack - 1, pack, pack + 1, 2 * pack, 3 * pack + 5, block_width - 1, block_width,
                   block_width + 1, block_width + pack, 2 * block_width + 3 * pack, 3 * block_width - 1,
                   10000, 33023})


def reference(data, block_width, streams):
    """Output byte i is f(0, i) ^ f(0, i + pack) ^ ... ^ f(streams - 1, i + pack), where
    f(s, k) is input byte k plus 1 + s within the input and 0 past its end."""
    n = len(data)
    pack = block_width // 8

    def f(s, k):
        return (data[k] + 1 + s) & 0xFF if k < n else 0

    out = bytearray(n)
    for i in range(n):
        r = f(0, i)
        for s in range(streams):
            r ^= f(s, i + pack)
        out[i] = r
    return bytes(out)


def run(exe, path, block_width, streams, seg):
    cmd = [exe, path, "-BlockSize=%d" % block_width, "-streams=%d" % streams,
           "-object-cache-salt=unwritten_wide_test"]
    if seg is not None:
        cmd.append("-segment-size=" + seg)
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
        for block_width in BLOCK_WIDTHS:
            for n in lengths(block_width):
                path = os.path.join(tmp, "in%d_%d.bin" % (block_width, n))
                data = bytes(rng.getrandbits(8) for _ in range(n))
                with open(path, "wb") as f:
                    f.write(data)
                for streams in STREAM_COUNTS:
                    expected = reference(data, block_width, streams)
                    for seg in SEGMENT_SIZES:
                        cases += 1
                        rc, out, err, cmd = run(exe, path, block_width, streams, seg)
                        if rc != 0 or out != expected:
                            failures += 1
                            print("FAIL: %s" % cmd)
                            if rc != 0:
                                print("  exited with %d: %s" % (rc, err.strip()[:400]))
                            else:
                                first = next((i for i, (a, b) in enumerate(zip(expected, out)) if a != b),
                                             min(len(expected), len(out)))
                                print("  output differs from reference at byte %d (lengths %d, %d)"
                                      % (first, len(expected), len(out)))
                        elif verbose:
                            print("ok: %s" % cmd)
    print("%d of %d cases failed" % (failures, cases))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
