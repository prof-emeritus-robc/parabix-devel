#!/usr/bin/env python3
"""
selfmerge_test.py - regression test for BPE self-merges (X+X -> XX).

A self-merge must pair a run of equal tokens left to right, using each token once:
XXXXX -> XX XX X. The Parabix merge kernels resolve this with run-parity logic
(selfMergeFireStarts in bpe.cpp), so this test feeds the tokenizer runs of repeated
tokens and compares its token ids against a reference BPE.

The input is generated: runs of length 1..12 of single bytes, multi-byte characters
and multi-character units, bare, inside other text and next to runs of other tokens,
plus mixed alternations. The reference is a self-contained byte-level BPE built from
merges.txt (no HuggingFace needed): a priority queue over adjacent pairs, lowest rank
first and leftmost first among equal ranks, which is how HuggingFace tokenizers apply
merges. Every position of the input is one sequence, as in the tokenizer's default
mode (no --pretokenizer: '\\n' is content).

Each --config NAME=FLAGS runs the tokenizer with those flags (after --merges); the
default set covers the main merge-kernel strategies. Exit status 1 on any mismatch.

    python selfmerge_test.py --tokenizer ../../../build22/bin/tokenizer
    python selfmerge_test.py --config 'mine=--level-partition --some-new-flag'
"""

import argparse
import heapq
import os
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../.."))
TOKENIZER = os.path.join(REPO_ROOT, "build19/bin/tokenizer")
MERGES    = os.path.join(REPO_ROOT, "tools/lex/tokenizer_files/merges.txt")

DEFAULT_CONFIGS = [
    ("plain", ""),
    ("level", "--level-partition"),
    ("level+geometric", "--level-partition --compact-base=2 --geometric-compaction --batch-writeback"),
    ("bits+xfrm10", "--level-partition --compact-base=2 --compaction=by-output-bits --batch-writeback "
                    "--partition-by-merge-id-bits --max-bit-xfrm-limit=10"),
]

UNITS = ['-', '=', '.', ' ', '*', '#', '/', '_', '0', '1', '9', 'a', 'l', 'k', 't', 's',
         'ä', 'ö', '€', '—', 'é', '\t', '!', '?', ',', ':',
         'ab', 'la', '--', '==', '..', '  ', '0x', 'aa', 'ää', 'kk']
MIXED = ['--==--', '...---...', '  \t  \t', 'aaabbbaaa', 'ääää kkkk', '0000.0000',
         '====----====', '-=-=-=-=', '..  ..  ..', '****  ****']


def make_input():
    lines = []
    for u in UNITS:
        for n in range(1, 13):
            lines.append(f'<{u * n}>')                    # run between delimiters
            lines.append(f'x{u * n}y')                    # run inside a word
            lines.append(u * n + ' ' + u * (n + 1))       # two runs, odd/even lengths
    for m in MIXED:
        for k in range(1, 5):
            lines.append(m * k)
    return ('\n'.join(lines) + '\n').encode('utf-8')


def bytes_to_unicode():
    bs = (list(range(ord('!'), ord('~') + 1)) + list(range(ord('¡'), ord('¬') + 1))
          + list(range(ord('®'), ord('ÿ') + 1)))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return dict(zip(bs, (chr(c) for c in cs)))


def load_merges(path):
    """(pair id tuple -> rank) and (byte -> base id). Base ids are positions in the
    bytes_to_unicode order; merged ids are 256 + rank, as in GPT-2's vocab.json."""
    b2u = bytes_to_unicode()
    vocab = {ch: i for i, ch in enumerate(b2u.values())}
    byte_id = {b: vocab[ch] for b, ch in b2u.items()}
    rank = {}
    with open(path, encoding='utf-8') as f:
        lines = [l.rstrip('\n') for l in f if l.strip() and not l.startswith('#version')]
    for r, line in enumerate(lines):
        a, b = line.split(' ')
        if a in vocab and b in vocab:
            rank[(vocab[a], vocab[b])] = r
            vocab[a + b] = 256 + r
    return rank, byte_id


def reference_bpe(data, rank, byte_id):
    ids = [byte_id[b] for b in data]
    n = len(ids)
    nxt = list(range(1, n + 1))
    prv = list(range(-1, n - 1))
    alive = [True] * n
    heap = [(rank[(ids[i], ids[i + 1])], i) for i in range(n - 1) if (ids[i], ids[i + 1]) in rank]
    heapq.heapify(heap)
    while heap:
        r, i = heapq.heappop(heap)
        j = nxt[i] if alive[i] else n
        # stale entry: i or its right neighbour was merged away or changed since the push
        if j >= n or rank.get((ids[i], ids[j])) != r:
            continue
        ids[i] = 256 + r
        alive[j] = False
        nxt[i] = nxt[j]
        if nxt[i] < n:
            prv[nxt[i]] = i
        if prv[i] >= 0 and (ids[prv[i]], ids[i]) in rank:
            heapq.heappush(heap, (rank[(ids[prv[i]], ids[i])], prv[i]))
        if nxt[i] < n and (ids[i], ids[nxt[i]]) in rank:
            heapq.heappush(heap, (rank[(ids[i], ids[nxt[i]])], i))
    return [ids[i] for i in range(n) if alive[i]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tokenizer', default=TOKENIZER)
    ap.add_argument('--merges', default=MERGES)
    ap.add_argument('--config', action='append', default=[], metavar='NAME=FLAGS',
                    help='tokenizer flags to test (repeatable); default: a built-in set')
    ap.add_argument('--keep', metavar='DIR', help='write the input, reference and outputs here')
    args = ap.parse_args()
    if not os.path.exists(args.tokenizer):
        sys.exit(f'tokenizer not found: {args.tokenizer}')
    configs = [tuple(c.split('=', 1)) for c in args.config] or DEFAULT_CONFIGS

    data = make_input()
    rank, byte_id = load_merges(args.merges)
    expected = reference_bpe(data, rank, byte_id)
    expected_text = ''.join(f'{x}\n' for x in expected)

    outdir = args.keep or tempfile.mkdtemp(prefix='selfmerge_')
    os.makedirs(outdir, exist_ok=True)
    input_path = os.path.join(outdir, 'selfmerge_input.txt')
    with open(input_path, 'wb') as f:
        f.write(data)
    with open(os.path.join(outdir, 'selfmerge_expected.txt'), 'w') as f:
        f.write(expected_text)
    print(f'input: {len(data)} bytes -> {len(expected)} reference tokens ({input_path})')

    failures = 0
    for name, flags in configs:
        cmd = [args.tokenizer, f'--merges={args.merges}'] + flags.split() + [input_path]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        got = proc.stdout
        with open(os.path.join(outdir, f'selfmerge_{name}.txt'), 'w') as f:
            f.write(got)
        if proc.returncode != 0:
            print(f'ERROR     {name}: exit {proc.returncode}\n{proc.stderr[-2000:]}')
            failures += 1
        elif got == expected_text:
            print(f'MATCH     {name}')
        else:
            got_ids = got.split()
            k = next((i for i, (a, b) in enumerate(zip(got_ids, map(str, expected))) if a != b),
                     min(len(got_ids), len(expected)))
            print(f'MISMATCH  {name}: {len(got_ids)} tokens vs {len(expected)} expected, '
                  f'first difference at token {k}')
            failures += 1
    print(f'{len(configs) - failures}/{len(configs)} configurations match')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
