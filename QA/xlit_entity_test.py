#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit --entity-groups: conversion groups applied by their entity plans
#  (doc/ldml-entity-groups-plan.md, steps 2 to 4, in Unicode mode and in UTF-8
#  mode, where a group is matched one position per character on classes of
#  the bytes and its results applied as selectors (the hybrid); rules that
#  begin within others' matches selected in stages, and those on cycles in a
#  While loop; before contexts matched against the converted text given by
#  the closed form of §5.3).  For rules files of sets
#  with strings (strings extending single characters of the set, a union of
#  such sets), optional items, after contexts (properties, a negated set that
#  holds at the end of the text), expansions and deletions, on random text of
#  their characters and others (combining marks alone, characters of 1 to 4
#  UTF-8 bytes, line feeds), the output must be the regular plan's and ICU's,
#  and the groups must be applied by their entity plans (--entity-groups-trace):
#  with the class layer's Div32 code where its criterion holds (20 or more
#  rules in 4 or more Div32 blocks, none with more than half), and otherwise
#  (also with --class-layer=0) with a stream for each class in Unicode mode, or
#  the multiplexed code of the classes in UTF-8 mode.
#
#  xlit_entity_test.py <xlit binary> [--extended-testing]

import os
import random
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

SMALL = """
$V = [a e {e̞} i {ɪ̈}];
$C = [k {k͡h} {k͡hʷ} t];
$V ː? → A;
$C → C;
[bd] } [:L:] → D;
x → yz;
q → ;
"""

#  30 rules whose first characters are in 6 Div32 blocks (Latin, Greek, two of
#  Cyrillic, IPA, Hebrew), none with more than half of them.
LAYERED = """
$A = [a b c {a̰}];
$G = [α β γ {ά̃}];
$K = [к л м {н̆}];
$I = [ɐ ɑ ɒ {ɓ̃}];
$H = [א ב ג {ד̇}];
[$A $G] ʼ → U;
$A ː? → A;
d } [:L:] → D;
e → EE;
f → ;
g ʼ? → G;
$G → Γ;
δ } [^[:L:]] → Δ;
ε → ;
ζ → ZZ;
η → H;
$K → K;
о → O;
п } [:N:] → P;
р → ;
с → SS;
$I → I;
ɔ → O;
ɕ → ;
ɖ → DD;
ɗ ː? → D;
$H → H;
ה → ;
ו → VV;
ז } [^[:L:]] → Z;
ח → X;
"""

#  Rules that may begin within others' matches, without cycles (step 3): y z
#  within x y, z within y z (three stages), $V ː? within k $V (at a set with
#  strings), and a rule a later one may begin within.
OVERLAPS = """
$V = [a e {e̞} o];
x y → P;
$V ː? → A;
k $V → K;
y z → QQ;
z → ;
"""

#  Before contexts matched against the converted text, given by the closed
#  form (step 4): the state is 1 after a character of $B (in no text, after no
#  result) and through the deletions of one position in state 1 (ʔ → ;, so
#  that the second ʔ of " ʔʔe" is at a boundary); and a rule that may begin
#  within its own matches (a cycle: "eʔeʔe"), selected from the left in a
#  While loop.
STATES = """
$B = [^[:L:][:M:][:N:]];
$V = [a e {e̞}];
$B {ʔ? $V} → W;
$V ʔ $V → XY;
$V → v;
ʔ → ;
"""

#  (rules, the code expected with --class-layer=1)
CASES = [("small", SMALL, "streams"), ("layered", LAYERED, "div32"), ("overlaps", OVERLAPS, "streams"),
         ("states", STATES, "streams")]


def pool_of(rules):
    chars = set()
    for line in rules.splitlines():
        if "→" in line or "=" in line:
            chars.update(c for c in line.split("→")[0] if c not in " $[]{}?;=:^→" and not c.isascii())
    chars.update("abcdefgkqtxyz")
    chars.update(["̰", "̃", "̆", "̇", "̞", "ː", "ʼ", " ", "1", "\n", "ж", "中", "𝒜"])
    strings = ["e̞", "ɪ̈", "k͡h", "k͡hʷ", "a̰", "ά̃", "н̆", "ɓ̃", "ד̇",
               "xyz", "xyzz", "yzz", "ke̞ː", "kaː", " ʔʔe", "eʔeʔeʔe", "ʔʔʔ", " ʔe̞ʔa"]
    return sorted(chars) + strings


def run(xlit, args):
    return subprocess.run([xlit] + args, capture_output=True)


def main():
    xlit = sys.argv[1]
    failures = 0
    checks = 0
    n = 40000 if EXTENDED else 8000
    with tempfile.TemporaryDirectory() as d:
        for name, rules, code in CASES:
            rules_path = os.path.join(d, name + ".txt")
            with open(rules_path, "w", encoding="utf-8") as f:
                f.write(rules)
            rng = random.Random(name)
            pool = pool_of(rules)
            text = "".join(rng.choice(pool) for _ in range(n))
            input_path = os.path.join(d, name + "-in.txt")
            with open(input_path, "w", encoding="utf-8") as f:
                f.write(text)
            icu = run(xlit, ["--rules-file=" + rules_path, "--use-ICU=.*", input_path])
            for mode in ("0", "1"):
                common = ["--rules-file=" + rules_path, "--utf8-mode=" + mode, "--use-ICU=0", input_path]
                regular = run(xlit, common)
                checks += 1
                if regular.returncode != 0 or icu.returncode != 0 or regular.stdout != icu.stdout:
                    failures += 1
                    print("FAIL %s --utf8-mode=%s: the regular plan and ICU differ (or fail): %s %s" % (
                          name, mode, regular.stderr.decode(errors="replace")[:300], icu.stderr.decode(errors="replace")[:300]))
                    continue
                #  (In UTF-8 mode, the hybrid: a code on the bytes, the class
                #  layer's or the multiplexed one.)
                for layer in ("1", "0"):
                    if mode == "0":
                        expected = code if layer == "1" else "streams"
                    else:
                        expected = (code if code != "streams" else "multiplexed") if layer == "1" else "multiplexed"
                    checks += 1
                    ent = run(xlit, ["--entity-groups", "--entity-groups-trace", "--class-layer=" + layer] + common)
                    trace = ent.stderr.decode(errors="replace")
                    problems = []
                    if ent.returncode != 0:
                        problems.append("exit status %d" % ent.returncode)
                    if ent.stdout != regular.stdout:
                        problems.append("output differs from the regular plan's")
                    if (": applied (UTF-8 mode)" if mode == "1" else ": applied") not in trace:
                        problems.append("not applied by its entity plan")
                    if "code " + expected not in trace:
                        problems.append("code not " + expected)
                    if problems:
                        failures += 1
                        print("FAIL %s --utf8-mode=%s --class-layer=%s: %s; %s" % (name, mode, layer, ", ".join(problems), trace[:400]))
    print("%d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
