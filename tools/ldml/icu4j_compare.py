#!/usr/bin/env python3
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

#  icu4j_compare.py: apply every transform xlit knows with ICU4J, with ICU4C
#  (xlit --use-ICU) and with xlit (--use-ICU=0), each line of an input a text
#  of its own (--lines), and compare the results.
#
#  icu4j_compare.py --xlit <xlit> --out <dir> plan [--lines-per-input N]
#      The inputs (the sources of the transform's CLDR test files, and the
#      first N lines, default 500, of its input of the compile and run time
#      survey, tools/ldml/xlit-coverage/perf/inputs.json), the rules files and
#      the commands for ICU4JRun: for each transform, the CLDR transforms its
#      rules invoke registered first (depth first, under the names by which
#      they are invoked, as xlit's registerInvokedWithICU does, from the
#      transform steps of xlit --plan), then the transform made from its rules
#      (createFromRules), or a built-in by its name.
#  icu4j_compare.py --out <dir> icu4j --java <java> --javac <javac> --jar <icu4j jar>
#  icu4j_compare.py --xlit <xlit> --out <dir> icu4c
#      xlit --lines --use-ICU=<the transform's name>: ICU4C, made as above.
#  icu4j_compare.py --xlit <xlit> --out <dir> xlit
#      xlit --lines --use-ICU=0 (in the default object cache).
#  icu4j_compare.py --out <dir> report [--md PATH]
#      The differences, per transform, between the three.
#
#  The rules text is that of xlit's ruleText (the <tRule> elements decoded
#  and joined, each followed by a line end), with ::Null ; made ::Null (Null) ;
#  as icu_transform.cpp's invertibleNull makes it for both ICUs.

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PERF_INPUTS = os.path.join(HERE, 'xlit-coverage', 'perf', 'inputs.json')
TEST_DATA = os.path.join(ROOT, 'third_party', 'cldr', 'common', 'testData', 'transforms')


def file_name(name):
    return name.replace('/', '%')


def list_transforms(xlit):
    out = subprocess.run([xlit, '--list-transforms'], capture_output=True, text=True, check=True).stdout
    entries = {}
    for line in out.splitlines():
        if not line.strip():
            continue
        fields = [f.strip() for f in line.split('  ')]
        info = [f for f in fields[1:] if f.startswith('[') and f not in ('[built-in]', '[internal]')]
        entries[fields[0]] = dict(builtin='[built-in]' in fields[1:],
                                  file=info[0][1:-1].split(',')[0] if info else '',
                                  backward=bool(info) and 'backward' in info[0],
                                  aliases=[f for f in fields[1:] if not f.startswith('[')])
    return entries


def decode_xml_text(s):
    #  As ldml::decodeXMLText: CDATA verbatim, comments dropped, five entities.
    out, i = [], 0
    ent = dict(lt='<', gt='>', amp='&', quot='"', apos="'")
    while i < len(s):
        if s.startswith('<![CDATA[', i):
            end = s.index(']]>', i + 9)
            out.append(s[i + 9:end])
            i = end + 3
        elif s.startswith('<!--', i):
            i = s.index('-->', i + 4) + 3
        elif s[i] == '&':
            semi = s.index(';', i)
            out.append(ent[s[i + 1:semi]])
            i = semi + 1
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def rule_text(path):
    #  As xlit's ruleText (ldml::extractTRules), then invertibleNull.
    xml = open(path, encoding='utf-8').read()
    parts, pos = [], 0
    while True:
        o = xml.find('<tRule', pos)
        if o < 0:
            break
        start = xml.find('>', o)
        if xml[start - 1] == '/':
            pos = start + 1
            continue
        end = xml.find('</tRule>', start)
        parts.append(decode_xml_text(xml[start + 1:end]))
        pos = end + 8
    text = ''.join(p + '\n' for p in parts)
    return re.sub(r'(^|\n)(\s*)::\s*[Nn]ull\s*;', r'\1\2::Null (Null);', text)


