#!/usr/bin/env python3
#
# Part of the Parabix Project, under the Open Software License 3.0.
# SPDX-License-Identifier: OSL-3.0
#
# Quick combined test of the normalization tools (nfd, nfc, nfkd), covering a
# few critical combinations of tool, input and pipeline mode:
#
#  - reference: a normalization form of the conformance text in Normalization,
#    checked against the reference file for the tool's form, in the default mode
#    and in one alternative pipeline mode per tool (-SeparatedPipelineStages=4,
#    -MultiStage, -NoFocus);
#  - regression: the small cases in nfd/TestFiles and nfc/TestFiles, concatenated
#    into one input per tool (each ends in a newline, so nothing composes across
#    them), checked against the concatenated ExpectedOutput files;
#  - focus: a generated text mixing sparse and dense work, run with a small
#    --segment-size so that segment boundaries fall inside work regions, checked
#    against Python's unicodedata.normalize.  The default focus mode runs for nfd
#    and nfc, and the alternative focus modes are spread over the three tools.
#    The text, from focus/focus_modes_test.py (which runs every focus mode), uses
#    only characters whose normalization is stable across Unicode versions.
#
# The fuller tests remain available to run by hand: `make nfd_test`, `make nfc_test`
# and focus/focus_modes_test.py.
#
# Usage: python3 normalization_test.py <bin directory>

import os
import subprocess
import sys
import tempfile
import unicodedata

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'focus'))
from focus_modes_test import text as focus_text

NORMALIZATION_DIR = 'Normalization'

REFERENCE_CASES = [
    ('nfd', [], 'NF-source', 'NFD'),
    ('nfd', ['-SeparatedPipelineStages=4'], 'NFKC', 'NFKD'),
    ('nfc', [], 'NF-source', 'NFC'),
    ('nfc', ['-MultiStage'], 'NFD', 'NFC'),
    ('nfkd', [], 'NF-source', 'NFKD'),
    ('nfkd', ['-NoFocus'], 'NFC', 'NFKD'),
]

# hangul_lines comes first, at the same offsets as when run on its own.
REGRESSION_CASES = [
    ('nfd', [('nfd/TestFiles', 'hangul_lines'), ('nfd/TestFiles', 'ccc_tests1'),
             ('nfd/TestFiles', 'singletons'), ('nfd/TestFiles', 'x212b_323'),
             (NORMALIZATION_DIR, 'lv_t')], 'nfd/ExpectedOutput'),
    ('nfc', [('nfc/TestFiles', 'self_composable_test'), ('nfc/TestFiles', 'x212b_323'),
             (NORMALIZATION_DIR, 'lv_t')], 'nfc/ExpectedOutput'),
]

FOCUS_COMMON = ['--thread-num=3', '--segment-size=1024']

FOCUS_CASES = [
    ('nfd', []),
    ('nfc', []),
    ('nfd', ['-focus-filter=bit', '-reintegrate=bit']),
    ('nfc', ['-focus-filter=byte', '-reintegrate=byte-replace', '-unmodified-filter=byte']),
    ('nfkd', ['-reintegrate=byte-spread', '-fill-run-gaps=3']),
    ('nfc', ['-focus-layers=filter,transform']),
]

FORM = {'nfd': 'NFD', 'nfc': 'NFC', 'nfkd': 'NFKD'}


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def write(path, data):
    with open(path, 'wb') as f:
        f.write(data)


def main():
    bin_dir = sys.argv[1]
    cases = []
    for tool, flags, source, form in REFERENCE_CASES:
        cases.append((tool, flags, os.path.join(NORMALIZATION_DIR, source),
                      read(os.path.join(NORMALIZATION_DIR, form)), source))
    with tempfile.TemporaryDirectory() as tmp:
        for tool, files, expected_dir in REGRESSION_CASES:
            path = os.path.join(tmp, tool + '_regression')
            write(path, b''.join(read(os.path.join(d, f)) for d, f in files))
            expected = b''.join(read(os.path.join(expected_dir, f + '.' + tool)) for _, f in files)
            cases.append((tool, [], path, expected, 'regression files'))
        text = focus_text()
        focus_path = os.path.join(tmp, 'focus_text')
        write(focus_path, text.encode('utf-8'))
        for tool, flags in FOCUS_CASES:
            expected = unicodedata.normalize(FORM[tool], text).encode('utf-8')
            cases.append((tool, FOCUS_COMMON + flags, focus_path, expected, 'focus text'))
        failures = []
        for tool, flags, path, expected, label in cases:
            result = subprocess.run([os.path.join(bin_dir, tool)] + flags + [path], stdout=subprocess.PIPE)
            if result.returncode != 0 or result.stdout != expected:
                failures.append('%s %s on %s' % (tool, ' '.join(flags) or '(default)', label))
    if failures:
        print('normalization_test failed:\n  ' + '\n  '.join(failures))
        return 1
    print('normalization_test passed (%d cases)' % len(cases))
    return 0


if __name__ == '__main__':
    sys.exit(main())
