#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Summarize a -TraceStridesPerSegment report: for each traced kernel, the number of
# segments, how many of them processed no strides, and the mean number of strides of
# the others.
#
# In a pipeline with phase boundaries, each phase numbers its segments separately,
# and the report lists all phases' kernels side by side by segment number.  A
# kernel's segments are the rows up to its last nonzero entry; empty segments of a
# later phase are the ones its threads ran while the earlier phase had produced
# nothing new for it.
#
# Usage: python3 phase_segments.py trace.txt

import csv
import sys


def summarize(path):
    lines = [l for l in open(path, errors='replace') if l.strip()]
    start = next(i for i, l in enumerate(lines) if l.startswith('SEG #'))
    rows = list(csv.reader(lines[start:]))
    header, data = rows[0][1:], [r[1:] for r in rows[1:] if r and r[0].strip().isdigit()]
    result = []
    for k, name in enumerate(header):
        counts = [int(r[k]) for r in data if k < len(r) and r[k].strip()]
        while counts and counts[-1] == 0:
            counts.pop()
        nonzero = [c for c in counts if c]
        result.append((name, len(counts), len(counts) - len(nonzero),
                       sum(nonzero) / len(nonzero) if nonzero else 0.0))
    return result


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: phase_segments.py trace.txt')
    print('| kernel | segments | empty | mean strides of the others |')
    print('|---|---|---|---|')
    for name, segments, empty, mean in summarize(sys.argv[1]):
        print('| %s | %d | %d | %.1f |' % (name[:60], segments, empty, mean))


if __name__ == '__main__':
    main()
