#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Tests that cached kernels are keyed by the source of the modules they depend
# on (see cmake/ParabixSourceHash.cmake and Kernel::getSourceTag).  Each tool
# runs with --trace-object-cache against a fresh cache directory.
#
# Quick checks (always run; base64 only, so that ctest stays under a second):
#   - a first run compiles and writes every kernel; a second run, with
#     -VerifyCache, reads every kernel back from the cache and verifies that it
#     matches the IR generated for it now (pipelines, whose code generation is
#     not deterministic, must instead be reported as not verified);
#   - every kernel's tag comes from a registered source hash, and the cache
#     name of every kernel ends with that tag.
#
# Full checks (--full, or --build-dir): the quick checks for icgrep as well,
# which takes many seconds to compile from an empty cache.
#
# Rebuild checks (--build-dir): for each scenario, append a comment to one
# source file, rebuild the tools, and check that exactly the kernels whose
# code generation depends on the edited module (per the module lists in
# <build>/srchash/*.inputs), and the pipelines holding them, are recompiled,
# while all the others are read from the cache.  The scenarios cover a module
# of the RE front end, the pipeline compiler (only pipelines depend on it), the
# pipeline driver and kernel.core (every kernel depends on those).  The file is then
# restored and rebuilt, and every kernel must be read from the cache, and
# verified, again.  These checks edit the source tree (restoring it afterwards)
# and rebuild much of the framework; `make objcache_test` runs them.
#
# Usage: python3 objcache_test.py [options] <bin_dir>
#
#   python3 objcache_test.py ../build/bin                       (ctest)
#   python3 objcache_test.py --build-dir ../build ../build/bin
#

import argparse
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time

SOURCE_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# (module, file in that module) edited by each rebuild scenario.
SCENARIOS = {
    're.printer': 'lib/re/printer/re_printer.cpp',
    # the pipeline compiler: only pipeline kernels depend on it
    'kernel.pipeline': 'lib/kernel/pipeline/compiler/codegen/pipeline_logic.cpp',
    # the driver optimizes and compiles every kernel
    'kernel.pipeline/driver': 'lib/kernel/pipeline/driver/driver.cpp',
    'kernel.core': 'lib/kernel/core/kernel.cpp',
}

EDIT_MARKER = '\n// objcache_test.py: temporary edit\n'

TAG_LINE = re.compile(r'^Source tag of (.*): ([0-9a-f]{16}) \((.*?) of ([^,)]*)(, with the tags of its nested kernels)?\)$')
LOOKUP_LINE = re.compile(r'^(No cache file|Read cache file|Failed to load cache file|Mismatched signature in cache file|'
                         r'Recompiling at a higher optimization level): (\S+)\.kernel')
WRITE_LINE = re.compile(r'^Wrote cache file: (\S+)\.kernel')
VERIFY_LINE = re.compile(r'^(Verified cache file|Not verified \(nondeterministic code generation\)): parabix-(\S+)\.kernel')


class Lookup:
    def __init__(self, name, tag, basis, image, nested):
        self.name = name
        self.tag = tag
        self.basis = basis
        self.image = image
        self.nested = nested
        self.result = None
        self.cache_name = None
        self.verified = False
        self.unverifiable = False

    def hit(self):
        return self.result == 'Read cache file'


class Failures:
    def __init__(self):
        self.messages = []

    def check(self, condition, message):
        if not condition:
            self.messages.append(message)
            print('FAIL: ' + message)
        return condition


def parse_args():
    parser = argparse.ArgumentParser(description='Object cache source-tag test.')
    parser.add_argument('bin_dir', help='directory holding the icgrep and base64 executables')
    parser.add_argument('--full', action='store_true', help='also run the quick checks for icgrep')
    parser.add_argument('--build-dir', help='build directory; enables the rebuild checks (implies --full)')
    parser.add_argument('-j', '--jobs', type=int, default=4,
                        help='parallel jobs for the rebuilds (default: %(default)s)')
    parser.add_argument('--scenarios', default=','.join(SCENARIOS),
                        help='comma-separated rebuild scenarios (default: %(default)s)')
    parser.add_argument('--flags', default='',
                        help='extra flags to pass to the executables under test')
    parser.add_argument('-v', '--verbose', action='store_true', help='show each invocation and its trace')
    return parser.parse_args()


