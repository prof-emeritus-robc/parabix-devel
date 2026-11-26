#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  xlit: DivCode's class layer in the regular plan's fused kernels
#  (doc/ldml-divcode-plan.md §13.5, step 4; --class-layer).  In UTF-8 mode, a
#  fused kernel with 20 or more rules whose first characters are in 4 or more
#  Div32 blocks, none with more than half of them, reads its classes as Div32
#  pseudo-codepoints and decides its rules in Ifs by the blocks of their first
#  characters.  For rules files of string rules and single-character rules over
#  several scripts (before and after contexts, a negated set, properties given
#  as streams of their own (external sets), deletions,
#  expansions, a rule that a string rule yields to, a filter), on random text
#  of their characters and others (1 to 4 UTF-8 bytes), with long lines and
#  sparse matches (blocks where an If is skipped before and after a match, and
#  contexts in them), the output must be that of the regular plan
#  (--class-layer=0) and ICU's (but with the filter, see filtered_rules), with
#  the default threads and with four threads
#  and segments of one stride (the correction kernels of the rules with before
#  contexts matched against the converted text layered as well), in UTF-8 mode (the classes coded in place of the
#  multiplexed code) and in Unicode mode (in place of a stream for each class).  The layer must be used (--class-layer-report).
#
#  xlit_class_layer_test.py <xlit binary> [--extended-testing]

import json
import os
import random
import subprocess
import sys
import tempfile

from xlit_test_mode import EXTENDED

#  Scripts of several Div32 blocks: the first and second halves of a range of
#  letters of each.
SCRIPTS = [(0x3B1, 0x3C9), (0x430, 0x44F), (0x561, 0x586), (0x5D0, 0x5EA), (0x10D0, 0x10F0)]


def string_rules(seed, pairs, extras):
    #  String rules of two letters of a script, the first from its first half
    #  and the second from its second half (no rule begins within another), and
    #  single-character rules for some first letters; with extras, contexts and
    #  a rule that a string rule yields to (where an after context it does not
    #  have holds), and no deletions (a deletion would let a before context see
    #  converted text, and the rules would be decided by a correction kernel);
    #  without, deletions and expansions.
    rng = random.Random(seed)
    lines = []
    for lo, hi in SCRIPTS:
        mid = (lo + hi) // 2
        firsts = list(range(lo, mid))
        seconds = list(range(mid, hi + 1))
        rng.shuffle(firsts)
        for i in range(pairs):
            a, b = chr(firsts[i % len(firsts)]), chr(rng.choice(seconds))
            out = "".join(rng.choice("ABCDEFGH") for _ in range(rng.randint(1 if extras else 0, 3)))
            lines.append("%s%s \u2192 %s ;" % (a, b, out))
        for c in firsts[:3]:
            lines.append("%s \u2192 %s ;" % (chr(c), rng.choice("abcdefgh")))
        if extras:
            a, b = chr(firsts[-1]), chr(seconds[0])
            lines.append("1 { %s%s \u2192 X ;" % (a, b))
            lines.append("%s%s } 2 \u2192 YY ;" % (chr(firsts[-2]), chr(seconds[1])))
            lines.append("%s } %s3 \u2192 Q ;" % (chr(firsts[-3]), chr(seconds[2])))
            lines.append("%s%s \u2192 R ;" % (chr(firsts[-3]), chr(seconds[2])))
            lines.append("[^\u0e01] { %s%s \u2192 Z ;" % (chr(firsts[-4]), chr(seconds[3])))
            # (Sets the class layer gives as streams of their own: external.)
            lines.append("[^[:L:]] { %s%s \u2192 W ;" % (chr(firsts[-5]), chr(seconds[4])))
            lines.append("%s%s } [:N:] \u2192 N ;" % (chr(firsts[-6]), chr(seconds[5])))
            lines.append("[^\u0e02] { %s%s \u2192 V ;" % (chr(firsts[-7]), chr(seconds[6])))
    return "\n".join(sorted(set(lines), key=lines.index)) + "\n"


def filtered_rules(seed, pairs):
    #  A filter excluding a range of Cyrillic letters.  (Compared with the
    #  regular plan only: ICU, as --use-ICU applies these rules, converts the
    #  characters the filter excludes.)
    rules = string_rules(seed, pairs, True)
    used = set(rules)
    ends = [c for c in range(0x430, 0x440) if chr(c) not in used]
    return ":: [^%s-%s] ;\n" % (chr(0x42F), chr(ends[-1])) + rules


RULES = {
    "pairs": string_rules(1, 6, False),
    "contexts": string_rules(2, 5, True),
    "filtered": filtered_rules(3, 6),
}

