#!/bin/zsh
# Reproduce issue #26 part 3: crash in the greedy register allocator
# (HoistSpillHelper::runHoistSpills) when compiling the pipeline kernel of
# nfc/nfd/nfkd with -focus-layers=focus.  See doc/issue-26-regalloc-crash.md.
#
# usage: QA/focus/issue26_regalloc_repro.sh [build-dir ...]   (default: build18 build22)
# Run from the repository root.  Each run uses a fresh object cache, so the
# pipeline kernel is always compiled.

builds=("$@")
(( $#builds )) || builds=(build18 build22)
work=$(mktemp -d)
trap 'rm -rf $work' EXIT

python3 -c "import sys; sys.path.insert(0, 'QA/focus'); import focus_modes_test as f; open(sys.argv[1], 'wb').write(f.text().encode())" $work/fm.txt

check() {
    local b=$1 t=$2; shift 2
    rm -rf $work/cache; mkdir $work/cache
    $b/bin/$t --object-cache-dir=$work/cache "$@" $work/fm.txt > $work/out 2>/dev/null
    local rc=$?
    local ok=$(python3 -c "import unicodedata,sys; print(unicodedata.normalize(sys.argv[3], open(sys.argv[1], encoding='utf-8').read()).encode() == open(sys.argv[2], 'rb').read())" $work/fm.txt $work/out ${(U)t})
    printf '%-8s %-5s %-50s exit=%-3s correct=%s\n' $b $t "$*" $rc $ok
}

for b in $builds; do
    for t in nfc nfd nfkd; do
        check $b $t
        check $b $t -focus-layers=focus
        check $b $t -focus-layers=focus -regalloc=basic
        check $b $t -focus-layers=focus --backend-optimization-level=none
        check $b $t -focus-layers=focus -split-spill-mode=size
    done
done
