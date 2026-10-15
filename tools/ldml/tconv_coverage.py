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
#      coverage-meta.json   where the results come from: the tconv commit, the
#                           transforms and test data directories, and the runs
#      changes.md           (with --update) what changed from the previous results
#
#  Incremental runs: with --update, the previous results in the output directory
#  are kept, and only the selected transforms (--only, --builtins, --blocked,
#  and transforms tconv now knows that the results do not have) are planned and
#  tested again, with the transforms that use them (directly or through others),
#  whose status depends on theirs.  The classes, totals and tables are then
#  computed again for all.  --update refuses directories other than those of the
#  previous results (unless --force).
#
#  --table-doc PATH writes the per-transform table document (coverage.md with a
#  header); --report-doc PATH rewrites the generated blocks of the status report,
#  between <!-- coverage:NAME --> and <!-- /coverage:NAME --> (see REPORT_BLOCKS),
#  leaving the rest of it as it is.
#
#  Transforms whose analysis exceeds the memory available (the Han transforms
#  need 8-14 GB) are reported as "analysis failed"; use a small --jobs.

import argparse
import collections
import concurrent.futures
import datetime
import json
import os
import re
import subprocess
import sys

#  Work items: the problem classes of tconv --plan, grouped by the kind of
#  work needed in tconv / the Parabix framework (see doc/ldml-tconv-coverage.md).
#  A string rule that may match within the text of another (W2b) is classed by
#  the overlaps of the string rules of its conversion group (see
#  ldml::StringRuleOverlaps): all one-way, with an acyclic precedence graph
#  (applicable sequentially in a single pass), or not (self-overlaps, longer
#  cycles, two-way overlaps).
WORK_ITEMS = collections.OrderedDict([
    ('W1 filters', ['F-transform-in-filtered', 'F-filtered-transform-rule']),
    ('W2b1 one-way string overlaps', ['S-W2b-single-pass']),
    ('W2b2 cyclic/two-way string overlaps', ['S-W2b-self', 'S-W2b-cycle', 'S-W2b-two-way', 'S-match-within-string-rule']),
    ('W2c expanded non-final', ['S-expanded-nonfinal']),
    ('W3 variable-length text', ['T-repetition', 'T-setstrings', 'T-variable', 'T-boundary', 'T-capture-overlap',
                                 'T-capture-pattern']),
    ('W4 insertion', ['T-insertion']),
    ('W5 captures/functions', ['R-backref', 'R-funcall', 'R-other']),
    ('W6 revisit', ['C-revisit']),
    ('W7 parser semantics', ['P-parse/semantic']),
    ('W8 built-ins', ['B-builtin']),
    ('W10 engine lookaheads', ['E-engine-lookahead']),
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


#  The transforms of entries to plan again (all, without previous results): the
#  selected ones, the ones the previous results do not have (or whose plan is
#  missing), and the transforms that use any of these, directly or through
#  others (whose exit status of tconv --plan depends on theirs).
def select(args, entries, previous):
    if previous is None:
        return list(entries)
    index = {}
    for name, e in entries.items():
        for n in [name] + e['aliases']:
            index.setdefault(n.lower(), name)
    chosen = set()
    for n in args.only or []:
        if n.lower() not in index:
            sys.exit('tconv_coverage.py: unknown transform ' + n)
        chosen.add(index[n.lower()])
    if args.builtins:
        chosen |= {name for name, e in entries.items() if e['builtin']}
    if args.blocked:
        chosen |= {name for name, e in previous.items() if name in entries and not e['implementable']}
    plans = os.path.join(args.out, 'plans')
    chosen |= {name for name in entries
               if name not in previous or not os.path.exists(os.path.join(plans, file_name(name)))}
    users = collections.defaultdict(set)
    for name, e in previous.items():
        for u in e.get('uses', []):
            users[u].add(name)
    work = list(chosen)
    while work:
        for u in users[work.pop()]:
            if u in entries and u not in chosen:
                chosen.add(u)
                work.append(u)
    return [name for name in entries if name in chosen]


def run_plans(args, entries, names):
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
        for name, rc in ex.map(run, names):
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
    if 'ends with a repetition followed by an after context' in p:
        return 'T-repetition'
    if ('may overlap this SingleCaptureDoubleReplacement rule' in p or 'may overlap this capture rule' in p
            or 'may overlap this MultipleCaptureMultipleReplacement rule' in p):
        return 'T-capture-overlap'
    if '(a MultipleCaptureMultipleReplacement rule, but' in p:
        return 'T-capture-pattern'
    if '(an insertion rule, but' in p:
        return 'T-insertion'
    if 'does not support in lookaheads' in p:
        return 'E-engine-lookahead'
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
        #  The string rule overlaps of each conversion group.
        groups = []
        for line in lines:
            m = re.match(r'    string rule overlaps: (\d+) pairs \((\d+) one-way, (\d+) two-way, (\d+) self\); (.*)$', line)
            if m:
                verdict = m.group(5)
                g = dict(pairs=int(m.group(1)), one_way=int(m.group(2)), two_way=int(m.group(3)), self=int(m.group(4)))
                if verdict.startswith('single pass'):
                    g['verdict'] = 'single-pass'
                    g['stages'] = int(re.search(r'in (\d+) stages', verdict).group(1))
                elif g['two_way']:
                    g['verdict'] = 'two-way'
                elif re.match(r'(not single pass|selected sequentially): self-overlaps', verdict):
                    g['verdict'] = 'self'
                    g['stages'] = int(re.search(r'in (\d+) stages', verdict).group(1))
                else:
                    g['verdict'] = 'cycle'
                    m = re.search(r'cycle of (\d+)', verdict)
                    g['cycle'] = int(m.group(1)) if m else 0
                groups.append(g)
        e['overlap_groups'] = groups
        w2b = {'two-way': 'S-W2b-two-way', 'self': 'S-W2b-self', 'cycle': 'S-W2b-cycle', 'single-pass': 'S-W2b-single-pass'}
        verdicts = {g['verdict'] for g in groups}
        w2b_class = next((w2b[v] for v in ('two-way', 'self', 'cycle', 'single-pass') if v in verdicts),
                         'S-match-within-string-rule')
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
                    c = classify_problem(p)
                    own[w2b_class if c == 'S-match-within-string-rule' else c] += 1
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


#  The test files of all the transforms are listed; those of the transforms of
#  names (all, by default) are run, and the results of the others are those of
#  previous (if they are still implementable).
def run_tests(args, entries, names=None, previous=None):
    index = {}
    for name, e in entries.items():
        for n in [name] + e['aliases']:
            index.setdefault(n.lower(), name)
    names = set(entries) if names is None else set(names)
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
        if name not in names:
            old = (previous or {}).get(name, {}).get('test_results', {}).get(f)
            if old is not None:
                e.setdefault('test_results', {})[f] = old
                continue
        with open(os.path.join(args.test_data, f), encoding='utf-8') as t:
            pairs = [l.rstrip('\n').split('\t') for l in t if '\t' in l and not l.startswith('#')]
        src = os.path.join(tests, f)
        with open(src, 'w', encoding='utf-8') as t:
            t.write(''.join(p[0] + '\n' for p in pairs))
        try:
            r = subprocess.run([args.tconv, '--transforms-dir=' + args.transforms_dir, name, src],
                               capture_output=True, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            e.setdefault('test_results', {})[f] = dict(total=len(pairs), failed=len(pairs), timeout=True)
            continue
        out = r.stdout.decode('utf-8', 'replace').split('\n')
        failing = [i for i, p in enumerate(pairs) if i >= len(out) or out[i] != p[1]]
        # Each case is a text of its own: in the text of all the cases, one per
        # line, a context may see the line break instead of the text boundary.
        # The failing cases are run again, each as a text (tconv --lines, which
        # runs the pipeline once per line, too slowly for all the cases).
        if failing:
            retry = src + '.failing'
            with open(retry, 'w', encoding='utf-8') as t:
                t.write(''.join(pairs[i][0] + '\n' for i in failing))
            try:
                r = subprocess.run([args.tconv, '--transforms-dir=' + args.transforms_dir, '--lines', name, retry],
                                   capture_output=True, timeout=args.timeout)
                out = r.stdout.decode('utf-8', 'replace').split('\n')
                failing = [i for k, i in enumerate(failing) if k >= len(out) or out[k] != pairs[i][1]]
            except subprocess.TimeoutExpired:
                pass
        e.setdefault('test_results', {})[f] = dict(total=len(pairs), failed=len(failing))


def summary(entries):
    md = []
    pops = populations(entries)
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
        for w, cum in greedy_order(pop):
            md.append('| %s | %d | %d | %d |' % (w, sum(1 for e in pop if w in e['work']),
                                                sum(1 for e in pop if e['work'] == [w]), cum))
        md.append('')
    md.append('## Problem classes (all CLDR-defined transforms)\n')
    md.append('| Class | work item | transforms with own problems | transforms needing (transitively) | problem rules |')
    md.append('|---|---|---:|---:|---:|')
    pop = pops[0][1]
    classes = sorted({c for e in pop for c in e['classes']}, key=lambda c: (CLASS_TO_ITEM.get(c, 'W9'), c))
    for c in classes:
        md.append('| %s | %s | %d | %d | %d |' % (c, CLASS_TO_ITEM.get(c, 'W9 analysis scale').split()[0],
                                                sum(1 for e in pop if c in e['own']),
                                                sum(1 for e in pop if c in e['classes']),
                                                sum(e['own'].get(c, 0) for e in pop)))
    md.append('')
    md.append('## String rule overlaps (W2b)\n')
    md.append('The conversion groups (of all CLDR-defined transforms, each group counted once per transform) '
              'whose string rules overlap, by verdict (see ldml::StringRuleOverlaps).\n')
    groups = [(e['name'], g) for e in pop for g in e.get('overlap_groups', [])]
    md.append('| verdict | groups | transforms | one-way pairs | two-way pairs | self-overlaps |')
    md.append('|---|---:|---:|---:|---:|---:|')
    for v in ('single-pass', 'cycle', 'self', 'two-way'):
        gs = [g for _, g in groups if g['verdict'] == v]
        md.append('| %s | %d | %d | %d | %d | %d |' % (v, len(gs), len({n for n, g in groups if g['verdict'] == v}),
                                                      sum(g['one_way'] for g in gs), sum(g['two_way'] for g in gs),
                                                      sum(g['self'] for g in gs)))
    for v, label in (('single-pass', 'the single-pass groups'), ('self', 'the self-overlap groups, apart from their self-overlaps')):
        stages = collections.Counter(g['stages'] for _, g in groups if g['verdict'] == v)
        md.append('\nStages of %s: %s\n' % (label, ', '.join('%d stages: %d groups' % kv for kv in sorted(stages.items()))))
    cycles = collections.Counter(g['cycle'] for _, g in groups if g['verdict'] == 'cycle')
    if cycles:
        md.append('Cycles of the cyclic groups (one-way, not only self-overlaps): %s\n' %
                  ', '.join('%d rules: %d groups' % kv for kv in sorted(cycles.items())))
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


def git_info(path):
    def git(*a):
        r = subprocess.run(['git', '-C', path] + list(a), capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    commit = git('rev-parse', '--short=9', 'HEAD')
    if commit is None:
        return None
    return dict(commit=commit, branch=git('rev-parse', '--abbrev-ref', 'HEAD'),
                dirty=bool(git('status', '--porcelain', '--untracked-files=no')))


#  The tconv source (the repository of this script), and the CLDR data.
def source_info(args):
    def directory(d):
        if not d:
            return None
        d = os.path.realpath(os.path.expanduser(d))
        return dict(path=d, git=git_info(d))
    return dict(tconv=git_info(os.path.dirname(os.path.realpath(__file__))),
                transforms_dir=directory(args.transforms_dir), test_data=directory(args.test_data))


def describe_commit(g):
    if not g:
        return 'unknown commit'
    return '`%s`%s' % (g['commit'], ' (with uncommitted changes)' if g['dirty'] else '')


#  The populations of the summary, and the greedy order of the work items for one.
def populations(entries):
    return [('all CLDR-defined', [e for e in entries.values() if not e['builtin']]),
            ('public CLDR-defined (not internal)', [e for e in entries.values() if not e['builtin'] and not e['internal']])]


def greedy_order(pop):
    solved, remaining, order = set(), set(WORK_ITEMS), []
    while remaining:
        best = max(sorted(remaining), key=lambda w: sum(1 for e in pop if set(e['work']) <= solved | {w}))
        solved.add(best)
        remaining.remove(best)
        order.append((best, sum(1 for e in pop if set(e['work']) <= solved)))
    return order


def totals(entries):
    t = collections.OrderedDict()
    t['built-in'] = sum(e['implementable'] for e in entries.values() if e['builtin'])
    for label, pop in populations(entries):
        t[label] = sum(e['implementable'] for e in pop)
    pop = populations(entries)[0][1]
    for w in WORK_ITEMS:
        t[w + ': needed by'] = sum(1 for e in pop if w in e['work'])
        t[w + ': only blocker of'] = sum(1 for e in pop if e['work'] == [w])
    t['failing test cases'] = sum(r['failed'] for e in entries.values() for r in e.get('test_results', {}).values())
    return t


def status_of(e):
    return 'ok' if e['implementable'] else ', '.join(w.split()[0] for w in e['work']) or '?'


#  What changed from the previous results: transforms added and removed, status,
#  own problem classes and test results, and the totals.
def changes(previous, entries, names, meta):
    md = ['# Coverage changes\n',
          'Run of %s: tconv %s; %d transforms planned again: %s\n' % (
              meta['runs'][-1]['date'], describe_commit(meta['source']['tconv']), len(names), ', '.join(names) or 'none')]
    added = [n for n in entries if n not in previous]
    removed = [n for n in previous if n not in entries]
    if added:
        md.append('New transforms: %s\n' % ', '.join(added))
    if removed:
        md.append('Transforms no longer known: %s\n' % ', '.join(removed))
    rows = []
    for name, e in entries.items():
        old = previous.get(name)
        if old is None:
            continue
        diffs = []
        if status_of(old) != status_of(e):
            diffs.append('status %s → %s' % (status_of(old), status_of(e)))
        if old.get('own', {}) != e['own']:
            fmt = lambda own: ', '.join('%s (%d)' % kv for kv in sorted(own.items())) or 'none'
            diffs.append('own problems %s → %s' % (fmt(old.get('own', {})), fmt(e['own'])))
        for f in sorted(set(old.get('test_results', {})) | set(e.get('test_results', {}))):
            a, b = old.get('test_results', {}).get(f), e.get('test_results', {}).get(f)
            fmt = lambda r: '%d/%d failed' % (r['failed'], r['total']) if r else 'not run'
            if a != b:
                diffs.append('%s %s → %s' % (f, fmt(a), fmt(b)))
        if diffs:
            rows.append('| %s | %s |' % (name, '; '.join(diffs)))
    md.append('## Transforms\n')
    md += (['| Transform | changes |', '|---|---|'] + rows) if rows else ['No changes.']
    md.append('\n## Totals\n')
    md += ['| | before | after |', '|---|---:|---:|']
    before, after = totals(previous), totals(entries)
    for k in after:
        if before.get(k) != after[k] or not k.startswith('W'):
            md.append('| %s | %s | %s |' % (k, before.get(k, ''), after[k]))
    return '\n'.join(md) + '\n'


#  The per-transform table document: coverage.md, headings one level down.
def table_doc(entries, meta, note):
    src = meta['source']
    t = src['transforms_dir']
    cldr = 'CLDR transforms in `%s`%s' % (t['path'], ' at ' + describe_commit(t['git']) if t and t['git'] else '') if t else 'CLDR transforms'
    branch = src['tconv']['branch'] if src['tconv'] else None
    head = ('# tconv coverage: per-transform table\n\n'
            'Generated by `tools/ldml/tconv_coverage.py` (tconv%s at %s%s; %s). '
            'See [ldml-tconv-coverage.md](ldml-tconv-coverage.md) for the status report, '
            'the work items and the problem classes.\n\n') % (
        ' on branch ' + branch if branch else '', describe_commit(src['tconv']), ': ' + note if note else '', cldr)
    return head + re.sub(r'(?m)^## ', '### ', summary(entries))


def bar(n, total):
    return '█' * round(40 * n / total)


#  The generated blocks of the status report (doc/ldml-tconv-coverage.md), by
#  name, each given the entries and the current text of the block.
def block_summary(entries, old):
    builtins = [e for e in entries.values() if e['builtin']]
    pop, public = populations(entries)[0][1], populations(entries)[1][1]
    files = len({e['file'] for e in pop})
    pct = lambda n, d: '%d (%d%%)' % (n, round(100 * n / d))
    return '\n'.join([
        '| | names | implementable |', '|---|---:|---:|',
        '| Built-in transforms (UTS #35 / ICU) | %d | %d |' % (len(builtins), sum(e['implementable'] for e in builtins)),
        '| Transforms defined by CLDR files (%d files) | %d | %s |' % (files, len(pop), pct(sum(e['implementable'] for e in pop), len(pop))),
        '| …of which public (not `visibility="internal"`) | %d | %s |' % (len(public), pct(sum(e['implementable'] for e in public), len(public)))])


def block_chart(entries, old):
    pop = populations(entries)[0][1]
    now = sum(e['implementable'] for e in pop)
    lines = ['```', 'cumulative implementable CLDR-defined transforms (of %d), greedy order' % len(pop),
             '%-28s %3d  %s' % ('now', now, bar(now, len(pop)))]
    prev = now
    for w, cum in greedy_order(pop):
        if cum > prev:
            lines.append('%-28s %3d  %s' % ('+ ' + w, cum, bar(cum, len(pop))))
            prev = cum
    return '\n'.join(lines + ['```'])


def block_builtins_missing(entries, old):
    missing = ['`%s`' % e['name'] for e in entries.values() if e['builtin'] and not e['implementable']]
    if not missing:
        return 'Not implemented: none.'
    return 'Not implemented: %s.' % (', '.join(missing[:-1]) + ' and ' + missing[-1] if len(missing) > 1 else missing[0])


#  Table 1 of the report: the labels and efforts of its rows are kept (by the
#  work item's number), the counts computed; work items needed by no transform
#  and not in the table are left out.
def block_work_items(entries, old):
    pop = populations(entries)[0][1]
    kept = collections.OrderedDict()
    for line in old.splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) == 4 and re.match(r'W\d', cells[0]):
            kept[cells[0].split()[0]] = (cells[0], cells[3])
    rows = []
    for w in WORK_ITEMS:
        wid = w.split()[0]
        needed = sum(1 for e in pop if w in e['work'])
        if wid in kept or needed:
            label, effort = kept.get(wid, (w, '?'))
            rows.append((wid in kept and list(kept).index(wid), label, needed,
                         sum(1 for e in pop if e['work'] == [w]), effort))
    rows.sort(key=lambda r: (r[0] is False, r[0] if r[0] is not False else 0))
    return '\n'.join(['| work item | needed by | the only blocker of | effort |', '|---|---:|---:|---|'] +
                     ['| %s | %d | %d | %s |' % r[1:] for r in rows])


def block_failing_tests(entries, old):
    rows = ['- %s (%s: %d of %d cases)' % (e['name'], f, r['failed'], r['total'])
            for e in entries.values() for f, r in sorted(e.get('test_results', {}).items()) if r['failed']]
    return '\n'.join(rows) if rows else 'No test file fails.'


REPORT_BLOCKS = collections.OrderedDict([
    ('summary', block_summary), ('chart', block_chart), ('builtins-missing', block_builtins_missing),
    ('work-items', block_work_items), ('failing-tests', block_failing_tests)])


def update_report(path, entries):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    found = []
    for name, fn in REPORT_BLOCKS.items():
        pattern = re.compile(r'(<!-- coverage:%s -->\n)(.*?)(\n?<!-- /coverage:%s -->)' % (name, name), re.S)
        m = pattern.search(text)
        if m:
            found.append(name)
            text = text[:m.start()] + m.group(1) + fn(entries, m.group(2)) + '\n' + m.group(3).lstrip('\n') + text[m.end():]
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tconv', required=True)
    ap.add_argument('--transforms-dir', required=True)
    ap.add_argument('--test-data')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=1200)
    ap.add_argument('--out', default='tconv-coverage')
    ap.add_argument('--update', action='store_true', help='keep the previous results in --out; plan and test only the selected transforms')
    ap.add_argument('--only', nargs='+', metavar='NAME', help='with --update: these transforms')
    ap.add_argument('--builtins', action='store_true', help='with --update: all the built-in transforms')
    ap.add_argument('--blocked', action='store_true', help='with --update: all the transforms not implementable before')
    ap.add_argument('--force', action='store_true', help='with --update: accept directories other than those of the previous results')
    ap.add_argument('--table-doc', metavar='PATH', help='write the per-transform table document')
    ap.add_argument('--note', help='for --table-doc: what the tconv version includes')
    ap.add_argument('--report-doc', metavar='PATH', help='rewrite the generated blocks of the status report')
    args = ap.parse_args()
    if (args.only or args.builtins or args.blocked or args.force) and not args.update:
        ap.error('--only, --builtins, --blocked and --force apply to --update')
    os.makedirs(args.out, exist_ok=True)
    meta_path = os.path.join(args.out, 'coverage-meta.json')
    source = source_info(args)
    previous, meta = None, None
    if args.update:
        with open(os.path.join(args.out, 'coverage.json'), encoding='utf-8') as f:
            previous = json.load(f, object_pairs_hook=collections.OrderedDict)
        if os.path.exists(meta_path):
            with open(meta_path, encoding='utf-8') as f:
                meta = json.load(f)
            for key in ('transforms_dir', 'test_data'):
                old, new = meta['source'].get(key), source[key]
                if (old and old['path']) != (new and new['path']) and not args.force:
                    sys.exit('tconv_coverage.py: --%s %s differs from %s of the previous results (use --force)'
                             % (key.replace('_', '-'), new and new['path'], old and old['path']))
        else:
            print('tconv_coverage.py: no %s; assuming the previous results used these directories' % meta_path,
                  file=sys.stderr)
    entries = list_transforms(args)
    names = select(args, entries, previous)
    print('tconv_coverage.py: planning %d transforms' % len(names), file=sys.stderr)
    if previous is not None:
        for name in entries:
            if name not in names:
                entries[name]['rc'] = previous[name]['rc']
    run_plans(args, entries, names)
    classify(args, entries)
    today = datetime.date.today().isoformat()
    checked = dict(tconv=source['tconv'] and source['tconv']['commit'], date=today)
    for name, e in entries.items():
        if name in names:
            e['checked'] = checked
        elif 'checked' in previous[name]:
            e['checked'] = previous[name]['checked']
    if args.test_data:
        run_tests(args, entries, names, previous)
    elif previous is not None:
        for name, e in entries.items():
            for key in ('tests', 'test_results'):
                if key in previous[name] and (key == 'tests' or (name not in names and e['implementable'])):
                    e[key] = previous[name][key]
    run = dict(date=today, tconv=source['tconv'], mode='update' if args.update else 'full',
               planned=len(names), selected=names if args.update else 'all')
    meta = dict(source=source, runs=(meta['runs'] if meta else []) + [run])
    with open(os.path.join(args.out, 'coverage.json'), 'w', encoding='utf-8') as f:
        json.dump(entries, f, indent=1, ensure_ascii=False)
    with open(os.path.join(args.out, 'coverage.md'), 'w', encoding='utf-8') as f:
        f.write(summary(entries))
    with open(meta_path, 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=1, ensure_ascii=False)
    if previous is not None:
        with open(os.path.join(args.out, 'changes.md'), 'w', encoding='utf-8') as f:
            f.write(changes(previous, entries, names, meta))
    if args.table_doc:
        with open(args.table_doc, 'w', encoding='utf-8') as f:
            f.write(table_doc(entries, meta, args.note))
    if args.report_doc:
        found = update_report(args.report_doc, entries)
        print('tconv_coverage.py: %s: blocks rewritten: %s' % (args.report_doc, ', '.join(found) or 'none (no markers)'),
              file=sys.stderr)


if __name__ == '__main__':
    main()
