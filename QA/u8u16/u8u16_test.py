#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Runs the u8u16 executable over every file in TestFiles and diffs both its
# output and stderr messages against ExpectedOutput/{Files,Messages}.
#
# Usage: python3 u8u16_test.py [options] <u8u16_executable>
#
# Extra flags may be forwarded to the executable under test, e.g.:
#
#   python3 u8u16_test.py --flags='-thread-num=2 --use-mcjit' ../build/bin/u8u16
#

import argparse
import os
import shlex
import shutil
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(description='Run u8u16 conformance tests.')
    parser.add_argument('program', help='path to the u8u16 executable under test')
    parser.add_argument('-d', '--input_dir', default='TestFiles')
    parser.add_argument('-e', '--expected_dir', default='ExpectedOutput')
    parser.add_argument('-o', '--output_dir', default='TestOutput')
    parser.add_argument('--flags', default='',
                         help="extra flags to pass to the executable under test, e.g. "
                              "--flags='-thread-num=2'")
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
    os.makedirs(os.path.join(output_dir, 'Files'))
    os.makedirs(os.path.join(output_dir, 'Messages'))


def main():
    args = parse_args()
    extra_flags = shlex.split(args.flags)

    reset_output_dir(args.output_dir)

    inputs = sorted(os.listdir(args.input_dir))
    if not inputs:
        print('No input files found in %s' % args.input_dir)
        sys.exit(1)

    for fbase in inputs:
        in_path = os.path.join(args.input_dir, fbase)
        out_path = os.path.join(args.output_dir, 'Files', fbase)
        msg_path = os.path.join(args.output_dir, 'Messages', fbase)
        cmd = [args.program] + extra_flags + [in_path, out_path]
        if args.verbose:
            print('Running: %s' % ' '.join(shlex.quote(c) for c in cmd))
        with open(msg_path, 'wb') as msg_f:
            subprocess.run(cmd, stderr=msg_f)

    files_diff = subprocess.run(['diff', '-q', '--exclude=.svn',
                                  os.path.join(args.expected_dir, 'Files'),
                                  os.path.join(args.output_dir, 'Files')])
    if files_diff.returncode != 0:
        if args.verbose:
            subprocess.run(['diff', '-ru', '--exclude=.svn',
                             os.path.join(args.expected_dir, 'Files'),
                             os.path.join(args.output_dir, 'Files')])
        sys.exit(2)

    messages_diff = subprocess.run(['diff', '--exclude=.svn',
                                     os.path.join(args.expected_dir, 'Messages'),
                                     os.path.join(args.output_dir, 'Messages')])
    sys.exit(messages_diff.returncode)


if __name__ == '__main__':
    main()
