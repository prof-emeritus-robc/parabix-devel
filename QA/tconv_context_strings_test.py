#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: sets with strings in contexts take their longest strings, as ICU
#  matches them (see ldml::possessiveContext): outward from the text, forward in
#  an after context and backward (from the right) in a before context, and the
#  repetitions of a before context possessively.  a $e* { c does not match abc
#  ($e* takes ab, as no a precedes it), nor abbc or ababc; g [{gh} h] { i does
#  not match ghi; p } [{pq} p] q does not match pq.  The rules are matched by
#  the fused kernels (the single-character rules) and by the engine (the string
#  rule, and the repetition of $e).  The expected results are ICU's
#  (createFromRules, each line a text).
#
#  tconv_context_strings_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RULES = ("$e = [{ab} b] ;\n"
         "a $e* { c → X ;\n"
         "g [{gh} h] { i → Y ;\n"
         "k [{kl} l] { m → V ;\n"
         "p } [{pq} p] q → Z ;\n"
         "d [{def} {ef} f] { s → S ;\n"
         "n [{no} o]* { rr → R ;\n")

# (line, expected result)
CASES = [
    ('abc', 'abc'),
    ('aabc', 'aabX'),
    ('abbc', 'abbc'),
    ('ababc', 'ababc'),
    ('ac', 'aX'),
    ('aababc', 'aababX'),
    ('ghi', 'ghi'),
    ('gghi', 'gghY'),
    ('klm', 'klm'),
    ('kklm', 'kklV'),
    ('pq', 'pq'),
    ('ppqq', 'Zpqq'),
    ('ppq', 'ppq'),
    ('defs', 'defs'),
    ('ddefs', 'ddefS'),
    ('dfs', 'dfS'),
    ('efs', 'efs'),
    ('nrr', 'nR'),
    ('nnorr', 'nnoR'),
    ('noorr', 'noorr'),
    ('nonorr', 'nonorr'),
]


def main():
    tconv = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)
    if run.returncode != 0:
        print("FAIL: exit status %d\n%s" % (run.returncode, run.stderr.decode("utf-8", "replace")))
        return 1
    got = run.stdout.decode("utf-8", "replace").split("\n")
    failures = 0
    for i, (line, want) in enumerate(CASES):
        if i >= len(got) or got[i] != want:
            failures += 1
            print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
