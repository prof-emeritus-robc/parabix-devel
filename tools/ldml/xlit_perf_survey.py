#!/usr/bin/env python3
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0

#  xlit_perf_survey.py: compile times, run times, kernel counts and modes of
#  every transform xlit knows.
#
#  xlit_perf_survey.py --xlit <xlit> --out <dir> corpora
#      Builds a 20 MB input in the source of each transform (see Corpora) in
#      <dir>/corpora, and records which input each transform uses in
#      <dir>/inputs.json.
#  xlit_perf_survey.py --xlit <xlit> --out <dir> run [--only NAME ...] [--with-lines]
#      For each transform: xlit --plan (status, mode), --kernel-counts, and
#      the runs below; with --with-lines, also --plan --lines (region scope),
#      --kernel-counts --lines and the runs with --lines (off by default).
#      The runs, with --compile-threads=3 and -thread-num=4, in an object
#      cache of their own (emptied first):
#        cold   a run on a one-line input with the cache empty,
#        warm   the same run again (the kernels now cached),
#        run    the 20 MB input with the cache warm (best of two runs when the
#               first takes less than 10 s); the run time is its wall time less
#               the warm time.
#      With --digests, the output of the 20 MB input (an untimed run) is
#      recorded by its SHA-256 and size; with --reference-args ARGS also that of
#      a run with ARGS added (its own empty object cache: a cold compilation),
#      to check that the configuration timed gives the same output as the one
#      ARGS selects (e.g. the features of a change turned off).
#      Results are appended to <dir>/results.jsonl as they are computed; a run
#      interrupted resumes from there (transforms already in it are skipped,
#      also when they were surveyed without --with-lines: use another <dir>).
#      Each result records a digest of the --plan output (and of --plan --lines),
#      the plan text going to <dir>/plans.
#  xlit_perf_survey.py --xlit <xlit> --out <dir> run --update-changed-plans [--only NAME ...] [--with-lines]
#      Updates the results of <dir>: plans every transform again and surveys
#      again (replacing its result) each one whose plan changed, also those not
#      yet in the results.  A plan is compared by its digest (the --plan output
#      with the path of the rules file reduced to its file name; with --lines
#      too where the result has the --lines runs, which are made again with
#      it); a result without one (surveyed before the digests) by its status,
#      mode, ICU steps and --lines scope, and when these are the same, the
#      digest is recorded without surveying it again.  results.jsonl is
#      rewritten after each transform, so an interrupted update resumes.
#  xlit_perf_survey.py --xlit <xlit> --out <dir> run --seed-plans [--only NAME ...]
#      Records the plan digests (and plan texts) of the results without them,
#      from <xlit> built at the commit that made the results, without surveying
#      anything: a result is given the digest only where its recorded status,
#      mode, ICU steps, --lines scope and kernel counts are those of <xlit>;
#      the others are listed (a sign that <xlit> is not what made them).
#  xlit_perf_survey.py --out <dir> report [--table-doc PATH] [--csv PATH]
#      Prints the summary, and with --table-doc writes the per-transform table
#      between <!-- perf:table --> and <!-- /perf:table --> of PATH, and with
#      --csv PATH the same table as CSV (times unrounded to the millisecond).
#      The --lines columns are included where the results have them.
#
#  The mode (UTF-8, mixed, Unicode, or ICU for a built-in ICU applies) is the
#  one xlit --plan reports, for the built-ins too.
#
#  Run one at a time, with nothing else running: the timings use the four
#  performance cores.
#
#  Corpora: the input of a transform is text in its source (the part of its
#  canonical name before the first '-'), chosen in this order:
#    1. natural text in that language or script: Twain (QA/TestCorpora) for
#       Latin and English, the Wikibooks dumps of ~/Wikibooks (the text of
#       the articles, without the lines of templates, tables and links) for
#       de, es, fi, fr, id, tr, vi, ar, fa, el, ru, ja, ko, th, zh, and a mix of
#       all of them for Any;
#    2. the sources of the CLDR test files of the transforms with that source,
#       shuffled and joined into lines of about 70 characters;
#    3. natural text of a related language (Cyrillic languages: ru; ...);
#    4. text produced by a transform into that source from an input of these
#       kinds (Latn-Deva on English, Any-NFD on Korean for Jamo, Any-Hex/Java
#       for Hex-Any, ...);
#    5. English.
#  Each input is repeated or cut (at a line end) to 20,000,000 bytes.

import argparse
import hashlib
import html
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time

