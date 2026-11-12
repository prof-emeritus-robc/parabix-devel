#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  The input and output conventions of the Parabix tools (toolchain/toolio.h):
#  - the same results from an input file, the standard input (a file, a pipe,
#    one whose data comes late, "-"), with --mmap, and to --output (when nothing goes to the standard
#    output);
#  - an input that cannot be read is reported (tool: file: reason) on the
#    standard error, with a nonzero exit status and no output;
#  - icgrep colors its output only on a terminal (--colors=auto), not in a
#    file of --output nor with NO_COLOR or TERM=dumb;
#  - icgrep searches the standard input ("-") in its place among the files,
#    labeled "(standard input)" (or --label) in every output mode.
#  The tools not in the directory are skipped (make installcheck runs this
#  on those installed).
#
#  tool_io_test.py <directory of the tools>

import os
import subprocess
import sys
import tempfile
import time

failures = 0


def fail(case, message):
    global failures
    failures += 1
    print("FAIL %s: %s" % (case, message))


TEXT = ("The quick brown fox jumps over the lazy dog.\n"
        "Étude, naïve café; Ελληνικά 漢字 ﬁ ½ Å\n"
        "МОСКВА столица\n").encode() * 3000
CSV = b"date,county,state,fips\n" + b"".join(b"2020-0%d-1%d,Snohomish,Washington,53061\n2020-0%d-1%d,Cook,Illinois,17031\n"
                                             % (m, d, m, d) for m in range(1, 10) for d in range(10)) * 20
U32 = TEXT.decode().encode("utf-32-be")

# (tool, arguments, input, results printed with the file names)
CASES = [
    ("icgrep", ["fox"], TEXT, False),
    ("icgrep", ["-c", "\\p{Cyrillic}+"], TEXT, False),
    ("icgrep", ["-n", "--colors=always", "caf."], TEXT, False),
    ("wc", [], TEXT, True),
    ("wc", ["-l", "-m"], TEXT, True),
    ("base64", [], TEXT, False),
    ("csvcut", ["--columns=1,3"], CSV, False),
    ("csvgrep", ["--regex=Wash", "--columns=3"], CSV, False),
    ("csv2json", [], CSV, False),
    ("tconv", ["Any-Lower"], TEXT, False),
    ("tconv", ["Any-Hex"], TEXT, False),
    ("hexlines", [], TEXT, False),
    ("gb18030", [], b"plain ascii text\n" * 3000, False),
    ("nfc", [], TEXT, False),
    ("nfd", [], TEXT, False),
    ("nfkd", [], TEXT, False),
    ("u8u16", [], TEXT, False),
    ("x8u16", [], TEXT, False),
    ("u32u8", [], U32, False),
    ("u32u16", [], U32, False),
    ("ucount", ["\\p{L}"], TEXT, True),
    ("ufilter", ["\\p{Greek}"], TEXT, False),
    ("matchprior", [], TEXT, True),
    ("xch", ["-src=abc", "-tgt=xyz"], TEXT, False),
    ("ztf-runs", [], TEXT, False),
]

# The tools reading a single input (reporting one that cannot be read)
SINGLE_INPUT = ["csvcut", "hexlines", "nfc", "nfd", "u8u16", "u32u8", "ufilter", "tconv"]


def without_names(output, path):
    """The results without the name of the file (or "-") after the counts."""
    lines = output.decode(errors="replace").replace(path, "").splitlines()
    return [l[:-2].rstrip() if l.endswith(" -") else l.rstrip() for l in lines]


def check_tool(bin_dir, tmp, tool, args, data, named):
    prog = [os.path.join(bin_dir, tool)] + args
    path = os.path.join(tmp, "input")
    with open(path, "wb") as f:
        f.write(data)
    label = " ".join([tool] + args)

    def run(extra, **kw):
        r = subprocess.run(prog + extra, capture_output=True, **kw)
        return r.returncode, r.stdout, r.stderr

    def run_late(extra):
        """With the input on a pipe holding nothing when the tool starts (the
        size of a pipe, as some systems give it, is no estimate of the input)."""
        p = subprocess.Popen(prog + extra, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.2)
        stdout, stderr = p.communicate(data, timeout=120)
        return p.returncode, stdout, stderr

    status, expected, err = run([path])
    if status != 0 or not expected:
        fail(label, "from a file: status %d, %d bytes: %s" % (status, len(expected), err[:300]))
        return
    out = os.path.join(tmp, "output")
    if os.path.exists(out):
        os.unlink(out)
    with open(path, "rb") as f:
        redirected = run([], stdin=f)
    with open(path, "rb") as f:
        dash = run(["-"], stdin=f)
    runs = {
        "standard input": redirected,
        "pipe": run([], input=data),
        "pipe, data late": run_late(["-read-ahead=false"]),
        "-": dash,
        "--mmap": run(["--mmap", path]),
    }
    status, stdout, err = run(["--output=" + out, path])
    if stdout:
        fail(label + " --output", "%d bytes on the standard output" % len(stdout))
    runs["--output"] = (status, open(out, "rb").read() if os.path.exists(out) else b"", err)
    for name, (status, output, err) in runs.items():
        same = (without_names(output, path) == without_names(expected, path)) if named else (output == expected)
        if status != 0 or not same:
            fail(label + " (" + name + ")", "status %d, %d bytes (%d expected): %s" % (status, len(output), len(expected), err[:300]))


