#!/usr/bin/env python3
#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  string_set_test.py <string_set binary>
#
#  The longest matches of sets with strings (kernel::StringSetKernel, see
#  tests/string_set.cpp), forward and backward, over the 21-bit basis and over a
#  multiplexed code, compared with a reference: at each codepoint, the length of
#  the longest member beginning (backward: ending) there.  The sets are some of
#  und_FONIPA-ar's and random ones (fixed seed, so that the kernels are cached
#  across runs), with strings that are prefixes and suffixes of one another,
#  single characters that begin or end strings, combining marks, and characters
#  of 2, 3 and 4 UTF-8 bytes; the texts are long enough to cross many blocks.

import os
import random
import subprocess
import sys
import tempfile

POOL = ['a', 'b', 'e', 'i', 'n', 'ɪ', 'ʃ', 't', '̞', '̈', '̊', '̼', '͡', '中', '𝒜']

FIXED = [
    #  $UVowel
    ('yuʉɯʏøɤoɞɔwʍʷ', ['ɨ', 'ʉ', 'ɪ̈', 'ʊ̈', 'ɯ̽', 'ʊ', 'ø̞', 'ɤ̞', 'o̞', 'w̥']),
    #  [{n̼̊} {n̼} {n̥} n {ɳ̊} ɳ {ɲ̊} {ɲ̥} ɲ]
    ('nɳɲ', ['n̼̊', 'n̼', 'n̥', 'ɳ̊', 'ɲ̊', 'ɲ̥']),
    #  [{t͡ʃ} ʧ]
    ('ʧ', ['t͡ʃ']),
    #  $IVowel
    ('iɪe', ['e̞']),
]


def random_set(rng):
    chars = ''.join(c for c in POOL if rng.random() < 0.4)
    strings = set()
    for _ in range(rng.randint(1, 5)):
        s = ''.join(rng.choice(POOL) for _ in range(rng.randint(2, 4)))
        strings.add(s)
        if rng.random() < 0.5:
            strings.add(s + rng.choice(POOL))       # an extension
        if rng.random() < 0.3:
            strings.add(rng.choice(POOL) + s)       # a longer string ending with s
    return chars, sorted(strings)


def text_for(rng, chars, strings, n):
    pieces = []
    members = list(chars) + strings
    while sum(len(p) for p in pieces) < n:
        if members and rng.random() < 0.5:
            pieces.append(rng.choice(members))
        else:
            pieces.append(rng.choice(POOL))
        if rng.random() < 0.02:
            pieces.append('\n')
    return ''.join(pieces)


def reference(text, chars, strings, backward):
    members = [c for c in chars] + [s for s in strings if len(s) > 1]
    lengths = sorted(set(len(m) for m in members))
    out = bytearray()
    for i in range(len(text)):
        best = 0
        for m in members:
            if backward:
                if i + 1 >= len(m) and text[i + 1 - len(m):i + 1] == m:
                    best = max(best, len(m))
            elif text.startswith(m, i):
                best = max(best, len(m))
        out.append(1 << lengths.index(best) if best else 0)
    return bytes(out)


def set_file(chars, strings):
    first = ' '.join('%X' % ord(c) for c in sorted(set(chars)))
    rest = [' '.join('%X' % ord(c) for c in s) for s in strings]
    return '\n'.join([first] + rest) + '\n'


def main():
    binary = sys.argv[1]
    rng = random.Random(20261126)
    cases = list(FIXED) + [random_set(rng) for _ in range(6)]
    failures = 0
    runs = 0
    with tempfile.TemporaryDirectory() as d:
        for k, (chars, strings) in enumerate(cases):
            text = text_for(rng, chars, strings, 3000)
            text_path = os.path.join(d, 'text%d.txt' % k)
            with open(text_path, 'w', encoding='utf-8') as f:
                f.write(text)
            spec_path = os.path.join(d, 'set%d.txt' % k)
            with open(spec_path, 'w') as f:
                f.write(set_file(chars, strings))
            for backward in (False, True):
                expected = reference(text, chars, strings, backward)
                for mux in (False, True):
                    args = [binary, '-set', spec_path, text_path]
                    if backward:
                        args.append('-backward')
                    if mux:
                        args.append('-mux')
                    runs += 1
                    r = subprocess.run(args, capture_output=True)
                    if r.returncode != 0 or r.stdout != expected:
                        failures += 1
                        where = next((i for i in range(min(len(r.stdout), len(expected)))
                                      if r.stdout[i] != expected[i]), min(len(r.stdout), len(expected)))
                        print('FAIL set %d (%s %s) %s%s: at codepoint %d of %d, got %s, expected %s; %s' % (
                              k, chars, strings, 'backward' if backward else 'forward', ' mux' if mux else '',
                              where, len(expected), r.stdout[where:where + 8].hex(), expected[where:where + 8].hex(),
                              r.stderr.decode(errors='replace')[:200]))
    print('%d runs, %d failures' % (runs, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
