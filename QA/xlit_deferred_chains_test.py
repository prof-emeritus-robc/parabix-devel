#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: chains of blocked triggers in DeferredPlan.  A trigger is not ready
#  while its before context may reach back to a trigger not yet replaced: one
#  that matches a deferred rule, or is not ready itself and may match once it
#  is.  In b s s ... s y with ([bc]) { s → d $1 ; and s { s } y → z ;, only
#  the first s matches the copy rule at first, and the last s matches the
#  second rule in the text as it is; in ICU it sees the converted d b before
#  it and copies b instead.  The last s is blocked only through the chain of
#  the s before it, each of which matches nothing until the one before it is
#  replaced.  The copy rule writes d before its copy, so it does not reproduce
#  its context and DeferredRuns does not take the chain.  (DeferredPlan
#  followed such chains through 9 triggers until the While loop of 5a372fd94,
#  and applied z at the 11th s and later: the cases with 11 or more.)  The
#  second set has the triggers apart, through the repetition a* of the
#  capture, which the before contexts reach through.  Each round replaces one
#  trigger of a chain, so the runs have --deferred-rule-rounds=16, more than
#  the longest chain.  The expected results are ICU's (createFromRules, each
#  line a text).
#
#  xlit_deferred_chains_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

CHAINS = "([bc]) { s → d $1;\ns { s } y → z;\n"

CHAINS_CASES = [
    ("bsy", "bdby"),
    ("bssy", "bdbdby"),
    ("bsssssssssy", "bdbdbdbdbdbdbdbdbdby"),
    ("bssssssssssy", "bdbdbdbdbdbdbdbdbdbdby"),
    ("bsssssssssssy", "bdbdbdbdbdbdbdbdbdbdbdby"),
    ("bssssssssssssy", "bdbdbdbdbdbdbdbdbdbdbdbdby"),
    ("bsssssssssssssssy", "bdbdbdbdbdbdbdbdbdbdbdbdbdbdbdby"),
    ("csssssssssssy bsssssssssssssy", "cdcdcdcdcdcdcdcdcdcdcdcy bdbdbdbdbdbdbdbdbdbdbdbdbdby"),
    ("bsssyssssssssssy", "bdbdbdbyssssssssszy"),
    ("ssssssssssssy", "ssssssssssszy"),
]

#  (s } y → z: the text s with the after context y; the planner gives it the
#  before context [^a-c] a*, where the copy rule does not match.)
REACH_CHAINS = "([bc] a*) { s → d $1;\ns } y → z;\n"

REACH_CHAINS_CASES = [
    ("basy", "badbay"),
    ("caasy", "caadcaay"),
    ("basasasy", "badbaadbaaadbaaay"),
    ("cssaasy", "cdcdcaadcaay"),
    ("basasasasasasasasasasy", "badbaadbaaadbaaaadbaaaaadbaaaaaadbaaaaaaadbaaaaaaaadbaaaaaaaaadbaaaaaaaaaadbaaaaaaaaaay"),
    ("csssssssssaasy", "cdcdcdcdcdcdcdcdcdcaadcaay"),
    ("basasasasasasasasasasasy", "badbaadbaaadbaaaadbaaaaadbaaaaaadbaaaaaaadbaaaaaaaadbaaaaaaaaadbaaaaaaaaaadbaaaaaaaaaaadbaaaaaaaaaaay"),
    ("cssssssssssaasy", "cdcdcdcdcdcdcdcdcdcdcaadcaay"),
    ("basasasasasasasasasasasasy", "badbaadbaaadbaaaadbaaaaadbaaaaaadbaaaaaaadbaaaaaaaadbaaaaaaaaadbaaaaaaaaaadbaaaaaaaaaaadbaaaaaaaaaaaadbaaaaaaaaaaaay"),
    ("csssssssssssaasy", "cdcdcdcdcdcdcdcdcdcdcdcaadcaay"),
]


def check(xlit, tmp, name, rules_text, cases):
    failures = 0
    rules = os.path.join(tmp, name + ".rules")
    with open(rules, "w", encoding="utf-8") as f:
        f.write(rules_text)
    input = os.path.join(tmp, name + ".txt")
    with open(input, "w", encoding="utf-8") as f:
        f.write("".join(line + "\n" for line, _ in cases))
    plan = subprocess.run([xlit, "--plan", "--rules-file=" + rules, input], capture_output=True)
    text = plan.stdout.decode("utf-8", "replace")
    if ": implementable" not in text.split("\n")[0] or "warning" in text or "deferred rules (2" not in text:
        failures += 1
        print("FAIL %s: the rules are not implementable as deferred rules without warnings\n%s" % (name, text))
    run = subprocess.run([xlit, "--use-ICU=0", "--lines", "--deferred-rule-rounds=16", "--rules-file=" + rules, input],
                         capture_output=True)
    if run.returncode != 0:
        failures += 1
        print("FAIL %s: exit status %d\n%s" % (name, run.returncode, run.stderr.decode("utf-8", "replace")))
        return failures
    got = run.stdout.decode("utf-8", "replace").split("\n")
    for i, (line, want) in enumerate(cases):
        if i >= len(got) or got[i] != want:
            failures += 1
            print("FAIL %s %r:\n  want %r\n  got  %r" % (name, line, want, got[i] if i < len(got) else None))
    return failures


def main():
    xlit = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        failures = check(xlit, tmp, "chains", CHAINS, CHAINS_CASES)
        failures += check(xlit, tmp, "reach_chains", REACH_CHAINS, REACH_CHAINS_CASES)
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