SIZE = 20000000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WIKI = os.path.expanduser('~/Wikibooks')
NATURAL = {
    'en': os.path.join(ROOT, 'QA/TestCorpora/Twain.txt'),
    'de': 'dewikibooks-20141216-pages-articles.xml', 'es': 'eswikibooks-20141223-pages-articles.xml',
    'fi': 'fiwikibooks-20141221-pages-articles.xml', 'fr': 'frwikibooks-20150106-pages-articles.xml',
    'id': 'idwikibooks-20141221-pages-articles.xml', 'tr': 'trwikibooks-20141227-pages-articles.xml',
    'vi': 'viwikibooks-20141221-pages-articles.xml', 'ar': 'arwikibooks-20150102-pages-articles.xml',
    'fa': 'fawikibooks-20141217-pages-articles.xml', 'el': 'elwikibooks-20141226-pages-articles.xml',
    'ru': 'ruwikibooks-20150123-pages-articles.xml', 'ja': 'jawikibooks-20150103-pages-articles.xml',
    'ko': 'kowikibooks-20141223-pages-articles.xml', 'th': 'thwikibooks-20150104-pages-articles.xml',
    'zh': 'zhwikibooks-20141225-pages-articles.xml',
}
#  Source tags written in natural text of one of the corpora.
EXACT = {
    'Latn': 'en', 'Latin': 'en', 'en': 'en', 'ASCII': 'en', 'Publishing': 'en', 'Accents': 'en',
    'de': 'de', 'es': 'es', 'es_419': 'es', 'fi': 'fi', 'fr': 'fr', 'id': 'id', 'tr': 'tr', 'vi': 'vi',
    'ar': 'ar', 'Arab': 'ar', 'fa': 'fa', 'el': 'el', 'Grek': 'el', 'ru': 'ru', 'Cyrl': 'ru',
    'ja': 'ja', 'Jpan': 'ja', 'Hira': 'ja', 'Kana': 'ja', 'Hrkt': 'ja', 'ja_Hrkt': 'ja',
    'Halfwidth': 'ja', 'Fullwidth': 'ja',
    'ko': 'ko', 'Hang': 'ko', 'th': 'th', 'Thai': 'th',
    'zh': 'zh', 'Han': 'zh', 'Hani': 'zh', 'Hans': 'zh', 'Hant': 'zh', 'Any': 'mix',
}
#  Source tags with natural text of a related language (after the test data).
APPROX = {
    'uk': 'ru', 'be': 'ru', 'bg': 'ru', 'mk': 'ru', 'sr': 'ru', 'kk': 'ru', 'ky': 'ru', 'mn': 'ru',
    'tk_Cyrl': 'ru', 'uz_Cyrl': 'ru', 'az_Cyrl': 'ru', 'az': 'tr', 'ps': 'ar', 'ug': 'ar',
    'uz_Latn': 'tr', 'lt': 'en', 'nl': 'de',
}
LONG_SCRIPT = {'Bengali': 'Beng', 'Devanagari': 'Deva', 'Gujarati': 'Gujr', 'Gurmukhi': 'Guru',
               'Kannada': 'Knda', 'Malayalam': 'Mlym', 'Oriya': 'Orya', 'Tamil': 'Taml', 'Telugu': 'Telu'}
#  Source tags produced by a transform from the input of another tag.
GENERATE = {
    'InterIndic': ('Deva-InterIndic', 'Deva'),
    'Jamo': ('Any-NFD', 'ko'), 'ConjoiningJamo': ('Any-NFD', 'ko'),
    'Spacedhan': ('Han-Spacedhan', 'zh'),
    'ThaiLogical': ('Thai-ThaiLogical', 'th'), 'ThaiSemi': ('Thai-ThaiSemi', 'th'),
    'Pinyin': ('Han-Latin', 'zh'), 'zh_Latn_PINYIN': ('Han-Latin', 'zh'),
    'NumericPinyin': ('Latin-NumericPinyin', 'Pinyin'),
    'Name': ('Any-Name', 'mix1'),
    'ru_Latn': ('ru-ru_Latn/BGN', 'ru'), 'ja_Latn': ('Jpan-Latn', 'ja'),
    'und_FONIPA': ('es-es_FONIPA', 'es'), 'es_FONIPA': ('es-es_FONIPA', 'es'),
    'Ethi': ('Latn-Ethi', 'en'), 'am': ('Latn-Ethi', 'en'), 'am_Ethi': ('Latn-Ethi', 'en'),
    'byn_Ethi': ('Latn-Ethi', 'en'), 'gz_Ethi': ('Latn-Ethi', 'en'), 'sgw_Ethi': ('Latn-Ethi', 'en'),
    'hy': ('Latn-Armn', 'en'), 'hy_AREVMDA': ('Latn-Armn', 'en'), 'he': ('Latn-Hebr', 'en'),
    'ka': ('Latn-Geor', 'en'), 'dv': ('Latn-Thaa', 'en'),
}


def tag_of(name):
    t = name.split('-')[0]
    return LONG_SCRIPT.get(t, t)


