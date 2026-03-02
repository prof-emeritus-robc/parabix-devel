#!/usr/bin/env python3
#
# tokenizertest.py - Simple tokenizer test runner
#
# Usage: python3 tokenizertest.py <path/to/tokenizer>
#
# This script:
# 1. Reads test case from tokenizertest.xml
# 2. Runs the tokenizer with specified pretokenizer
# 3. Counts output tokens and compares to expected count
#

import sys
import subprocess
import os
import shlex
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

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tokenizertest.py <path/to/tokenizer>")
        print("Example: python3 tokenizertest.py ./tokenizer")
        sys.exit(1)
    
    tokenizer_path = sys.argv[1]
    
    # Check tokenizer exists
    if not os.path.isfile(tokenizer_path):
        print(f"Error: Tokenizer not found: {tokenizer_path}")
        sys.exit(1)
    
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
    
    # Run all tests
    passed = 0
    failed = 0
    
    for test_num, test in enumerate(tests, 1):
        input_text = test['input']
        pretokenizer = test['pretokenizer']
        behavior = test['behavior']
        expectedcount = test['expected']
        
        # Print test header
        print("=" * 60)
        if behavior:
            print(f"Test {test_num}: {pretokenizer.capitalize()} Pretokenizer + {behavior.capitalize()} Behavior")
        else:
            print(f"Test {test_num}: {pretokenizer.capitalize()} Pretokenizer")
        print("=" * 60)
        print()
        
        # Create input file
        input_file = os.path.join(testdir, f"{test['id']}.txt")
        
        with open(input_file, 'w') as f:
            f.write(input_text)
        
        print(f"Input text:     {repr(input_text)}")
        print(f"Input file:     {input_file}")
        print(f"Pretokenizer:   {pretokenizer if pretokenizer else '(default)'}")
        if behavior:
            print(f"Behavior:       {behavior}")
        print()
        
        # Run tokenizer - build command with optional flags
        cmd = [tokenizer_path, input_file]
        
        # Only add --pretokenizer flag if it has a non-empty value
        if pretokenizer:
            cmd += ["--pretokenizer", pretokenizer]
        
        # Only add --behavior flag if it has a non-empty value
        if behavior:
            cmd += ["--behavior", behavior]
        
        print(f"Command: {' '.join(shlex.quote(part) for part in cmd)}")
        print()
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            output = result.stdout
            
            # Count tokens (each token is on its own line)
            if output:
                # Remove final newline if present, then split
                output_clean = output.rstrip('\n')
                tokens = output_clean.split('\n') if output_clean else []
            else:
                tokens = []
            
            token_count = len(tokens)
            
            print(f"Output tokens ({token_count}):")
            if tokens:
                for i, token in enumerate(tokens, 1):
                    print(f"  Token {i}: {repr(token)}")
            else:
                print("  (no tokens)")
            
            print()
            print("-" * 60)
            print(f"Expected token count:  {expectedcount}")
            print(f"Actual token count:    {token_count}")
            print("-" * 60)
            print()
            
            if token_count == int(expectedcount):
                print("✓ TEST PASSED")
                passed += 1
            else:
                print("✗ TEST FAILED")
                print()
                print("Debug info:")
                print(f"  Return code: {result.returncode}")
                if result.stderr:
                    print(f"  stderr: {result.stderr}")
                print(f"  Full output: {repr(output)}")
                failed += 1
                
        except subprocess.TimeoutExpired:
            print("✗ TEST FAILED: Timeout (tokenizer took too long)")
            failed += 1
        except Exception as e:
            print(f"✗ TEST FAILED: {e}")
            import traceback
            traceback.print_exc()
            failed += 1
        
        print()
    
    # Print summary
    print("=" * 60)
    print(f"SUMMARY: {passed} passed, {failed} failed out of {len(tests)} tests")
    print("=" * 60)
    
    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
