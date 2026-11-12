#!/bin/sh
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
# Run by the freebsd job of ci.yml when tests fail: the CPU of the VM, and
# backtraces of programs that crash (SIGILL), run under lldb.
#
#   freebsd-crash-report.sh <build directory>

BUILD=$(cd "$1" && pwd)
LLDB=$(command -v lldb || ls /usr/local/llvm*/bin/lldb 2>/dev/null | head -1)

echo "=== CPU"
sysctl hw.model hw.ncpu
grep -E "Features|AMD Features|Structured Extended" /var/run/dmesg.boot | head -8
for LLC in /usr/local/llvm*/bin/llc; do
    "$LLC" --version | grep -i "host cpu"
done

# Runs a program under lldb: the backtrace, instruction and registers where it stops
crash_report() {
    echo "=== $*"
    if [ -z "$LLDB" ]; then
        echo "(no lldb)"
        "$@"
        echo "exit status $?"
        return
    fi
    "$LLDB" --batch \
        -o "process launch" \
        -o "thread backtrace all" \
        -o "disassemble --pc --count 12" \
        -o "register read" \
        -o "image lookup --verbose --address \$pc" \
        -o "source list --address \$pc --count 30" \
        -- "$@" 2>&1 | tail -150
}

WORK=$(mktemp -d)
python3 - "$WORK" <<'PY'
import sys
work = sys.argv[1]
with open(work + "/rules.txt", "w", encoding="utf-8") as f:
    f.write("$umedial = [ျ-ှ];\n($umedial*) ([ေ]+) ($umedial*) → $1 $3 $2;\n")
with open(work + "/input.txt", "w", encoding="utf-8") as f:
    f.write("ြေေှ\nေှ\n")
PY

cd "$BUILD/tests/bin" && crash_report ./test_emptyprogram
cd "$WORK" && crash_report "$BUILD/bin/tconv" --use-ICU=0 --lines --rules-file=rules.txt input.txt
# The same, with the generated code checked
cd "$WORK" && crash_report "$BUILD/bin/tconv" --use-ICU=0 --lines --rules-file=rules.txt -EnableAsserts -VerifyIR input.txt
rm -rf "$WORK"
