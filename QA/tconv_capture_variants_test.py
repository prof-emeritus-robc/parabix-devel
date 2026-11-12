#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: MultipleCaptureMultipleReplacement rules merged as the variants of
#  one set (ldml::MultipleCaptureMultipleReplacementSet::variants): rules of a
#  conversion group differing only in the character at one position of a text
#  (the selector) and in their results, as a function call on a capture of
#  single characters expands to, matched once, with the replacements of each
#  match chosen by its selector.  NumericPinyin-Latin's rules written forward
#  (the tone digit at the end of a match selecting the accent inserted after
#  the first capture: carried from the end to the start), and rules whose
#  selector is at a fixed offset from the start.  The expected results are
#  ICU's (createFromRules, each line a text).
#
#  tconv_capture_variants_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

TONES = "$vowel = [aAeEiIoOuUvV];\n$consonant = [[a-z A-Z] - [$vowel]];\n([aAeE]) ($vowel* $consonant*) 1 \u2192 $1 \u0304 $2 ;\n([aAeE]) ($vowel* $consonant*) 2 \u2192 $1 \u0301 $2 ;\n([aAeE]) ($vowel* $consonant*) 3 \u2192 $1 \u030c $2 ;\n([aAeE]) ($vowel* $consonant*) 4 \u2192 $1 \u0300 $2 ;\n([aAeE]) ($vowel* $consonant*) 5 \u2192 $1 $2 ;\n($vowel) ($consonant*) 1 \u2192 $1 \u0304 $2 ;\n($vowel) ($consonant*) 2 \u2192 $1 \u0301 $2 ;\n($vowel) ($consonant*) 3 \u2192 $1 \u030c $2 ;\n($vowel) ($consonant*) 4 \u2192 $1 \u0300 $2 ;\n($vowel) ($consonant*) 5 \u2192 $1 $2 ;\n[:Letter:] { 1 \u2192 \u0304 ;\n[:Letter:] { 2 \u2192 \u0301 ;\n[:Letter:] { 3 \u2192 \u030c ;\n[:Letter:] { 4 \u2192 \u0300 ;\n[:Letter:] { 5 \u2192 ;\n"

TONE_CASES = [
    ("ma1", "ma\u0304"),
    ("zhong1guo2", "zho\u0304ngguo\u0301"),
    ("an53", "an\u030c"),
    ("an43", "a\u0300n\u030c"),
    ("a53", "a\u030c"),
    ("a43", "a\u03003"),
    ("xian4", "xia\u0300n"),
    ("ou3", "ou\u030c"),
    ("Er2", "E\u0301r"),
    ("shuang5", "shuang"),
    ("iu1", "iu\u0304"),
    ("ng2", "ng\u0301"),
    ("hm5", "hm"),
    ("b3", "b\u030c"),
    ("n53", "n\u030c"),
    ("ai4ai3", "a\u0300ia\u030ci"),
    ("zhuang4 ba1", "zhua\u0300ng ba\u0304"),
    ("iu2ong4ma1 ou", "iu\u0301o\u0300ngma\u0304 ou"),
    ("r53yao3 ma2zhong2ai1 ", "r\u030cya\u030co ma\u0301zho\u0301nga\u0304i "),
    ("a4", "a\u0300"),
    ("ou4", "ou\u0300"),
    ("maouera1er", "maouera\u0304er"),
    ("a1yaoiuong1 ", "a\u0304ya\u0304oiuong "),
    ("ang22", "a\u0301ng\u0301"),
    ("ou1eryao4eng5ou5", "ou\u0304erya\u0300oengou"),
    ("guo5qiongou4eng4", "guoqiongou\u0300e\u0300ng"),
    ("er5o2", "ero\u0301"),
    ("zhong52o3ma5 E43", "zhong\u0301o\u030cma E\u03003"),
    ("n4 iu2zhong2 e34ng", "n\u0300 iu\u0301zho\u0301ng e\u030c4ng"),
    ("n14e2 ma3 ", "n\u03044e\u0301 ma\u030c "),
    ("a", "a"),
    ("ou", "ou"),
    ("uin4Ou3 ", "ui\u0300nOu\u030c "),
    ("ma53eng", "ma\u030ceng"),
    ("eng", "eng"),
    ("ng1uie22 Ou2n4", "ng\u0304uie\u03012 Ou\u0301n\u0300"),
    ("E1", "E\u0304"),
    ("ma5", "ma"),
    ("engE5yao5n2", "engEyaon\u0301"),
    ("E3ang21 ", "E\u030ca\u0301ng\u0304 "),
]

