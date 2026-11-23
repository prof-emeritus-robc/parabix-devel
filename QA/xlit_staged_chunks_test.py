#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: the staged rounds of a whole group whose string matches are selected
#  sequentially, in chunks (--staged-chunk-rules; RulePipelineBuilder::
#  conversionGroup): the string rules whose before contexts are matched against
#  the converted text in the kernels of each round, the others matched once
#  and applied in chunks where selected, their results merged
#  (MergeFusedResults), and the subgroups applied outside the static matches.
#  The group here has both kinds of string rules, two-way overlaps (bc, cb), a
#  replacement longer than its text (an insertion count) and a subgroup whose
#  before context is matched against the converted text.  Each case is run
#  chunked (--staged-chunk-rules=0) and unchunked, both staged
#  (--sequential-while=0).  The expected results are ICU's (createFromRules,
#  each line a text).  With --extended-testing, also Latn-Deva (through
#  Latin-InterIndic, 2,388 string rules, chunked by default), whose before
#  contexts were matched against the text itself before (xi, hao).
#
#  xlit_staged_chunks_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

RULES = ("x \u2192 a ;\n[aQ] { bc \u2192 Q ;\ncb \u2192 R ;\nbcb \u2192 S ;\nmn \u2192 MNO ;\n"
         "k \u2192 kh ;\n[Q] { d \u2192 D ;\nd \u2192 e ;\n")
CASES = [("xbcbcd", "aQQD"),
         ("cbcbxbcd", "RRaQD"),
         ("xbcmnbcbd", "aQMNOSe"),
         ("kdxbcbcbcbcd", "kheaQQQQD"),
         ("mnxbcbck", "MNOaQQkh"),
         ("xbcbcbcbcbcbcbcd", "aQQQQQQQD"),
         ("bcbdcbxbcd", "SeRaQD")]

LATN_DEVA = [("xi", "\u0915\u094d\u0938\u093f"), ("hao", "\u0939\u094a"),
             ("kxi", "\u0915\u094d\u0915\u094d\u0938\u093f")]

CONFIGS = [["--staged-chunk-rules=0"], ["--staged-chunk-rules=100000"],
           ["--staged-chunk-rules=0", "-thread-num=4", "-segment-size=1"]]


def check(xlit, args, cases, label):
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in cases))
        run = subprocess.run([xlit, "--use-ICU=0", "--lines"] + args + [input], capture_output=True)
        if run.returncode != 0:
            print("FAIL %s: exit status %d\n%s" % (label, run.returncode, run.stderr.decode("utf-8", "replace")))
            return 1
        got = run.stdout.decode("utf-8", "replace").split("\n")
        for i, (line, want) in enumerate(cases):
            if i >= len(got) or got[i] != want:
                failures += 1
                print("FAIL %s with %r:\n  want %r\n  got  %r" % (label, line, want, got[i] if i < len(got) else None))
    return failures


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        with open(rules, "w", encoding="utf-8") as f:
            f.write(RULES)
        for config in CONFIGS:
            failures += check(xlit, ["--sequential-while=0"] + config + ["--rules-file=" + rules], CASES, " ".join(config))
    if EXTENDED:
        failures += check(xlit, ["Latn-Deva"], LATN_DEVA, "Latn-Deva")
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
