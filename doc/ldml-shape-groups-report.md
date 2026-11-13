# Shape groups in tconv: kernel counts and correctness

How `tconv --apply-shape-groups` changes the pipelines of the CLDR transforms,
compared with the regular plans and with
[ldml-tconv-coverage-table.md](ldml-tconv-coverage-table.md) (generated at
`d97c03f75`).  Measured on 2026-11-13 with tconv built from master
(`3d56b029b`) plus the uncommitted shape-group work; CLDR transforms in
`~/cldr/common/transforms`, test data in `~/cldr/testData/transforms`.

## What shape groups are

A conversion group is planned as search-and-replace passes of the regular
expression engine, one per *shape* of the rules' results (see
`include/ldml/shape_groups.h`):

- The rules with the same result shape are matched together. The search
  pattern is a branch-reset alternation of their source sides, and the
  replacement is the shared shape.
- Function calls (`&T($k)`) are deferred: the replacement writes a mark before
  the argument, and the marks are mapped to the function's values at the end,
  as one string-rule group.
- Rules that map single characters to literal text, without contexts, form
  one character-map pass, planned by `ldml::planTransform`.
- Ordering between shapes is arranged by guards (`(?<!…)`) and pass order.
- Before contexts read the converted text through a closure over the deleted
  characters, for example `(?<=\p{letter}(?:\x{FDD9}5|5)*)`.

`tconv --shape-groups <transform>` prints the plan; `--apply-shape-groups`
applies it. It is on by default; `--apply-shape-groups=0` uses the regular
plan only. `tconv --plan` shows the plan chosen for each segment.

**Per-segment choice.** A *segment* is the run of conversion groups between
consecutive `::X` transform rules. When the regular plan and the shape groups
have the same transform rules and no filters, each segment uses whichever
plan builds fewer kernels. The counting is done in a scratch pipeline that is
built but never compiled.

**Within a filter.** A transform invoked within a filter (e.g. by a
transform with a global filter) uses its shape groups too. The filter's mask
is the engine's *text mask* (`RE_context::textMask`): the text a match covers
must lie within the filter, while its contexts (positive lookarounds) see the
whole text, as in ICU, which applies a filtered transform to each run of the
filter's characters. The engine's analyses are those of the pattern as given;
the RE compiler restricts the characters matched outside positive lookarounds
to the mask (`RE_Compiler::setTextMask`), as do the kernels of string
classes and chain stars. The filter masks are carried through each
replacement, so the text it writes, including the marks of deferred calls,
stays within the filter. Not supported within a mask (`textMaskFailures`):
starts of matches found by reversal, and negative lookaheads of variable
length. A transform with a filter of its own still uses its regular plan.

A transform that the regular plan cannot implement is applied entirely by its
shape groups, if they are implementable.

## Transforms whose pipelines change

Kernel counts are from `tconv --kernel-counts`. "Group kernels" counts the
transform's own conversion groups. "Kernels" counts the whole transform,
including the transforms it uses.

| Transform | group kernels: table / regular / **shape groups** | kernels: table / regular / **shape groups** | change | CLDR tests vs ICU | random text vs ICU |
|---|---:|---:|---:|---|---|
| NumericPinyin-Latin | 699 / 699 / **188** | 848 / 848 / **337** | −60% | (no test file) | identical (300 lines) |
| am-am_FONIPA | 516 / 538 / **190** | 516 / 538 / **190** | −65% | am-fonipa-t-am: 607 cases, 0 differ (0 from expected) | identical |
| am-ar (uses am-am_FONIPA) | 4 / 4 / 4 | 816 / 816 / **468** | −43% | ar-t-am: 54 cases, 0 differ (0 from expected) | identical |
| am-fa (uses am-am_FONIPA) | 0 / 0 / 0 | 762 / 785 / **437** | −44% | fa-t-am: 54 cases, 0 differ (0 from expected) | identical |
| am-chr (uses am-am_FONIPA) | 0 / 0 / 0 | 777 / 799 / **451** | −44% | output identical to the regular plan's (see notes) | identical |

- **NumericPinyin-Latin:** the regular plan expands `&NumericPinyin-Pinyin($3)`
  into 5 variants of each of 15 rules (75 rules). The shape groups keep the
  15 rules in 3 passes, plus one mark-mapping group.
