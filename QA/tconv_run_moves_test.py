#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv: run moves (ldml::MarkRunMoves), fa-fa_FONIPA's ([َ-ِ]) ّ → ّ | $1,
#  which ICU applies again and again, writing the run of shaddas after the
#  vowel and then converting the vowel with what follows the run; replaced by
#  rules writing a mark at the vowel (where the group reaches it), the result
#  after the run, and a group deleting the marks.  The rules are
#  fa-fa_FONIPA's, reduced, with the copies of the consonant before a shadda
#  ($1 for each shadda of a run) and the deletion of the other shaddas.  Also
#  the deferred rules deleting a run of triggers in one round (DeferredPlan),
#  as the copy rule ([bc]) { s → $1 leaves runs of more than
#  --deferred-rule-rounds (8) triggers that the deletion s → ; would take a
#  round each to delete.  The expected results are ICU's (createFromRules,
#  each line a text).
#
#  tconv_run_moves_test.py <tconv binary>

import os
import subprocess
import sys
import tempfile

RUN_MOVES = """\
::NFD;
$VOWEL = [ َ ِ ُ ا و ی];
$BOUNDARY = [^[:L:][:M:][:N:]];
$IPA_CONSONANT = [ b d s ʃ h {t͡ʃ} ];
یّ → jj;
([ َ ِ ُ])ّ → ّ | $1;
َیْ → æj;
ِی → ej;
{ َ ه} $BOUNDARY → æ;
[e] {ه} $BOUNDARY → ;
َ → æ;
ِ → e;
$IPA_CONSONANT {ی} $VOWEL → iːj;
{ی} $VOWEL → j;
ی → iː;
ا → ɒː;
ب → b;
د → d;
س → s;
ش → ʃ;
چ → t͡ʃ;
ه → h;
ْ → ;
::NULL;
([$IPA_CONSONANT|$VOWEL]){ّ} → $1;
ّ → ;
::NFC;
"""

RUN_MOVE_CASES = [
    ("\u0628\u0650\u0651", "bbe"),
    ("\u0628\u064e\u0651\u06cc\u0652", "bb\u00e6j"),
    ("\u0628\u0650\u0651\u06cc", "bbej"),
    ("\u062f\u064e\u0651\u0647", "dd\u00e6"),
    ("\u062f\u0650\u0651\u0647", "dde"),
    ("\u0633\u064f\u0651", "ss\u064f"),
    ("\u0628\u0650\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "bee"),
    ("\u0627\u064f\u0650\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "\u0252\u02d0\u064fee"),
    ("\u0628\u064e\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u06cc", "bbbbbbbbbbbbb\u00e6i\u02d0"),
    ("\u0686\u064e\u0651\u0651\u0651", "t\u0361\u0283t\u0361\u0283t\u0361\u0283t\u0361\u0283\u00e6"),
    ("\u06cc\u0651", "jj"),
    ("\u06cc\u064e\u0651", "j\u00e6"),
    ("\u0628\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "bbbbbbbbbbb"),
    ("\u0634\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0647 \u0628", "\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283\u0283e b"),
    ("\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0628\u0634\u0633\u0650\u0651\u0651\u0651\u0651", "eb\u0283ssssse"),
    ("\u0652 \u0650\u0651\u0651\u0651\u06cc", " ej"),
    ("\u062f\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "ddddddddd\u064f"),
    ("\u064e\u0651\u0651\u0651\u0651\u0633", "\u00e6s"),
    ("\u0650\u0651\u0651\u0651\u0651\u0652\u064f\u0651\u0651\u0651\u0651\u06cc\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u064e\u0651\u0651\u0651\u0651\u0651 ", "\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064fej\u00e6e "),
    ("\u0628\u0647 ", "bh "),
    ("\u0627\u0633\u0652", "\u0252\u02d0s"),
    ("\u064e\u0651\u0651\u0651\u0651\u0651\u0651", "\u00e6"),
    ("\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u064f\u0651\u0652\u0634\u064e\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0650\u0651\u0651", "\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u0283\u00e6ee"),
    ("\u0647\u0628\u064e\u0651\u0651\u0651\u0651\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0647", "hb\u00e6\u064fh"),
    ("\u062f\u0650\u0651\u0651\u0651\u0651\u0651\u064e\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "d\u00e6e"),
    ("\u0686\u0633", "t\u0361\u0283s"),
    (" \u06cc \u0647\u0627\u0633", " i\u02d0 h\u0252\u02d0s"),
    ("\u0628", "b"),
    ("\u0652\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u06cc\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0650\u0651\u0651\u0651\u0651\u0651\u064f\u0651\u0651\u0651\u0627", "\u064fj\u064fee\u0252\u02d0"),
    ("\u0647\u0651\u0652\u064f\u0651\u0651\u0651", "hhhhh\u064f"),
    ("\u0647\u0650\u0651\u0651\u0651\u0651\u0651\u0651 \u0647\u0628", "hhhhhhhe hb"),
    ("\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0650\u0651\u0651\u0651\u0651\u0651\u0686\u0647\u0628\u0651\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064fet\u0361\u0283hbbbbbbbbbbe"),
    ("\u0647\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u06cc", "hhhhhhh\u064fi\u02d0"),
    ("\u0633\u0627\u0647", "s\u0252\u02d0h"),
    ("\u064e\u0651\u0651\u0651\u0651\u0627\u064e\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0633\u06cc", "\u00e6\u0252\u02d0\u00e6si\u02d0"),
    ("\u0686\u0628\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "t\u0361\u0283bbbbbbbbb\u064f"),
    ("\u0634\u0633\u0628\u0634\u064e\u0651\u0651", "\u0283sb\u0283\u0283\u0283\u00e6"),
    ("\u0627\u064e\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "\u0252\u02d0\u00e6"),
    ("\u0634\u0650\u0651\u0650\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0647\u0686 ", "\u0283eeht\u0361\u0283 "),
    ("\u0651\u0633\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u0651\u064f\u0651\u0651\u0651\u0651\u0651\u0651\u0651", "s\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f\u064f"),
]

DELETION_RUNS = "([bc]) { s → $1;\ns → ;\n"

DELETION_RUN_CASES = [
    ("ose", "oe"),
    ("osse", "oe"),
    ("osssssssse", "oe"),
    ("ossssssssse", "oe"),
    ("osssssssssse", "oe"),
    ("ossssssssssssssssse", "oe"),
    ("osssssssssssssssssssssssssssssse", "oe"),
    ("ssoobsecsbsoesoos", "oobbeccbboeoo"),
    ("cocs", "cocc"),
    ("csssscsssssbsbsbseobsscbcsss", "cccccccccccbbbbbbeobbbcbcccc"),
    ("cob", "cob"),
    ("cesscsscoobbsosbcss", "ceccccoobbbobccc"),
    ("bscscses", "bbcccce"),
    ("sbssbscssesscss", "bbbbbccceccc"),
    ("bbsses", "bbbbe"),
    ("scsscsssososbbosocobbbess", "cccccccoobboocobbbe"),
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
    if ": implementable" not in text.split("\n")[0] or "warning" in text:
        failures += 1
        print("FAIL %s: the rules are not implementable without warnings\n%s" % (name, text))
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
        failures = check(tconv, tmp, "run_moves", RUN_MOVES, RUN_MOVE_CASES)
        failures += check(tconv, tmp, "deletion_runs", DELETION_RUNS, DELETION_RUN_CASES)
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
