# UTF-8 marker placement in the NFD/NFKD kernels: Advance vs LookAhead

The Pablo kernels of nfd and nfkd that compute a property per character can place
the marker bit on one of two bytes of each multibyte UTF-8 sequence:

- `pablo::BitMovementMode::Advance` (the previous behaviour): on the **last** byte. The kernels move information forward from the earlier bytes
  with `Advance`, which carries bits from one block to the next.
- `pablo::BitMovementMode::LookAhead` (the default since the commit after this
  report): on the **first** byte. The kernels read the
  later bytes with `Lookahead`, which reads ahead in the input buffer and keeps no
  state.

The new option `-u8-bit-movement=advance|lookahead` (category "Normalization
Options", `NormalizationOptions()`) selects the mode for `NFD_PipelineBuilder`. It
applies to nfd, nfkd and tconv's Any-NFD/Any-NFKD.

## Changes

| stage | kernel | Advance mode | LookAhead mode |
|---|---|---|---|
| focus | `UnicodePropertyKernelBuilder` (dt=Can; ccc=NR) | last byte | first byte |
| focus | `CharClassesKernel` (NFKD: characters NFKD changes) | last byte | first byte |
| focus | `NFD_Focus` (work items: dt=Can or ccc≠0, and the next character) | marks and "next character" use the last bytes (`u8index`) | they use the first bytes (`u8initial = ¬Advance(¬u8index)`); kernel `NFD_Focus_LookAhead` |
| focus | `U8Spans` (the bytes of the marked characters) | `LookAhead` form: extends last-byte marks back over the sequence | `Advance` form: `MatchStar` from first-byte marks forward |
| placement | `CharClassesKernel` (UTF-8 insertion and deletion BixNums) | last byte | first byte |
| placement | `CreateU8_FilterMask` (bytes to delete) | the positions before the last byte (`Lookahead`); `u8_delmask@-1` | the positions after the first byte (`Advance`); `u8_delmask@+1` |
| placement | `InsertionSpreadMask` (`InsertPosition::After`) | room after the last byte | room after the first byte, within the sequence; only the number of positions in a work region matters for placement |
| transform | `UTF8_Decoder` | codepoint at the last byte | at the first byte (`UTF8_Decoder_LookAhead`) |
| transform | codepoint positions for `FilterByMask` | `UTF8_index` (last bytes) | `UTF8_InitialMask` (new: bytes that are not `10xxxxxx`) |

`ComputeWorkPlacement` and `CreateU8_FilterMask` take the mode as a parameter.
Each kernel's name includes its mode, so both modes coexist in the object cache. The
U21 parts of the transform (decomposition, Hangul, canonical ordering) are
position-independent and unchanged.

Two changes affect both modes:

- **`UnicodePropertyKernelBuilder`** declared `LookAhead(3)` on its input in either
  mode. In the Advance mode it never looks ahead, so it now declares the lookahead
  only in the LookAhead mode, as `CharClassesKernel` does. Before, the UCD property
  kernels of nfd were provisional-lookahead (PLS) kernels for no reason.
- **`ExpandFilter`** is now one kernel (`FillInsertedPositions`: `spread | ¬spreadMask`)
  instead of `Invert` and `OrCombine`. nfd's placement has two ExpandFilters with
  the same spread mask, and kernel deduplication shared their `Invert`. In the
  LookAhead mode (and with some phase boundaries in either mode), that shared
  `Invert`'s partition stopped before the final partial block. The other
  ExpandFilter's `OrCombine` then left thread-local items unconsumed, and the
  output was cut off at a multiple of 128 bytes (`-EnablePipelineAsserts`:
  "local available item count (150) does not match its processed item count
  (128)"). A small reproduction, with the two-kernel ExpandFilter: `nfd
  -u8-bit-movement=lookahead -focus-filter=bit -reintegrate=bit` on `'x각' * 15`
  gives 128 bytes instead of 150. The single kernel avoids the shared partition.
  It also fixes the end-of-data loss with `-focus-layers=filter` in nfd that the
  phase issue draft describes. The pipeline bug itself, a partition that ends
  before its input's final partial block, is still open.

## Correctness

Both modes match the unfocused output (`-NoFocus`, which equals Python's NFD/NFKD on
these inputs) on de, ko, vi, their NFD forms, a 300 KB sample, the conformance
files and the generated focus-modes text. This holds in the default, bit, byte
(replace), gap-filling and 4-stage modes, at 1 and 3 threads. `ctest -j3`: 42/42.

One combination fails: `-u8-bit-movement=lookahead -focus-layers=focus` crashes
LLVM 18's register allocator (`HoistSpillHelper::runHoistSpills`), as
`nfc -focus-layers=focus` does.

## Performance

`QA/focus_baseline.py`, best of 5, AC power, inputs of about 50 MB (ko.nfd 96 MB).
"Ceiling" is the single-threaded time divided by the time of the largest kernel
that is not data-parallel (`<StateFree>`).

| tool | input | mode | 1 thread s | 3 threads s | ceiling |
|---|---|---|---|---|---|
| nfd | de | Advance | 0.159 | 0.095 | 14.8 |
| nfd | de | LookAhead | 0.161 | 0.096 | **19.6** |
| nfd | vi | Advance | 0.380 | 0.178 | 19.6 |
| nfd | vi | LookAhead | 0.376 | 0.177 | **28.2** |
| nfd | ko | Advance | 0.694 | 0.293 | 5.0 |
| nfd | ko | LookAhead | **0.660** | **0.277** | **20.1** |
| nfd | ko.nfd | Advance | 0.227 | 0.128 | 7.5 |
| nfd | ko.nfd | LookAhead | 0.224 | 0.126 | **16.8** |
| nfd -NoFocus | de | Advance / LookAhead | 0.412 / 0.407 | 0.175 / 0.173 | 15.3 / 15.0 |
| nfd -NoFocus | ko | Advance / LookAhead | 0.544 / 0.536 | 0.224 / 0.219 | 19.8 / 19.4 |
| nfkd | de | Advance | 0.178 | 0.111 | 14.0 |
| nfkd | de | LookAhead | 0.180 | 0.112 | **20.2** |
| nfkd | vi | Advance | 0.420 | 0.206 | 19.5 |
| nfkd | vi | LookAhead | 0.417 | 0.204 | **28.6** |
| nfkd | ko | Advance | 0.789 | 0.336 | 5.5 |
| nfkd | ko | LookAhead | **0.755** | **0.322** | **21.4** |

### Findings

- **The LookAhead mode makes the character-class and property kernels state-free.**
  A Pablo `Advance` carries bits from block to block, so a kernel that uses one
  processes segments in order; a `Lookahead` reads ahead in the input buffer
  instead. In the Advance mode, the UTF-8 compilation of a character class or
  property moves the prefix information forward with Advances. In the
  LookAhead mode it looks ahead to the suffix bytes, and the pipeline marks
  these kernels `<StateFree>`. The kernels:
  - the placement's insertion character-class kernel: 19.9% of single-threaded
    time on Korean, the scaling bound of nfd on ko;
  - its deletion character-class kernel;
  - the dt=Can and ccc kernels;
  - NFKD's decomposable-character kernel.
- **The scaling ceiling rises sharply**: nfd on ko from 5.0x to 20.1x, ko.nfd
  from 7.5x to 16.8x, de from 14.8x to 19.6x, vi from 19.6x to 28.2x; nfkd on ko
  from 5.5x to 21.4x. On ko, the serialized share of placement drops from 25.9% to
  6.2% of single-threaded time.
- **Single-threaded cost is about the same**, so at 3 threads (where the old
  ceilings do not yet bind) wall times change little: −4 to −5% on ko (nfd and nfkd),
  ±1% elsewhere. The gain is for machines with many cores, where the Advance mode's
  ceiling of 5x on Korean would limit both tools.
- **The kernels that stay serialized need forward movement:**
  - `NFD_Focus` marks the character after each changing character.
  - `U8Spans` in the Advance form extends first-byte marks forward.
  - `InsertionSpreadMask` works from running counts.

  They are cheap: on ko, 0.8% for U8Spans; the largest is InsertionSpreadMask,
  at 5.0%, which now bounds the ceiling. Placing the marks on the first byte
  cannot remove a dependence on the preceding character.
- **`-NoFocus` is unaffected** (±2%): the whole-text transform already decodes with
  data-parallel kernels, and its serialized kernels are elsewhere.

### Recommendation

Make `-u8-bit-movement=lookahead` the default (done). It is as fast or faster on every
input, and it removes the largest serialized kernels of nfd and nfkd on dense
input. The same change would likely help other tools that compute per-character
properties with the Advance mode, such as nfc's candidate classes and the UCD
property kernels of icgrep.
