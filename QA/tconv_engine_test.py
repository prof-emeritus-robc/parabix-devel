#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: conversion groups of a single rule applied by the search and replace
#  of the regular expression engine (see engineSearchReplace in tconv.cpp).
#  Each rules file must be planned for the engine (tconv --plan), and its
#  result must be that of Python's re.sub on the whole text (as ICU applies
#  these rules: their matches cannot overlap, and their repetitions match as
#  ICU's possessive ones do), and that of tconv's own kernels
#  (--disable-engine-search-replace).
#
#  tconv_engine_test.py <tconv binary>

import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

TEXT = "abx cd abc. abcd #ab-1; #k-23; #-4; cdcd\nx#a-9;\n"

# (LDML rule, Python pattern, Python replacement template)
CASES = [
    (r"([a-z]) } [^x] → $1 q ;", r"([a-z])(?!x)", r"\1q"),                 # a capture, adjacent matches
    (r"cd → ;", r"cd", r""),                                                 # a string rule, deleted
    (r"a { bc } [^a-z] → XY ;", r"(?<=a)bc(?![a-z])", r"XY"),               # a string rule with contexts
    (r"\# ([a-z]+) \- ([0-9]) \; → $1 \= $2 ;", r"#([a-z]+)-([0-9]);", r"\1=\2"),   # two captures
]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        text = os.path.join(tmp, "input.txt")
        with open(text, "w", encoding="utf-8") as f:
            f.write(TEXT)
        argvs = []
        for i, (rule, _, _) in enumerate(CASES):
            rules = os.path.join(tmp, "rules%d.txt" % i)
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rule + "\n")
            argvs.append([tconv, "--plan", "--rules-file=" + rules])
            argvs.append([tconv, "--rules-file=" + rules, text])
            argvs.append([tconv, "--disable-engine-search-replace", "--rules-file=" + rules, text])
        # The runs of tconv, three at a time.
        with ThreadPoolExecutor(max_workers=3) as pool:
            runs = list(pool.map(lambda argv: subprocess.run(argv, capture_output=True), argvs))
        for i, (rule, pattern, replacement) in enumerate(CASES):
            plan, engine, kernels = runs[3 * i: 3 * i + 3]
            want = re.sub(pattern, replacement, TEXT)
            if b"applied by the search and replace of the regular expression engine" not in plan.stdout:
                failures += 1
                print("FAIL %s: not planned for the engine\n%s" % (rule, plan.stdout.decode("utf-8", "replace")))
            for label, run in (("engine", engine), ("kernels", kernels)):
                got = run.stdout.decode("utf-8", "replace")
                if run.returncode != 0 or got != want:
                    failures += 1
                    print("FAIL %s (%s): exit %d\n  got:      %r\n  expected: %r\n%s"
                          % (rule, label, run.returncode, got, want, run.stderr.decode()[:300]))
    if failures:
        sys.exit(1)
    print("tconv_engine_test: all tests passed")


if __name__ == "__main__":
    main()
