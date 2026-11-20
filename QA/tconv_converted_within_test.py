#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: a string rule whose before context is matched against the converted
#  text, beginning at a later position of the text of another string rule's
#  match.  ICU, having applied that match, applies no rule within it.  The
#  overlaps of the string rules (analyzeStringRuleOverlaps) leave such a pair out
#  where the rule's before context cannot match there in the text, but against
#  the converted text it may, the other rule having applied (x { bb → c ;
#  aab → ; on xaabb: aab deleted, x before bb).  Both applied, their changes
#  combined into U+D801 at the shared position.  The regular plan now applies
#  such a rule only outside the applied matches of the others
#  (correctedWithin), and DivCode leaves such a group to it.  Each case with
#  the default plan and with --divcode-groups=0.  The expected results are
#  ICU's (createFromRules, each line a text).
#
#  tconv_converted_within_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

# (rules, [(line, expected result)])
CASES = [
    ('x { bb → c ;\naab → ;', [
        ('xaabb', 'xb'),
        ('bxaabb', 'bxb'),
        ('xxaabbcx', 'xxbcx'),
        ('xcxaabbbcx', 'xcxccx'),
        ('accxaabbbb', 'accxcb'),
        ('xbb', 'xc'),
        ('aabb', 'b'),
    ]),
    ('b { bc → ab ;\naab → ;', [
        ('abbaabc', 'abbc'),
        ('abaabcxb', 'abcxb'),
        ('bbc', 'bab'),
        ('baabc', 'bc'),
        ('aabc', 'c'),
    ]),
    ('x { bb → c ;\ncb → ;\nx { baa → x ;', [
        ('xcbb', 'xb'),
        ('xxcbb', 'xxb'),
        ('xbbcbb', 'xcb'),
        ('xcbbaa', 'xx'),
        ('cbb', 'b'),
    ]),
    ('cab → x ;\ncba → ;\nx { ba → ab ;', [
        ('caba', 'xa'),
        ('cabaa', 'xaa'),
        ('xba', 'xab'),
        ('cbaba', 'ba'),
        ('xcaba', 'xxa'),
    ]),
]

CONFIGS = [[], ["--divcode-groups=0"]]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n, (rules, lines) in enumerate(CASES):
            rules_path = os.path.join(tmp, "rules%d.txt" % n)
            with open(rules_path, "w", encoding="utf-8") as f:
                f.write(rules + "\n")
            input_path = os.path.join(tmp, "input%d.txt" % n)
            with open(input_path, "w", encoding="utf-8") as f:
                f.write("".join(line + "\n" for line, _ in lines))
            for config in CONFIGS:
                args = [tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules_path] + config + [input_path]
                r = subprocess.run(args, capture_output=True)
                what = "rules %d %s" % (n, " ".join(config) or "default")
                if r.returncode != 0:
                    failures += 1
                    print("FAIL %s: exit %d: %s" % (what, r.returncode, r.stderr.decode("utf-8", "replace")[:300]))
                    continue
                got = r.stdout.decode("utf-8", "replace").split("\n")
                for i, (line, want) in enumerate(lines):
                    have = got[i] if i < len(got) else None
                    if have != want:
                        failures += 1
                        print("FAIL %s: %r gives %r, expected %r" % (what, line, have, want))
    print("tconv_converted_within_test: %s" % ("%d failures" % failures if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
