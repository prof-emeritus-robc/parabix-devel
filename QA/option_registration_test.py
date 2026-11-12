#
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  Every program registers its command-line options, and those of the
#  libraries it is linked with, when it is loaded.  An option name defined
#  twice (e.g., a tool's -d and a library's -d) makes LLVM abort with
#  "Option 'd' registered more than once!" before main.  With
#  ARTIFACT_MODE=Single every program loads every library, so the options of
#  all libraries must have distinct names, and the names of a tool's own
#  options must differ from them.  Options for some tools only (file
#  selection, CSV) are created on demand by those tools (see
#  argv::RegisterFileSelectOptions).
#
#  Runs each program of the given directories with --version and reports
#  those whose options collide.
#
#  option_registration_test.py <bin directory>...

import os
import subprocess
import sys

# Not a command-line tool: the object cache cleanup daemon is handed a cache
# path.  It is built in <build>/libexec/parabix, but older build trees have
# it in bin.
SKIP = {"cachejanitord"}


def main():
    failures = []
    checked = 0
    for bindir in sys.argv[1:]:
        if not os.path.isdir(bindir):
            continue
        for name in sorted(os.listdir(bindir)):
            path = os.path.join(bindir, name)
            if name in SKIP or not os.path.isfile(path) or not os.access(path, os.X_OK):
                continue
            proc = subprocess.run([path, "--version"], capture_output=True, timeout=60)
            checked += 1
            stderr = proc.stderr.decode(errors="replace")
            if "registered more than once" in stderr:
                messages = [line for line in stderr.splitlines() if "registered more than once" in line]
                failures.append(name + ": " + "; ".join(sorted(set(messages))))
    for failure in failures:
        print(failure)
    print("{} programs checked, {} with colliding options".format(checked, len(failures)))
    sys.exit(1 if failures or checked == 0 else 0)


if __name__ == "__main__":
    main()