class Tool:
    def __init__(self, name, args):
        self.name = name
        self.args = args


def make_tools(args, work_dir):
    text = os.path.join(work_dir, 'input.txt')
    with open(text, 'w') as f:
        f.write('hello world\nfoo bar\nworkers of the world\n')
    tools = [Tool('base64', [text])]
    if args.full or args.build_dir:
        tools.append(Tool('icgrep', ['wor[a-z]+', text]))
    return tools


def run_tool(args, tool, cache_dir, verify=False):
    cmd = [os.path.join(args.bin_dir, tool.name), '--trace-object-cache', '--object-cache-dir=' + cache_dir]
    if verify:
        cmd.append('-VerifyCache')
    cmd += shlex.split(args.flags) + tool.args
    if args.verbose:
        print('Doing: ' + ' '.join(shlex.quote(c) for c in cmd))
    result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, universal_newlines=True,
                            timeout=600)
    if args.verbose:
        print(result.stderr)
    if result.returncode != 0:
        raise RuntimeError('%s exited with %d:\n%s' % (tool.name, result.returncode, result.stderr))
    lookups = []
    written = []
    verified = set()
    unverifiable = set()
    for line in result.stderr.splitlines():
        m = VERIFY_LINE.match(line)
        if m:
            (verified if m.group(1).startswith('Verified') else unverifiable).add(m.group(2))
            continue
        m = TAG_LINE.match(line)
        if m:
            lookups.append(Lookup(m.group(1), m.group(2), m.group(3), m.group(4), m.group(5) is not None))
            continue
        m = LOOKUP_LINE.match(line)
        if m and lookups and lookups[-1].result is None:
            lookups[-1].result = m.group(1)
            lookups[-1].cache_name = m.group(2)
            continue
        m = WRITE_LINE.match(line)
        if m:
            written.append(m.group(1))
    for k in lookups:
        k.verified = k.cache_name in verified
        k.unverifiable = k.cache_name in unverifiable
    return lookups, written


def check_verified(failures, what, k):
    """K was verified, or, if it is a pipeline, reported as not verifiable."""
    if k.nested and k.unverifiable:
        return True
    return failures.check(k.verified and not k.unverifiable, '%s: %s was not verified' % (what, describe(k)))


def describe(lookup):
    return '%s [%s, %s]' % (lookup.name, lookup.tag, lookup.image)


def quick_checks(args, tools, failures):
    for tool in tools:
        with tempfile.TemporaryDirectory(prefix='objcache-') as cache_dir:
            first, written = run_tool(args, tool, cache_dir)
            failures.check(len(first) > 0, '%s: no kernels were traced' % tool.name)
            for k in first:
                failures.check(k.result == 'No cache file', '%s, first run: %s: %s' % (tool.name, describe(k), k.result))
                failures.check(k.basis == 'source hash',
                               '%s: tag of %s does not come from a source hash (%s)' % (tool.name, describe(k), k.basis))
                failures.check(k.cache_name is not None and k.cache_name.endswith('_' + k.tag),
                               '%s: cache name %s does not end with the tag %s' % (tool.name, k.cache_name, k.tag))
            failures.check(sorted(written) == sorted(k.cache_name for k in first),
                           '%s, first run: wrote %s, looked up %s' % (tool.name, sorted(written),
                                                                      sorted(k.cache_name for k in first)))
            failures.check(any(k.nested for k in first), '%s: no pipeline was traced' % tool.name)
            second, written = run_tool(args, tool, cache_dir, verify=True)
            failures.check(sorted(k.cache_name for k in second) == sorted(k.cache_name for k in first),
                           '%s: second run looked up different kernels' % tool.name)
            for k in second:
                failures.check(k.hit(), '%s, second run: %s: %s' % (tool.name, describe(k), k.result))
                check_verified(failures, '%s, second run' % tool.name, k)
            failures.check(not written, '%s, second run: wrote %s' % (tool.name, written))
            print('%s: %d kernels written, then read back and verified' % (tool.name, len(first)))


