#!/usr/bin/env python3
#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  utf8_classes_test.py <utf8_classes binary>: character classes computed on the
#  UTF-8 bytes (kernel::CharClassesKernel, both bit movement modes), compared
#  with a reference, for large classes of CJK characters (whose compilation
#  nests If blocks) and text placing the characters at every offset from the
#  block boundaries.

import os, random, subprocess, sys, tempfile

def ranges(cps):
    cps = sorted(cps)
    out, i = [], 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1: j += 1
        out.append('%X' % cps[i] if i == j else '%X-%X' % (cps[i], cps[j]))
        i = j + 1
    return ' '.join(out)

def expected(text, classes, mode):
    out = bytearray()
    for ch in text:
        k = len(ch.encode('utf-8'))
        bits = sum(1 << i for i, c in enumerate(classes) if ord(ch) in c)
        cell = [0] * k
        cell[k - 1 if mode == 'advance' else 0] = bits
        out.extend(cell)
    return bytes(out)

def main():
    binary = sys.argv[1]
    rng = random.Random(12345)
    #  A sample of CJK characters (3 bytes) and some others (1, 2 and 4 bytes).
    pool = rng.sample(range(0x4E00, 0x9FB0), 3000) + list(range(0x3000, 0x3040)) + list(range(0xFF00, 0xFF20))
    others = [0x61, 0x62, 0x20, 0xE9, 0x3B1, 0x1F600, 0x20000]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for trial in range(3):
            if trial == 0:
                #  Two classes with 给 (U+7ED9, E7 BB 99): a range covering
                #  the whole block of second byte BB in one, as a nested If of the
                #  other.  At byte 126 of the text, its last byte begins a block
                #  (xlit Hans-Hant gave 獷 for it).
                classes = [set(range(0x7ECB, 0x7F36)), set()]
                for r in ('7605-7606 766B 76B1-76B2 76EE 770D 777E 7792 77FE-7801 7877 78B1 7934 7977-7978 '
                          '7984-7985 79FD-79FE 7A33 7A77 7A8D-7A8E 7ADE 7B0B 7B7E 7B80 7BF1 7C16 7C74 7CBE '
                          '7CFB 7D27 7D77 7E9F-7F35 7FD8-7FDA').split():
                    lo, _, hi = r.partition('-')
                    classes[1].update(range(int(lo, 16), int(hi or lo, 16) + 1))
            elif trial % 2:
                #  Classes of several hundred characters, as the coded classes
                #  of a large character mapping (each bit of a code, a class).
                classes = [set(rng.sample(pool, rng.randint(200, 1500))) for _ in range(8)]
            else:
                #  Classes of runs of characters (whole blocks of code units).
                classes = []
                for _ in range(8):
                    c = set()
                    for _ in range(rng.randint(5, 40)):
                        lo = rng.randrange(0x4E00, 0x9F00)
                        c.update(range(lo, lo + rng.choice((1, 2, 30, 64, 100, 200))))
                    classes.append(c)
            cfile = os.path.join(tmp, 'classes.txt')
            with open(cfile, 'w') as f:
                for c in classes: f.write(ranges(c) + '\n')
            #  Text: the characters at every offset (padding of 0..255 ASCII
            #  bytes before a line of CJK text), and random mixtures.
            parts = []
            line = ''.join(chr(c) for c in rng.sample(pool, 60)) if trial else '\u7ed9' * 60
            for pad in range(256):
                parts.append('a' * pad + '\n' + line + '\n')
            for _ in range(200):
                parts.append(''.join(chr(rng.choice(pool + others)) for _ in range(rng.randint(1, 80))) + '\n')
            text = ''.join(parts)
            tfile = os.path.join(tmp, 'text.txt')
            with open(tfile, 'w', encoding='utf-8') as f: f.write(text)
            for mode in ('advance', 'lookahead'):
                run = subprocess.run([binary, '-classes=' + cfile, '-mode=' + mode, tfile], capture_output=True)
                got = run.stdout
                if run.returncode != 0:
                    print('FAIL trial %d mode %s: exit code %d %s' % (trial, mode, run.returncode, run.stderr.decode(errors='replace')[:300]))
                    failures += 1
                    continue
                want = expected(text, classes, mode)
                if got != want:
                    failures += 1
                    bad = [i for i in range(min(len(got), len(want))) if got[i] != want[i]]
                    print('FAIL trial %d mode %s: %d of %d bytes differ (lengths %d, %d); first at %s'
                          % (trial, mode, len(bad), len(want), len(got), len(want), bad[:8]))
    print('utf8_classes_test: %s' % ('%d failures' % failures if failures else 'all passed'))
    return 1 if failures else 0

if __name__ == '__main__':
    sys.exit(main())
