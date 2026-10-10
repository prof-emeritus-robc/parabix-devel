#!/usr/bin/env python3
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

#  tconv_coverage.py: assess which LDML transforms tconv implements.
#
#  tconv_coverage.py --tconv <tconv binary> --transforms-dir <CLDR common/transforms>
#                    [--test-data <CLDR common/testData/transforms>] [--jobs N]
#                    [--out <directory>]
#
#  Runs tconv --plan on every transform tconv knows (built-in and defined by
#  the CLDR files), classifies the problems it reports by the kind of work
#  needed to implement them, and propagates the classes through the transform
#  rules (:: X) that invoke other transforms.  With --test-data, each CLDR test
#  file of an implementable transform is run through tconv and compared with
#  its expected results.
#
#  Writes to the output directory (default ./tconv-coverage):
#      plans/<name>.txt     the --plan output of each transform ('/' as '%')
#      coverage.json        per transform: status, own problem classes,
#                           transforms used, transitive work items
#      coverage.md          summary tables and the per-transform table
#
#  Transforms whose analysis exceeds the memory available (the Han transforms
#  need 8-14 GB) are reported as "analysis failed"; use a small --jobs.

import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess

#  Work items: the problem classes of tconv --plan, grouped by the kind of
#  work needed in tconv / the Parabix framework (see doc/ldml-tconv-coverage.md).
WORK_ITEMS = collections.OrderedDict([
    ('W1 filters', ['F-transform-in-filtered', 'F-filtered-transform-rule']),
    ('W2 parallel conflicts', ['S-match-within-string-rule', 'S-expanded-nonfinal']),
    ('W3 variable-length text', ['T-repetition', 'T-setstrings', 'T-variable', 'T-boundary']),
    ('W4 insertion', ['T-insertion']),
    ('W5 captures/functions', ['R-backref', 'R-funcall', 'R-other']),
    ('W6 revisit', ['C-revisit']),
    ('W7 parser semantics', ['P-parse/semantic']),
    ('W8 built-ins', ['B-builtin']),
    ('W9 analysis scale', ['X-analysis-failed', 'X-unknown-transform', 'X-recursive',
                           'O-order-dependent', 'Z-other']),
])
CLASS_TO_ITEM = {c: w for w, cs in WORK_ITEMS.items() for c in cs}


def file_name(name):
    return name.replace('/', '%') + '.txt'


def list_transforms(args):
    out = subprocess.run([args.tconv, '--transforms-dir=' + args.transforms_dir, '--list-transforms'],
                         capture_output=True, text=True, check=True).stdout
    entries = collections.OrderedDict()
    for line in out.splitlines():
        if not line.strip():
            continue
        fields = [f.strip() for f in line.split('  ')]
        name, rest = fields[0], fields[1:]
        info = [f for f in rest if f.startswith('[') and f not in ('[built-in]', '[internal]')]
        entries[name] = dict(
            name=name,
            builtin='[built-in]' in rest,
            internal='[internal]' in rest,
            file=os.path.basename(info[0][1:-1].split(',')[0]) if info else '',
            backward=bool(info) and 'backward' in info[0],
            aliases=[f for f in rest if not f.startswith('[')])
    return entries


def run_plans(args, entries):
    plans = os.path.join(args.out, 'plans')
    os.makedirs(plans, exist_ok=True)

    def run(name):
        try:
            r = subprocess.run([args.tconv, '--transforms-dir=' + args.transforms_dir, '--plan', name],
                               capture_output=True, timeout=args.timeout)
            text, rc = (r.stdout + r.stderr).decode('utf-8', 'replace'), r.returncode
        except subprocess.TimeoutExpired:
            text, rc = 'timeout\n', None
        with open(os.path.join(plans, file_name(name)), 'w', encoding='utf-8') as f:
            f.write(text)
        return name, rc

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for name, rc in ex.map(run, entries):
            entries[name]['rc'] = rc


#  The text to replace of a printed conversion rule (without contexts).
def rule_text(rule):
    lhs = rule.split('→')[0]
    parts = re.split(r' \{ ', lhs, maxsplit=1)
    text = parts[-1]
    if len(parts) > 1 or ' } ' in text:
        text = re.split(r'(?:^| )\} ', text)[0]
    return text.strip()


