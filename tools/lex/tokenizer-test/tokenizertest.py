#!/usr/bin/env python3
#
# tokenizertest.py - Simple tokenizer test runner
#
# Usage: python3 tokenizertest.py <path/to/tokenizer> [input_file]
#
# This script:
# 1. Reads test cases from tokenizertest.xml
# 2. Runs the tokenizer with specified pretokenizer
# 3. Counts output tokens and compares to expected count
#

import sys
import subprocess
import os
import shlex
import argparse
import xml.etree.ElementTree as ET  # For XML parsing

VALID_PRETOKENIZERS = {
    "uax29",
    "whitespace",
    "whitespacesplit",
    "digits",
    "punctuation",
    "simplewordboundaries",
    "bytelevel",
    "chardelimiter",
    "bert",
    "sequence_whitespace_punctuation",
}

VALID_BEHAVIORS = {
    "removed",
    "isolated",
    "mergedwithprevious",
    "mergedwithnext",
    "contiguous",
}

def normalize_pretokenizer(name):
    return (name or "").strip()

def read_all_tests_from_xml(xml_file):
    """Read all test cases from XML file"""
    tree = ET.parse(xml_file)
    root = tree.getroot()
    
    # Create a mapping of inputdata ids to text
    inputdata_map = {}
    for inputdata in root.findall('inputdata'):
        data_id = inputdata.get('id')
        text = inputdata.text if inputdata.text else ""
        inputdata_map[data_id] = text.strip()
    
    # Get all test cases
    tests = []
    for testcase in root.findall('tokenizercase'):
        inputdata_id = testcase.get('inputdata')
        input_text = inputdata_map.get(inputdata_id, "")
        pretokenizer = normalize_pretokenizer(testcase.get('pretokenizer', ""))
        behavior = (testcase.get('behavior', "") or "").strip()  # Optional behavior attribute
        expectedcount = testcase.get('expectedcount', "2")

        if pretokenizer and pretokenizer not in VALID_PRETOKENIZERS:
            raise ValueError(
                f"Invalid pretokenizer '{pretokenizer}' in testcase '{inputdata_id}'. "
                f"Valid values: {sorted(VALID_PRETOKENIZERS)}"
            )

        if behavior and behavior not in VALID_BEHAVIORS:
            raise ValueError(
                f"Invalid behavior '{behavior}' in testcase '{inputdata_id}'. "
                f"Valid values: {sorted(VALID_BEHAVIORS)}"
            )
        
        tests.append({
            'input': input_text,
            'pretokenizer': pretokenizer,
            'behavior': behavior,
            'expected': expectedcount,
            'id': inputdata_id
        })
    
    return tests

DEFAULT_INPUT    = "/Users/munizahashim/parabix-devel/build19/test.txt"
DEFAULT_TOKENIZER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tokenizer")
OUTPUT_FILE      = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tokenizertest_output.txt")

class Tee:
    """Writes to both terminal and a file simultaneously."""
    def __init__(self, file): self.file = file
    def write(self, msg): sys.stdout.write(msg); self.file.write(msg)
    def flush(self): sys.stdout.flush(); self.file.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("tokenizer",   nargs="?", default=DEFAULT_TOKENIZER, help="Path to tokenizer binary")
    parser.add_argument("input_file",  nargs="?", default=DEFAULT_INPUT,     help="Path to input text file")
    args = parser.parse_args()

    tokenizer_path = args.tokenizer
    input_file_path = args.input_file

    # Check tokenizer exists
    if not os.path.isfile(tokenizer_path):
        print(f"Error: Tokenizer not found: {tokenizer_path}")
        sys.exit(1)

    # Read input text from file
    with open(input_file_path, "r", encoding="utf-8") as f:
        input_text_from_file = f.read().strip()

    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Create test directory
    testdir = os.path.join(script_dir, "testfiles")
    if not os.path.exists(testdir):
        os.makedirs(testdir)

    # Read all tests from XML
    xml_file = os.path.join(script_dir, "tokenizertest.xml")
    if not os.path.isfile(xml_file):
        print(f"Error: Test file not found: {xml_file}")
        return 1

    tests = read_all_tests_from_xml(xml_file)

    if not tests:
        print("Error: No test cases found in XML file")
        return 1

    # Run all tests, writing to terminal and file simultaneously
    passed = 0
    failed = 0

    out_f = open(OUTPUT_FILE, "w", encoding="utf-8")
    out = Tee(out_f)

    out.write(f"Input: {repr(input_text_from_file)}\n\n")

    for test_num, test in enumerate(tests, 1):
        input_text = input_text_from_file
        pretokenizer = test['pretokenizer']
        behavior = test['behavior']
        expectedcount = test['expected']
        
        # Print test header
        out.write("=" * 60 + "\n")
        if behavior:
            out.write(f"Test {test_num}: {pretokenizer.capitalize()} Pretokenizer + {behavior.capitalize()} Behavior\n")
        else:
            out.write(f"Test {test_num}: {pretokenizer.capitalize()} Pretokenizer\n")
        out.write("=" * 60 + "\n\n")

        # Create input file
        input_file = os.path.join(testdir, f"{test['id']}.txt")
        with open(input_file, 'w') as f:
            f.write(input_text)

        out.write(f"Input text:     {repr(input_text)}\n")
        out.write(f"Pretokenizer:   {pretokenizer if pretokenizer else '(default)'}\n")
        if behavior:
            out.write(f"Behavior:       {behavior}\n")
        out.write("\n")

        # Run tokenizer
        cmd = [tokenizer_path, input_file]
        if pretokenizer:
            cmd += ["--pretokenizer", pretokenizer]
        if behavior:
            cmd += ["--behavior", behavior]

        out.write(f"Command: {' '.join(shlex.quote(part) for part in cmd)}\n\n")

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            output = result.stdout

            output_clean = output.rstrip('\n')
            tokens = output_clean.split('\n') if output_clean else []
            token_count = len(tokens)

            out.write(f"Output tokens ({token_count}):\n")
            for i, token in enumerate(tokens, 1):
                out.write(f"  {i}: {repr(token)}\n")
            if not tokens:
                out.write("  (no tokens)\n")

            out.write("\n" + "-" * 60 + "\n")
            out.write(f"Expected token count:  {expectedcount}\n")
            out.write(f"Actual token count:    {token_count}\n")
            out.write("-" * 60 + "\n\n")

            if token_count == int(expectedcount):
                out.write("✓ TEST PASSED\n")
                passed += 1
            else:
                out.write("✗ TEST FAILED\n\n")
                out.write(f"  Return code: {result.returncode}\n")
                if result.stderr:
                    out.write(f"  stderr: {result.stderr}\n")
                out.write(f"  Full output: {repr(output)}\n")
                failed += 1

        except subprocess.TimeoutExpired:
            out.write("✗ TEST FAILED: Timeout\n")
            failed += 1
        except Exception as e:
            out.write(f"✗ TEST FAILED: {e}\n")
            failed += 1

        out.write("\n")

    out.write("=" * 60 + "\n")
    out.write(f"SUMMARY: {passed} passed, {failed} failed out of {len(tests)} tests\n")
    out.write("=" * 60 + "\n")

    out_f.close()
    print(f"Output written to: {OUTPUT_FILE}")

    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