def image_modules(build_dir, image):
    """The Parabix modules the image named IMAGE depends on, from build/srchash/<target>.inputs."""
    target = image
    if target.startswith('libparabix_'):
        target = target[len('libparabix_'):]
        for ext in ('.dylib', '.so', '.a'):
            if target.endswith(ext):
                target = target[:-len(ext)]
    path = os.path.join(build_dir, 'srchash', re.sub(r'[^A-Za-z0-9_]', '_', target) + '.inputs')
    with open(path) as f:
        first = f.readline()
    if not first.startswith('# modules:'):
        raise RuntimeError('%s has no module list' % path)
    return set(first[len('# modules:'):].split())


def rebuild(args, tools):
    cmd = ['cmake', '--build', args.build_dir, '--target'] + [t.name for t in tools] + ['--parallel', str(args.jobs)]
    if args.verbose:
        print('Doing: ' + ' '.join(cmd))
    subprocess.run(cmd, check=True, stdout=None if args.verbose else subprocess.DEVNULL)


def expected_misses(args, lookups, module):
    """Kernels whose image depends on MODULE miss; so do pipelines holding a kernel that misses."""
    affected = {k.cache_name for k in lookups if module in image_modules(args.build_dir, k.image)}
    if any(not k.nested for k in lookups if k.cache_name in affected):
        affected |= {k.cache_name for k in lookups if k.nested}
    return affected


def rebuild_checks(args, tools, failures):
    with tempfile.TemporaryDirectory(prefix='objcache-') as cache_dir:
        for tool in tools:
            run_tool(args, tool, cache_dir)
        for module in args.scenarios.split(','):
            path = os.path.join(SOURCE_DIR, SCENARIOS[module])
            with open(path, 'rb') as f:
                original = f.read()
            try:
                # GNU make compares whole-second timestamps: let a second pass so the
                # edited file is newer than everything the previous build produced.
                time.sleep(1.1)
                with open(path, 'ab') as f:
                    f.write(EDIT_MARKER.encode())
                rebuild(args, tools)
                for tool in tools:
                    lookups, _ = run_tool(args, tool, cache_dir)
                    expected = expected_misses(args, lookups, module)
                    missed = {k.cache_name for k in lookups if not k.hit()}
                    for k in lookups:
                        should_miss = k.cache_name in expected
                        failures.check(should_miss != k.hit(), '%s, after editing %s: %s: %s (expected %s)' % (
                            tool.name, module, describe(k), k.result, 'a miss' if should_miss else 'a hit'))
                    print('%s, after editing %s: %d of %d kernels recompiled' % (tool.name, module, len(missed), len(lookups)))
            finally:
                time.sleep(1.1)
                with open(path, 'wb') as f:
                    f.write(original)
            rebuild(args, tools)
            for tool in tools:
                lookups, _ = run_tool(args, tool, cache_dir, verify=True)
                for k in lookups:
                    what = '%s, after restoring %s' % (tool.name, module)
                    failures.check(k.hit(), '%s: %s: %s' % (what, describe(k), k.result))
                    check_verified(failures, what, k)
            print('after restoring %s: all kernels read from the cache and verified' % module)


def terminate(signum, frame):
    # Raise, so that the rebuild checks restore the file they edited.
    raise KeyboardInterrupt('terminated by signal %d' % signum)


def main():
    args = parse_args()
    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGHUP, terminate)
    for module in args.scenarios.split(','):
        if module not in SCENARIOS:
            print('Unknown scenario %s; choose from %s' % (module, ', '.join(SCENARIOS)))
            sys.exit(2)
    failures = Failures()
    with tempfile.TemporaryDirectory(prefix='objcache-input-') as work_dir:
        tools = make_tools(args, work_dir)
        quick_checks(args, tools, failures)
        if args.build_dir:
            rebuild_checks(args, tools, failures)
    if failures.messages:
        print('%d failures' % len(failures.messages))
        sys.exit(1)
    print('All object cache tests passed')


if __name__ == '__main__':
    main()
