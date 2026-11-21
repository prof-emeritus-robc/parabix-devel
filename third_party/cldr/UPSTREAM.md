# CLDR data imported from upstream

This directory holds files of the Unicode CLDR repository, at the same paths
as in CLDR (`LICENSE` is the CLDR license, which covers them):

| | |
|---|---|
| Repository | https://github.com/unicode-org/cldr.git |
| Commit | `267bae66b10db620caca6d5df88c1719fc0fd457` (release-49-12-g267bae66b, 2026-11-19) |
| Paths | `LICENSE` `common/README.md` `common/testData/README.md` `common/transforms` `common/testData/transforms`  |
| Files | 675 |

## Branches

- `cldr-upstream` holds only pristine imports, made by
  `scripts/CLDR/import_cldr.sh <cldr-ref> <worktree>`.  Nothing else is ever
  committed to it.
- Development branches merge `cldr-upstream`.  Local changes to these files
  are commits of their own on top (subject prefix `cldr-local:`, one fix per
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
