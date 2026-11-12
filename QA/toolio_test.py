#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The input and output conventions of the Parabix tools (toolchain/toolio.h),
#  tested on toolio_cat (tests/toolio_cat.cpp), a cat program of them:
#  input from a file, "-" or the standard input (a file, or a pipe), read or
#  mapped (--mmap); output to the standard output or --output (written under
#  a temporary name and renamed into place, but for files that are not
#  regular); messages on the standard error only.
#
#  toolio_test.py <toolio_cat binary>

import os
import stat
import subprocess
import sys
import tempfile

failures = 0


def fail(case, message):
    global failures
    failures += 1
    print("FAIL %s: %s" % (case, message))


def run(cat, args, stdin=None, input=None):
    return subprocess.run([cat] + args, stdin=stdin, input=input, capture_output=True)


def first_difference(got, expected):
    for i, (a, b) in enumerate(zip(got, expected)):
        if a != b:
            end = i
            while end < min(len(got), len(expected)) and got[end] != expected[end]:
                end += 1
            return "first difference at byte %d (page %d, offset %d), %d bytes differ from there: %r, expected %r" % (
                i, i // 4096, i % 4096, end - i, got[i:i + 16], expected[i:i + 16])
    return "the shorter is a prefix of the other"


def expect_output(case, result, expected, stdout=True):
    """The run succeeded, wrote expected (to stdout if stdout), and no messages."""
    if result.returncode != 0:
        fail(case, "exit status %d: %s" % (result.returncode, result.stderr.decode(errors="replace")[:300]))
        return
    if result.stderr:
        fail(case, "messages: %s" % result.stderr.decode(errors="replace")[:300])
    if stdout and result.stdout != expected:
        fail(case, "%d bytes written, %d expected; %s" % (len(result.stdout), len(expected),
                                                         first_difference(result.stdout, expected)))
    if not stdout and result.stdout:
        fail(case, "%d bytes written to the standard output" % len(result.stdout))


def expect_error(case, result, text):
    """The run failed, reporting text (prefixed by the program name), and wrote nothing."""
    message = result.stderr.decode(errors="replace")
    if result.returncode == 0:
        fail(case, "succeeded")
    if not message.startswith("toolio_cat: ") or text not in message:
        fail(case, "message %r, expected one with %r" % (message[:300], text))
    if result.stdout:
        fail(case, "%d bytes written to the standard output" % len(result.stdout))


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    cat = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        def path(name):
            return os.path.join(tmp, name)

        # Data of several segments, every byte value
        data = bytes((i * 7 + i // 251) % 256 for i in range(1 << 20)) + b"the end\n"
        inputs = {"large": data, "small": b"hello, world\n", "empty": b""}
        for name, content in inputs.items():
            with open(path(name), "wb") as f:
                f.write(content)

        for name, content in inputs.items():
            for mmap in ([], ["--mmap"]):
                tag = "%s%s" % (name, " --mmap" if mmap else "")
                expect_output("file " + tag, run(cat, mmap + [path(name)]), content)
                with open(path(name), "rb") as f:
                    expect_output("stdin from a file " + tag, run(cat, mmap, stdin=f), content)
                with open(path(name), "rb") as f:
                    expect_output("- from a file " + tag, run(cat, mmap + ["-"], stdin=f), content)
                expect_output("stdin from a pipe " + tag, run(cat, mmap, input=content), content)
                expect_output("- from a pipe " + tag, run(cat, mmap + ["-"], input=content), content)

        # Mapped files of whole pages and part of a block (the final partial
        # block at a page boundary), with one thread and several
        for size in (16384 + 8, 3 * 16384 + 100, (1 << 20) + 1):
            mapped = data[:size]
            with open(path("pages"), "wb") as f:
                f.write(mapped)
            for threads in ("1", "4"):
                expect_output("--mmap, %d bytes, %s threads" % (size, threads),
                              run(cat, ["--mmap", "-thread-num=" + threads, path("pages")]), mapped)

        # Read-ahead (a pipe read by a thread while compiling): turned off, and
        # beyond its 64 MiB limit (the thread waiting for the program)
        expect_output("pipe -read-ahead=false", run(cat, ["-read-ahead=false"], input=data), data)
        huge = data * 70
        expect_output("pipe of 70 MiB", run(cat, [], input=huge), huge)

        # The standard input read in part already: the rest (not mapped from the start)
        for mmap in ([], ["--mmap"]):
            with open(path("large"), "rb") as f:
                f.seek(100)
                expect_output("stdin after 100 bytes" + (" --mmap" if mmap else ""),
                              run(cat, mmap, stdin=f), data[100:])

        # --output
        out = path("out")
        expect_output("--output", run(cat, [path("large"), "--output=" + out]), b"", stdout=False)
        if read(out) != data:
            fail("--output", "the file differs from the input")
        expect_output("--output=-", run(cat, [path("small"), "--output=-"]), inputs["small"])
        expect_output("--output, from a pipe", run(cat, ["--output=" + out], input=inputs["small"]), b"", stdout=False)
        if read(out) != inputs["small"]:
            fail("--output, from a pipe", "the file differs from the input")
        leftovers = [f for f in os.listdir(tmp) if f.startswith(".")]
        if leftovers:
            fail("--output", "temporary files left: %s" % leftovers)

        # Results written by C++ (toolio::out) follow the pipeline's
        expect_output("--trailer", run(cat, [path("small"), "--trailer=END"]), inputs["small"] + b"END")
        run(cat, [path("small"), "--trailer=END", "--output=" + out])
        if read(out) != inputs["small"] + b"END":
            fail("--trailer --output", "the file holds %r" % read(out))

        # The output file may be the input
        same = path("same")
        with open(same, "wb") as f:
            f.write(data)
        expect_output("--output the input", run(cat, [same, "--output=" + same]), b"", stdout=False)
        if read(same) != data:
            fail("--output the input", "the file holds %d bytes, %d expected" % (len(read(same)), len(data)))

        # The permissions of a file replaced, and of a new file
        os.chmod(out, 0o640)
        run(cat, [path("small"), "--output=" + out])
        if stat.S_IMODE(os.stat(out).st_mode) != 0o640:
            fail("permissions of a replaced file", "%o, expected 640" % stat.S_IMODE(os.stat(out).st_mode))
        mask = os.umask(0)
        os.umask(mask)
        new = path("new")
        run(cat, [path("small"), "--output=" + new])
        if stat.S_IMODE(os.stat(new).st_mode) != 0o666 & ~mask:
            fail("permissions of a new file", "%o, expected %o" % (stat.S_IMODE(os.stat(new).st_mode), 0o666 & ~mask))

        # A symbolic link stays one: the file it refers to is replaced
        link = path("link")
        os.symlink(new, link)
        run(cat, [path("large"), "--output=" + link])
        if not os.path.islink(link) or read(new) != data:
            fail("--output a symbolic link", "the link or its file was not kept")

        # A file that is not regular is written in place
        result = run(cat, [path("small"), "--output=/dev/null"])
        expect_output("--output=/dev/null", result, b"", stdout=False)
        if not stat.S_ISCHR(os.stat("/dev/null").st_mode):
            fail("--output=/dev/null", "/dev/null is no longer a device")

        # Errors: reported on the standard error, nothing written
        expect_error("no input file", run(cat, [path("missing")]), "missing: No such file or directory")
        expect_error("a directory as input", run(cat, [tmp]), "Is a directory")
        expect_error("--output in a missing directory",
                     run(cat, [path("small"), "--output=" + path("nodir/out")]), "No such file or directory")
        expect_error("--output a directory", run(cat, [path("small"), "--output=" + tmp]), "Is a directory")
        readonly = path("readonly")
        with open(readonly, "wb") as f:
            f.write(b"kept\n")
        os.chmod(readonly, 0o444)
        if os.geteuid() != 0:
            expect_error("--output a read-only file", run(cat, [path("small"), "--output=" + readonly]),
                         "Permission denied")
            if read(readonly) != b"kept\n":
                fail("--output a read-only file", "the file was changed")
        if os.path.exists("/dev/full"):
            expect_error("write error", run(cat, [path("large"), "--output=/dev/full"]), "No space left on device")

        # A reader that stops early (as head does): the program ends, with no message
        proc = subprocess.Popen([cat, path("large")], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        proc.stdout.read(10)
        proc.stdout.close()
        stderr = proc.stderr.read()
        proc.wait(timeout=60)
        if stderr:
            fail("a reader stopping early", "messages: %s" % stderr.decode(errors="replace")[:300])

    print("%d failures" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
