ab*c test file generation.

(1)  Generate the test files and expected results, and the list of quick
     cases (abc/QuickTests.txt):
python3 abc_testgen.py

     To rewrite only the list of quick cases for the existing test files:
python3 abc_testgen.py --quick-list-only

(2)  Perform the tests: the quick cases by default, or all of them with
     --extended-testing.
python3 abc_test.py ../build/bin/icgrep
python3 abc_test.py --extended-testing ../build/bin/icgrep
