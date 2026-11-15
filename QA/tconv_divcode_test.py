#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Smoke test of the Div32Code / Div64Code kernels (doc/ldml-divcode-plan.md):
#  tconv --divcode-codes=G replaces each character by U+4000 | (the external
#  classes it is in << 10) | its code in the alphabet of conversion group G,
#  and describes the alphabet (shift, blocks, external sets) on the standard
#  error.  For conversion groups with small and large alphabets (up to 9 code
#  bits), with and without external sets, for div32 and div64, the codes and
#  classes of random text (characters of each block, of the external sets,
#  and others, of 1 to 4 UTF-8 bytes) must be those the description gives.
#
#  The translation kernels (tconv --divcode-groups), on the 21-bit basis and on
#  the UTF-8 code units (UTF-8 mode, --utf8-mode, where the transform can be):
#  the output must be that of the regular plan (--divcode-groups=0), and ICU's
#  for rules files, for
#  - the CLDR conversion groups eligible without expansions, on text made of
#    the texts of their rules, characters of their alphabets and of their
#    contexts, and others (div32 and div64), and on their CLDR test data;
#  - rules files of the cases: literal strings with single-character rules
#    within them (coverage), first sets split by blocks, before and after
#    contexts (explicit, negated with the text boundary, [^X$], properties),
#    priority (an after context added by order disambiguation), deletions,
#    filters, expansions (positions inserted after the matches, counted by
#    the Count kernel; before contexts through them), one-way overlaps of
#    string rules (stages: no match within an applied match of an earlier stage),
#    and contexts of two or more items.
#  The kernels must be used (their names in the trace of the object cache).
#  (Skipped without the CLDR transforms.)
#
#  tconv_divcode_test.py <tconv binary>

import glob
import json
import os
import random
import subprocess
import sys
import tempfile

GROUPS = [
    ("Bopo-Latn", 1),                       # 3 blocks
    ("Halfwidth-Fullwidth", 1),             # 10 blocks
    ("und_FONIPA-und_FONXSAMP", 1),         # 22 blocks (5 bits)
    ("byn_Ethi-byn_Latn/Xaleget", 1),       # an external set
    ("tk_Cyrl-tk/BGN", 1),                  # two external sets
    ("de-ASCII", 1),                        # an external set
    ("Hant-Hans", 1),                       # about 450 blocks (9 bits)
]
OTHER = "aZ 09.,é€中😀𐍈́\t"


def valid(cp):
    return not (0xD800 <= cp <= 0xDFFF) and cp != 0 and cp <= 0x10FFFF


def sample(rnd, ranges):
    lo, hi = rnd.choice(ranges)
    return rnd.randint(lo, hi)


