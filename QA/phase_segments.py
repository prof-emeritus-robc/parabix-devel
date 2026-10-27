#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Summarize a -TraceStridesPerSegment report of a pipeline with phase boundaries.
#
# Each phase numbers its segments separately, and the report lists all phases'
# kernels side by side by segment number.  Given the id of the first kernel of each
# later phase (as in the report's header, e.g., "84" for "84.Invert"), this prints,
# for each phase, its number of segments and how many of them processed nothing in
# any of the phase's traced kernels.
#
# A kernel with zero strides in a segment does not by itself mean an empty segment:
# within a phase, a partition may process nothing in one segment and twice as much
# in the next.
#
# Usage: python3 phase_segments.py trace.txt FIRST_KERNEL_OF_PHASE_2 [FIRST_KERNEL_OF_PHASE_3 ...]

import csv
import sys


def summarize(path, boundaries):
    lines = [l for l in open(path, errors='replace') if l.strip()]
    start = next(i for i, l in enumerate(lines) if l.startswith('SEG #'))
    rows = list(csv.reader(lines[start:]))
    header, data = rows[0], [r for r in rows[1:] if r and r[0].strip().isdigit()]
    starts = [0] + sorted(boundaries)

    def phase_of(column):
        kid = int(header[column].split('.')[0])
        return max(i for i, s in enumerate(starts) if kid >= s)

    def value(row, column):
        return int(row[column]) if column < len(row) and row[column].strip() else 0

    result = []
    for phase in range(len(starts)):
        columns = [k for k in range(1, len(header)) if phase_of(k) == phase]
        busy = [any(value(r, k) for k in columns) for r in data]
        segments = max((j + 1 for j, b in enumerate(busy) if b), default=0)
        result.append((phase + 1, len(columns), segments, segments - sum(busy[:segments])))
    return result


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: phase_segments.py trace.txt [first kernel id of each later phase ...]')
    print('| phase | traced kernels | segments | empty segments |')
    print('|---|---|---|---|')
    for phase, kernels, segments, empty in summarize(sys.argv[1], [int(a) for a in sys.argv[2:]]):
        print('| %d | %d | %d | %d |' % (phase, kernels, segments, empty))


if __name__ == '__main__':
    main()
