#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Runs a grep-compatible executable with a fixed 'ab*c' pattern over every file
# in abc/TestFiles and diffs its output against abc/ExpectedOutput.  By default,
# only the files listed in abc/QuickTests.txt (written by abc_testgen.py) are
# tested; --extended-testing tests every file.
#
# Usage: python3 abc_test.py [options] <grep_executable>
#
# Extra flags may be forwarded to the executable under test, e.g. to select
# a non-default JIT backend:
#
#   python3 abc_test.py --flags='--use-mcjit' ../build/bin/icgrep
#

import argparse
import filecmp
import os
import shlex
import shutil
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(description="Run the 'ab*c' grep conformance test.")
    parser.add_argument('program', help='path to the grep-compatible executable under test (e.g. icgrep)')
    parser.add_argument('-p', '--pattern', default='ab*c',
                         help="regex pattern to search for (default: 'ab*c')")
    parser.add_argument('-d', '--input_dir', default='abc/TestFiles')
    parser.add_argument('-e', '--expected_dir', default='abc/ExpectedOutput')
    parser.add_argument('-o', '--output_dir', default='abc/TestOutput')
    parser.add_argument('--flags', default='',
                         help="extra flags to pass to the executable under test, e.g. "
                              "--flags='--use-mcjit'")
    parser.add_argument('-q', '--quick_list', default='abc/QuickTests.txt',
                         help='the files tested without --extended-testing')
    parser.add_argument('--extended-testing', action='store_true',
                         help='test every file in the input directory')
    parser.add_argument('-v', '--verbose', action='store_true',
                         help='show each test invocation and a full diff on failure')
    return parser.parse_args()


def reset_output_dir(output_dir):
    if os.path.isdir(output_dir):
        backup_dir = output_dir + '.bak'
        if os.path.isdir(backup_dir):
            shutil.rmtree(backup_dir)
        os.rename(output_dir, backup_dir)
    os.makedirs(output_dir)


def main():
    args = parse_args()
    extra_flags = shlex.split(args.flags)

    reset_output_dir(args.output_dir)

    if args.extended_testing:
        inputs = sorted(os.listdir(args.input_dir))
    else:
        with open(args.quick_list) as f:
            inputs = [line.strip() for line in f if line.strip()]
    if not inputs:
        print('No input files found in %s' % args.input_dir)
        sys.exit(1)

    for fbase in inputs:
        in_path = os.path.join(args.input_dir, fbase)
        out_path = os.path.join(args.output_dir, fbase)
        cmd = [args.program] + extra_flags + [args.pattern]
        if args.verbose:
            print('Running: %s < %s' % (' '.join(shlex.quote(c) for c in cmd), in_path))
        with open(in_path, 'rb') as in_f, open(out_path, 'wb') as out_f:
            subprocess.run(cmd, stdin=in_f, stdout=out_f)

    if args.extended_testing:
        diff_cmd = ['diff', '-q', args.expected_dir, args.output_dir]
        print(' '.join(diff_cmd))
        result = subprocess.run(diff_cmd)
        if result.returncode != 0 and args.verbose:
            subprocess.run(['diff', '-ru', args.expected_dir, args.output_dir])
        sys.exit(result.returncode)

    failures = 0
    for fbase in inputs:
        expected_path = os.path.join(args.expected_dir, fbase)
        out_path = os.path.join(args.output_dir, fbase)
        if not filecmp.cmp(expected_path, out_path, shallow=False):
            print('Files %s and %s differ' % (expected_path, out_path))
            if args.verbose:
                subprocess.run(['diff', '-u', expected_path, out_path])
            failures += 1
    print('%d of %d abc tests failed' % (failures, len(inputs)) if failures
          else '%d abc tests passed' % len(inputs))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