STARTS = "([bdpt]) \u02b2\u02d0 \u2192 $1 \u02d0\u02b2 ;\n([bdpt]) \u02b7\u02d0 \u2192 $1 \u02d0\u02b7 ;\n([bdpt]) \u02e0\u02d0 \u2192 $1 \u02d0\u02e0 ;\n"

START_CASES = [
    ("ap", "ap"),
    ("\u02b2d\u02e0\u02d0\u02d0\u02e0\u02d0\u02d0b\u02d0\u02b7", "\u02b2d\u02d0\u02e0\u02d0\u02e0\u02d0\u02d0b\u02d0\u02b7"),
    ("\u02d0\u02d0\u02d0\u02b7\u02b7pbd", "\u02d0\u02d0\u02d0\u02b7\u02b7pbd"),
    ("\u02b7pd", "\u02b7pd"),
    ("\u02d0\u02e0p\u02b7", "\u02d0\u02e0p\u02b7"),
    ("\u02d0\u02d0p\u02b7db\u02d0\u02d0", "\u02d0\u02d0p\u02b7db\u02d0\u02d0"),
    ("pb", "pb"),
    ("aad\u02d0", "aad\u02d0"),
    ("\u02d0\u02d0bd", "\u02d0\u02d0bd"),
    ("\u02b2\u02b2\u02d0\u02e0ppd\u02b2b", "\u02b2\u02b2\u02d0\u02e0ppd\u02b2b"),
    ("\u02b2p\u02d0\u02d0d\u02d0d", "\u02b2p\u02d0\u02d0d\u02d0d"),
    ("\u02e0\u02b2\u02b7b\u02e0\u02b7", "\u02e0\u02b2\u02b7b\u02e0\u02b7"),
    ("\u02b2pd", "\u02b2pd"),
    ("\u02d0\u02d0ad\u02d0", "\u02d0\u02d0ad\u02d0"),
    ("\u02e0\u02d0\u02d0bpbddp", "\u02e0\u02d0\u02d0bpbddp"),
    ("\u02b2d", "\u02b2d"),
    ("a\u02e0\u02b2\u02b7dbdt\u02b7", "a\u02e0\u02b2\u02b7dbdt\u02b7"),
    ("\u02d0\u02d0\u02b7a\u02d0\u02d0\u02d0\u02d0", "\u02d0\u02d0\u02b7a\u02d0\u02d0\u02d0\u02d0"),
    ("\u02b2\u02b7\u02b2bp\u02d0pt", "\u02b2\u02b7\u02b2bp\u02d0pt"),
    ("ta", "ta"),
]


def check(tconv, tmp, name, rules_text, cases):
    failures = 0
    rules = os.path.join(tmp, name + ".rules")
    with open(rules, "w", encoding="utf-8") as f:
        f.write(rules_text)
    input = os.path.join(tmp, name + ".txt")
    with open(input, "w", encoding="utf-8") as f:
        f.write("".join(line + "\n" for line, _ in cases))
    plan = subprocess.run([tconv, "--plan", "--rules-file=" + rules, input], capture_output=True)
    text = plan.stdout.decode("utf-8", "replace")
    if ": implementable" not in text.split("\n")[0] or "variants by" not in text:
        failures += 1
        print("FAIL %s: the rules are not planned as variants\n%s" % (name, text))
    run = subprocess.run([tconv, "--use-ICU=0", "--lines", "--rules-file=" + rules, input], capture_output=True)
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
    tconv = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        failures = check(tconv, tmp, "tones", TONES, TONE_CASES)
        failures += check(tconv, tmp, "starts", STARTS, START_CASES)
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