OTHERS = "xyz 123กข中文😀\n"


def alphabet(rules):
    chars = [chr(c) for lo, hi in SCRIPTS for c in range(lo, hi + 1)]
    return chars + list(OTHERS.replace("\n", "")) * 4


def make_text(rules, seed, lines):
    rng = random.Random(seed)
    chars = alphabet(rules)
    out = []
    for i in range(lines):
        kind = i % 4
        if kind == 0:
            n = rng.randint(1, 60)
            out.append("".join(rng.choice(chars) for _ in range(n)))
        elif kind == 1:
            # Long lines: many blocks, most with some rules' characters.
            n = rng.randint(500, 3000)
            out.append("".join(rng.choice(chars) for _ in range(n)))
        else:
            # Sparse: runs of other characters (blocks where the Ifs are
            # skipped), then a few characters of the rules, across block ends.
            parts = []
            for _ in range(rng.randint(1, 6)):
                parts.append("x" * rng.randint(100, 400))
                parts.append("".join(rng.choice(chars) for _ in range(rng.randint(1, 5))))
            out.append("".join(parts))
    return "".join(line + "\n" for line in out)


def run(xlit, args):
    return subprocess.run([xlit] + args, capture_output=True)


def main():
    xlit = sys.argv[1]
    failures = 0
    configs = [[], ["-thread-num=4", "-segment-size=1"]]
    lines = 400 if EXTENDED else 80
    with tempfile.TemporaryDirectory() as tmp:
        for name, rules in RULES.items():
            rules_path = os.path.join(tmp, name + ".txt")
            with open(rules_path, "w", encoding="utf-8") as f:
                f.write(rules)
            input_path = os.path.join(tmp, name + ".in")
            with open(input_path, "w", encoding="utf-8") as f:
                f.write(make_text(rules, 7, lines))
            for mode in ("1", "0"):
                label = "%s, --utf8-mode=%s" % (name, mode)
                # (The regular plan: these groups are eligible for DivCode's own kernels.)
                common = ["--rules-file=" + rules_path, "--utf8-mode=" + mode, "--divcode-groups=0"]
                regular = run(xlit, common + ["--use-ICU=0", "--class-layer=0", input_path])
                if regular.returncode != 0:
                    failures += 1
                    print("FAIL %s: the regular plan: exit status %d\n%s" % (label, regular.returncode, regular.stderr.decode("utf-8", "replace")))
                    continue
                icu = run(xlit, common + ["--use-ICU=.*", input_path]) if not rules.startswith("::") and mode == "1" else None
                if icu is not None and icu.returncode == 0 and icu.stdout != regular.stdout:
                    failures += 1
                    print("FAIL %s: the regular plan differs from ICU" % label)
                for config in configs:
                    # (With the default threads, the kernels reported: one must be layered.)
                    report = [] if config else ["--class-layer-report"]
                    layered = run(xlit, common + config + report + ["--use-ICU=0", "--class-layer=1", input_path])
                    if report:
                        kernels = [json.loads(l) for l in layered.stderr.decode("utf-8", "replace").split("\n")
                                   if l.startswith("{\"kernel\"")]
                        if not any(k.get("layered") for k in kernels):
                            failures += 1
                            print("FAIL %s: no kernel with the class layer (%d kernels)" % (label, len(kernels)))
                        # (With contexts matched against the converted text: a
                        # correction kernel with the layer, doc/ldml-divcode-plan.md
                        # §13.16, step 4e.)
                        if name != "pairs" and not any(k.get("layered") and k.get("kind") in ("correction", "loop", "post")
                                                       for k in kernels):
                            failures += 1
                            print("FAIL %s: no correction kernel with the class layer" % label)
                    if layered.returncode != 0:
                        failures += 1
                        print("FAIL %s %s: exit status %d\n%s" % (label, " ".join(config), layered.returncode,
                                                                layered.stderr.decode("utf-8", "replace")))
                        continue
                    if layered.stdout != regular.stdout:
                        failures += 1
                        got = layered.stdout.decode("utf-8", "replace").split("\n")
                        want = regular.stdout.decode("utf-8", "replace").split("\n")
                        for i, (g, w) in enumerate(zip(got, want)):
                            if g != w:
                                print("FAIL %s %s, line %d:\n  want %r\n  got  %r" % (label, " ".join(config), i + 1, w[:80], g[:80]))
                                break
                        else:
                            print("FAIL %s %s: %d lines, want %d" % (label, " ".join(config), len(got), len(want)))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
