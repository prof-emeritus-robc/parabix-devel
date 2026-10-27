#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Summarize -EnableCycleCounter reports by pipeline stage.
#
# Kernels are grouped by the stage labels set with PipelineBuilder::SetStageLabel,
# which the cycle counter report prints as "[label] name".  Kernels without a label
# (e.g., PopCount kernels inserted by the pipeline) are assigned to the stage of the
# next labelled kernel in the report, normally their consumer.
#
# With --graph (the output of --PrintPipelineGraph for the same program), the report
# also gives a scaling ceiling: kernels that are not <StateFree> process one segment
# at a time, so no number of threads can make the program faster than its largest
# such kernel's single-threaded time.  The ceiling is total time divided by that.
#
# Usage: python3 stage_report.py --t1 cc_1thread.txt [--tN cc_Nthreads.txt] [--graph graph.dot]

import argparse
import math
import re
import sys
from collections import OrderedDict


class Row:
    def __init__(self, kid, label, name, items, ticks, sync, pipe, execute):
        self.kid, self.label, self.name = kid, label, name
        self.labelled = label is not None  # labelled in the report (not assigned to a stage)
        self.items, self.ticks = items, ticks
        self.sync, self.pipe, self.execute = sync, pipe, execute


def parse_cycle_counter(path):
    """Kernel rows and the total (pipeline) ticks of a cycle counter report."""
    rows, total = [], None
    for line in open(path, errors='replace'):
        p = line.split()
        if len(p) < 3 or not p[0].isdigit():
            continue
        if '+-' in p:
            # columns: id, [label], name, ITEMS, CYCLES, RATE, SYNC, PART, EXPD, COPY, PIPE, EXEC, %, +-, cov
            j = next(i for i in range(2, len(p) - 1) if p[i].isdigit() and p[i + 1].isdigit())
            name_tokens = p[1:j]
            label = None
            if name_tokens and name_tokens[0].startswith('[') and name_tokens[0].endswith(']'):
                label = name_tokens[0][1:-1]
                name_tokens = name_tokens[1:]
            pct = lambda v: 0.0 if math.isnan(float(v)) else float(v)  # kernels that did no work print nan
            rows.append(Row(int(p[0]), label, ' '.join(name_tokens), int(p[j]), int(p[j + 1]),
                            pct(p[j + 3]), pct(p[j + 7]), pct(p[j + 8])))
        elif len(p) >= 4 and p[2].isdigit() and p[3].isdigit():
            # the pipeline row: id, name, items, cycles, ...
            total = int(p[3])
    # assign unlabelled kernels to the next labelled one
    pending = []
    for r in rows:
        if r.label is None:
            pending.append(r)
        else:
            for q in pending:
                q.label = r.label
            pending = []
    for q in pending:
        q.label = '(unlabelled)'
    if total is None:
        total = sum(r.ticks for r in rows)
    return rows, total


def state_free_kernels(path):
    ids = set()
    for m in re.finditer(r'label="\[(\d+)\] [^"]*?<StateFree>', open(path, errors='replace').read()):
        ids.add(int(m.group(1)))
    return ids


def by_stage(rows):
    stages = OrderedDict()
    for r in rows:
        stages.setdefault(r.label, []).append(r)
    return stages


def weighted(rows, attr):
    ticks = sum(r.ticks for r in rows)
    return sum(r.ticks * getattr(r, attr) for r in rows) / ticks if ticks else 0.0


def summarize(t1, tN=None, graph=None):
    """Returns (per-stage dicts, overall dict)."""
    rows1, total1 = parse_cycle_counter(t1)
    stagesN, totalN = (None, None)
    if tN:
        rowsN, totalN = parse_cycle_counter(tN)
        stagesN = by_stage(rowsN)
    free = state_free_kernels(graph) if graph else None
    result = []
    for label, rs in by_stage(rows1).items():
        d = OrderedDict(stage=label, kernels=len(rs), t1_ticks=sum(r.ticks for r in rs))
        d['t1_pct'] = 100.0 * d['t1_ticks'] / total1
        if stagesN is not None:
            rn = stagesN.get(label, [])
            ticks = sum(r.ticks for r in rn)
            d['tN_pct'] = 100.0 * ticks / sum(sum(r.ticks for r in v) for v in stagesN.values())
            d['tN_sync'] = weighted(rn, 'sync')
            d['tN_pipe'] = weighted(rn, 'pipe')
        if free is not None:
            serial = [r for r in rs if r.kid not in free]
            worst = max(serial, key=lambda r: r.ticks, default=None)
            d['serial_pct'] = 100.0 * sum(r.ticks for r in serial) / total1
            d['worst_serial'] = (worst.name, 100.0 * worst.ticks / total1) if worst else None
        result.append(d)
    overall = OrderedDict(t1_total=total1, tN_total=totalN)
    if free is not None:
        serial = [r for r in rows1 if r.kid not in free]
        worst = max(serial, key=lambda r: r.ticks, default=None)
        overall['ceiling'] = total1 / worst.ticks if worst else float('inf')
        overall['worst_serial'] = (worst.label, worst.name, 100.0 * worst.ticks / total1) if worst else None
    return result, overall


def format_report(stages, overall):
    out = []
    has_n = 'tN_pct' in stages[0] if stages else False
    has_g = 'serial_pct' in stages[0] if stages else False
    head = '| stage | kernels | 1-thread % |'
    sep = '|---|---|---|'
    if has_n:
        head += ' N-thread % | sync % | pipe % |'
        sep += '---|---|---|'
    if has_g:
        head += ' serialized % | largest serialized kernel |'
        sep += '---|---|'
    out += [head, sep]
    for d in stages:
        line = '| %s | %d | %.1f |' % (d['stage'], d['kernels'], d['t1_pct'])
        if has_n:
            line += ' %.1f | %.1f | %.1f |' % (d['tN_pct'], d['tN_sync'], d['tN_pipe'])
        if has_g:
            w = d['worst_serial']
            line += ' %.1f | %s |' % (d['serial_pct'], '%s (%.1f%%)' % (w[0][:40], w[1]) if w else '-')
        out.append(line)
    if 'ceiling' in overall:
        w = overall['worst_serial']
        out.append('')
        out.append('Scaling ceiling: %.1fx (bounded by [%s] %s at %.1f%% of single-threaded time)'
                   % (overall['ceiling'], w[0], w[1][:50], w[2]))
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description='Summarize -EnableCycleCounter reports by pipeline stage.')
    ap.add_argument('--t1', required=True, help='cycle counter report of a single-threaded run')
    ap.add_argument('--tN', help='cycle counter report of a multithreaded run')
    ap.add_argument('--graph', help='--PrintPipelineGraph output, for the scaling ceiling')
    args = ap.parse_args()
    stages, overall = summarize(args.t1, args.tN, args.graph)
    print(format_report(stages, overall))


if __name__ == '__main__':
    main()