def classify_problem(p):
    if p.startswith('built-in transform'):
        return 'B-builtin'
    if p.startswith('filtered transform'):
        return 'F-filtered-transform-rule'
    if 'within a filtered transform' in p:
        return 'F-transform-in-filtered'
    if p.startswith('rules not loaded'):
        return 'P-parse/semantic'
    if p.startswith('recursive'):
        return 'X-recursive'
    if 'order dependent' in p:
        return 'O-order-dependent'
    if '(may match within the text of' in p:
        return 'S-match-within-string-rule'
    if 'is expanded for a longer replacement' in p:
        return 'S-expanded-nonfinal'
    if '(the result has text to revisit)' in p:
        return 'C-revisit'
    if '(the result is not a fixed string)' in p:
        rhs = p.split('→')[-1]
        if '&' in rhs:
            return 'R-funcall'
        if re.search(r'\$[0-9]', rhs):
            return 'R-backref'
        return 'R-other'
    if 'includes the text boundary' in p:
        return 'T-boundary'
    if '(the text to replace is not a fixed-length' in p:
        text = rule_text(p)
        if text == '':
            return 'T-insertion'
        if re.search(r'\[[^\]]*\{', text):
            return 'T-setstrings'
        if re.search(r'(?<!\\)[+*?]', text):
            return 'T-repetition'
        return 'T-variable'
    return 'Z-other'


def classify(args, entries):
    index = {}
    for name, e in entries.items():
        for n in [name] + e['aliases']:
            index.setdefault(n.lower(), name)

    def resolve(tid):
        k = re.sub(r'\(\s*\)$', '', tid.strip().lower())
        if k in index:
            return index[k]
        if '-' not in k.split('/')[0]:
            return index.get('any-' + k)
        return None

    for name, e in entries.items():
        with open(os.path.join(args.out, 'plans', file_name(name)), encoding='utf-8') as f:
            lines = f.read().splitlines()
        own, uses = collections.Counter(), set()
        if e['rc'] not in (0, 2):
            own['X-analysis-failed'] += 1
        for line in lines:
            if line.startswith('  transform '):
                used = resolve(line.split()[-1])
                if used:
                    uses.add(used)
                else:
                    own['X-unknown-transform'] += 1
            elif line.startswith('  problem: '):
                p = line[len('  problem: '):]
                if not p.startswith(('unimplemented transform', 'unknown transform')):
                    own[classify_problem(p)] += 1
        e['own'] = dict(own)
        e['uses'] = sorted(uses)
        e['warnings'] = sum(1 for line in lines if line.startswith('    warning:'))

    memo = {}

    def closure(name, stack=()):
        if name in memo:
            return memo[name]
        if name in stack:
            return set()
        classes = set(entries[name]['own'])
        for u in entries[name]['uses']:
            classes |= closure(u, stack + (name,))
        memo[name] = classes
        return classes

    for name, e in entries.items():
        e['classes'] = sorted(closure(name))
        e['work'] = sorted({CLASS_TO_ITEM.get(c, 'W9 analysis scale') for c in e['classes']})
        e['implementable'] = e['rc'] == 0


def run_tests(args, entries):
    index = {}
    for name, e in entries.items():
        for n in [name] + e['aliases']:
            index.setdefault(n.lower(), name)
    tests = os.path.join(args.out, 'tests')
    os.makedirs(tests, exist_ok=True)
    for f in sorted(os.listdir(args.test_data)):
        if not f.endswith('.txt'):
            continue
        name = index.get(f[:-4].lower())
        if name is None:
            continue
        e = entries[name]
        e.setdefault('tests', []).append(f)
        if not e['implementable']:
            continue
        with open(os.path.join(args.test_data, f), encoding='utf-8') as t:
            pairs = [l.rstrip('\n').split('\t') for l in t if '\t' in l and not l.startswith('#')]
        src = os.path.join(tests, f)
        with open(src, 'w', encoding='utf-8') as t:
            t.write(''.join(p[0] + '\n' for p in pairs))
        r = subprocess.run([args.tconv, '--transforms-dir=' + args.transforms_dir, name, src],
                           capture_output=True, timeout=args.timeout)
        out = r.stdout.decode('utf-8', 'replace').split('\n')
        failed = sum(1 for i, p in enumerate(pairs) if i >= len(out) or out[i] != p[1])
        e.setdefault('test_results', {})[f] = dict(total=len(pairs), failed=failed)


