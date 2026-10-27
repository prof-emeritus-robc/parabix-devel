#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Baseline measurements of the focus/filter/transform/reintegrate structure of the
# normalization tools (nfd, nfc, nfkd) over a set of input files.
#
# For each input file and tool configuration, it
#   - checks that the output with N threads matches the tool's default
#     configuration with one thread;
#   - records the best of R wall times with 1 and N threads, with the system time
#     of the best run and the peak resident set size;
#   - records -EnableCycleCounter reports with 1 and N threads and the pipeline
#     graph, summarized by stage with stage_report.py (including the scaling ceiling);
#   - estimates the work density: the number of bytes the transform stage
#     receives, as a fraction of the input.
#
# With --layers, each configuration also runs with each given -focus-layers setting.
#
# Runs are sequential; N defaults to 3.  The JIT object cache is used as is, and each
# configuration is run once before measurement, so compilation is not measured.
#
# Usage: python3 focus_baseline.py --bin <bin dir> --inputs <dir> [--out <dir>]

import argparse
import hashlib
import os
import platform
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stage_report  # noqa: E402

CONFIGS = {
    'nfd': [[], ['-focus-filter=bit', '-reintegrate=bit'], ['-reintegrate=byte-replace'], ['-NoFocus']],
    'nfc': [[], ['-focus-filter=byte', '-reintegrate=bit'], ['-NoFocus']],
    'nfkd': [[], ['-focus-filter=bit', '-reintegrate=bit'], ['-NoFocus']],
}


def run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL):
    return subprocess.run(cmd, stdout=stdout, stderr=stderr, timeout=1800)


def output_digest(cmd):
    """SHA-256 of the program's output, read from a pipe so that no file is written
    (writing large files would disturb the timing of the following runs)."""
    h = hashlib.sha256()
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as p:
        for chunk in iter(lambda: p.stdout.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


class Timing:
    def __init__(self):
        self.wall = None    # best wall time, s
        self.sys = None     # system time of that run, s
        self.rss = 0        # peak resident set size over all runs, MB


def best_time(cmd, repeats):
    t = Timing()
    rss_unit = 1 if platform.system() == 'Darwin' else 1024  # ru_maxrss: bytes or KB
    for _ in range(repeats):
        start = time.perf_counter()
        # Wait with wait4, not with a timeout: with one, Python polls for the child's
        # exit with sleeps of up to 50 ms, which quantizes the measured times.
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        _, _, usage = os.wait4(p.pid, 0)
        elapsed = time.perf_counter() - start
        p.returncode = 0
        if t.wall is None or elapsed < t.wall:
            t.wall, t.sys = elapsed, usage.ru_stime
        t.rss = max(t.rss, usage.ru_maxrss * rss_unit / 1e6)
    return t


def density(cc_path):
    rows, _ = stage_report.parse_cycle_counter(cc_path)
    # the first kernels labelled as such (unlabelled kernels, such as the PopCount
    # kernels of a filter, are assigned to the following stage)
    source = next((r for r in rows if r.labelled and r.label == 'source'), None)
    transform = next((r for r in rows if r.labelled and r.label == 'transform'), None)
    if not source or not transform or not source.items:
        return None
    return 100.0 * transform.items / source.items


def main():
    ap = argparse.ArgumentParser(description='Focus/transform/reintegrate baseline for nfd and nfc.')
    ap.add_argument('--bin', required=True, help='directory holding the nfd and nfc executables')
    ap.add_argument('--inputs', required=True, help='directory of input files (*.txt)')
    ap.add_argument('--out', default='focus_baseline_out', help='directory for reports')
    ap.add_argument('--threads', type=int, default=3)
    ap.add_argument('--repeats', type=int, default=2)
    ap.add_argument('--tools', default='nfd,nfc,nfkd')
    ap.add_argument('--config', action='append', default=None,
                    help='a configuration (options separated by spaces, "" for the default) to run '
                         'instead of the built-in list; may be repeated')
    ap.add_argument('--layers', action='append', default=None,
                    help='a -focus-layers setting (e.g., "transform" or "placement,transform"; "none" '
                         'for no boundary) to run with each configuration; may be repeated')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    N = args.threads
    inputs = sorted(f for f in os.listdir(args.inputs) if f.endswith('.txt'))
    summary = ['| tool | config | input | MB | work density %% | 1-thread s | %d-thread s | speedup | ceiling '
               '| %d-thread sys s | peak RSS MB | output |' % (N, N),
               '|---|---|---|---|---|---|---|---|---|---|---|---|']
    details = []
    for tool in args.tools.split(','):
        exe = os.path.join(args.bin, tool)
        for name in inputs:
            path = os.path.join(args.inputs, name)
            mb = os.path.getsize(path) / 1e6
            reference = output_digest([exe, '--thread-num=1', path])
            configs = [c.split() for c in args.config] if args.config is not None else CONFIGS[tool]
            if args.layers is not None:
                configs = [c + ([] if l == 'none' else ['-focus-layers=' + l]) for c in configs for l in args.layers]
            for config in configs:
                tag = '%s%s.%s' % (tool, ''.join(config), name)
                base = [exe] + config
                run(base + ['--thread-num=1', path])  # ensure the configuration is compiled
                t1 = best_time(base + ['--thread-num=1', path], args.repeats)
                tN = best_time(base + ['--thread-num=%d' % N, path], args.repeats)
                rss = max(t1.rss, tN.rss)
                same = output_digest(base + ['--thread-num=%d' % N, path]) == reference
                cc1 = os.path.join(args.out, tag + '.cc1')
                ccN = os.path.join(args.out, tag + '.cc%d' % N)
                graph = os.path.join(args.out, tag + '.dot')
                with open(cc1, 'wb') as f:
                    run(base + ['-EnableCycleCounter', '--thread-num=1', path], stderr=f)
                with open(ccN, 'wb') as f:
                    run(base + ['-EnableCycleCounter', '--thread-num=%d' % N, path], stderr=f)
                with open(graph, 'wb') as f:
                    run(base + ['--PrintPipelineGraph', path], stderr=f)
                stages, overall = stage_report.summarize(cc1, ccN, graph)
                d = density(cc1) if '-NoFocus' not in config else None
                cfg = ' '.join(config) or 'default'
                summary.append('| %s | %s | %s | %.0f | %s | %.3f | %.3f | %.2f | %.1f | %.3f | %.0f | %s |'
                               % (tool, cfg, name, mb, '%.1f' % d if d is not None else '-', t1.wall, tN.wall,
                                  t1.wall / tN.wall, overall.get('ceiling', 0.0), tN.sys, rss,
                                  'ok' if same else 'DIFF'))
                details.append('### %s %s on %s\n\n%s\n' % (tool, cfg, name, stage_report.format_report(stages, overall)))
                print(summary[-1], flush=True)
    report = os.path.join(args.out, 'report.md')
    with open(report, 'w') as f:
        f.write('# Focus/transform/reintegrate baseline\n\n')
        f.write('\n'.join(summary) + '\n\n## Stage breakdowns\n\n' + '\n'.join(details))
    print('Report written to ' + report)


if __name__ == '__main__':
    main()