- **am-am_FONIPA:** the saving is all in its second segment, 487 → 139 kernels.
- **Regular counts that differ from the table:** am-am_FONIPA (516 → 538) and
  its users have grown since the table was generated. These are changes on
  master, not shape groups.

## Run-time performance

**Setup.** Each transform was run with `tconv -thread-num=4` on 8 MB of input,
one run at a time on an otherwise idle machine (Apple M3, 4 performance
cores):

- NumericPinyin-Latin: random numeric pinyin text.
- The am-* transforms: the Amharic sources of the CLDR test files, repeated.

Measurements:

- **Cold:** the first run with an empty object cache (a fresh
  `-object-cache-salt`), so it includes JIT compilation.
- **Warm:** the median of 5 runs with the kernels cached.
- **Start-up:** the same warm run on an empty input. Every run plans the
  rules again, and with shape groups also counts the kernels of both plans.
- **Net:** warm minus start-up, the time spent on the 8 MB.

The output of each transform is identical with and without shape groups.

| Transform | cold: regular / shape groups | warm: regular / shape groups (boundaries) / **shape groups** | start-up: regular / shape groups | net: regular / **shape groups** |
|---|---:|---:|---:|---:|
| NumericPinyin-Latin | 16.7 s / **5.7 s** | 0.575 / 0.474 / **0.460** s | 0.146 / 0.145 s | 0.43 / **0.32** s (−27%) |
| am-am_FONIPA | 6.9 s / **2.3 s** | 0.164 / 0.269 / **0.179** s | 0.095 / 0.121 s | 0.069 / **0.058** s (−16%) |
| am-ar | 24.8 s / **18.6 s** | 0.693 / 1.620 / **0.836** s | 0.494 / 0.653 s | 0.20 / **0.18** s (−8%) |
| am-fa | 36.4 s / **29.1 s** | 12.71 / 1.43 / **0.68** s | 12.61 / 0.48 s | 0.10 / 0.20 s (noisy) |
| am-chr | 16.1 s / **8.5 s** | 0.264 / 0.752 / **0.374** s | 0.122 / 0.245 s | 0.14 / **0.13** s (−9%) |

- **JIT compilation is 3× faster** for the transforms using shape groups
  themselves: 16.7 → 5.7 s and 6.9 → 2.3 s. It is 25–45% faster for the
  transforms that use them (am-ar, am-fa, am-chr), as these have fewer
  kernels to compile.
- **Phase boundaries.** The first version put a pipeline phase boundary after
  each engine pass. That kept the passes from running together, and the
  shape-group pipelines were then up to 2.8× slower than the regular ones
  (the "boundaries" column). The boundaries had been added while
  investigating the failure of pipelines with many passes, and did not fix
  it. They are now off by default (`--shape-phase-boundaries` restores them).
- **Processing (net)** is faster with shape groups wherever it can be
  measured: −27% for NumericPinyin-Latin, and −8% to −16% for the am-*
  transforms.
- **Start-up** is the remaining cost: +0.03 to +0.16 s, for planning the
  shape groups and building both candidate pipelines to count their kernels,
  on every run. For short inputs this outweighs the processing saving
  (am-chr, warm: 0.264 → 0.374 s). Caching the choice per transform, or
  counting kernels without building the pipelines, would remove most of it.
- **am-fa's regular pipeline spends 12.6 s at start-up** on every run, even
  on an empty input, with every kernel served from the object cache. Its
  planning takes 0.09 s and its pipeline graph builds in 0.3 s
  (`--kernel-counts`), so the time goes into setting up the pipeline at run
  time: probably the pipeline compiler's analysis of this particular graph.
  That is a problem of the regular path; with shape groups am-am_FONIPA's
  segment is different and start-up takes 0.48 s. Its net times are within
  the noise of that 12.6 s.

**Bopo-Latn** invokes NumericPinyin-Latin within its filter
`[ˇˉ-ˋ˙ㄅ-ㄩ]`: 969 → 465 kernels with its shape groups. Its output is
identical to the regular plan's on 400 lines of random Bopomofo, tone marks,
Latin letters and digits, except for 2 lines where the regular plan changes
or matches characters outside the filter: for `ㄧ˙5` it deletes the `5`
after the filter's `yi5` (`yi`, shape groups `yi5`), and for `ㄗ˙ㄊ3` it
applies the tone of the `3` outside the filter (`zǐt3`, shape groups
`zit3`). (Real ICU applies no tones in Bopo-Latn at all; see the notes.)

