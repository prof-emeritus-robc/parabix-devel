#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Round-trip test for u32u8: converts every file in u8u16/TestFiles from
# UTF-8 to UTF-32LE (via iconv), runs the executable under test to convert
# back to UTF-8, and diffs the result against the original.
#
# Usage: python3 u32u8_test.py [options] <u32u8_executable>
#
# Extra flags may be forwarded to the executable under test, e.g.:
#
#   python3 u32u8_test.py --flags='--use-mcjit' ../build/bin/u32u8
#

import argparse
import os
import shlex
import shutil
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(description='Round-trip UTF-8 -> UTF-32LE -> UTF-8 conformance test for u32u8.')
    parser.add_argument('program', help='path to the u32u8 executable under test')
    parser.add_argument('-d', '--input_dir', default='u8u16/TestFiles',
                         help='UTF-8 input files (default: u8u16/TestFiles)')
    parser.add_argument('-u', '--u32_dir', default='u32TestFiles')
    parser.add_argument('-o', '--output_dir', default='u32u8output')
    parser.add_argument('--flags', default='',
                         help="extra flags to pass to the executable under test, e.g. "
                              "--flags='--use-mcjit'")
    parser.add_argument('-v', '--verbose', action='store_true',
                         help='show each test invocation')
    return parser.parse_args()


def main():
    args = parse_args()
    extra_flags = shlex.split(args.flags)

    if os.path.isdir(args.u32_dir):
        shutil.rmtree(args.u32_dir)
    if os.path.isdir(args.output_dir):
        backup_dir = args.output_dir + '.bak'
        if os.path.isdir(backup_dir):
            shutil.rmtree(backup_dir)
        os.rename(args.output_dir, backup_dir)
    os.makedirs(args.u32_dir)
    os.makedirs(args.output_dir)

    inputs = sorted(os.listdir(args.input_dir))
    if not inputs:
        print('No input files found in %s' % args.input_dir)
        sys.exit(1)

    for fbase in inputs:
        in_path = os.path.join(args.input_dir, fbase)
        u32_path = os.path.join(args.u32_dir, fbase)
        out_path = os.path.join(args.output_dir, fbase)

        with open(u32_path, 'wb') as u32_f:
            subprocess.run(['iconv', '-f', 'UTF-8', '-t', 'UTF-32LE', in_path], stdout=u32_f, check=True)

        cmd = [args.program] + extra_flags + [u32_path]
        if args.verbose:
            print('Running: %s > %s' % (' '.join(shlex.quote(c) for c in cmd), out_path))
        with open(out_path, 'wb') as out_f:
            subprocess.run(cmd, stdout=out_f)

    diff_cmd = ['diff', '-q', args.input_dir, args.output_dir]
    print(' '.join(diff_cmd))
    result = subprocess.run(diff_cmd)
    if result.returncode != 0 and args.verbose:
        subprocess.run(['diff', '-ru', args.input_dir, args.output_dir])
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
