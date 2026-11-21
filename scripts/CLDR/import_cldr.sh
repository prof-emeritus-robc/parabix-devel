#!/usr/bin/env bash
#
#  import_cldr.sh <cldr-ref> <worktree>
#
#  Imports the CLDR transform data at <cldr-ref> (a commit, tag or branch of
#  https://github.com/unicode-org/cldr) into third_party/cldr of <worktree>,
#  which must be a checkout of the branch cldr-upstream, and commits it there.
#
#  cldr-upstream holds only pristine CLDR imports: no local changes are ever
#  committed to it.  Development branches merge it, and local fixes are commits
#  of their own on top, so that
#      git diff cldr-upstream -- third_party/cldr
#  shows every local change against CLDR (see third_party/cldr/UPSTREAM.md).
#
#  Imported paths (the same paths as in CLDR):
#      LICENSE, common/README.md, common/testData/README.md,
#      common/transforms/, common/testData/transforms/
#
#  The CLDR clone (blobless, so only the blobs of the imported files are
#  downloaded) is kept in $CLDR_CLONE (default ~/.cache/parabix/cldr-clone)
#  and fetched again on each run.
#
#  Typical use, from the development checkout:
#      git worktree add ../cldr-upstream-wt cldr-upstream
#      scripts/CLDR/import_cldr.sh release-49 ../cldr-upstream-wt
#      git worktree remove ../cldr-upstream-wt
#      git merge cldr-upstream
#
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <cldr-ref> <worktree on branch cldr-upstream>" >&2
    exit 2
fi
REF=$1
WT=$(cd "$2" && pwd)
URL=https://github.com/unicode-org/cldr.git
CLONE=${CLDR_CLONE:-$HOME/.cache/parabix/cldr-clone}
DEST=third_party/cldr
PATHS=(LICENSE common/README.md common/testData/README.md common/transforms common/testData/transforms)

branch=$(git -C "$WT" branch --show-current)
if [ "$branch" != cldr-upstream ]; then
    echo "error: $WT is on branch '$branch', not cldr-upstream" >&2
    exit 1
fi
if [ -n "$(git -C "$WT" status --porcelain -- "$DEST")" ]; then
    echo "error: $WT/$DEST has uncommitted changes" >&2
    exit 1
fi

if [ ! -d "$CLONE/.git" ]; then
    mkdir -p "$(dirname "$CLONE")"
    git clone --quiet --filter=blob:none --no-checkout "$URL" "$CLONE"
fi
git -C "$CLONE" fetch --quiet --tags origin '+refs/heads/*:refs/remotes/origin/*'
COMMIT=$(git -C "$CLONE" rev-parse --verify "$REF^{commit}" 2>/dev/null ||
         git -C "$CLONE" rev-parse --verify "origin/$REF^{commit}")
SHORT=$(git -C "$CLONE" rev-parse --short=12 "$COMMIT")
DATE=$(git -C "$CLONE" log -1 --format=%cs "$COMMIT")
DESCRIBE=$(git -C "$CLONE" describe --tags --match 'release-*' "$COMMIT" 2>/dev/null || echo "$SHORT")

#  Fetch the blobs of the imported files in one request (the clone is blobless,
#  and would otherwise fetch them one at a time).
git -C "$CLONE" ls-tree -r "$COMMIT" -- "${PATHS[@]}" | awk '{print $3}' |
    xargs git -C "$CLONE" -c gc.auto=0 -c fetch.negotiationAlgorithm=noop fetch --quiet \
        --no-tags --no-write-fetch-head --recurse-submodules=no --filter=blob:none origin

#  Replace the imported paths with their contents at COMMIT.  The blobs are
#  written as they are stored in CLDR (git cat-file, no attribute or line-end
#  conversion), so each file has the same blob hash as in CLDR.
for p in "${PATHS[@]}"; do rm -rf "${WT:?}/$DEST/$p"; done
git -C "$CLONE" ls-tree -r "$COMMIT" -- "${PATHS[@]}" | while IFS=$'\t' read -r meta path; do
    set -- $meta
    if [ "$2" != blob ]; then echo "error: $path is a $2" >&2; exit 1; fi
    mkdir -p "$WT/$DEST/$(dirname "$path")"
    git -C "$CLONE" cat-file blob "$3" > "$WT/$DEST/$path"
    if [ "$1" = 100755 ]; then chmod +x "$WT/$DEST/$path"; fi
done

#  Check: every imported file has its CLDR blob hash, and nothing else is there.
expected=$(git -C "$CLONE" ls-tree -r "$COMMIT" -- "${PATHS[@]}" | awk -F'\t' '{split($1,m," "); print m[3], $2}' | sort)
actual=$(cd "$WT/$DEST" && find "${PATHS[@]}" -type f | sort | while read -r f; do
    echo "$(git hash-object --no-filters "$f") $f"; done | sort)
if [ "$expected" != "$actual" ]; then
    echo "error: imported files differ from CLDR $SHORT:" >&2
    diff <(echo "$expected") <(echo "$actual") | head -20 >&2
    exit 1
fi
count=$(echo "$expected" | wc -l | tr -d ' ')

cat > "$WT/$DEST/UPSTREAM.md" <<EOF
# CLDR data imported from upstream

This directory holds files of the Unicode CLDR repository, at the same paths
as in CLDR (\`LICENSE\` is the CLDR license, which covers them):

| | |
|---|---|
| Repository | $URL |
| Commit | \`$COMMIT\` ($DESCRIBE, $DATE) |
| Paths | $(printf '`%s` ' "${PATHS[@]}") |
| Files | $count |

## Branches

- \`cldr-upstream\` holds only pristine imports, made by
  \`scripts/CLDR/import_cldr.sh <cldr-ref> <worktree>\`.  Nothing else is ever
  committed to it.
- Development branches merge \`cldr-upstream\`.  Local changes to these files
  are commits of their own on top (subject prefix \`cldr-local:\`, one fix per
  commit, naming the CLDR ticket if there is one).

## Reviewing local changes against CLDR

All local changes:

    git diff cldr-upstream -- third_party/cldr

As a patch for a CLDR checkout at the commit above:

    git diff cldr-upstream -- third_party/cldr | git -C <cldr> apply -p3

## Updating

    git worktree add ../cldr-upstream-wt cldr-upstream
    scripts/CLDR/import_cldr.sh <cldr-ref> ../cldr-upstream-wt
    git worktree remove ../cldr-upstream-wt
    git merge cldr-upstream

Conflicts arise only where a local change and an upstream change meet; a local
change that CLDR has adopted disappears from the diff after the merge.
EOF

git -C "$WT" add -A -- "$DEST"
if git -C "$WT" diff --cached --quiet -- "$DEST"; then
    echo "Nothing to import: $DEST already matches CLDR $SHORT."
    exit 0
fi
git -C "$WT" commit --quiet -m "Import CLDR $SHORT ($DESCRIBE, $DATE)

Pristine copy of $(printf '%s ' "${PATHS[@]}")from $URL
at $COMMIT ($count files), made by scripts/CLDR/import_cldr.sh." -- "$DEST"
echo "Imported CLDR $SHORT ($count files) on cldr-upstream: $(git -C "$WT" rev-parse --short HEAD)"
