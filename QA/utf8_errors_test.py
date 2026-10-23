#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Ill-formed UTF-8: the characters after an ill-formed sequence keep their
#  values (UTF8_Decoder), with the 21-bit Unicode basis and with
#  multiplexed codes (--re-multiplex=force).
#
#  utf8_errors_test.py <icgrep binary>

import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# (line bytes, pattern, expected count): lookaheads that need full Unicode
# indexing, after ill-formed sequences whose prefixes once gave their bits to
# the characters that follow them.
CASES = [
    (b"\xff\x80qd", "^..(?=qd)", 1),
    (b"\xff\x80qd", ".(?=..d)", 1),
    (b"\xff\xffqd", "^..(?=qd)", 1),
    (b"\xe6\x97\xa5\xffqd", "^日.(?=qd)", 1),
    (b"\xf0\x9f\x98\x80\xf5qd", "^..(?=qd)", 1),
    (b"aaqd", "^..(?=qd)", 1),
]

CONFIGURATIONS = [[], ["--re-multiplex=force"]]


def run(icgrep, path, pattern, flags):
    proc = subprocess.run([icgrep, "-c"] + flags + [pattern, path], capture_output=True)
    if proc.returncode not in (0, 1):
        return f"exit {proc.returncode}: {proc.stderr.decode(errors='replace')}"
    return int(proc.stdout.decode().strip() or "0")


def main():
    icgrep = sys.argv[1]
    errors = []
    with tempfile.TemporaryDirectory() as tmp:
        jobs = []
        for k, (line, pattern, expected) in enumerate(CASES):
            path = os.path.join(tmp, f"case{k}.txt")
            with open(path, "wb") as f:
                f.write(line + b"\n")
            for flags in CONFIGURATIONS:
                jobs.append((line, pattern, expected, path, flags))
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda j: run(icgrep, j[3], j[1], j[4]), jobs))
    for (line, pattern, expected, path, flags), got in zip(jobs, results):
        if got != expected:
            errors.append(f"{line!r} {pattern} {' '.join(flags)}: got {got}, expected {expected}")
    for e in errors:
        print("FAIL", e)
    if errors:
        sys.exit(1)
    print(f"utf8_errors_test: {len(jobs)} runs agree")


if __name__ == "__main__":
    main()