def list_transforms(xlit):
    out = subprocess.run([xlit, '--list-transforms'], capture_output=True, text=True, check=True).stdout
    entries = {}
    for line in out.splitlines():
        if not line.strip():
            continue
        fields = [f.strip() for f in line.split('  ')]
        entries[fields[0]] = dict(builtin='[built-in]' in fields[1:], internal='[internal]' in fields[1:],
                                  aliases=[f for f in fields[1:] if not f.startswith('[')])
    return entries


def write_sized(path, lines):
    #  lines repeated or cut (at a line end) to SIZE bytes.
    lines = [l for l in lines if l.strip()]
    if not lines:
        raise RuntimeError('no text for ' + path)
    data = [(l.rstrip('\n') + '\n').encode('utf-8') if isinstance(l, str) else l.rstrip(b'\n') + b'\n'
            for l in lines]
    total = 0
    with open(path, 'wb') as f:
        while True:
            for b in data:
                if total + len(b) > SIZE:
                    return
                f.write(b)
                total += len(b)


def wiki_lines(path, limit=80 << 20):
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read(limit)
    lines = []
    for m in re.finditer(r'<text[^>]*>(.*?)</text>', text, re.S):
        for l in html.unescape(m.group(1)).split('\n'):
            s = l.strip()
            if not s or s[0] in '{|!<=_#:;' or (s.startswith('[[') and s.endswith(']]')):
                continue
            #  The text of the wiki markup: templates and tags removed, links
            #  replaced by their text, no quote marks.
            for _ in range(3):
                s = re.sub(r'\{\{[^{}]*\}\}', '', s)
            s = re.sub(r'\[\[(?:[^\[\]|]*\|)?([^\[\]]*)\]\]', r'\1', s)
            s = re.sub(r'\[https?://\S*\s?([^\]]*)\]', r'\1', s)
            s = re.sub(r'<[^>]*>', '', s)
            s = re.sub(r"'{2,}", '', s).strip(' *')
            if len(s) >= 20:
                lines.append(s)
    return lines


def file_lines(path):
    with open(path, 'rb') as f:
        return f.read().split(b'\n')


