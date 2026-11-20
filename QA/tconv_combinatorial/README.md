# Combinatorial testing of tconv

`tconv_ct.py` tests tconv with t-way covering arrays (`covering.py`, the IPOG
strategy that NIST's ACTS uses) instead of sweeps over all transforms. Every
pair of parameter values (or every triple, at t=3) is covered by some test case
in each pass, and each pass draws different transforms and inputs.

## The model

| Parameter | Values |
|---|---|
| class | the class of the transform by its plan (`--plan --lines`): its mode (`utf8`, `mixed[<reasons for its Unicode segments>]`, `unicode`, `icu`), whether `--lines` bounds its contexts (`bounded`) or is not supported (`nolines`), and whether it has a filter |
| mode | `mixed` (`--utf8-mode=1 --mixed-modes=1`), `utf8-only` (`--mixed-modes=0`), `unicode` (`--utf8-mode=0`) |
| scope | `text`, `lines` (`--lines`), `lines-sep` (`--lines` with `--region-separators`: the lines separated by a character of the transform's own alphabet) |
| filter | `none`, `half` (a global filter of a random half of the characters, around the transform) |
| input | `cldr` (lines of the transform's CLDR test data), `random` (random text in the alphabet of that data) |
| length | `tiny` (≤8 characters), `block` (100–180, around the first block boundaries), `multi` (450–700), `large` (5000–7000) |
| segsize | `default`, `small` (`-segment-size=1`) |
| capture_strings, after_streams, shape_groups | `0`, `1` (`--capture-strings`, `--after-streams`, `--apply-shape-groups`) |

The transforms are those of the CLDR test data (`--cldr`). A class value stands
for its transforms in turn: each pass takes the next one of the class.
Constraint: a `nolines` class is not combined with the line scopes.

## The checks

- **Reference.** A case's output must equal that of the reference run: the same
  transform, input, scope and filter, in Unicode mode with the default switches.
  Mode, segment size and the implementation switches must not change the output.
  For `lines-sep`, the reference is the `lines` run, rejoined with the separator.
  Errors (crashes, fatal errors) and timeouts are also anomalies. "Not supported"
  is not an anomaly.
- **CLDR expected results.** The reference run of a `lines` case on CLDR input is
  compared with the expected column, line by line.
- **ICU.** The reference run of a `lines` case is compared with ICU's result
  (`--use-ICU=.*`), line by line.

The mismatches against CLDR or ICU found at the first commit tested are the
baseline. After that, a new mismatching line (CLDR), or a first mismatch for a
transform (ICU), is an anomaly.

## Commands

```bash
# Continuous testing of the commits of a branch, in the background, on one core
nohup python3 QA/tconv_combinatorial/tconv_ct.py run --branch LDML > ~/.parabix/tconv-ct.log 2>&1 &
python3 QA/tconv_combinatorial/tconv_ct.py report -v      # what it has found
# Foreground: a covering array restricted to the transforms or values a change affects, in a time budget
python3 QA/tconv_combinatorial/tconv_ct.py once --tconv build/bin/tconv --budget 120 \
        --transform am-fonipa-t-am --set mode=mixed --set scope=lines
# An anomaly against a binary (exit 1 if it is still there), and git bisect over commits
python3 QA/tconv_combinatorial/tconv_ct.py check <id> --tconv build/bin/tconv
python3 QA/tconv_combinatorial/tconv_ct.py bisect <id> [--good C] [--bad C] [--jobs N]
```

`run` follows a branch from a worktree of its own (`<repo>-ct`, detached at the
commit tested). It builds tconv there with one job (`--jobs`), copying the CMake
settings of `--reference-build`. So it tests committed code only, and a rebuild
in the working tree does not affect it. It runs tconv with `-thread-num=1
-compile-threads=1` and its own object cache, which it clears at each new commit.

When the branch moves, it finishes the current case, builds the new commit, and
first reruns the open anomalies (marking them fixed or not). Then it continues
with new passes. It starts at strength t (`--t`, default 2) and moves to t+1
after `--pairwise-passes` passes.

## The state (`--state`, default `~/.parabix/tconv-ct`)

- `state.json`: the commits tested, the passes, the rotation of the classes, and
  the known CLDR/ICU mismatches.
- `results.jsonl`: one line per case (commit, parameters, status, seconds).
- `anomalies/<id>/`:
  - `anomaly.json`: the case, the commands, the first difference, the commit
    first seen and the commit tested before it (the range to bisect);
  - `input.txt`;
  - `shrunk.txt`: the input reduced by delta debugging (lines, then characters)
    while the anomaly persists, within 3 minutes.

Anomalies are found after the commit that caused them, possibly several commits
later. `previous_commit` and `first_seen` bound the range, and `bisect` narrows
it in a second worktree (`<repo>-ct-bisect`).

`covering.py --self-test` checks that the arrays cover every t-way combination
and respect the constraints.