ANNOTATION = re.compile(r' \((segment|by|see|applied)[^()]*\)$')


def plan_steps(xlit, out, name):
    path = os.path.join(out, 'plans', file_name(name) + '.txt')
    if not os.path.exists(path):
        r = subprocess.run([xlit, '--plan', name], capture_output=True, timeout=1200)
        with open(path, 'wb') as f:
            f.write(r.stdout + r.stderr)
    ids = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'^  transform (.+)$', line.rstrip('\n'))
        if m:
            ids.append(ANNOTATION.sub('', m.group(1)).strip())
    return ids


def make_plan(args):
    entries = list_transforms(args.xlit)
    for d in ('plans', 'rules', 'inputs', 'icu4j', 'icu4c', 'xlit'):
        os.makedirs(os.path.join(args.out, d), exist_ok=True)
    index = {}
    for name, e in entries.items():
        for n in [name] + e['aliases']:
            index.setdefault(n.lower(), name)

    def lookup(id):
        k = id.lower()
        if k in index:
            return index[k]
        if '-' not in k and ('any-' + k) in index:
            return index['any-' + k]
        return None

    rules_files = {}

    def rules_of(name):
        e = entries[name]
        key = (e['file'], e['backward'])
        if key not in rules_files:
            p = os.path.join(args.out, 'rules', os.path.basename(e['file'])[:-4] + ('.backward' if e['backward'] else '') + '.txt')
            with open(p, 'w', encoding='utf-8') as f:
                f.write(rule_text(e['file']))
            rules_files[key] = p
        return rules_files[key]

    #  The inputs.
    perf = json.load(open(PERF_INPUTS))
    tests = {}
    for f in sorted(os.listdir(TEST_DATA)):
        n = lookup(f[:-4]) if f.endswith('.txt') else None
        if n:
            with open(os.path.join(TEST_DATA, f), encoding='utf-8') as t:
                tests.setdefault(n, []).extend(l.split('\t')[0] for l in t if '\t' in l and not l.startswith('#'))
    commands, meta = [], {}
    for name, e in entries.items():
        lines = list(dict.fromkeys(tests.get(name, [])))
        with open(perf[name]['path'], encoding='utf-8', errors='replace') as f:
            for i, l in enumerate(f):
                if i >= args.lines_per_input:
                    break
                lines.append(l.rstrip('\n'))
        lines = [l.replace('\r', '') for l in lines]
        inp = os.path.join(args.out, 'inputs', file_name(name) + '.txt')
        with open(inp, 'w', encoding='utf-8') as f:
            f.write(''.join(l + '\n' for l in lines))
        out = os.path.join(args.out, 'icu4j', file_name(name) + '.txt')
        meta[name] = dict(builtin=e['builtin'], input=inp, lines=len(lines), tests=len(tests.get(name, [])),
                          corpus=perf[name]['desc'])
        if e['builtin']:
            commands.append('\t'.join(['NAME', name, inp, out]))
            continue
        registered = set()

        def register(entry_name):
            for id in plan_steps(args.xlit, args.out, entry_name):
                used = lookup(id)
                if used is None or entries[used]['builtin'] or id in registered:
                    continue
                registered.add(id)
                register(used)
                commands.append('\t'.join(['REG', id, '1' if entries[used]['backward'] else '0', rules_of(used)]))

        register(name)
        commands.append('\t'.join(['RULES', name, '1' if e['backward'] else '0', rules_of(name), inp, out]))
        print(name, len(lines), flush=True)
    with open(os.path.join(args.out, 'icu4j.cmds'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(commands) + '\n')
    json.dump(meta, open(os.path.join(args.out, 'meta.json'), 'w'), indent=1, ensure_ascii=False)


def run_icu4j(args):
    classes = os.path.join(args.out, 'classes')
    os.makedirs(classes, exist_ok=True)
    subprocess.run([args.javac, '-cp', args.jar, '-d', classes, os.path.join(HERE, 'ICU4JRun.java')], check=True)
    subprocess.run([args.java, '-Xss64m', '-cp', args.jar + os.pathsep + classes, 'ICU4JRun',
                    os.path.join(args.out, 'icu4j.cmds')], check=True)


def ecma_escape(name):
    return re.sub(r'([.^$|?*+()\[\]{}\\/])', r'\\\1', name)


def run_xlit(args, icu):
    meta = json.load(open(os.path.join(args.out, 'meta.json')))
    sub = 'icu4c' if icu else 'xlit'
    for name, m in meta.items():
        out = os.path.join(args.out, sub, file_name(name) + '.txt')
        if os.path.exists(out) or os.path.exists(out + '.err'):
            continue
        #  (In the default object cache: nothing here is timed.)
        cmd = [args.xlit, '--lines', '--use-ICU=' + (ecma_escape(name) if icu else '0')]
        try:
            with open(out + '.tmp', 'wb') as f:
                r = subprocess.run(cmd + [name, m['input']], stdout=f, stderr=subprocess.PIPE, timeout=args.timeout)
            rc, err = r.returncode, r.stderr.decode('utf-8', 'replace')
        except subprocess.TimeoutExpired:
            rc, err = 'timeout', ''
        if rc == 0:
            os.rename(out + '.tmp', out)
        else:
            os.remove(out + '.tmp')
            with open(out + '.err', 'w') as f:
                f.write('%s: %s\n' % (rc, err.strip()[-500:]))
        print(sub, name, rc, flush=True)


def read_out(path):
    if not os.path.exists(path):
        err = path + '.err'
        return None, (open(err).read().strip() if os.path.exists(err) else 'missing')
    return open(path, encoding='utf-8', errors='replace').read().split('\n')[:-1], None


def report(args):
    meta = json.load(open(os.path.join(args.out, 'meta.json')))
    rows = []
    for name, m in meta.items():
        src, _ = read_out(m['input'])
        outs = {k: read_out(os.path.join(args.out, k, file_name(name) + '.txt')) for k in ('icu4j', 'icu4c', 'xlit')}
        r = dict(name=name, builtin=m['builtin'], lines=m['lines'], errors={k: v[1] for k, v in outs.items() if v[1]})
        for a, b in (('icu4j', 'icu4c'), ('xlit', 'icu4c'), ('xlit', 'icu4j')):
            x, y = outs[a][0], outs[b][0]
            if x is None or y is None:
                r[a + '/' + b] = None
                continue
            d = [i for i in range(max(len(x), len(y))) if i >= len(x) or i >= len(y) or x[i] != y[i]]
            r[a + '/' + b] = len(d)
            r[a + '/' + b + ' examples'] = [(src[i] if i < len(src) else '', x[i] if i < len(x) else None,
                                             y[i] if i < len(y) else None) for i in d[:3]]
        rows.append(r)
    json.dump(rows, open(os.path.join(args.out, 'report.json'), 'w'), indent=1, ensure_ascii=False)
    for key in ('icu4j/icu4c', 'xlit/icu4c', 'xlit/icu4j'):
        diff = [r for r in rows if r.get(key)]
        print('%s: %d transforms differ, %d lines; not compared: %d' % (
            key, len(diff), sum(r[key] for r in diff), sum(1 for r in rows if r.get(key) is None)))
    for r in rows:
        if r['errors']:
            print('errors', r['name'], r['errors'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlit')
    ap.add_argument('--out', required=True)
    ap.add_argument('--java')
    ap.add_argument('--javac')
    ap.add_argument('--jar')
    ap.add_argument('--lines-per-input', type=int, default=500)
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('command', choices=['plan', 'icu4j', 'icu4c', 'xlit', 'report'])
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.command == 'plan':
        make_plan(args)
    elif args.command == 'icu4j':
        run_icu4j(args)
    elif args.command in ('icu4c', 'xlit'):
        run_xlit(args, args.command == 'icu4c')
    else:
        report(args)


if __name__ == '__main__':
    main()
