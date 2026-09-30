#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Runs the csv2json executable over every *.csv file in a test-file directory
# and diffs its output against the expected .json output for that file.
#
# Usage: python3 csv2json_test.py [options] <csv2json_executable>
#
# Extra flags may be forwarded to the executable under test, e.g. to select
# a non-default JIT backend or compile-time option:
#
#   python3 csv2json_test.py --flags='--use-mcjit' ../../../build/bin/csv2json
#   python3 csv2json_test.py --flags='--compile-threads=4 -enable-object-cache=0' ../../../build/bin/csv2json
#

import argparse
import difflib
import os
import shlex
import shutil
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(description='Run csv2json conformance tests.')
    parser.add_argument('program', help='path to the csv2json executable under test')
    parser.add_argument('-d', '--input_dir', default='../TestFiles',
                         help='directory of input .csv files (default: ../TestFiles)')
    parser.add_argument('-e', '--expected_dir', default='ExpectedOutput',
                         help='directory of expected .json output (default: ExpectedOutput)')
    parser.add_argument('-o', '--output_dir', default='TestOutput',
                         help='directory to write actual .json output (default: TestOutput)')
    parser.add_argument('--flags', default='',
                         help="extra flags to pass to the executable under test, e.g. "
                              "--flags='--use-mcjit'")
    parser.add_argument('-v', '--verbose', action='store_true',
                         help='show each test invocation and a diff for any failure')
    return parser.parse_args()


def reset_output_dir(output_dir):
    if os.path.isdir(output_dir):
        backup_dir = output_dir + '.bak'
        if os.path.isdir(backup_dir):
            shutil.rmtree(backup_dir)
        os.rename(output_dir, backup_dir)
    os.makedirs(output_dir)


def run_one(program, extra_flags, in_path, out_path, verbose):
    cmd = [program] + extra_flags + [in_path]
    if verbose:
        print('Running: %s' % ' '.join(shlex.quote(c) for c in cmd))
    with open(out_path, 'wb') as out_f:
        result = subprocess.run(cmd, stdout=out_f, stderr=subprocess.PIPE)
    return result


def show_diff(expected_path, actual_path):
    try:
        with open(expected_path, encoding='utf-8') as f:
            expected_lines = f.readlines()
        with open(actual_path, encoding='utf-8') as f:
            actual_lines = f.readlines()
    except UnicodeDecodeError:
        print('  (binary output, no diff shown)')
        return
    diff = difflib.unified_diff(expected_lines, actual_lines,
                                 fromfile=expected_path, tofile=actual_path)
    sys.stdout.writelines(diff)


def main():
    args = parse_args()
    extra_flags = shlex.split(args.flags)

    reset_output_dir(args.output_dir)

    inputs = sorted(f for f in os.listdir(args.input_dir) if f.endswith('.csv'))
    if not inputs:
        print('No .csv input files found in %s' % args.input_dir)
        sys.exit(1)

    failures = []
    for fname in inputs:
        base = fname[:-len('.csv')]
        in_path = os.path.join(args.input_dir, fname)
        out_path = os.path.join(args.output_dir, base + '.json')
        expected_path = os.path.join(args.expected_dir, base + '.json')

        result = run_one(args.program, extra_flags, in_path, out_path, args.verbose)

        if result.returncode != 0:
            failures.append((base, 'exited with code %d: %s'
                              % (result.returncode, result.stderr.decode(errors='replace').strip())))
            continue

        if not os.path.exists(expected_path):
            failures.append((base, 'no expected output at %s' % expected_path))
            continue

        with open(out_path, 'rb') as f:
            actual = f.read()
        with open(expected_path, 'rb') as f:
            expected = f.read()

        if actual != expected:
            failures.append((base, 'output differs from expected'))
            if args.verbose:
                show_diff(expected_path, out_path)
        elif args.verbose:
            print('Test success: %s' % base)

    if failures:
        print('%d/%d tests FAILED:' % (len(failures), len(inputs)))
        for name, reason in failures:
            print('  %s: %s' % (name, reason))
        sys.exit(1)
    else:
        print('%d/%d tests passed.' % (len(inputs), len(inputs)))


if __name__ == '__main__':
    main()