class Corpora:
    def __init__(self, args, entries):
        self.args, self.entries = args, entries
        self.dir = os.path.join(args.out, 'corpora')
        os.makedirs(self.dir, exist_ok=True)
        self.pools = self.test_pools()
        self.made = {}

    def test_pools(self):
        index = {}
        for name, e in self.entries.items():
            for n in [name] + e['aliases']:
                index.setdefault(n.lower(), name)
        pools = {}
        d = self.args.test_data
        for f in sorted(os.listdir(d)):
            name = index.get(f[:-4].lower()) if f.endswith('.txt') else None
            if name is None:
                continue
            with open(os.path.join(d, f), encoding='utf-8') as t:
                src = [l.split('\t')[0] for l in t if '\t' in l and not l.startswith('#')]
            pools.setdefault(tag_of(name), []).extend(s for s in src if s.strip())
        return pools

    def path(self, key):
        return os.path.join(self.dir, key.replace('/', '%') + '.txt')

    #  The corpus of a source tag: (key, description).
    def resolve(self, tag):
        if tag in EXACT:
            return self.natural(EXACT[tag])
        if len(set(self.pools.get(tag, []))) >= 10:
            return self.pool(tag)
        if tag in APPROX:
            return self.natural(APPROX[tag])
        gen = GENERATE.get(tag)
        if gen is None and 'Latn-' + tag in self.entries:
            gen = ('Latn-' + tag, 'en')
        if gen is None and tag.endswith('_FONIPA'):
            base = tag[:-len('_FONIPA')]
            if base + '-' + tag in self.entries:
                gen = (base + '-' + tag, base)
        if gen is None and tag == 'Hex':
            gen = ('Any-Hex/Java', 'mix1')
        if gen is not None:
            return self.generated(*gen)
        return self.natural('en')

    def natural(self, lang):
        key = 'nat-' + lang
        if key not in self.made:
            p = self.path(key)
            if not os.path.exists(p):
                if lang in ('mix', 'mix1'):
                    #  1/16 of each natural corpus, in blocks of 200 lines (mix1: 1/20 of the size).
                    parts = []
                    for l in NATURAL:
                        with open(self.natural(l)[0] and self.path('nat-' + l), 'rb') as f:
                            parts.append(f.read(SIZE // len(NATURAL) // (20 if lang == 'mix1' else 1)).split(b'\n')[:-1])
                    lines = []
                    for i in range(0, max(len(x) for x in parts), 200):
                        for x in parts:
                            lines.extend(x[i:i + 200])
                    if lang == 'mix1':
                        with open(p, 'wb') as f:
                            f.write(b''.join(l + b'\n' for l in lines))
                    else:
                        write_sized(p, lines)
                elif lang == 'en':
                    write_sized(p, file_lines(NATURAL['en']))
                else:
                    write_sized(p, wiki_lines(os.path.join(WIKI, NATURAL[lang])))
            self.made[key] = 'natural ' + ({'en': 'English (Twain)', 'mix': 'mix of all the natural corpora',
                                             'mix1': 'mix (1 MB)'}.get(lang, lang + ' Wikibooks'))
        return key, self.made[key]

    def pool(self, tag):
        key = 'pool-' + tag
        if key not in self.made:
            p = self.path(key)
            if not os.path.exists(p):
                words = sorted(set(self.pools[tag]))
                rng = random.Random(1)
                lines, cur = [], []
                n = 0
                while n < SIZE + 1000:
                    w = rng.choice(words)
                    cur.append(w)
                    if sum(len(x) + 1 for x in cur) >= 70:
                        l = ' '.join(cur)
                        lines.append(l)
                        n += len(l.encode('utf-8')) + 1
                        cur = []
                write_sized(p, lines)
            self.made[key] = 'CLDR test sources of %s (%d distinct)' % (tag, len(set(self.pools[tag])))
        return key, self.made[key]

    def generated(self, transform, base):
        bkey, bdesc = self.resolve(base) if base not in ('mix1',) else self.natural('mix1')
        key = 'gen-%s-from-%s' % (transform, bkey)
        if key not in self.made:
            p = self.path(key)
            if not os.path.exists(p):
                tmp = p + '.tmp'
                with open(tmp, 'wb') as out:
                    r = subprocess.run([self.args.xlit, '-object-cache-dir=' + os.path.join(self.args.out, 'gencache'),
                                        transform, self.path(bkey)], stdout=out, stderr=subprocess.PIPE)
                if r.returncode != 0:
                    raise RuntimeError('%s failed: %s' % (transform, r.stderr.decode()[:500]))
                with open(tmp, 'rb') as f:
                    data = f.read()
                #  Any-Hex and Any-Name encode the line ends too: lines again.
                for nl in (b'\\u000A', b'\\x{A}', b'\\x{000A}', b'&#xA;', b'&#10;', b'U+000A', b'\\N{<control-000A>}'):
                    if data.count(b'\n') < 100 and nl in data:
                        data = data.replace(nl, nl + b'\n')
                write_sized(p, data.split(b'\n'))
                os.remove(tmp)
            self.made[key] = '%s of %s' % (transform, bdesc)
        return key, self.made[key]


def build_corpora(args):
    entries = list_transforms(args.xlit)
    c = Corpora(args, entries)
    inputs = {}
    for name in entries:
        tag = tag_of(name)
        try:
            if name.startswith('Hex-Any'):
                #  Each variant of Hex-Any on the escapes of its Any-Hex.
                key, desc = c.generated('Any-Hex/' + (name.split('/')[1] if '/' in name else 'Java'), 'mix1')
            else:
                key, desc = c.resolve(tag)
        except RuntimeError as e:
            print('%s: %s; English instead' % (name, e), file=sys.stderr)
            key, desc = c.natural('en')
        inputs[name] = dict(corpus=key, path=c.path(key), desc=desc)
        print(name, '->', desc, flush=True)
    for key in set(i['corpus'] for i in inputs.values()):
        with open(c.path(key), 'rb') as f:
            first = b''
            for l in f:
                if l.strip():
                    first = l
                    break
        with open(c.path(key) + '.line', 'wb') as f:
            f.write(first)
    with open(os.path.join(args.out, 'inputs.json'), 'w') as f:
        json.dump(inputs, f, indent=1, ensure_ascii=False)


def timed(cmd, timeout, stdout=subprocess.DEVNULL):
    t = time.perf_counter()
    try:
        r = subprocess.run(cmd, stdout=stdout, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, 'timeout', ''
    return time.perf_counter() - t, r.returncode, r.stderr.decode('utf-8', 'replace')[-400:]


def output_digest(cmd, timeout, scratch):
    #  The SHA-256 and size of the output of cmd (written to a file in scratch),
    #  or an error.
    path = os.path.join(scratch, 'output')
    with open(path, 'wb') as out:
        t, rc, err = timed(cmd, timeout, stdout=out)
    if rc != 0:
        return {'error': '%s: %s' % (rc, err.strip().splitlines()[-1] if err.strip() else '')}
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    size = os.path.getsize(path)
    os.remove(path)
    return {'sha256': h.hexdigest(), 'bytes': size}


def plan_info(text):
    mode = next((l.strip()[len('mode: '):] for l in text.splitlines() if l.strip().startswith('mode: ')), None)
    scope = next((l.strip()[len('region scope (--lines): '):] for l in text.splitlines()
                  if l.strip().startswith('region scope (--lines): ')), None)
    icu = 'ICU' in text
    return mode, scope, icu


def kernels_of(text):
    m = re.search(r'total: \d+ groups, \d+ rules(?:, \d+ group kernels, (\d+) kernels)?', text)
    return int(m.group(1)) if m and m.group(1) else None


def plan_text(xlit, name, extra=()):
    p = subprocess.run([xlit, '--plan'] + list(extra) + [name], capture_output=True, timeout=600)
    return p.returncode, (p.stdout + p.stderr).decode('utf-8', 'replace')


#  The digest of a plan: of its text with the paths (of the rules files)
#  reduced to their file names, which do not depend on where the data is.
def plan_digest(text):
    norm = re.sub(r'/[^\s()]*/([^/\s()]+)', r'\1', text)
    return hashlib.sha256(norm.encode('utf-8')).hexdigest()[:16]


def save_plan(args, name, text, suffix=''):
    d = os.path.join(args.out, 'plans')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name.replace('/', '%') + suffix + '.txt'), 'w', encoding='utf-8') as f:
        f.write(text)


#  The survey of one transform (the plans given where already made).
def survey_one(args, name, entries, inputs, with_lines, plan=None, plan_lines=None):
    r = dict(name=name, builtin=entries[name]['builtin'], internal=entries[name]['internal'],
             input=inputs[name]['desc'])
    rc, text = plan or plan_text(args.xlit, name)
    save_plan(args, name, text)
    r['plan_rc'] = rc
    r['plan_digest'] = plan_digest(text)
    r['mode'], r['scope'], r['icu'] = plan_info(text)
    if rc != 0:
        r['problems'] = [l for l in text.splitlines() if 'problem' in l.lower()][:5]
    if with_lines:
        _, tl = plan_lines or plan_text(args.xlit, name, ['--lines'])
        save_plan(args, name, tl, '.lines')
        r['plan_lines_digest'] = plan_digest(tl)
        r['mode_lines'], r['scope_lines'], _ = plan_info(tl)
    for key, extra in (('kernels', []), ('kernels_lines', ['--lines']))[:2 if with_lines else 1]:
        k = subprocess.run([args.xlit, '--kernel-counts'] + extra + [name], capture_output=True, timeout=600)
        r[key] = kernels_of((k.stdout + k.stderr).decode('utf-8', 'replace'))
    if rc == 0:
        cache = os.path.join(args.out, 'cache')
        base = [args.xlit, '--compile-threads=3', '-thread-num=4']
        corpus = inputs[name]['path']
        for mode, extra in (('std', []), ('lines', ['--lines']))[:2 if with_lines else 1]:
            shutil.rmtree(cache, ignore_errors=True)
            os.makedirs(cache)
            cmd = base + ['-object-cache-dir=' + cache] + extra + [name]
            m = {}
            m['cold'], rc2, err = timed(cmd + [corpus + '.line'], args.timeout)
            if rc2 != 0:
                m['error'] = '%s: %s' % (rc2, err.strip().splitlines()[-1] if err.strip() else '')
                r[mode] = m
                continue
            m['warm'], rc2, err = timed(cmd + [corpus + '.line'], args.timeout)
            full, rc2, err = timed(cmd + [corpus], args.run_timeout)
            if rc2 != 0:
                m['error'] = '20 MB run %s: %s' % (rc2, err.strip().splitlines()[-1] if err.strip() else '')
            elif full < 10:
                again, rc3, _ = timed(cmd + [corpus], args.run_timeout)
                if rc3 == 0:
                    full = min(full, again)
            m['full'] = full
            if full is not None and m['warm'] is not None:
                m['run'] = max(full - m['warm'], 0.0)
            if args.digests and 'error' not in m:
                m['output'] = output_digest(cmd + [corpus], args.run_timeout, args.out)
                if args.reference_args:
                    refCache = os.path.join(args.out, 'refcache')
                    shutil.rmtree(refCache, ignore_errors=True)
                    os.makedirs(refCache)
                    ref = base + ['-object-cache-dir=' + refCache] + extra + args.reference_args.split() + [name]
                    m['reference'] = output_digest(ref + [corpus], args.timeout + args.run_timeout, args.out)
                    shutil.rmtree(refCache, ignore_errors=True)
            r[mode] = m
        shutil.rmtree(cache, ignore_errors=True)
    return r


def print_result(r, with_lines):
    keys = ('name', 'plan_rc', 'mode', 'kernels', 'std') + (('lines',) if with_lines else ())
    print(json.dumps({k: r.get(k) for k in keys}), flush=True)


def run_survey(args):
    if args.seed_plans:
        return seed_plans(args)
    if args.update_changed_plans:
        return update_changed_plans(args)
    entries = list_transforms(args.xlit)
    with open(os.path.join(args.out, 'inputs.json')) as f:
        inputs = json.load(f)
    path = os.path.join(args.out, 'results.jsonl')
    done = set()
    if os.path.exists(path):
        with open(path) as f:
            done = {json.loads(l)['name'] for l in f if l.strip()}
    for name in args.only or list(entries):
        if name in done:
            continue
        r = survey_one(args, name, entries, inputs, args.with_lines)
        with open(path, 'a') as f:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
        print_result(r, args.with_lines)


def write_results(path, rows):
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    os.replace(tmp, path)


#  Whether the plan of a result changed: by the digests where the result has
#  them, else by what it recorded of the plan (None: the same).
def plan_change(r, plan, plan_lines):
    rc, text = plan
    if 'plan_digest' in r:
        if r['plan_digest'] != plan_digest(text):
            return 'plan'
        if plan_lines is not None and r.get('plan_lines_digest') != plan_digest(plan_lines[1]):
            return 'plan --lines'
        return None
    mode, scope, icu = plan_info(text)
    old = (r['plan_rc'], r.get('mode'), r.get('scope'), bool(r.get('icu')))
    new = (rc, mode, scope, icu)
    if plan_lines is not None:
        old += (r.get('mode_lines'), r.get('scope_lines'))
        new += plan_info(plan_lines[1])[:2]
    if old != new:
        return 'recorded %s -> %s' % (old, new)
    return None


def kernel_counts(xlit, name, extra=()):
    k = subprocess.run([xlit, '--kernel-counts'] + list(extra) + [name], capture_output=True, timeout=600)
    return kernels_of((k.stdout + k.stderr).decode('utf-8', 'replace'))


def seed_plans(args):
    path = os.path.join(args.out, 'results.jsonl')
    rows = load(args)
    only = set(args.only or [])
    seeded, mismatched = 0, []
    for n, r in enumerate(rows):
        if 'plan_digest' in r or (only and r['name'] not in only):
            continue
        lines = has_lines([r])
        plan = plan_text(args.xlit, r['name'])
        plan_lines = plan_text(args.xlit, r['name'], ['--lines']) if lines else None
        why = plan_change(r, plan, plan_lines)
        if why is None:
            kernels = (kernel_counts(args.xlit, r['name']),)
            old = (r.get('kernels'),)
            if lines:
                kernels += (kernel_counts(args.xlit, r['name'], ['--lines']),)
                old += (r.get('kernels_lines'),)
            if kernels != old:
                why = 'kernels %s -> %s' % (old, kernels)
        if why is not None:
            mismatched.append(r['name'])
            print('%s: not seeded (%s)' % (r['name'], why), flush=True)
            continue
        r['plan_digest'] = plan_digest(plan[1])
        save_plan(args, r['name'], plan[1])
        if plan_lines is not None:
            r['plan_lines_digest'] = plan_digest(plan_lines[1])
            save_plan(args, r['name'], plan_lines[1], '.lines')
        seeded += 1
        if seeded % 25 == 0:
            write_results(path, rows)
            print('%d seeded (%d of %d)' % (seeded, n + 1, len(rows)), flush=True)
    write_results(path, rows)
    print('plans seeded: %d; not seeded: %d %s' % (seeded, len(mismatched), ' '.join(mismatched)))


def update_changed_plans(args):
    entries = list_transforms(args.xlit)
    with open(os.path.join(args.out, 'inputs.json')) as f:
        inputs = json.load(f)
    path = os.path.join(args.out, 'results.jsonl')
    rows = load(args) if os.path.exists(path) else []
    index = {r['name']: i for i, r in enumerate(rows)}
    counts = dict(unchanged=0, recorded=0, changed=0, new=0)
    for name in args.only or list(entries):
        if name not in inputs:
            print('%s: no input in inputs.json (run corpora again); skipped' % name, file=sys.stderr)
            continue
        i = index.get(name)
        old = rows[i] if i is not None else None
        #  The --lines runs as the result has them (for a new one, as --with-lines says).
        lines = has_lines([old]) if old is not None else args.with_lines
        plan = plan_text(args.xlit, name)
        plan_lines = plan_text(args.xlit, name, ['--lines']) if lines else None
        if old is not None:
            why = plan_change(old, plan, plan_lines)
            if why is None:
                if 'plan_digest' in old:
                    counts['unchanged'] += 1
                    continue
                old['plan_digest'] = plan_digest(plan[1])
                save_plan(args, name, plan[1])
                if plan_lines is not None:
                    old['plan_lines_digest'] = plan_digest(plan_lines[1])
                    save_plan(args, name, plan_lines[1], '.lines')
                counts['recorded'] += 1
                write_results(path, rows)
                continue
            print('%s: changed (%s)' % (name, why), flush=True)
        else:
            print('%s: new' % name, flush=True)
        r = survey_one(args, name, entries, inputs, lines, plan, plan_lines)
        if i is None:
            index[name] = len(rows)
            rows.append(r)
            counts['new'] += 1
        else:
            rows[i] = r
            counts['changed'] += 1
        write_results(path, rows)
        print_result(r, lines)
    gone = [r['name'] for r in rows if r['name'] not in entries]
    print('plans: %(changed)d changed, %(new)d new, %(unchanged)d unchanged, '
          '%(recorded)d unchanged by what was recorded (digest now recorded)' % counts)
    if gone:
        print('in the results but no longer listed by xlit (kept): ' + ', '.join(gone))


def fmt(x, digits=2):
    return '' if x is None else ('%.*f' % (digits, x))


def load(args):
    with open(os.path.join(args.out, 'results.jsonl')) as f:
        return [json.loads(l) for l in f if l.strip()]


#  The mode as xlit --plan reports it (for the built-ins too; a built-in that
#  ICU applies, ICU).
def utf8_label(mode, r=None):
    if r is not None and r.get('builtin') and r.get('icu'):
        return 'ICU'
    if mode is None:
        return ''
    if mode.startswith('UTF-8'):
        return 'UTF-8'
    if mode.startswith('mixed'):
        return 'mixed'
    return 'Unicode'


def has_lines(rows):
    return any('lines' in r or r.get('scope_lines') is not None for r in rows)


def report(args):
    rows = load(args)
    modes = ('std', 'lines') if has_lines(rows) else ('std',)
    if args.table_doc:
        out = []
        if len(modes) == 2:
            out.append('| Transform | status | UTF-8 | --lines | input | kernels | cold s | warm s | run s | MB/s | cold s --lines | warm s --lines | run s --lines | MB/s --lines |')
            out.append('|---|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
        else:
            out.append('| Transform | status | UTF-8 | input | kernels | cold s | warm s | run s | MB/s |')
            out.append('|---|---|---|---|---:|---:|---:|---:|---:|')
        for r in sorted(rows, key=lambda r: (r['builtin'], r['name'].lower())):
            status = '**ok**' if r['plan_rc'] == 0 else 'blocked'
            if r.get('icu'):
                status += ' (ICU steps)'
            cells = []
            for mode in modes:
                m = r.get(mode, {})
                if not m:
                    cells += ['', '', '', '']
                    continue
                if 'error' in m and m.get('full') is None:
                    if 'timeout' in m['error'] and m.get('warm') is not None:
                        cells += [fmt(m.get('cold')), fmt(m.get('warm')), '> %d' % args.run_timeout,
                                  '< %.2f' % (SIZE / 1e6 / args.run_timeout)]
                    else:
                        #  Failed (in the compilation, if before the warm run), after this long.
                        cells += (['failed after %s' % fmt(m.get('cold'), 0), '', '', ''] if 'warm' not in m
                                  else [fmt(m.get('cold')), fmt(m.get('warm')), 'failed', ''])
                    continue
                run = m.get('run')
                #  Runs within the noise of the measurement (about 0.02 s).
                cells += [fmt(m.get('cold')), fmt(m.get('warm')), fmt(run),
                          ('%.0f' % (SIZE / 1e6 / run)) if run and run >= 0.02 else '> 1000']
            scope = r.get('scope_lines') or r.get('scope') or ''
            if scope.startswith('supported'):
                scope = ', '.join(['yes'] + (['contexts bounded'] if 'contexts bounded' in scope else [])
                                  + (['ICU per line'] if 'ICU line by line' in scope else []))
            else:
                scope = scope.split(':')[0]
            if r['plan_rc'] != 0:
                scope = ''
            #  (xlit --kernel-counts --lines gives the same counts as without --lines.)
            fields = [r['name'], status, utf8_label(r.get('mode'), r)] + ([scope] if len(modes) == 2 else []) + [
                re.sub(r'CLDR test sources of (\S+) \((\d+) distinct\)', r'CLDR tests of \1 (\2 words)', r['input']),
                str(r.get('kernels')) if r.get('kernels') is not None else '', ' | '.join(cells)]
            out.append('| ' + ' | '.join(fields) + ' |')
        #  Between <!-- perf:table --> and <!-- /perf:table --> of the document
        #  (appended to it, with the markers, if it has none).
        block = '<!-- perf:table -->\n' + '\n'.join(out) + '\n<!-- /perf:table -->'
        doc = open(args.table_doc, encoding='utf-8').read() if os.path.exists(args.table_doc) else ''
        m = re.search(r'<!-- perf:table -->.*?<!-- /perf:table -->', doc, re.S)
        doc = doc[:m.start()] + block + doc[m.end():] if m else doc.rstrip('\n') + '\n\n' + block + '\n'
        with open(args.table_doc, 'w', encoding='utf-8') as f:
            f.write(doc)
    if args.csv:
        write_csv(args, rows)
    summary(rows)


#  The table of --table-doc as CSV, with the times in seconds unrounded to the
#  millisecond and the MB/s computed for every run; the note of each mode says
#  where a run did not finish, failed, or is within the noise of the measurement.
def write_csv(args, rows):
    import csv
    modes = ('std', 'lines') if has_lines(rows) else ('std',)
    head = ['transform', 'builtin', 'internal', 'status', 'icu_steps', 'utf8_mode', 'plan_mode'] + (
        ['lines_scope'] if len(modes) == 2 else []) + ['input', 'kernels']
    for suffix in ('', '_lines')[:len(modes)]:
        head += [k + suffix for k in ('cold_s', 'warm_s', 'run_s', 'mb_per_s', 'note')]
    with open(args.csv, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(head)
        for r in sorted(rows, key=lambda r: (r['builtin'], r['name'].lower())):
            row = [r['name'], int(r['builtin']), int(r['internal']), 'ok' if r['plan_rc'] == 0 else 'blocked',
                   int(bool(r.get('icu'))), utf8_label(r.get('mode'), r) if r['plan_rc'] == 0 else '',
                   r.get('mode') or ''] + ([(r.get('scope_lines') or '') if r['plan_rc'] == 0 else ''] if len(modes) == 2 else []) + [
                   re.sub(r'CLDR test sources of (\S+) \((\d+) distinct\)', r'CLDR tests of \1 (\2 words)', r['input']),
                   '' if r.get('kernels') is None else r['kernels']]
            for mode in modes:
                m = r.get(mode, {})
                run = m.get('run')
                note = ''
                if 'error' in m and m.get('full') is None:
                    if 'timeout' in m['error'] and m.get('warm') is not None:
                        note = 'run longer than %d s' % args.run_timeout
                    elif 'warm' not in m:
                        note = 'compilation failed: ' + m['error']
                    else:
                        note = 'run failed: ' + m['error']
                elif run is not None and run < 0.02:
                    note = 'run within measurement noise'
                t = lambda k: '' if m.get(k) is None else '%.3f' % m[k]
                row += [t('cold'), t('warm'), t('run'), ('%.1f' % (SIZE / 1e6 / run)) if run else '', note]
            w.writerow(row)


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else None


def summary(rows):
    ok = [r for r in rows if r['plan_rc'] == 0]
    print('transforms %d, implementable %d' % (len(rows), len(ok)))
    from collections import Counter
    print('modes', Counter(utf8_label(r.get('mode'), r) for r in ok))
    for mode in ('std', 'lines') if has_lines(rows) else ('std',):
        ms = [r[mode] for r in ok if mode in r and r[mode].get('run') is not None]
        errs = [r['name'] for r in ok if mode in r and 'error' in r[mode]]
        print(mode, 'measured', len(ms), 'errors', len(errs), errs[:20])
        for k in ('cold', 'warm', 'run'):
            v = [m[k] for m in ms if m.get(k) is not None]
            if v:
                v.sort()
                print('  %s median %.2f p90 %.2f max %.2f total %.0f' % (k, median(v), v[int(len(v) * .9)], v[-1], sum(v)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlit')
    ap.add_argument('--out', required=True)
    ap.add_argument('--test-data', default=os.path.join(ROOT, 'third_party/cldr/common/testData/transforms'))
    ap.add_argument('--timeout', type=int, default=900, help='for the compilations (s)')
    ap.add_argument('--run-timeout', type=int, default=300, help='for the 20 MB runs (s)')
    ap.add_argument('--only', nargs='+')
    ap.add_argument('--with-lines', action='store_true',
                    help='run: also survey with --lines (--plan, --kernel-counts and the timed runs)')
    ap.add_argument('--update-changed-plans', action='store_true',
                    help='run: survey again only the transforms whose plan changed (and new ones)')
    ap.add_argument('--seed-plans', action='store_true',
                    help='run: record the plan digests of the results without them, from the xlit that made them')
    ap.add_argument('--digests', action='store_true',
                    help='run: record the SHA-256 and size of the output of the 20 MB input (an untimed run)')
    ap.add_argument('--reference-args',
                    help='run, with --digests: also the output with these xlit arguments added, for comparison')
    ap.add_argument('--table-doc')
    ap.add_argument('--csv', help='report: write the per-transform table as CSV')
    ap.add_argument('command', choices=['corpora', 'run', 'report'])
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    {'corpora': build_corpora, 'run': run_survey, 'report': report}[args.command](args)


if __name__ == '__main__':
    main()
