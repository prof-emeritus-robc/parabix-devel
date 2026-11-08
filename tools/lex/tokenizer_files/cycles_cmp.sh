#!/bin/zsh
# Per-kernel time (-EnableCycleCounter; ticks = ns on Apple silicon) for several configs, side
# by side. Each config is the best config plus its extra flags, with cold tail caches; ROUNDS
# interleaved rounds of --bench-loop=LOOPS runs each; the first run of each round dropped, median
# of the rest per kernel. One pipeline at a time. A single short round can be off by 30% (a busy
# machine); at 4 threads times are summed over the threads, so use 1 thread to compare kernels.
#
# Usage: ./cycles_cmp.sh INPUT THREADS "name:extra flags" ["name:extra flags" ...]
#   ./cycles_cmp.sh val_4MB.txt 1 "best:" "L8:--tail-len-groups=8" "L8k:--tail-len-groups=8 --tail-len-kernels"
# INPUT: a path, or a file name in this folder (tokenizer_files).
# Env: LOOPS (default 20), ROUNDS (default 3), OUT (raw output folder, default $TMPDIR/cycles_cmp).
# Self-contained: the paths come from this script's location (tools/lex/tokenizer_files).

T=${0:A:h}                                   # tools/lex/tokenizer_files
R=${T:h:h:h}                                 # the repository
TOK=$R/build19/bin/tokenizer
PY=$R/.venv/bin/python; [[ -x $PY ]] || PY=python3
# The best config (the thesis configuration; same as BEST in config.sh), without
# --pretokenizer and --thread-num, and the cold-cache benchmark flags.
BEST="--skip-identity-normalize --memsource-block-capacity --pretok-compress-classes --pretok-class-code --pretok-byte-domain --pretok-utf8-classes --tail-from-bytes --tail-vocab-table --memsource-in-place --tail-long-cache --tail-scan-kernel --tail-scan-ir-cache --tail-batch-bpe --tail-hard-split --tail-simd-emit --tail-trigram-split --tail-long-admit=3"
COLD="--tail-cache-pool --bench-cold-cache"

if (( $# < 3 )); then
  sed -n '8,11p' $0 | sed 's/^# \{0,1\}//'
  exit 1
fi
INPUT=$1; TH=$2; shift 2
[[ -f $INPUT ]] || INPUT=$T/$INPUT
[[ -f $INPUT ]] || { echo "cycles_cmp: no input file $1"; exit 1; }
[[ -x $TOK ]] || { echo "cycles_cmp: no tokenizer at $TOK (build it: make -C build19 tokenizer)"; exit 1; }
LOOPS=${LOOPS:-20}
ROUNDS=${ROUNDS:-3}
D=${OUT:-${TMPDIR:-/tmp}/cycles_cmp}
mkdir -p $D
for spec in "$@"; do : > $D/${spec%%:*}_${INPUT:t:r}_t$TH.txt; done
for r in $(seq 1 $ROUNDS); do
  for spec in "$@"; do
    name=${spec%%:*}; extra=${spec#*:}
    $TOK --merges=$T/merges.txt --pretokenizer=bytelevel ${=BEST} ${=extra} --thread-num=$TH ${=COLD} \
        --bench-loop=$LOOPS -EnableCycleCounter $INPUT > /dev/null 2> $D/tmp.txt
    echo "ROUND $r" >> $D/${name}_${INPUT:t:r}_t$TH.txt
    cat $D/tmp.txt >> $D/${name}_${INPUT:t:r}_t$TH.txt
  done
done
rm -f $D/tmp.txt
files=()
for spec in "$@"; do files+=("${spec%%:*}=$D/${spec%%:*}_${INPUT:t:r}_t$TH.txt"); done
$PY - "${files[@]}" <<'EOF'
import re, sys, statistics
def short(n):
    n = n.split('+')[0]
    n = re.sub(r'_[0-9a-f]{16,}', '', n)                 # hashes in kernel names
    n = re.sub(r'^(cc)_[0-9a-f]+', r'\1 (char classes)', n)
    n = re.sub(r'^P[0-9a-f]{20,}$', 'TOTAL (pipeline)', n)
    return n
cfgs, order, data = [], [], {}
for arg in sys.argv[1:]:
    name, path = arg.split('=', 1)
    cfgs.append(name)
    runs, cur, first = [], None, False
    for line in open(path):
        if line.startswith('ROUND'):
            first = True; continue
        if line.startswith('CYCLE COUNTER'):
            cur = {}
            if not first: runs.append(cur)       # the first run of each round is dropped
            first = False
            continue
        m = re.match(r'\s*\d+\s+(\S+)\s+\d+\s+(\d+)\s', line)
        if m and cur is not None:
            k = short(m.group(1)); cur[k] = cur.get(k, 0) + int(m.group(2))
            if k not in order: order.append(k)
    for k in order:
        vals = [r[k] for r in runs if k in r]
        if vals: data[(name, k)] = statistics.median(vals) / 1e6
if not data:
    sys.exit("cycles_cmp: no cycle-counter tables (did the tokenizer fail? see the raw output)")
order = [k for k in order if k != 'TOTAL (pipeline)'] + ['TOTAL (pipeline)']
w = max(len(k) for k in order) + 2
print(f"{'kernel (ms, median)':{w}}" + ''.join(f"{c:>12}" for c in cfgs))
for k in order:
    if k == 'TOTAL (pipeline)': print('-' * (w + 12 * len(cfgs)))
    print(f"{k:{w}}" + ''.join(f"{data[(c, k)]:12.3f}" if (c, k) in data else f"{'-':>12}" for c in cfgs))
EOF
echo "raw output: $D"
