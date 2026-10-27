#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  File selection in recursive search (-r, -R): .gitignore patterns anchored
#  to their directory ("/name", "!/name", "a/b", "a/**/b"), an empty
#  .gitignore, symlinks found while recursing (followed only with -R),
#  directory loops (-R), FIFOs (skipped unless -D read), an unreadable
#  directory with a .gitignore (whose patterns must not apply elsewhere),
#  and -TraceFileSelect output.
#
#  recursive_select_test.py <icgrep binary>

import os
import subprocess
import sys
import tempfile

FILES = [
    "root/build/f.txt",         # excluded: /build
    "root/sub/build/f.txt",     # /build is anchored to root
    "root/other.log",           # excluded: *.log
    "root/keep.log",            # re-included: !/keep.log
    "root/sub/keep.log",        # excluded: !/keep.log is anchored to root
    "root/x/frotz",             # excluded: x/frotz
    "root/doc/x/frotz",         # x/frotz is anchored to root
    "root/doc/deep",            # excluded: doc/**/deep
    "root/doc/a/b/deep",        # excluded: doc/**/deep
    "root/deep",
    "outside.txt",
    "zz/f.txt",                 # the *.txt of locked/.gitignore must not apply
] + ["empty/file%02d.txt" % i for i in range(40)]

GITIGNORE = "/build\n*.log\n!/keep.log\nx/frotz\ndoc/**/deep\n"


def run(icgrep, args, cwd):
    try:
        proc = subprocess.run([icgrep] + args, cwd=cwd, capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        # e.g. blocked reading root/fifo
        return "", "timed out"
    return proc.stdout.decode(), proc.stderr.decode()


def main():
    icgrep = os.path.abspath(sys.argv[1])
    failures = []
    def check(condition, message):
        if not condition:
            failures.append(message)
    with tempfile.TemporaryDirectory() as d:
        for f in FILES:
            os.makedirs(os.path.join(d, os.path.dirname(f)), exist_ok=True)
            with open(os.path.join(d, f), "w") as out:
                out.write("hit\n")
        with open(os.path.join(d, "root/.gitignore"), "w") as out:
            out.write(GITIGNORE)
        open(os.path.join(d, "empty/.gitignore"), "w").close()
        os.makedirs(os.path.join(d, "locked"))
        with open(os.path.join(d, "locked/.gitignore"), "w") as out:
            out.write("*.txt\n")
        os.chmod(os.path.join(d, "locked"), 0o311)
        os.symlink("../outside.txt", os.path.join(d, "root/link.txt"))
        os.symlink("..", os.path.join(d, "root/sub/loop"))
        os.mkfifo(os.path.join(d, "root/fifo"))
        try:
            out, err = run(icgrep, ["-r", "-l", "-TraceFileSelect", "hit", "locked", "root", "empty", "zz"], d)
            # (the error message for locked/ is on stdout)
            found = sorted(line for line in out.splitlines() if not line.startswith("icgrep:"))
            expected = sorted(["root/sub/build/f.txt", "root/keep.log", "root/doc/x/frotz", "root/deep",
                               "zz/f.txt"] + [f for f in FILES if f.startswith("empty/")])
            check(found == expected, "-r selected %s, expected %s" % (found, expected))
            traced = [line for line in err.splitlines() if line.startswith("Selecting:")]
            check(traced and all(line != "Selecting: " for line in traced),
                  "-TraceFileSelect printed empty names: %s" % traced)
            out, err = run(icgrep, ["-R", "-l", "hit", "root"], d)
            check("root/link.txt" in out.split(), "-R did not follow root/link.txt")
            check("recursive directory loop" in err, "-R did not report the loop root/sub/loop")
        finally:
            os.chmod(os.path.join(d, "locked"), 0o755)
    for f in failures:
        print("FAIL:", f)
    if failures:
        sys.exit(1)
    print("recursive_select_test: all checks passed")


if __name__ == "__main__":
    main()
