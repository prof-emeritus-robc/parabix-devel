#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Quick test for xch: property-based and source/target character translation
# on short inputs, in both UTF-8 and U21 modes, and rejection of unknown properties.
#
# Usage: python3 xch_test.py <xch_executable>

import os
import subprocess
import sys
import tempfile

CASES = [
    (['--prop=Lowercase_Mapping'], 'Hello World ÄÖ\n', 'hello world äö\n'),
    (['--prop=Uppercase_Mapping'], 'Straße ǆ\n', 'STRASSE Ǆ\n'),
    (['--U21', '--prop=Uppercase_Mapping'], 'Straße ǆ\n', 'STRASSE Ǆ\n'),
    (['--prop=Simple_Lowercase_Mapping'], 'ÀBC\n', 'àbc\n'),
    (['--src=abc', '--tgt=xyz'], 'aabbcc dd\n', 'xxyyzz dd\n'),
]


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: xch_test.py <xch_executable>')
    xch = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'input.txt')
        for flags, text, expected in CASES:
            with open(path, 'w', encoding='utf-8') as f:
                f.write(text)
            result = subprocess.run([xch] + flags + [path], capture_output=True)
            actual = result.stdout.decode('utf-8', errors='replace')
            if result.returncode != 0 or actual != expected:
                failures += 1
                print('FAIL %s: expected %r, got %r (exit %d)' % (' '.join(flags), expected, actual, result.returncode))
        result = subprocess.run([xch, '--prop=No_Such_Property', path], capture_output=True)
        if b'Unknown property' not in result.stderr:
            failures += 1
            print('FAIL: unknown property not reported (exit %d)' % result.returncode)
    if failures:
        sys.exit('%d xch test(s) failed' % failures)
    print('All xch tests passed.')


if __name__ == '__main__':
    main()
