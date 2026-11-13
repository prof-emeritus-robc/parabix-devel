#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv --shape-groups: a conversion group planned as search and replace
#  passes, one for each shape of the results of its rules (see
#  include/ldml/shape_groups.h).  The passes of NumericPinyin-Latin (the
#  backward rules of Latin-NumericPinyin), as printed by tconv, are applied
#  with icgrep --replace to the text in NFD, the deferred function calls
#  (their marks and arguments) replaced by their values, and the result (in
#  NFC) compared with ICU's (tconv --use-ICU=.*, if tconv is built with ICU),
#  or else with tconv's regular plan (--apply-shape-groups=0); and so is tconv
#  --apply-shape-groups (the default), which applies the same passes itself.  (Skipped without the CLDR transforms.)
#
#  tconv_shape_groups_test.py <tconv binary> <icgrep binary>

import os
import re
import subprocess
import sys
import tempfile
import unicodedata

TRANSFORM = "NumericPinyin-Latin"

TEXT = [
    "hao3 hao33 ha53 ha33 ha353 x553 x5x eao3",
    "nu\u03083 lu\u0308e43 au\u030833 ang1 a5 xian4 o2 gou3 lu\u0308n2",
    "Zhong1guo2 ren2min2 gong4he2guo2 BEI3JING1 Lv4 lu:4 Nu\u03085",
    "no tones here 123 9a 4",
    "",
    "shuang1 chuang2 kuai4 lüe4 jiong3 zhuan4 yue4 er2 ri4 yi1 wu3",
] + ["zhong1wen2 " * 400]       # a line of several blocks

# Bopo-Latn applies NumericPinyin-Latin within its filter (Bopomofo and tone
# marks), by its shape groups: the text of their matches lies within the
# filter, while contexts see the whole text (see RE_context::textMask).  The
# characters outside the filter (here the Latin text and the digits) are not
# changed, nor matched: ㄧ˙ is yi5, whose 5 is deleted but not the one after
# it; and the tone digit after ㄗ˙ㄊ is outside the filter.
FILTERED = "Bopo-Latn"
FILTERED_CASES = [
    ("ㄋㄧˇ ㄏㄠˇ ma3 ㄇㄚˇ", "nǐ hǎo ma3 mǎ"),
    ("ㄧ˙5", "yi5"),
    ("ㄗ˙ㄊ3", "zit3"),
    ("ㄓㄨㄥ ㄨㄣˊ hao3", "zhōng wén hao3"),
    ("nü3ㄌㄩˋ", "nü3lǜ"),
]


def plan(tconv):
    """The passes (search, replacement) and the values of the marked calls."""
    run = subprocess.run([tconv, "--shape-groups", TRANSFORM], capture_output=True)
    if b"unknown transform" in run.stderr:
        print("tconv_shape_groups_test: skipped (no CLDR transforms directory)")
        sys.exit(0)
    out = run.stdout.decode("utf-8")
    passes, values, steps = [], {}, []
    mark = None
    for line in out.split("\n"):
        s = line.strip()
        if s.startswith("transform "):
            steps.append(("transform", s.split()[1]))
        elif s.startswith("conversion group"):
            steps.append(("group", None))
        elif s.startswith("search: "):
            passes.append([s[len("search: "):], None])
        elif s.startswith("replacement: "):
            passes[-1][1] = s[len("replacement: "):]
        elif s.startswith("engine: not accepted") or s.startswith("problem: "):
            raise RuntimeError("not planned: " + s)
        elif s.startswith("deferred call "):
            mark = chr(int(re.search(r"mark U\+([0-9A-F]+)", s).group(1), 16))
        elif s.startswith("value "):
            m = re.match(r"value U\+([0-9A-F]+) -> (.*)", s)
            arg = chr(int(m.group(1), 16))
            v = "" if m.group(2) == "(deleted)" else "".join(chr(int(x[2:], 16)) for x in m.group(2).split())
            values[mark + arg] = v
    return steps, passes, values


