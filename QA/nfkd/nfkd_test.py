#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Runs the nfkd (Unicode Normalization Form KD) executable over the NFC/NFD/
# NFKC/NFKD reference files in ../Normalization (and NF-source) and checks that
# each output is the NFKD reference file: NFKD of any normalization form of the
# source is the NFKD form.
#
# Usage: python3 nfkd_test.py [--flags='...'] <nfkd_executable>

import argparse
import os
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description='Run nfkd conformance tests.')
    parser.add_argument('program', help='path to the nfkd executable under test')
    parser.add_argument('-n', '--normalization_dir', default='../Normalization')
    parser.add_argument('--flags', default='', help='extra flags to pass to the executable under test')
    args = parser.parse_args()
    with open(os.path.join(args.normalization_dir, 'NFKD'), 'rb') as f:
        expected = f.read()
    failures = []
    for name in ['NF-source', 'NFC', 'NFD', 'NFKC', 'NFKD']:
        path = os.path.join(args.normalization_dir, name)
        result = subprocess.run([args.program] + shlex.split(args.flags) + [path], stdout=subprocess.PIPE)
        if result.returncode != 0 or result.stdout != expected:
            failures.append(name)
    if failures:
        print('nfkd_test failed on: ' + ' '.join(failures))
        return 1
    print('nfkd_test passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