TRANSLATE = [
    ("Any-Accents", 1), ("Bengali-InterIndic", 1), ("Halfwidth-Fullwidth", 1), ("Hira-Kana", 1),
    ("ja_Hrkt-ja_Latn/BGN", 2), ("Oriya-InterIndic", 1), ("Latn-Syrc", 1), ("cy-cy_FONIPA", 3),
    ("dsb-dsb_FONIPA", 2), ("ha-ha_NE", 1), ("my-my_FONIPA", 7), ("my-my_FONIPA", 8), ("pl_FONIPA-ja", 1),
    ("ru-ja", 1), ("sat_Olck-sat_FONIPA", 2), ("sk_FONIPA-ja", 3),
    # with expansions
    ("Sinh-Latn/UNGEGN", 1), ("Thaa-Latn", 1), ("byn_Ethi-byn_Latn/Xaleget", 1), ("ja_Hrkt-ja_Latn/BGN", 1),
    ("und_FONIPA-und_FONXSAMP", 1), ("InterIndic-Devanagari", 1), ("Khmr-Latn/UNGEGN", 3), ("si-si_FONIPA", 3),
    ("Cyrl-Latn", 1), ("tk_Cyrl-tk/BGN", 1), ("InterIndic-Kannada", 1), ("InterIndic-Telugu", 1),
    ("InterIndic-Malayalam", 1), ("InterIndic-Tamil", 1), ("ky-ky_Latn/BGN", 1), ("Latn-Armn", 1), ("Armn-Latn", 1),
    ("mn-mn_Latn/BGN", 1), ("si-si_Latn", 3), ("Tamil-InterIndic", 1), ("my-my_FONIPA", 9), ("my-my_FONIPA", 1),
    ("he-he_Latn/BGN", 1), ("cs-cs_FONIPA", 1), ("dsb-dsb_FONIPA", 1), ("gz_Ethi-Sarb", 1), ("eo-eo_FONIPA", 1),
    ("de-ASCII", 1), ("zh_Latn_PINYIN-ru", 2),
    # with one-way overlaps of string rules (stages)
    ("Ethi-Latn/ES3842", 1), ("Latn-Ethi/Williamson", 1), ("und_FONIPA-ar", 2), ("und_FONIPA-fa", 2),
    ("es_FONIPA-ja", 2), ("kk-kk_Latn/BGN", 1), ("uk-uk_Latn/BGN", 1), ("be-be_Latn/BGN", 1), ("bg-bg_Latn/BGN", 1),
    ("la-la_FONIPA", 1), ("Latn-Geor", 1), ("pl-pl_FONIPA", 2),
    # with contexts of two or more items (not ro_FONIPA-ja group 2, eligible but
    # after prefix runs that delete, whose blocking marks keep it on the regular
    # plan)
    ("Latn-Cans", 1), ("Gurmukhi-InterIndic", 1), ("InterIndic-ur", 1), ("Kannada-InterIndic", 1), ("Bopo-Latn", 1),
    ("my-my_Latn", 2), ("Telugu-InterIndic", 1), ("am_FONIPA-am", 8), ("cs_FONIPA-ja", 2), ("es_FONIPA-am", 2),
    ("es_FONIPA-zh", 3), ("my-my_FONIPA", 5), ("ru-ja", 2), ("ru-zh", 2), ("si-si_FONIPA", 8),
    ("sk_FONIPA-ja", 2), ("tlh-tlh_FONIPA", 1), ("Latn-Ethi/ALALOC", 1),
]
RULES = {
    "strings": "ab > X ; b > Y ; a > Z ; c > ;",
    "split": "[aä] b > Q ; ä > e ;",
    # (Results as long as the texts, so that the conversions cannot change what
    # the before contexts see; single-character rules on other first
    # characters, so that order disambiguation adds no contexts to them.)
    "before": "x { ab > PQ ; b > B ;",
    "after": "ab } y > R ; b > B ;",
    "negated-before": "[^x] { cd > ST ; d > D ;",
    "negated-after": "cd } [^y] > T ; d > D ;",
    "after-without-boundary": "ef } [^y$] > U ; f > F ;",
    "boundary-after": "gh } [$] > V ; h > H ;",
    "property-after": "gh } [:Lu:] > V ;",
    "property-before": "[:Lu:] { ij > wk ; j > k ;",
    "priority": "abc > 1 ; ab > 2 ; b > 3 ;",
    "deletion": "abc > z ; xy > ; q > ;",
    # (A second step, as ICU ignores the global filter of a transform of a
    # single conversion group.)
    "filter": ":: [a-m] ; ab > X ; mn > Y ; k > K ; :: Null ;",
    # (The end of a run of a global filter, of a compound transform, as the
    # end of the text: ldml::FilterRunEndMark.)
    "filter-run-end": ":: [a-m] ; :: Null ; ab } [$] > X ; k > K ; :: Null ;",
    # expansions (positions inserted after the matches)
    "expansion": "ab > XYZ ; c > DE ; b > B ;",
    "expansion-after": "ab } c > XYZ ; c > C ;",
    "expansion-negated-after": "ab } [^c] > XYZ ; c > C ;",
    # (A before context after an expanded match, through the positions inserted:
    # [bZ] holds for b and for the last character of its conversion alike.)
    "expansion-before": "ab > XYZ ; [bZ] { cd > KL ;",
    "expansion-negated-before": "ab > XYZ ; [^bZ] { cd > KL ;",
    "expansion-single": "ab > Q ; a > XY ;",
    "expansion-covered": "ab > Q ; b > XYZ ;",
    "expansion-priority": "abc > 1 ; ab > 234 ; b > 3 ;",
    "expansion-filter": ":: [a-m] ; ab > XYZ ; k > KK ; mn > Y ; :: Null ;",
    # characters of 1 to 4 UTF-8 bytes (for UTF-8 mode: lengths of texts and
    # results in bytes, contexts and set positions of multibyte characters)
    "utf8-lengths": "aé > ü ; Ωx > 中 ; 中 > zh ; 😀c > A ; bé > 😀😀 ;",
    "utf8-contexts": "[é中] { ab > xy ; cd } [^😀] > XY ; 😀 } e > E ;",
    "utf8-negated-before": "[^Ω] { ab > 中文 ; [^é] { cd > ço ;",
    "utf8-set-positions": "a [éè] > Q ; [xy] [中文] > R ;",
    "utf8-property": "xy } [:Han:] > HI ; [:Greek:] { yz > YZ ;",
    "utf8-boundary": "ü } [$] > U ; ü > uu ; éè > e ;",
    # one-way overlaps of string rules: a match within an applied match of a
    # rule of an earlier stage does not apply
    "stages-chain": "abcd > 1 ; bcd > 2 ; cd > 3 ;",
    "stages-kana": "kya > キャ ; ya > ヤ ; a > ア ; o > オ ;",
    "stages-expansion": "abc > X ; bc > QRS ; c > T ;",
    "stages-deletion": "xab > Z ; ab > ; b > B ;",
    "stages-after": "abc > 1 ; bc } d > 2 ;",
    "stages-utf8": "中文字 > 1 ; 文字 > 22 ; 字 > é ;",
    "stages-filter": ":: [a-m] ; abc > X ; bcd > Y ; k > K ; :: Null ;",
    # contexts of two or more items (fixed sequences of sets)
    "ctx2-before": "x y { ab > PQ ; b > B ;",
    "ctx2-after": "ab } c d > XY ; d > D ;",
    "ctx2-negated-outer": "[^x] y { ab > PQ ; ab } c [^d] > XY ;",
    "ctx2-boundary": "[$] x { ab > PQ ; cd } e [$] > XY ;",
    "ctx3": "x y z { ab > PQ ; cd } e f [^g] > XY ;",
    "ctx2-after-expansion": "ab } c d > X ; c > CC ;",
    "ctx2-utf8": "中 文 { ab > 字字 ; ab } é [^ü] > EF ;",
    "ctx2-property": "[:Han:] x { yz > YZ ; yz } [:Greek:] [:Greek:] > ZY ;",
    "ctx2-stages": "x { abc > 123 ; bc } d e > 45 ;",
}
RULES_ALPHABET = "abcdefghijkmnqxyzäAB \n.-éèüΩΑΒ中文😀"
#  (A narrower alphabet for the contexts of several items, which random text
#  over the full one rarely matches.)
CTX_ALPHABET = "abcdefgxyz中文éüΩΑ \n"
TRACE = ["-enable-object-cache", "-trace-object-cache"]


