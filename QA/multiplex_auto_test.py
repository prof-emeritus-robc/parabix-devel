#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The choice of multiplexed codes or the 21-bit Unicode basis with
#  --re-multiplex=auto (see RE_PipelineBuilder::ensurePrepared): the decision
#  icgrep reports with -ShowREs, and the same lines selected with
#  --re-multiplex=off, auto and force.
#
#  multiplex_auto_test.py <icgrep binary>

import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

# More than 256 single-character CCs (codes of more than 8 streams), in a
# lookahead by reversal (with full Unicode indexing).
WIDE = "x(?=(?:" + "|".join(f"\\x{{{0x100 + 2 * i:X}}}\\x{{{0x1000 + 2 * i:X}}}" for i in range(300)) + ")+d)"

# (pattern, flags, a part of the decision reported)
CASES = [
    ("x(?=(?:ab|c)+d)", ["--re-multiplex=auto"], "Multiplexed: 6 codes, 3 streams"),
    ("x(?=(?:ab|c)+d)", ["--re-multiplex=auto", "--multiplexed-stream-limit=2"],
     "Not multiplexed: its codes need 3 streams, more than 2"),
    ("x(?=(?:ab|c)+d)", ["--re-multiplex=off"], "Not multiplexed: multiplexing is off"),
    ("(a)(?=[^q]*q)\\1", ["--re-multiplex=auto"], "Not multiplexed: the reference"),
    ("\\p{Greek}x(?=(?:ab|c)+d)", ["--re-multiplex=auto"], "Not multiplexed: the property"),
    (WIDE, ["--re-multiplex=auto"], "Not multiplexed: its codes need 10 streams, more than 8"),
    (WIDE, ["--re-multiplex=force"], "Multiplexed: "),
]

TEXT = "".join(line + "\n" for line in [
    "xabcd", "xcd", "xab", "αxcabd", "aqa", "abq", "aaq", "xĀကd", "xĂဂĀကd", "xĂd",
])


def run(icgrep, path, pattern, flags):
    proc = subprocess.run([icgrep, "-c", "-ShowREs"] + flags + [pattern, path], capture_output=True)
    decisions = [l for l in proc.stderr.decode(errors="replace").split("\n") if "ultiplexed" in l]
    return proc.returncode, proc.stdout.decode().strip(), decisions[0] if decisions else ""


def main():
    icgrep = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "text.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write(TEXT)
        jobs = [(p, flags) for p, flags, _ in CASES]
        # The lines selected, for each pattern, without multiplexing.
        jobs += [(p, ["--re-multiplex=off"]) for p in sorted({c[0] for c in CASES})]
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda j: run(icgrep, path, j[0], j[1]), jobs))
    reference = {p: r[1] for (p, flags), r in zip(jobs[len(CASES):], results[len(CASES):])}
    for (pattern, flags, decision), (status, count, reported) in zip(CASES, results):
        name = (pattern if len(pattern) < 40 else pattern[:37] + "...") + " " + " ".join(flags)
        if status not in (0, 1):
            failures.append(f"{name}: exit {status}")
        elif decision not in reported:
            failures.append(f"{name}: reported \"{reported}\", expected \"{decision}\"")
        elif count != reference[pattern]:
            failures.append(f"{name}: {count} lines, {reference[pattern]} without multiplexing")
    for f in failures:
        print("FAIL", f)
    if failures:
        sys.exit(1)
    print(f"multiplex_auto_test: {len(CASES)} decisions as expected")


if __name__ == "__main__":
    main()
