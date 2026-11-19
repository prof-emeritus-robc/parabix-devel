#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: string rules whose before contexts are matched against the converted
#  text, resolved in the regular plan's correction kernel by the While loop
#  (doc/round-replacement.md step 3, --converted-string-while) instead of
#  rounds, with the rules whose selection depends on them.  The loop decides
#  such a rule at the first position of each match, from its before context and
#  the matches that may cover that position; the positions of its text then
#  have the outcomes of its result, or their own where it does not apply.
#  Cases: a rule whose result is its own before context, two alternating; a
#  deletion seen through and an expansion
#  seen at distance 2; a negated set at the start of the text; a repeated set
#  (X T*) in the before context; single-character rules within the text of the
#  string rule (coverage), and subgroup chains feeding it and fed by it; a
#  converted rule within the text of a string rule that precedes it; two of
#  different lengths; results shorter and longer than the text.  Then rules
#  whose selection depends on the rules decided in the loop (step 3(b)): a
#  converted rule containing another, one overlapping itself, a chain of three;
#  rules shaped as es-es_FONIPA's, zu-zu_FONIPA's and ja_Latn-ru's; characters
#  kept and deleted, as blt-blt_FONIPA's rules have them; and a match beginning
#  at the last position of an applied match, which the overlap order does not
#  have.  Most chains are longer than the default 8 rounds, which miss them
#  (--converted-string-while=0).  Then chains over 1,000 characters, across
#  blocks, also with one thread and with four threads and segments of one
#  stride (all with --extended-testing; otherwise the first only).  The
#  expected results are ICU's (createFromRules, each line a text).
#
#  tconv_converted_strings_while_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