def summary(entries):
    md = []
    pops = [('all CLDR-defined', [e for e in entries.values() if not e['builtin']]),
            ('public CLDR-defined (not internal)', [e for e in entries.values() if not e['builtin'] and not e['internal']])]
    builtins = [e for e in entries.values() if e['builtin']]
    md.append('## Built-in transforms\n')
    md.append('%d built-in names, %d implemented: %s\n' % (
        len(builtins), sum(e['implementable'] for e in builtins),
        ', '.join(e['name'] for e in builtins if e['implementable'])))
    md.append('Not implemented: %s\n' % ', '.join(e['name'] for e in builtins if not e['implementable']))
    for label, pop in pops:
        md.append('## %s transforms: %d\n' % (label.capitalize(), len(pop)))
        md.append('Implementable now: %d\n' % sum(e['implementable'] for e in pop))
        md.append('| Work item | needed by (transitively) | the only blocker of | cumulative implementable (greedy order) |')
        md.append('|---|---:|---:|---:|')
        solved, remaining, order = set(), set(WORK_ITEMS), []
        while remaining:
            best = max(sorted(remaining), key=lambda w: sum(1 for e in pop if set(e['work']) <= solved | {w}))
            solved.add(best)
            remaining.remove(best)
            order.append((best, sum(1 for e in pop if set(e['work']) <= solved)))
        for w, cum in order:
            md.append('| %s | %d | %d | %d |' % (w, sum(1 for e in pop if w in e['work']),
                                                sum(1 for e in pop if e['work'] == [w]), cum))
        md.append('')
    md.append('## Problem classes (all CLDR-defined transforms)\n')
    md.append('| Class | work item | transforms with own problems | transforms needing (transitively) | problem rules |')
    md.append('|---|---|---:|---:|---:|')
    pop = pops[0][1]
    classes = sorted({c for e in pop for c in e['classes']}, key=lambda c: CLASS_TO_ITEM.get(c, 'W9'))
    for c in classes:
        md.append('| %s | %s | %d | %d | %d |' % (c, CLASS_TO_ITEM.get(c, 'W9 analysis scale').split()[0],
                                                sum(1 for e in pop if c in e['own']),
                                                sum(1 for e in pop if c in e['classes']),
                                                sum(e['own'].get(c, 0) for e in pop)))
    md.append('')
    md.append('## Transforms\n')
    md.append('Status: **ok** implementable; otherwise the work items needed (own problems and those of the '
              'transforms used).  Tests: CLDR test files (failed/total cases for implementable transforms).\n')
    md.append('| Transform | file | status | own problem classes (rules) | uses | tests |')
    md.append('|---|---|---|---|---|---|')
    for e in sorted(entries.values(), key=lambda e: (e['builtin'], e['name'].lower())):
        status = '**ok**' if e['implementable'] else ', '.join(w.split()[0] for w in e['work']) or '?'
        own = ', '.join('%s (%d)' % (c, n) for c, n in sorted(e['own'].items()))
        tests = ', '.join('%s %d/%d' % (f, r['failed'], r['total']) for f, r in e.get('test_results', {}).items()) \
            or ', '.join(e.get('tests', []))
        md.append('| %s%s | %s | %s | %s | %s | %s |' % (
            e['name'], ' (internal)' if e['internal'] else '', e['file'] or 'built-in', status, own,
            ', '.join(e['uses']), tests))
    return '\n'.join(md) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tconv', required=True)
    ap.add_argument('--transforms-dir', required=True)
    ap.add_argument('--test-data')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=1200)
    ap.add_argument('--out', default='tconv-coverage')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    entries = list_transforms(args)
    run_plans(args, entries)
    classify(args, entries)
    if args.test_data:
        run_tests(args, entries)
    with open(os.path.join(args.out, 'coverage.json'), 'w', encoding='utf-8') as f:
        json.dump(entries, f, indent=1, ensure_ascii=False)
    with open(os.path.join(args.out, 'coverage.md'), 'w', encoding='utf-8') as f:
        f.write(summary(entries))


if __name__ == '__main__':
    main()