def check_errors(bin_dir, tmp, tool, args):
    missing = os.path.join(tmp, "missing")
    r = subprocess.run([os.path.join(bin_dir, tool)] + args + [missing], capture_output=True)
    message = r.stderr.decode(errors="replace")
    if r.returncode == 0 or r.stdout or not message.startswith(tool + ": ") or "No such file or directory" not in message:
        fail(tool + " (missing input)", "status %d, %d bytes of output, message %r" % (r.returncode, len(r.stdout), message[:200]))


def on_terminal(command, env=None):
    """The output of a command whose standard output is a terminal."""
    import pty
    master, slave = pty.openpty()
    p = subprocess.Popen(command, stdout=slave, stderr=subprocess.DEVNULL, env=env)
    os.close(slave)
    output = b""
    while True:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
    p.wait()
    os.close(master)
    return output


def check_colors(bin_dir, tmp):
    icgrep = os.path.join(bin_dir, "icgrep")
    path = os.path.join(tmp, "input")
    with open(path, "wb") as f:
        f.write(TEXT)
    escape = b"\x1b["
    if escape not in on_terminal([icgrep, "fox", path]):
        fail("icgrep colors", "no colors on a terminal")
    env = dict(os.environ, NO_COLOR="1")
    if escape in on_terminal([icgrep, "fox", path], env):
        fail("icgrep colors", "colors on a terminal with NO_COLOR")
    env = dict(os.environ, TERM="dumb")
    if escape in on_terminal([icgrep, "fox", path], env):
        fail("icgrep colors", "colors on a terminal with TERM=dumb")
    out = os.path.join(tmp, "colors")
    on_terminal([icgrep, "--output=" + out, "fox", path])
    if escape in open(out, "rb").read():
        fail("icgrep colors", "colors in the file of --output (the standard output a terminal)")
    if escape in subprocess.run([icgrep, "fox", path], capture_output=True).stdout:
        fail("icgrep colors", "colors on a pipe")
    if escape not in subprocess.run([icgrep, "--colors=always", "fox", path], capture_output=True).stdout:
        fail("icgrep colors", "no colors with --colors=always")


def check_stdin_place(bin_dir, tmp):
    icgrep = os.path.join(bin_dir, "icgrep")
    a = os.path.join(tmp, "a")
    b = os.path.join(tmp, "b")
    with open(a, "wb") as f:
        f.write(b"apple\nbanana\n")
    with open(b, "wb") as f:
        f.write(b"apple pie\ncherry\n")
    stdin = b"apple tart\nplum\n"
    S = "(standard input)"
    cases = [
        ([], [a, "-", b], "%s:apple\n%s:apple tart\n%s:apple pie\n" % (a, S, b)),
        (["-n"], ["-", a], "%s:1:apple tart\n%s:1:apple\n" % (S, a)),
        (["-c"], [a, "-", b], "%s:1\n%s:1\n%s:1\n" % (a, S, b)),
        (["-l"], [a, "-"], "%s\n%s\n" % (a, S)),
        (["-H"], [], "%s:apple tart\n" % S),
        (["-c", "--label=IN"], ["-", b], "IN:1\n%s:1\n" % b),
    ]
    for flags, files, expected in cases:
        for threads in ["1", "4"]:
            command = [icgrep, "-thread-num=" + threads] + flags + ["apple"] + files
            result = subprocess.run(command, input=stdin, capture_output=True)
            if result.stdout.decode() != expected:
                fail("icgrep stdin in place " + " ".join(command[1:]),
                     "output %r, expected %r" % (result.stdout.decode(), expected))


def main():
    bin_dir = sys.argv[1]
    present = lambda tool: os.path.exists(os.path.join(bin_dir, tool))
    tested = 0
    with tempfile.TemporaryDirectory() as tmp:
        for tool, args, data, named in CASES:
            if present(tool):
                check_tool(bin_dir, tmp, tool, args, data, named)
                tested += 1
        for tool in SINGLE_INPUT:
            if present(tool):
                args = next(a for t, a, d, n in CASES if t == tool)
                check_errors(bin_dir, tmp, tool, args)
        if present("icgrep"):
            check_colors(bin_dir, tmp)
            check_stdin_place(bin_dir, tmp)
    print("%d cases, %d failures" % (tested, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
