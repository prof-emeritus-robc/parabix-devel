# Focus / transform / reintegrate: Phase 2 results for nfc

nfc on the `FocusedTransform` builder (Phase 1 step 2, 39ce3350f) has the filter and
reintegration modes of nfd. These measurements compare them on the inputs where nfc
has work (`de.nfd`, `ko.nfd`) and on one where it has none (`de`, which is already NFC).

## Method

- `QA/focus_baseline.py --tools nfc --repeats 5` (best of 5 wall times; 3 threads; warm
  object cache), on AC power, with the inputs of Phase 0 (about 50 MB cut from
  `~/wiki-books-all.xml`; `ko.nfd` is 96 MB).
- Every configuration's output matched the default configuration's output with one
  thread, and that matched the previous nfc (checked before the change, in all modes
  at 1 and 3 threads).
- The default is now `-focus-filter=run -reintegrate=run -unmodified-filter=byte`
  (4341eae27). The previous nfc filtered the basis bits (`-ByteFiltering`: the bytes)
  and merged bytes with MergeByMask. No current mode is exactly the same: in
  `-focus-filter=bit -reintegrate=bit`, the unmodified data are filtered and merged
  as basis bits. So the old nfc is compared using the Phase 0 numbers, measured the
  same way on the same inputs but in an earlier session. Those comparisons are
  marked "Phase 0".
- Work density is the size of the focused stream as a percentage of the input. It is
  2.0% on de.nfd, 0.05% on de and 76% on ko.nfd, the same as in Phase 0.

## Wall times

Times are in seconds. "Ceiling" is the projected maximum speedup at any thread count
(single-threaded time / largest serialized kernel).

| input | configuration | 1 thread | 3 threads | ceiling |
|---|---|---|---|---|
| de.nfd | Phase 0 nfc (default) | 0.200 | 0.124 | 11.9 |
| de.nfd | **default** (run filter, run merge) | **0.167** | **0.108** | 9.5 |
| de.nfd | `-unmodified-filter=run` | 0.168 | 0.110 | **19.2** |
| de.nfd | `-fill-run-gaps=4` | 0.175 | 0.112 | 9.9 |
| de.nfd | `-reintegrate=byte-replace` | 0.184 | 0.118 | 10.5 |
| de.nfd | `-reintegrate=byte-spread` | 0.186 | 0.120 | 10.6 |
| de.nfd | `-focus-filter=byte -reintegrate=bit` | 0.218 | 0.137 | 12.4 |
| de.nfd | `-focus-filter=bit -reintegrate=bit` | 0.233 | 0.143 | 14.2 |
| de.nfd | `-NoFocus` | 0.723 | 0.295 | 10.1 |
| de | Phase 0 nfc (default) | 0.194 | 0.148 | 5.8 |
| de | default | 0.153 | 0.124 | 4.1 |
| de | **`-unmodified-filter=run`** | **0.149** | **0.121** | **10.0** |
| de | `-fill-run-gaps=4` | 0.160 | 0.124 | 4.2 |
| de | `-reintegrate=byte-replace` | 0.168 | 0.142 | 6.9 |
| de | `-reintegrate=byte-spread` | 0.183 | 0.155 | 7.9 |
| de | `-focus-filter=byte -reintegrate=bit` | 0.212 | 0.172 | 6.4 |
| de | `-focus-filter=bit -reintegrate=bit` | 0.230 | 0.177 | 7.2 |
| de | `-NoFocus` | 0.646 | 0.267 | 17.7 |
| ko.nfd | Phase 0 nfc (default) | 0.878 | 0.363 | 23.3 |
| ko.nfd | default | 0.947 | 0.389 | 25.6 |
| ko.nfd | **`-focus-filter=byte -reintegrate=bit`** | **0.900** | **0.373** | 24.0 |
| ko.nfd | `-reintegrate=byte-replace` | 0.920 | 0.377 | 24.6 |
| ko.nfd | `-focus-filter=bit -reintegrate=bit` | 0.917 | 0.378 | 24.6 |
| ko.nfd | `-reintegrate=byte-spread` | 0.922 | 0.378 | 24.7 |
| ko.nfd | `-unmodified-filter=run` | 0.976 | 0.398 | 26.2 |
| ko.nfd | `-fill-run-gaps=4` | 1.028 | 0.416 | 13.4 |
| ko.nfd | `-NoFocus` | 1.180 | 0.464 | 12.5 |

## Findings

- **Low density (de.nfd, de): the run-based modes win.** Compared with Phase 0,
  the default is 13% faster on de.nfd at 3 threads (17% at 1 thread) and 16%
  faster on de. Within this run, it is 24–30% faster than
  `-focus-filter=bit -reintegrate=bit`.
- **`-unmodified-filter=run` doubles the scaling ceiling at low density** (de.nfd
  9.5x → 19.2x, de 4.1x → 10.0x), at the same or slightly better wall time at 3
  threads. With the default byte filter of the unmodified bytes, that filter
  (`elemFilter_8`) is the largest serialized kernel: 24% of single-threaded time on
  de, 11% on de.nfd. On ko.nfd, `-unmodified-filter=run` costs 2%. It is the better
  default for many cores at low density, as for nfd ("RR" in the nfd report).
- **High density (ko.nfd, 76%): run filtering loses.** Runs are short, so the run
  filter (8.8% of time, against 4.3% for FilterByMask of the basis bits) costs more
  than it saves. The best mode, `-focus-filter=byte -reintegrate=bit`, is 4% faster
  than the default. Compared with Phase 0, the default is 7% slower and the best
  mode 3% slower. The transform is 70% of the time and 55% is serialized, so the
  filter and reintegration choice matters little here; the transform's kernels are
  the work for Phases 4–5. The largest serialized kernel is IndexedShiftBack, at 4%.
- **`-fill-run-gaps=4` does not help.** It is neutral to slightly worse at low density,
  and 7% worse on ko.nfd, where it raises the density from 76% to 80% and makes the
  transform 82% of the time (ceiling 13x, bounded by FindComposables3). This matches
  nfd.
- **`-NoFocus`** remains 2.2–2.7x slower at low density and 19% slower on ko.nfd.
  Unlike nfd on ko, nfc has no input where whole-text processing wins.
- **Placement is now the largest stage at low density** (27% of single-threaded time
  on de.nfd, 35% on de). Its largest kernel is NFC_Initial_Insertion with the work
  mask, which inserts room for decompositions over the whole source, only to compute
  the placement mask. Focus is another 14%. As with nfd, placement and focus are the
  next costs to reduce once reintegration is run-based.

## Suggested next steps

1. Consider `-unmodified-filter=run` as the default, for both nfc and nfd. This is
   the main scaling gain at low density.
2. Choose the filter mode adaptively by density, as was proposed for nfd's run merge.
   Byte or bit filtering wins at high density (ko.nfd), run filtering at low density.
3. Make placement cheaper. For nfc, compute the insertion bixnum only within the
   work regions, using the work mask to skip blocks without work, instead of over
   the whole source.