def run(tconv, args):
    return subprocess.run([tconv] + args, capture_output=True)


def translation(tconv, tmp, salt):
    failures = 0
    path = os.path.join(tmp, "text.txt")
    aliases = {}
    for line in run(tconv, ["--list-transforms"]).stdout.decode("utf-8").splitlines():
        fields = line.split("  ")
        aliases[fields[0]] = [f for f in fields[1:] if not f.startswith("[")]
    test_data = os.path.expanduser("~/cldr/common/testData/transforms")
    for name, group in TRANSLATE:
        for shift, mode in ((5, "1"), (5, "0"), (6, "1")):
            rnd = random.Random("%s %d %d translate" % (name, group, shift))
            with open(path, "w", encoding="utf-8") as f:
                f.write("a\n")
            r = run(tconv, ["--divcode-codes=%d" % group, "--divcode-shift=%d" % shift, name, path])
            desc = json.loads(r.stderr.decode("utf-8").splitlines()[0])
            pieces = list(desc["samples"]) or ["a"]
            singles = [chr((b << shift) | rnd.randrange(1 << shift)) for b in desc["blocks"] for _ in range(3)]
            singles = [c for c in singles if not 0xD800 <= ord(c) <= 0xDFFF]
            context = list(desc["context_chars"])
            other = list(" .,-\n") + list(OTHER)
            text = []
            for _ in range(4000):
                x = rnd.random()
                text.append(rnd.choice(pieces) if x < 0.5 else rnd.choice(singles or other) if x < 0.7
                            else rnd.choice(context or other) if x < 0.8 else rnd.choice(other))
            texts = ["".join(text)]
            for stem in [name] + aliases.get(name, []):
                for f in glob.glob(os.path.join(test_data, stem.replace("/", "-") + ".txt")):
                    with open(f, encoding="utf-8") as t:
                        texts.append("".join(l.split("\t")[0] + "\n" for l in t if "\t" in l and not l.startswith("#")))
            for i, t in enumerate(texts if shift == 5 else texts[:1]):
                with open(path, "w", encoding="utf-8") as f:
                    f.write(t)
                common = ["-object-cache-salt=" + salt, "--apply-shape-groups=0", "--divcode-shift=%d" % shift]
                regular = run(tconv, common + ["--divcode-groups=0", "--utf8-mode=0", name, path])
                divcode = run(tconv, common + TRACE + ["--divcode-groups=1", "--utf8-mode=" + mode, name, path])
                what = "%s#%d s=%d %s %s" % (name, group, shift, "random" if i == 0 else "test data",
                                             "UTF-8" if b"_U8" in divcode.stderr else "Unicode")
                if regular.returncode or divcode.returncode:
                    failures += 1
                    print("FAIL %s: exit %d/%d %s" % (what, regular.returncode, divcode.returncode, divcode.stderr[-300:]))
                elif ("Div%dCodeTranslate" % (1 << shift)).encode() not in divcode.stderr:
                    failures += 1
                    print("FAIL %s: no DivCode translation kernel" % what)
                elif regular.stdout != divcode.stdout:
                    failures += 1
                    a, b = regular.stdout.decode("utf-8", "replace"), divcode.stdout.decode("utf-8", "replace")
                    j = next((j for j in range(min(len(a), len(b))) if a[j] != b[j]), min(len(a), len(b)))
                    print("FAIL %s: differs at %d: regular %r, divcode %r (input %r)" % (what, j, a[j-10:j+20], b[j-10:j+20],
                                                                                 t[max(0, j-10):j+20]))
                else:
                    print("ok %s (%d characters)" % (what, len(t)))
    for case, rules in RULES.items():
        rnd = random.Random(case)
        rules_path = os.path.join(tmp, case + ".txt")
        with open(rules_path, "w", encoding="utf-8") as f:
            f.write(rules + "\n")
        report = run(tconv, ["--divcode-report", "--rules-file=" + rules_path])
        groups = [json.loads(l) for l in report.stdout.decode("utf-8").splitlines()]
        if not groups or not all(g["eligible"] for g in groups):
            failures += 1
            print("FAIL rules %s: not eligible: %s" % (case, [g["problems"] for g in groups]))
            continue
        with open(path, "w", encoding="utf-8") as f:
            f.write("".join(rnd.choice(CTX_ALPHABET if case.startswith("ctx") else RULES_ALPHABET) for _ in range(20000)))
        common = ["-object-cache-salt=" + salt, "--apply-shape-groups=0", "--rules-file=" + rules_path]
        regular = run(tconv, common + ["--use-ICU=0", "--divcode-groups=0", "--utf8-mode=0", path])
        icu = run(tconv, common + ["--use-ICU=.*", path])
        for mode in ("1", "0"):
            divcode = run(tconv, common + TRACE + ["--use-ICU=0", "--divcode-groups=1", "--utf8-mode=" + mode, path])
            what = "rules %s %s" % (case, "UTF-8" if b"_U8" in divcode.stderr else "Unicode")
            if regular.returncode or divcode.returncode:
                failures += 1
                print("FAIL %s: exit %d/%d %s" % (what, regular.returncode, divcode.returncode, divcode.stderr[-300:]))
            elif b"Div32CodeTranslate" not in divcode.stderr:
                failures += 1
                print("FAIL %s: no DivCode translation kernel" % what)
            elif divcode.stdout != regular.stdout or (icu.returncode == 0 and divcode.stdout != icu.stdout):
                failures += 1
                print("FAIL %s: divcode %s regular, %s ICU" % (what, "=" if divcode.stdout == regular.stdout else "!=",
                                                              "=" if divcode.stdout == icu.stdout else "!="))
            else:
                print("ok %s%s" % (what, "" if icu.returncode == 0 else " (no ICU)"))
    return failures


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, group in GROUPS:
            for shift in (5, 6):
                rnd = random.Random("%s %d %d" % (name, group, shift))
                path = os.path.join(tmp, "input.txt")
                # (The alphabet first, from a run on a small input.)
                with open(path, "w", encoding="utf-8") as f:
                    f.write("a\n")
                args = [tconv, "--divcode-codes=%d" % group, "--divcode-shift=%d" % shift, name, path]
                run = subprocess.run(args, capture_output=True)
                if b"unknown transform" in run.stderr:
                    print("tconv_divcode_test: skipped (no CLDR transforms directory)")
                    return
                if run.returncode != 0:
                    failures += 1
                    print("FAIL %s#%d s=%d: exit %d %s" % (name, group, shift, run.returncode, run.stderr[-300:]))
                    continue
                desc = json.loads(run.stderr.decode("utf-8").splitlines()[0])
                blocks = desc["blocks"]
                code_of = {b: i + 1 for i, b in enumerate(blocks)}
                externals = desc["externals"]
                # The text: characters of each block, of each external set, and others.
                chars = []
                for b in blocks:
                    for _ in range(4):
                        cp = (b << shift) | rnd.randrange(1 << shift)
                        if valid(cp):
                            chars.append(cp)
                for e in externals:
                    for _ in range(16):
                        cp = sample(rnd, e)
                        if valid(cp):
                            chars.append(cp)
                chars += [ord(c) for c in OTHER]
                for _ in range(64):
                    cp = rnd.choice([rnd.randint(0x20, 0x7FF), rnd.randint(0x800, 0xFFFF), rnd.randint(0x10000, 0x10FFFF)])
                    if valid(cp):
                        chars.append(cp)
                text = []
                for _ in range(20000):
                    text.append(rnd.choice(chars))
                    if rnd.random() < 0.02:
                        text.append(0x0A)
                text = "".join(chr(c) for c in text)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
                run = subprocess.run(args, capture_output=True)
                if run.returncode != 0:
                    failures += 1
                    print("FAIL %s#%d s=%d: exit %d %s" % (name, group, shift, run.returncode, run.stderr[-300:]))
                    continue
                out = run.stdout.decode("utf-8")
                if len(out) != len(text):
                    failures += 1
                    print("FAIL %s#%d s=%d: %d characters out, %d in" % (name, group, shift, len(out), len(text)))
                    continue
                bad = 0
                for i, (c, d) in enumerate(zip(text, out)):
                    cp = ord(c)
                    code = code_of.get(cp >> shift, 0)
                    ext = 0
                    for k, e in enumerate(externals):
                        if any(lo <= cp <= hi for lo, hi in e):
                            ext |= 1 << k
                    expected = 0x4000 | (ext << 10) | code
                    if ord(d) != expected:
                        if bad < 5:
                            print("FAIL %s#%d s=%d: U+%04X at %d: code %d, externals %d; expected %d, %d"
                                  % (name, group, shift, cp, i, (ord(d) - 0x4000) & 0x3FF, (ord(d) - 0x4000) >> 10, code, ext))
                        bad += 1
                if bad:
                    failures += 1
                else:
                    print("ok %s#%d s=%d: %d blocks, %d external sets (%s)" % (name, group, shift, len(blocks), len(externals),
                                                                              desc["kernel"]))
        failures += translation(tconv, tmp, "tconv_divcode_test")
    if failures:
        print("tconv_divcode_test: %d failures" % failures)
        sys.exit(1)
    print("tconv_divcode_test: passed")


if __name__ == "__main__":
    main()