## Segments where both plans were counted

33 transforms have segments with both plans. Shape groups were chosen in 2
segments:

| Transform | segment | regular | shape groups | chosen |
|---|---:|---:|---:|---|
| NumericPinyin-Latin | 2 | 699 | 188 | shape groups |
| am-am_FONIPA | 2 | 487 | 139 | shape groups |
| Any-Accents | 2 | 10 | 720 | regular |
| rm_SURSILV-rm_FONIPA_SURSILV | 3 | 13 | 420 | regular |
| ro-ro_FONIPA | 4 | 13 | 357 | regular |
| und_FONIPA-chr | 3, 5 | 31, 16 | 137, 30 | regular |
| my-my_FONIPA | 2, 7, 8 | 26, 26, 10 | 37, 92, 39 | regular |
| es_FONIPA-es_419_FONIPA | 1 | 16 | 25 | regular |
| nv-nv_FONIPA | 4 | 15 | 19 | regular |
| Han-Latin/Names | 1 | 19 | 20 | regular |

In the other 23 transforms the counts tie, so the regular plan stays. They
are mostly character maps (Georgian-Latin, Fullwidth-Halfwidth, Cans-Latn,
Syrc-Latn, the Pinyin pair, Accents-Any, Publishing-Any and others), which
both plans build with the same machinery.

**Pattern.** Shape groups win where the regular plan multiplies rules into
variants, or builds many sets of capture rules. They lose where a group
splits into many small shapes, since each pass is a whole engine
search-and-replace pipeline. Choosing the cheaper plan per segment keeps
every count above at or below the regular plan.

## Where shape groups do not apply

Problems that stop a transform with conversion groups from being planned
entirely by shape groups:

| Reason | transforms |
|---|---:|
| a filter rule (not supported) | 71 |
| more than one shape with before contexts | 19 |
| a shape whose matches may begin within another's, ending elsewhere | 16 |
| a shape applied after another, with an after context | 13 |
| a result with text to revisit or a cursor | 13 |
| a shape with before contexts, applied after others, with an after context | 8 |
| rules that may match at the same position | 5 |
| a before context of more than one set, against the converted text | 4 |
| too many rules for the pairwise analyses (`n(n−1)/2` > 200,000) | 4 |
| a transform rule with a filter | 4 |
| analyses of pairs of rules over budget (200,000) | 3 |
| a shape that may match the text written by an earlier shape | 3 |
| a pass not accepted by the engine | 2 |
| shapes whose matches may begin within each other's | 1 |

**Example:** Latin-NumericPinyin (forward) has two shapes with before
contexts, `e { (tone) r → r &f($1)` and the bare `(tone) → &f($1)`.

## Notes and limits

- **Correctness checks.** Each changed transform was run on 300 lines of
  random text, built from the characters of its rule file, and on its CLDR
  test file where one exists. Both were compared with ICU (`tconv
  --use-ICU='.*'`).
  - An earlier round used `--use-ICU=1`. That option takes a regular
    expression over transform names, and `1` matches none, so those runs
    compared tconv against its own regular pipeline. All results here are
    against ICU.
- **am-chr.** Run with all its test cases as lines of one text, tconv differs
  from ICU on 52 of 53 cases. The regular plan does exactly the same, and the
  coverage table, which runs each case as its own text, records 0/53
  failures. This is not a shape-group difference.
- **Bopo-Latn.** It invokes NumericPinyin-Latin within a filter (see "Within
  a filter" above). On random text both plans differ from ICU: ICU leaves
  the tone digits where tconv applies them, although ICU's filters otherwise
  behave as tconv's (text converted within a filter stays within it). This is
  a separate, existing issue.
- **Number of passes.** Pipelines with more than about 24 chained engine
  passes fail to build, either in the rate analysis (`boost::bad_rational`)
  or by exhausting virtual address space. Phase boundaries between the passes
  do not help. The per-segment choice avoids such plans, since they always
  have more kernels.
- **Planning time.** The pairwise rule analyses are limited to 200,000 per
  group. The 7,718-rule Amharic geminate word list used to take many minutes
  and is now rejected at once. Ordering the rules (by first characters,
  inner characters and literal prefixes) would make such groups cheap to
  analyze; that is not yet done.
- Kernel counts are not timings; see "Run-time performance" above for the
  measurements.
