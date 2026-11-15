#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  tconv's UTF-8 mode: a transform whose steps are context-free character
#  maps (and Any-Null) is applied to the UTF-8 code units of the text (see
#  kernel::UTF8_CharMapPipeline), by default.  For a few such transforms,
#  tconv --plan must report UTF-8 mode, and the output must be identical to
#  that of Unicode mode (--utf8-mode=0) on text with characters of 1 to 4
#  bytes, replacements longer, shorter and empty, and ill-formed UTF-8, with
#  each treatment of ill-formed input.  A transform with a filter must be in
#  Unicode mode.  (Skipped without the CLDR transforms.)
#
#  tconv_utf8_mode_test.py <tconv binary>

import os
import random
import subprocess
import sys
import tempfile

UTF8 = {
    # transform: characters it maps (some of them)
    "Publishing-Any": "“”‘’⟷⟵⟶↔←→≠≤≥±…©®™½¾⅞",
    "Geor-Latn": "აბგდევზთიკლმნოპჟრსტუფქღყშჩცძწჭხჯჰ",
    "Ethi-sgw_Ethi/Gurage_2013": "\U0001E7ED\U0001E7EE\U0001E7F0\U0001E7F1",
    "NumericPinyin-Pinyin": "12345",
    "Fullwidth-Halfwidth": "ガギグゲゴアイウエオ、。",
    # a single-character rule with contexts ($thai { ' ' } $thai), matched by
    # the regular expression engine on the UTF-8 code units
    "Thai-ThaiSemi": "กขคง    ",
    # normalizations (NFD, a map, NFC), Any-Lower (Final_Sigma), and a
    # filter with normalizations within it
    "Accents-Any": "éüñÅốệ\u0301\u0308\u0323",
    "lt-Lower": "ĮĖĄÌÍĨIJΣΑΒσς\u0307\u0300",
    "Deva-Beng": "कखगघचजटडतदनपबमयरलवशसह़ािीुूेैोौं\u0301",
}
UNICODE = ["Hira-Latn"]
OTHER = "aZ 09.,é€中😀𐍈\u0301"
BAD = [b"\x80", b"\xe2\x80", b"\xf0\x9f", b"\xff", b"\xc3", b"\xed\xa0\x80", b"\xf4\x90"]
ERRORS = ["use-replacement-char", "drop-bad-characters"]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, chars in UTF8.items():
            plan = subprocess.run([tconv, "--plan", name], capture_output=True)
            if b"unknown transform" in plan.stderr:
                print("tconv_utf8_mode_test: skipped (no CLDR transforms directory)")
                return
            if b"  mode: UTF-8" not in plan.stdout:
                failures += 1
                print("FAIL %s: not in UTF-8 mode: %r" % (name, plan.stdout[-300:]))
                continue
            rnd = random.Random(name)
            alphabet = list(chars) + list(OTHER)
            data = b""
            for _ in range(300):
                line = "".join(rnd.choice(alphabet) for _ in range(rnd.randint(0, 30))).encode("utf-8")
                if rnd.random() < 0.3:
                    k = rnd.randint(0, len(line))
                    line = line[:k] + rnd.choice(BAD) + line[k:]
                data += line + b"\n"
            path = os.path.join(tmp, "input.txt")
            with open(path, "wb") as f:
                f.write(data)
            for errors in ERRORS:
                out = {}
                for mode in ("1", "0"):
                    r = subprocess.run([tconv, "--utf8-mode=" + mode, "--utf8-errors=" + errors, name, path],
                                       capture_output=True)
                    if r.returncode != 0:
                        failures += 1
                        print("FAIL %s --utf8-mode=%s --utf8-errors=%s: exit %d %s"
                              % (name, mode, errors, r.returncode, r.stderr.decode("utf-8", "replace")[:300]))
                    out[mode] = r.stdout
                if out["1"] != out["0"]:
                    failures += 1
                    a, b = out["1"].split(b"\n"), out["0"].split(b"\n")
                    i = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                    print("FAIL %s --utf8-errors=%s: UTF-8 mode differs from Unicode mode at line %d:\n  %r\n  %r"
                          % (name, errors, i + 1, a[i] if i < len(a) else None, b[i] if i < len(b) else None))
        for name in UNICODE:
            plan = subprocess.run([tconv, "--plan", name], capture_output=True).stdout
            if b"  mode: Unicode" not in plan:
                failures += 1
                print("FAIL %s: not in Unicode mode" % name)
    if failures:
        sys.exit(1)
    print("tconv_utf8_mode_test: all tests passed")


if __name__ == "__main__":
    main()
