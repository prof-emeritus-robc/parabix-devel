#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The final partial stride of a kernel fed both from its own partition and
#  from another one (issue #44).  The kernels of a partition terminate with its
#  root, so the root's final invocation waits for the inputs that the other
#  kernels of its partition take from other partitions
#  (PipelineCompiler::partitionExternalInputsClosed).  Before, SelectStringMatches,
#  in the partition of one FusedRules kernel and fed by another in a partition
#  of its own that finished a segment later, never ran its final partial
#  stride, and the output stopped at the last multiple of the stride.
#
#  No rule matches the input, so the output must equal it, at lengths with and
#  without a partial final stride, in one thread and in four.
#
#  tconv_partition_final_stride_test.py <tconv binary> [--extended-testing]

import os
import subprocess
import sys
import tempfile

from tconv_test_mode import EXTENDED

# (rules, extra arguments): a filter, and the line regions as a mask
CASES = [
    (":: [a-z] ;\nabb → x ;\nbb } [^x] → c ;\n", []),
    ("ccb → a ;\ncc → ;\n", ["--lines"]),
]

LINES = [1, 81, 100, 1000] + ([333, 5003] if EXTENDED else [])
THREADS = [[], ["-thread-num=4", "-segment-size=1"]]


def main():
    tconv = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        rules = os.path.join(tmp, "rules.txt")
        input = os.path.join(tmp, "input.txt")
        for rules_text, extra in CASES:
            with open(rules, "w", encoding="utf-8") as f:
                f.write(rules_text)
            for n in LINES:
                text = "".join("xaxaxax\n" for _ in range(n))
                with open(input, "w", encoding="utf-8") as f:
                    f.write(text)
                for threads in THREADS:
                    args = [tconv, "--use-ICU=0"] + threads + extra + ["--rules-file=" + rules, input]
                    run = subprocess.run(args, capture_output=True)
                    got = run.stdout.decode("utf-8", "replace")
                    if run.returncode != 0 or got != text:
                        failures += 1
                        print("FAIL %r %s, %d lines: exit %d, %d of %d bytes"
                              % (rules_text, " ".join(threads + extra), n, run.returncode, len(got), len(text)))
    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
