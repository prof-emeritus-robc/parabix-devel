#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Input files of wc, ucount and matchprior: standard input (by default and
#  as "-"), no row for a file that cannot be read (with an error message,
#  silent with -s) and exit status 1, a total whenever more than one file is
#  selected (also with -r), and the file selection options shown by --help.
#
#  wc_files_test.py <bin directory>

import os
import subprocess
import sys
import tempfile


def run(binary, args, cwd, stdin=b""):
    proc = subprocess.run([binary] + args, cwd=cwd, input=stdin, capture_output=True, timeout=20)
    rows = [line.split() for line in proc.stdout.decode().splitlines()]
    return rows, proc.stderr.decode(), proc.returncode


def main():
    bindir = os.path.abspath(sys.argv[1])
    wc, ucount, matchprior = (os.path.join(bindir, t) for t in ("wc", "ucount", "matchprior"))
    failures = []
    def check(condition, message):
        if not condition:
            failures.append(message)
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "dir/sub"))
        with open(os.path.join(d, "x.txt"), "w") as out:
            out.write("a b\n")
        with open(os.path.join(d, "dir/y.txt"), "w") as out:
            out.write("c\n")
        with open(os.path.join(d, "dir/sub/z.txt"), "w") as out:
            out.write("d e\n")

        rows, err, status = run(wc, [], d, b"a b\nc\n")
        check(rows == [["2", "3", "6"]] and status == 0, "wc on standard input: %s %d" % (rows, status))
        rows, err, status = run(wc, ["-", "x.txt"], d, b"a b\nc\n")
        check(rows == [["2", "3", "6", "-"], ["1", "2", "4", "x.txt"], ["3", "5", "10", "total"]],
              "wc - x.txt: %s" % rows)
        rows, err, status = run(wc, ["nosuch", "x.txt"], d)
        check(rows == [["1", "2", "4", "x.txt"], ["1", "2", "4", "total"]], "wc nosuch x.txt: %s" % rows)
        check(err == "wc: nosuch: No such file.\n" and status == 1, "wc nosuch x.txt: %r %d" % (err, status))
        rows, err, status = run(wc, ["-s", "nosuch", "x.txt"], d)
        check(err == "" and status == 1, "wc -s nosuch x.txt: %r %d" % (err, status))
        rows, err, status = run(wc, ["dir"], d)
        check(rows == [] and "Is a directory" in err and status == 1, "wc dir: %s %r %d" % (rows, err, status))
        rows, err, status = run(ucount, ["\\p{L}", "-r", "dir"], d)
        check(sorted(rows) == [["1", "dir/y.txt"], ["2", "dir/sub/z.txt"], ["3", "total"]],
              "ucount -r dir: %s" % rows)
        rows, err, status = run(ucount, ["\\p{L}"], d, b"ab\n")
        check(rows == [["2"]], "ucount on standard input: %s" % rows)
        rows, err, status = run(matchprior, ["nosuch"], d)
        check(err == "matchprior: nosuch: No such file.\n" and status == 1, "matchprior nosuch: %r %d" % (err, status))
        for tool in (wc, ucount, matchprior):
            help_text = subprocess.run([tool, "--help"], capture_output=True).stdout.decode()
            check(" -r " in help_text and "--include" in help_text,
                  "%s --help does not show the file selection options" % os.path.basename(tool))
    for f in failures:
        print("FAIL:", f)
    if failures:
        sys.exit(1)
    print("wc_files_test: all checks passed")


if __name__ == "__main__":
    main()
