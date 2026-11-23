#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: the UTF-8 mask convention (doc/masked-transforms-plan.md, step 14):
#  in UTF-8 mode, every mask gives all the bytes of a character the same bit
#  (--check-utf8-masks checks each mask after each filter and each step, and
#  exits with status 3 if one does not).  The cases change the masks in each way
#  UTF-8 mode does: a filter's mask (utf8Mask), character maps that change the
#  number of bytes (1 to 2, 2 to 1, 1 to 4, one character to two) before and
#  after a capture rule applied in a Unicode segment (the masks decoded and
#  spread back), and a DivCode group of string rules on the bytes, with
#  characters of one to four bytes outside the filter, as a text and with
#  --lines (the regions' mask).  (That the output equals Unicode mode's is
#  left to the combinatorial tests, QA/xlit_combinatorial.)
#
#  xlit_utf8_masks_test.py <xlit binary> [--extended-testing]

import os
import random
import subprocess
import sys
import tempfile

RULES = {
    # Character maps in UTF-8 mode on both sides of a capture rule in a Unicode segment
    # (a capture rule that is not a string rule: MultipleCaptureMultipleReplacement, in Unicode mode)
    "segment": ":: [abcéxß] ;\nc → ç ;\né → e ;\nx → \\U0001D535 ;\nß → ss ;\n:: Null ;\n([ab]) ç ([ab]) → $2 ç $1 ;\n"
               ":: Null ;\nç → c ;\nb → \\U0001D535 ;\n",
    "divcode": ":: [abcü] ;\nab → X ;\nbc } a → Y ;\nü → u ;\n",
}
ALPHABET = "aéxßbcüq中😀 "


def main():
    xlit = sys.argv[1]
    rnd = random.Random(5)
    text = "\n".join("".join(rnd.choice(ALPHABET) for _ in range(rnd.randint(0, 30))) for _ in range(60)) + "\n"
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        for name, rules in RULES.items():
            rp = os.path.join(tmp, name + ".rules")
            with open(rp, "w", encoding="utf-8") as f:
                f.write(rules)
            for lines in ([], ["--lines"]):
                args = lines + ["--rules-file=" + rp, path]
                checked = subprocess.run([xlit, "--check-utf8-masks"] + args, capture_output=True)
                label = name + (" --lines" if lines else "")
                if checked.returncode != 0:
                    failures += 1
                    print("FAIL %s: exit status %d\n%s" % (label, checked.returncode, checked.stderr.decode("utf-8", "replace")[-600:]))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
