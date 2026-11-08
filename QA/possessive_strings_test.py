#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  icgrep: possessive repetitions of alternatives of strings (see
#  lib/re/transforms/resolve_possessive.cpp), which take at each repetition
#  the longest alternative that matches, as ICU matches a set with strings, and
#  never give back: the alternatives are made unambiguous (one matches at each
#  position) before the repetition is resolved.  (?:a|ab)*+b takes ab (PCRE
#  would take a); (?:aa|a){2}+ takes aa first; (?:ab|bc|a)*+ does not reach the
#  end of abc by a, bc.
#
#  possessive_strings_test.py <icgrep binary>

import os
import subprocess
import sys
import tempfile

# (pattern, lines, the lines that match)
CASES = [
    (r'^(?:ab|bc|a)*+$', ['abc', 'ab', 'abbc', 'a'], ['ab', 'abbc', 'a']),
    (r'^(?:aa|a){2}+a$', ['aaa', 'aaaa', 'aaaaa'], ['aaaaa']),
    (r'^(?:a|ab)*+b', ['ab', 'abb', 'aab'], ['abb']),
    (r'^x(?=(?:ab|bc|a)*+$)', ['xabc', 'xab'], ['xab']),
]


def main():
    icgrep = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'lines.txt')
        for pattern, lines, want in CASES:
            with open(path, 'w') as f:
                f.write('\n'.join(lines) + '\n')
            run = subprocess.run([icgrep, pattern, path], capture_output=True, text=True)
            got = sorted(l for l in run.stdout.split('\n') if l)
            if got != sorted(want):
                failures += 1
                print('FAIL %s:\n  want %r\n  got  %r\n%s' % (pattern, sorted(want), got, run.stderr))
    print('%d failures' % failures)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