def apply_pass(icgrep, search, replacement, lines, tmp):
    path = os.path.join(tmp, "pass.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(l + "\n" for l in lines))
    run = subprocess.run([icgrep, "--colors=never", "-n", "--replace=" + replacement, search, path], capture_output=True)
    if run.returncode not in (0, 1):
        raise RuntimeError("icgrep failed: " + run.stderr.decode("utf-8", "replace"))
    out = list(lines)
    for l in run.stdout.decode("utf-8").split("\n"):
        if not l:
            continue
        n, text = l.split(":", 1)
        out[int(n) - 1] = text
    return out


def main():
    tconv, icgrep = sys.argv[1], sys.argv[2]
    steps, passes, values = plan(tconv)
    if [k for k, _ in steps] != ["transform", "group", "transform"] or steps[0][1] != "NFD" or steps[2][1] != "NFC":
        print("FAIL: unexpected steps %r" % steps)
        sys.exit(1)
    if len(passes) != 3:
        print("FAIL: expected 3 passes, got %d" % len(passes))
        sys.exit(1)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        lines = [unicodedata.normalize("NFD", l) for l in TEXT]
        for search, replacement in passes:
            lines = apply_pass(icgrep, search, replacement, lines, tmp)
        marks = "".join(sorted(set(k[0] for k in values)))
        lines = [re.sub("[" + marks + "].", lambda m: values[m.group(0)], l) for l in lines]
        got = [unicodedata.normalize("NFC", l) for l in lines]
        # The expected text: ICU's, or tconv's.
        path = os.path.join(tmp, "input.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("".join(l + "\n" for l in TEXT))
        reference = []
        for icu in ("--use-ICU=.*", "--apply-shape-groups=0"):
            argv = [tconv, icu, TRANSFORM, path]
            run = subprocess.run(argv, capture_output=True)
            if run.returncode == 0:
                reference = run.stdout.decode("utf-8").split("\n")[:len(TEXT)]
                break
        for i, (g, w) in enumerate(zip(got, reference)):
            if g != w:
                failures += 1
                print("FAIL line %d:\n  got:      %r\n  expected: %r" % (i + 1, g[:200], w[:200]))
        if len(reference) != len(TEXT):
            failures += 1
            print("FAIL: no reference output")
        # The same passes, applied by tconv itself.
        run = subprocess.run([tconv, "--apply-shape-groups", TRANSFORM, path], capture_output=True)
        applied = run.stdout.decode("utf-8").split("\n")[:len(TEXT)]
        if run.returncode != 0 or run.stderr:
            failures += 1
            print("FAIL: tconv --apply-shape-groups: exit %d %s" % (run.returncode, run.stderr.decode()[:300]))
        for i, (g, w) in enumerate(zip(applied, reference)):
            if g != w:
                failures += 1
                print("FAIL --apply-shape-groups line %d:\n  got:      %r\n  expected: %r" % (i + 1, g[:200], w[:200]))
        # The shape groups within a filter.
        counts = subprocess.run([tconv, "--plan", FILTERED], capture_output=True).stdout.decode("utf-8")
        if "NumericPinyin-Latin (segment 2 by shape groups" not in counts:
            failures += 1
            print("FAIL: %s does not apply NumericPinyin-Latin by its shape groups:\n%s" % (FILTERED, counts[:500]))
        path = os.path.join(tmp, "filtered.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("".join(c + "\n" for c, _ in FILTERED_CASES))
        run = subprocess.run([tconv, FILTERED, path], capture_output=True)
        got = run.stdout.decode("utf-8").split("\n")
        if run.returncode != 0 or run.stderr:
            failures += 1
            print("FAIL: tconv %s: exit %d %s" % (FILTERED, run.returncode, run.stderr.decode()[:300]))
        for i, (source, expected) in enumerate(FILTERED_CASES):
            if i >= len(got) or got[i] != expected:
                failures += 1
                print("FAIL %s %r:\n  got:      %r\n  expected: %r" % (FILTERED, source, got[i] if i < len(got) else None, expected))
    if failures:
        sys.exit(1)
    print("tconv_shape_groups_test: all tests passed")


if __name__ == "__main__":
    main()