# (rules, [(line, expected result)])
CASES = [
    ('a { bc → a ;', [
        ('abcbcbcbcbcbcbcbcbcbcbcbc', 'aaaaaaaaaaaaa'),
        ('xbcbc abcbc', 'xbcbc aaa'),
        ('bcabc', 'bcaa'),
        ('abcxbc', 'aaxbc'),
    ]),
    ('a { bc → d ;\nd { bc → a ;', [
        ('abcbcbcbcbcbcbcbcbcbcbcbcbc', 'adadadadadadad'),
        ('dbcbc', 'dad'),
        ('abcbcxbcbc', 'adaxbcbc'),
    ]),
    ('ab → ;\nx { cd → y ;\ny { cd → x ;', [
        ('xabcd', 'xy'),
        ('xababcdabcdabababcdcdcdcdcdcdcdcdcd', 'xyxyxyxyxyxy'),
        ('abcd', 'cd'),
        ('xcdabcdcd', 'xyxy'),
    ]),
    ('q → pq ;\np q { rs → t ;\nq t { rs → q ;', [
        ('qrsrsrsrsrsrsrsrsrsrsrs', 'pqtqrsrsrsrsrsrsrsrsrs'),
        ('rsqrs', 'rspqt'),
        ('pqrs', 'ppqt'),
    ]),
    ('[^a] { bc → a ;\na { bc → b ;', [
        ('bc', 'a'),
        ('bcbcbcbcbcbcbcbcbcbcbc', 'abababababa'),
        ('abcbc', 'aba'),
        ('xbcbcbc', 'xaba'),
    ]),
    ('x m* { ab → m ;\nab → x ;', [
        ('xabababababababababababab', 'xmmmmmmmmmmmm'),
        ('xmmab', 'xmmm'),
        ('ababab', 'xmm'),
        ('xmyab', 'xmyx'),
    ]),
    ('x { ab → y ;\na → x ;\nb → c ;', [
        ('aab', 'xy'),
        ('aaaaaaaaaaaaab', 'xxxxxxxxxxxxy'),
        ('ab', 'xc'),
        ('aabab', 'xyxc'),
        ('babab', 'cxcxc'),
        ('xabab', 'xyxc'),
    ]),
    ('a { b → a ;\na { cd → e ;\ne { b → a ;', [
        ('abbbbbbbbbbbcdbbbbbbbbbbbbbbbcd', 'aaaaaaaaaaaaeaaaaaaaaaaaaaaae'),
        ('ecdbb', 'ecdbb'),
        ('acdcd', 'aecd'),
    ]),
    ('abc → q ;\nx { bc → x ;', [
        ('xbcbcbcbcbcbcbcbcbcbcbc', 'xxxxxxxxxxxx'),
        ('abcbc', 'qbc'),
        ('xabcbc', 'xqbc'),
        ('xbcabc', 'xxq'),
    ]),
    ('u { vw → u ;\nu { xyz → uu ;', [
        ('uvwxyzvwxyzvwvwvwvwvwvwvwvwvwxyz', 'uuuuuuuuuuuuuuuuuu'),
        ('vwuxyz', 'vwuuu'),
        ('uxyzvw', 'uuuu'),
    ]),
    ('k { lmn → k ;\nk { o → kk ;', [
        ('klmnolmnolmnolmnolmnolmnolmno', 'kkkkkkkkkkkkkkkkkkkkkk'),
        ('olmn', 'olmn'),
        ('kolmn', 'kkkk'),
    ]),
    # Selection depending on the rules decided in the loop (step 3(b)).
    ('x { ab → x ;\nbc → z ;', [
        ('xabcabcabcabcabcabcabcabcabcabc', 'xxcazazazazazazazazaz'),
        ('abc', 'az'),
        ('xababababababababababababc', 'xxxxxxxxxxxxxc'),
        ('xabxbc', 'xxxz'),
    ]),
    ('x { ab → x ;\nbb → c ;', [
        ('xabbbbb', 'xxcc'),
        ('abbbb', 'acc'),
        ('xabababababababababababbb', 'xxxxxxxxxxxxc'),
        ('xabbabbbabbbb', 'xxbacbacc'),
        ('bbbbbbb', 'cccb'),
    ]),
    ('x { abc → x ;\ncd → y ;\nde → z ;', [
        ('xabcdeabcde', 'xxzabye'),
        ('abcde', 'abye'),
        ('xabcabcabcabcabcabcabcabcabcabcde', 'xxxxxxxxxxxz'),
        ('xcde', 'xye'),
    ]),
    ('[^s] { xs → s ;\nss → q ;', [
        ('axsxsxsxsxsxsxsxsxsxss', 'asxsxsxsxsxsxsxsxsxq'),
        ('xss', 'ss'),
        ('sxss', 'sxq'),
        ('axssss', 'asqs'),
        ('ssss', 'qq'),
    ]),
    ('n { sh → t ;\n[^n] { sh → n ;\nhh → H ;\nhl → L ;', [
        ('nshh', 'nth'),
        ('ashl', 'anl'),
        ('shhh', 'nH'),
        ('ashshshshshshshshshshshshhl', 'antntntntntntL'),
        ('nshl', 'ntl'),
        ('hhhh', 'HH'),
    ]),
    ('q { uve → q ;\nex → E ;', [
        ('quvequvequvequvequvequvequvequvequvequvex', 'qqqqqqqqqqqqqqqqqqqqx'),
        ('uvex', 'uvE'),
        ('quvex', 'qqx'),
        ('quvuvex', 'quvuvE'),
    ]),
    ('x { (ab) → $1 x ;', [
        ('xababababababababababababab', 'xabxabxabxabxabxabxabxabxabxabxabxabxabx'),
        ('abab', 'abab'),
        ('xabcab', 'xabxcab'),
    ]),
    ('[xb] { (aa) → $1 b ;', [
        ('xaaaaaaaaaaaaaaaaaaaaaaaa', 'xaabaabaabaabaabaabaabaabaabaabaabaab'),
        ('aaaa', 'aaaa'),
        ('xaaxaaa', 'xaabxaaba'),
        ('baaa', 'baaba'),
    ]),
    ('q { (ab) → $1 q ;\n[bq] { (bb) → $1 r ;', [
        ('qabbbbbbbbbbbbbbbbbbbbb', 'qabqbbrbbbrbbbrbbbrbbbrbbbrbbbr'),
        ('abbb', 'abbbr'),
        ('qababababababababababababbb', 'qabqabqabqabqabqabqabqabqabqabqabqabqbbr'),
        ('bbbb', 'bbbrb'),
    ]),
    ('q { z (ab) → $1 q ;\n[bq] { (bb) → $1 r ;', [
        ('qzabzabzabzabzabzabzabzabzabzabzab', 'qabqabqabqabqabqabqabqabqabqabqabq'),
        ('zab', 'zab'),
        ('qzabbbb', 'qabqbbrb'),
        ('qzazab', 'qzazab'),
    ]),
]

