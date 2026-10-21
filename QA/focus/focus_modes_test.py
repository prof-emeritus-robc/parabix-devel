#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Runs a normalization tool (nfd, nfc or nfkd) in the main modes of its focused
# transform on a small generated text, and checks each output against Python's
# unicodedata.normalize.
#
# The text mixes sparse work (isolated precomposed, decomposed, compatibility and
# reorderable characters in ASCII lines) with a dense part (Hangul syllables and
# conjoining jamo, Latin with stacked combining marks).  The tool runs with a small
# --segment-size, so that segment boundaries fall inside work regions.  The text
# uses only characters whose normalization is stable across Unicode versions.
#
# Usage: python3 focus_modes_test.py <tool executable> [NFD|NFC|NFKD]

import os
import subprocess
import sys
import tempfile
import unicodedata

MODES = [
    [],
    ['-focus-filter=bit', '-reintegrate=bit'],
    ['-focus-filter=byte', '-reintegrate=byte-replace', '-unmodified-filter=byte'],
    ['-reintegrate=byte-spread', '-fill-run-gaps=3'],
    ['-focus-layers=filter,transform'],
    ['-NoFocus'],
]

COMMON = ['--thread-num=3', '--segment-size=1024']


def sparse_line(i):
    words = ['café', 'naïve', 'Ångström', 'ệ', 'ậ',
             'ﬁne', '①', 'ＡＢ', '½', '가', '가', 'Å',
             'Å', 'ẛ̣', '㎒', 'x̧́', ' ', 'Ω']
    return 'line %d: plain ascii text %s and more %s text\n' % (i, words[i % len(words)], words[(i * 7) % len(words)])


def dense_block():
    hangul = ''.join(chr(0xAC00 + (k * 37) % 11172) for k in range(400))
    jamo = ''.join('ᄀ' + chr(0x1161 + k % 21) + chr(0x11A8 + k % 27) for k in range(200))
    marks = ''.join(c + '̣́̈' for c in 'aeiouAEIOU' * 20)
    return hangul + '\n' + jamo + '\n' + marks + '\n'


def text():
    lines = [sparse_line(i) for i in range(80)]
    return ''.join(lines[:40]) + dense_block() + ''.join(lines[40:])


def main():
    program, form = sys.argv[1], sys.argv[2]
    source = text()
    expected = unicodedata.normalize(form, source).encode('utf-8')
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'input.txt')
        with open(path, 'wb') as f:
            f.write(source.encode('utf-8'))
        for mode in MODES:
            result = subprocess.run([program] + COMMON + mode + [path], stdout=subprocess.PIPE)
            if result.returncode != 0 or result.stdout != expected:
                failures.append(' '.join(mode) or 'default')
    name = os.path.basename(program)
    if failures:
        print('%s focus modes failed: %s' % (name, '; '.join(failures)))
        return 1
    print('%s focus modes passed (%d modes)' % (name, len(MODES)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
