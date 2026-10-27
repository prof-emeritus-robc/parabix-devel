#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Quick test for hexlines: compares output against a Python reference on a small
# generated input with lines of varied lengths (spanning many blocks and several
# segments), single and multithreaded.
#
# Usage: python3 hexlines_test.py <hexlines_executable>

import os
import random
import subprocess
import sys
import tempfile


def reference(data):
    return '\n'.join(' '.join('%02X' % c for c in line) for line in data.split(b'\n')).encode()


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: hexlines_test.py <hexlines_executable>')
    hexlines = sys.argv[1]
    rng = random.Random(1)
    lines = []
    for n in [0, 1, 31, 32, 33, 63, 64, 65, 127, 128, 300] + [rng.randrange(200) for _ in range(150)]:
        lines.append(bytes(rng.choice(b'abcXYZ019 \t\x00\xc3\xa9\xff') for _ in range(n)))
    data = b'\n'.join(lines)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'input.bin')
        with open(path, 'wb') as f:
            f.write(data)
        expected = reference(data)
        for flags in [['--thread-num=1'], ['--thread-num=3', '--segment-size=1']]:
            result = subprocess.run([hexlines] + flags + [path], capture_output=True)
            if result.returncode != 0 or result.stdout != expected:
                failures += 1
                print('FAIL %s (exit %d)' % (' '.join(flags), result.returncode))
    if failures:
        sys.exit('%d hexlines test(s) failed' % failures)
    print('All hexlines tests passed.')


if __name__ == '__main__':
    main()
