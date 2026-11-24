#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: Latin-Greek's breathing marks (Latn-Grek, the backward direction of
#  Greek-Latin), whose rules revisit vowel clusters of one to five
#  characters: the rough breathing written after the cluster that follows an
#  h or H (in place of the h), the smooth breathing after a cluster at the
#  start of a word.  The planner restates them without revisits, in groups of
#  their own before the rest of the group (ldml::RewriteBreathingMarks): an h
#  taken by a digraph (th, ph, rh, ch) is never reached; j, v, w revisit to
#  vowels that the smooth breathing and the iota subscript rules then match;
#  ICU matches the smooth breathing's before context against the converted
#  text, which the rules of the group change (:̱ → :, N' → Ν), and sees the
#  character before a deleted h; a smooth breathing within a rough cluster
#  with a diaeresis at the start of a word (hë → ἐ̈̔).  Also long texts across
#  blocks.  The expected results are ICU4C's and ICU4J's (identical).  With
#  --breathing-marks=0, Latn-Grek is not implementable.
#
#  xlit_breathing_marks_test.py <xlit binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

CASES = [
    ('ha', 'ἁ'),
    ('hai', 'αἱ'),
    ('hëa', 'ἐ̈̔α'),
    ('hë', 'ἐ̈̔'),
    ('Hë', 'Ἐ̈̔'),
    (' hë', ' ἐ̈̔'),
    ('ahë', 'ἀε̈̔'),
    ('häi', 'ἀ̈ἱ'),
    ('ēu', 'ηὐ'),
    ('hēu', 'ηὑ'),
    ('Ha', 'Ἁ'),
    ('Hai', 'Αἱ'),
    ('Hā', 'Ἁ'),
    ('HAI', 'ΑἹ'),
    ('HE', 'Ἑ'),
    ('the', 'θε'),
    ('phos', 'φος'),
    ('rha', 'ῥα'),
    ('Rha', 'Ῥα'),
    ('RHA', 'ῬΑ'),
    ('cha', 'χα'),
    ('tHa', 'τἉ'),
    ('rHe', 'ρἙ'),
    ('rHa', 'ρἉ'),
    ('of', 'ὀφ'),
    ('anywhere', 'ἀνυυἑρε'),
    ('whatsoever', 'ὐἁτσοεβερ'),
    ('eu', 'εὐ'),
    ('ai', 'αἰ'),
    ('āi', 'ᾀ'),
    ('ōī', 'ᾠ̄'),
    ('ūī', 'ῡ̓̄ͅ'),
    ('w̄i', 'ῡ̓ͅ'),
    ('ja', 'ἰα'),
    ('ju', 'ιὐ'),
    ('vs', 'ὐς'),
    ('vi', 'βι'),
    ('v̄u', 'ῡὐ'),
    ('V̄ui', 'Ῡὐι'),
    ('w', 'ὐ'),
    ('J', 'Ἰ'),
    (':̱a', ':ἀ'),
    ('?̱a', '?ἀ'),
    ('x:̱a', 'ξ:ἀ'),
    ("N'a", 'Να'),
    ("n'a", "ν'ἀ"),
    ("N'ka", 'Νκα'),
    ("'a", "'ἀ"),
    ("x'a", "ξ'ἀ"),
    ('hhēi', '̔ᾑ'),
    ('the hōmē of the hero', 'θε ὡμη ὀφ θε ἑρο'),
    ('Herodotus, Homer and Hesiod', 'Ἑροδοτυς, Ὁμερ ἀνδ Ἑσιοδ'),
    ("ha" * 70,
     'ἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁἁ'),
    ("x" + "hē" * 90 + "a",
     'ξἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡἡα'),
    ("a" + " hai" * 60,
     'ἀ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ αἱ'),
]

CONFIGS = [[], ["-thread-num=4", "-segment-size=1"]] + ([["-thread-num=1"]] if EXTENDED else [])


def main():
    xlit = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        input = os.path.join(tmp, "input.txt")
        with open(input, "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line, _ in CASES))
        for config in CONFIGS:
            run = subprocess.run([xlit, "--use-ICU=0", "--lines"] + config + ["Latn-Grek", input], capture_output=True)
            if run.returncode != 0:
                failures += 1
                print("FAIL %s: exit status %d\n%s" % (" ".join(config), run.returncode, run.stderr.decode("utf-8", "replace")))
                continue
            got = run.stdout.decode("utf-8", "replace").split("\n")
            for i, (line, want) in enumerate(CASES):
                if i >= len(got) or got[i] != want:
                    failures += 1
                    print("FAIL %s with %r:\n  want %r\n  got  %r" % (" ".join(config), line[:40], want[:60], got[i][:60] if i < len(got) else None))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
