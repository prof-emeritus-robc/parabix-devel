#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: prefix runs that only delete (te~ \u2192 | t ; fu~ \u2192 | f ;), applied before
#  their group (see ldml::ComposeRevisits, prefixRunConflict) although
#    - a rule revisits a character of P (' ' { t \u2192 | d), its text a character
#      whose prefix runs are those of the character revisited (t and d, with e~
#      and o~): the run after it, deleted before, is the one ICU deletes;
#    - an after context sees a character of S not within a run (the u of
#      ~ } t s u, after s; fu~).
#  An after context seeing a P not as its last item (~ } tsu, with te~: on
#  a~te~su, ICU keeps ~) keeps them from being applied so: the rules are then
#  either not implementable (exit status 2) or as ICU applies them.  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_prefix_run_revisits_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

REVISIT = ("te \\~ \u2192 | t ;\n"
         "to \\~ \u2192 | t ;\n"
         "de \\~ \u2192 | d ;\n"
         "do \\~ \u2192 | d ;\n"
         "' ' { t \u2192 | d ;\n"
         "t \u2192 T ;\n"
         "d \u2192 D ;\n"
         "e \u2192 E ;\n"
         "o \u2192 O ;\n")
REVISIT_CASES = [
    (' te~a', ' Da'),
    (' te~e~a', ' Da'),
    ('te~a', 'Ta'),
    (' to~', ' D'),
    (' t', ' D'),
    ('a te~o', 'a DO'),
    (' de~a', ' Da'),
    (' ta', ' Da'),
    ('xte~', 'xT'),
    (' te~te~a', ' DTa'),
]

AFTER = ("\\~ } t s u \u2192 ;\n"
         "fu \\~ \u2192 | f ;\n"
         "t \u2192 T ;\n"
         "s \u2192 S ;\n"
         "u \u2192 U ;\n"
         "f \u2192 F ;\n")
AFTER_CASES = [
    ('a~tsu', 'aTSU'),
    ('a~tsu~', 'aTSU~'),
    ('fu~tsu', 'FTSU'),
    ('a~fu~', 'a~F'),
    ('~tsu', 'TSU'),
    ('a~tsfu~', 'a~TSF'),
    ('fu~u~', 'F'),
]

BLOCKED = ("\\~ } tsu \u2192 ;\n"
         "te \\~ \u2192 | t ;\n"
         "t \u2192 T ;\n"
         "s \u2192 S ;\n"
         "u \u2192 U ;\n"
         "e \u2192 E ;\n")
BLOCKED_CASES = [
    ('a~tsu', 'aTSU'),
    ('a~te~su', 'a~TSU'),
    ('te~e~', 'T'),
    ('~tsu', 'TSU'),
]



def run(tconv, rules_text, cases):
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(rules_text)
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in cases))
        return subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)


def compare(r, cases):
    got = r.stdout.decode("utf-8", "replace").split("\n")
    failures = 0
    for i, (line, want) in enumerate(cases):
        if i >= len(got) or got[i] != want:
            failures += 1
            print("FAIL %r:\n  want %r\n  got  %r" % (line, want, got[i] if i < len(got) else None))
    return failures


def main():
    tconv = sys.argv[1]
    failures = 0
    for rules, cases, implementable in ((REVISIT, REVISIT_CASES, True), (AFTER, AFTER_CASES, True), (BLOCKED, BLOCKED_CASES, False)):
        r = run(tconv, rules, cases)
        if r.returncode == 0:
            failures += compare(r, cases)
        elif implementable or r.returncode != 2:
            print("FAIL: exit status %d\n%s" % (r.returncode, r.stderr.decode("utf-8", "replace")))
            failures += 1
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