LONG_CASES = [
    ("a { bc → a ;", [
        ("a" + "bc" * 700, "a" * 701),
        ("x" + "bc" * 300 + "a" + "bc" * 400, "x" + "bc" * 300 + "a" * 401),
    ]),
    ("x { ab → y ;\na → x ;\nb → c ;", [
        ("a" * 1500 + "b", "x" * 1499 + "y"),
        ("a" * 777 + "bab" + "a" * 600 + "b", "x" * 776 + "yxc" + "x" * 599 + "y"),
    ]),
    ("k { lmn → k ;\nk { o → kk ;", [
        ("k" + "lmno" * 400, "k" * 1201),
    ]),
    ("abc → q ;\nx { bc → x ;", [
        ("x" + "bc" * 500 + "abc" + "bc" * 200, "x" * 501 + "q" + "bc" * 200),
    ]),
]

CONFIGS = [[], ["-thread-num=1"], ["-thread-num=4", "-segment-size=1"]]


def run(tconv, tmp, name, rules, lines, config):
    rules_path = os.path.join(tmp, name + ".rules")
    with open(rules_path, "w", encoding="utf-8") as f:
        f.write(rules + "\n")
    input_path = os.path.join(tmp, name + ".txt")
    with open(input_path, "w", encoding="utf-8") as f:
        f.write("".join(line + "\n" for line, _ in lines))
    failures = 0
    plan = subprocess.run([tconv, "--plan", "--rules-file=" + rules_path, input_path], capture_output=True)
    text = plan.stdout.decode("utf-8", "replace")
    if ": implementable" not in text.split("\n")[0] or "warning" in text:
        failures += 1
        print("FAIL %s: the rules are not implementable without warnings\n%s" % (name, text))
    args = [tconv, "--use-ICU=0", "--apply-shape-groups=0", "--divcode-groups=0", "--utf8-mode=0",
            "--lines", "--rules-file=" + rules_path] + config + [input_path]
    r = subprocess.run(args, capture_output=True)
    what = "%s %s" % (name, " ".join(config) or "default")
    if r.returncode != 0:
        print("FAIL %s: exit %d: %s" % (what, r.returncode, r.stderr.decode("utf-8", "replace")[:300]))
        return failures + 1
    got = r.stdout.decode("utf-8").split("\n")
    for i, (line, want) in enumerate(lines):
        have = got[i] if i < len(got) else None
        if have != want:
            failures += 1
            if len(line) > 80:
                print("FAIL %s: line %d (%d characters) differs" % (what, i, len(line)))
            else:
                print("FAIL %s: %r gives %r, expected %r" % (what, line, have, want))
    return failures


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n, (rules, lines) in enumerate(CASES):
            failures += run(tconv, tmp, "rules%d" % n, rules, lines, [])
        for n, (rules, lines) in enumerate(LONG_CASES):
            for config in (CONFIGS if EXTENDED else CONFIGS[:1]):
                failures += run(tconv, tmp, "long%d" % n, rules, lines, config)
    print("tconv_converted_strings_while_test: %s" % ("%d failures" % failures if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
