#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Runs the nfc (Unicode Normalization Form C) executable over the NFC/NFD/
# NFKC/NFKD reference files in ../Normalization plus every file in TestFiles,
# and diffs the output against ExpectedOutput.
#
# The reference files' own expected output isn't checked in: any file whose
# name starts with "NF" is regenerated each run by copying the canonical NFC
# (or, for names starting "NFK", NFKC) reference content into ExpectedOutput
# before the diff -- normalizing any of NFC/NFD/NFKC/NFKD (or a few other
# "NF"-prefixed reference files, e.g. NF-source) to NFC must reproduce NFC's
# own content, and to NFKC for the "NFK"-prefixed ones. Files that don't start
# with "NF" (e.g. lv_t) keep their statically checked-in expected output.
#
# Usage: python3 nfc_test.py [options] <nfc_executable>
#
# Extra flags may be forwarded to the executable under test, e.g.:
#
#   python3 nfc_test.py --flags='--use-mcjit' ../build/bin/nfc
#

import argparse
import os
import shlex
import shutil
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(description='Run nfc (Unicode Normalization Form C) conformance tests.')
    parser.add_argument('program', help='path to the nfc executable under test')
    parser.add_argument('-n', '--normalization_dir', default='../Normalization',
                         help='directory containing the NFC/NFD/NFKC/NFKD reference files')
    parser.add_argument('-d', '--input_dir', default='TestFiles')
    parser.add_argument('-e', '--expected_dir', default='ExpectedOutput')
    parser.add_argument('-o', '--output_dir', default='TestOutput')
    parser.add_argument('--flags', default='',
                         help="extra flags to pass to the executable under test, e.g. "
                              "--flags='--use-mcjit'")
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


def regenerate_reference_expected_output(normalization_dir, expected_dir):
    nfkc_ref = os.path.join(normalization_dir, 'NFKC')
    nfc_ref = os.path.join(normalization_dir, 'NFC')
    for fname in sorted(os.listdir(normalization_dir)):
        if not fname.startswith('NF'):
            continue
        target = os.path.join(expected_dir, fname + '.nfc')
        source = nfkc_ref if fname.startswith('NFK') else nfc_ref
        shutil.copyfile(source, target)


def run_one(program, extra_flags, in_path, out_path, verbose):
    cmd = [program] + extra_flags + [in_path]
    if verbose:
        print('Running: %s > %s' % (' '.join(shlex.quote(c) for c in cmd), out_path))
    with open(out_path, 'wb') as out_f:
        subprocess.run(cmd, stdout=out_f)


def main():
    args = parse_args()
    extra_flags = shlex.split(args.flags)

    reset_output_dir(args.output_dir)
    regenerate_reference_expected_output(args.normalization_dir, args.expected_dir)

    for fname in sorted(os.listdir(args.normalization_dir)):
        in_path = os.path.join(args.normalization_dir, fname)
        out_path = os.path.join(args.output_dir, fname + '.nfc')
        run_one(args.program, extra_flags, in_path, out_path, args.verbose)

    for fname in sorted(os.listdir(args.input_dir)):
        in_path = os.path.join(args.input_dir, fname)
        out_path = os.path.join(args.output_dir, fname + '.nfc')
        run_one(args.program, extra_flags, in_path, out_path, args.verbose)

    diff_cmd = ['diff', '-q', args.expected_dir, args.output_dir]
    print(' '.join(diff_cmd))
    result = subprocess.run(diff_cmd)
    if result.returncode != 0 and args.verbose:
        subprocess.run(['diff', '-ru', args.expected_dir, args.output_dir])
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
