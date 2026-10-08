# Shader constants by vector (masseffect_native_constants_dirty)

Date: 2026-10-09. Status: implemented behind cvars that default to off. Not yet run on the console. Words you do not know
are in [glossary.md](glossary.md).

## 1. Where the ring thread's constant time goes

Source: the stack profile of the location tour `mass-effect-recomp/run/me1/tour_20261009_004721/rex_profile.sym`
(BIOA_END00, CPU 1785 / GPU 768 MHz, 127 "normal" 10 s blocks, 27.2 fps on average, ring thread 57.7 % of a core).
Numbers are core-% (percent of one core, averaged over the blocks). At 27 fps, 1 core-% is about 0.37 ms per frame.
How to read the profile is in [cpu-cost-analysis.md](cpu-cost-analysis.md) section 2; the per-pc sums below were taken
from the `pc image+0x...` lines of the ring thread (`GPU ring native`).

Caveat: no ELF of exactly that build was kept. The `.sym` file was symbolized at 01:28 with the ELF of that moment; the
names inside the ring's hot code are coherent (the addresses of one function sit together and inlined helpers sit
inside their callers), so the attribution below is trusted at function level, not at line level. The libc `memcpy`
sits at the start of `.text` (`app/function_order.ld`) and has no debug info, so its samples show up under bogus names
(`basic_string::_M_create`, `_M_mutate`, ... at `image+0x100..0x430`).

| What | core-% | ms per frame at 27 fps | Where |
|---|---|---|---|
| `ConstantsSame` (the full-bank compare of masseffect_native_constants_same_content: `EqualInLine` inlined, shown as `vld1q_u8` at `0x2221d10/14`) | **~2.0** | ~0.73 | draws side, every draw whose constant generation changed |
| `WriteConstantRunRaw` + `ApplyRunRaw` (NEON swap, compare, store; the `vld1q_u32` at `0xbf8bc` is the load of the guest words, 0.56) | ~2.0 | ~0.74 | ring sink, register writes |
| libc `memcpy` called from `ShadowConstants` (the full CPU copy of each uploaded bank) | ~0.8 | ~0.29 | draws side, every upload |
| libc `memcpy` total (vertices go through `CopyVertices`, so most of the rest is the constant uploads into the upload buffer, which is memory WITHOUT CPU cache by default, `masseffect_native_upload_memory = 0`) | 2.7 | 1.0 | |
| `WriteRegister` (single writes, not only constants) | ~0.7 | ~0.26 | ring sink |

The 10 s report of the same run says why the compare runs so often: `C6 set 4: ... constants uploaded again: VS 226948
by generation ...; PS 176896` in 273,559 draws. 83 % of the draws change a VS constant and 65 % a PS constant, so for
almost every draw `ConstantsSame` compares the whole bank the shader reads (it does not stop at the first difference)
before the upload, and then `ShadowConstants` copies the whole bank again into the CPU-cached shadow.

Constant batching itself already exists: every draw's VS/PS block goes into the one upload buffer of the submission
(`Reserve`, linear), is bound as a dynamic UBO offset of set 4, and NVK rewrites only the offsets that changed
(`C6 set 4 by differences`, mesa patch). There is no per-draw descriptor update or small buffer for constants. What is
left to cut on the CPU is the compare and the shadow copy (and, with dedupe, some uploads).

## 2. What changed

Three cvars, all off by default:

| cvar | default | what it does |
|---|---|---|
| `masseffect_native_constants_dirty` | false | ring sink (`me_native_system.cpp`): every write into 0x4000-0x47FF that changes a value sets the bit of its float4 vector (512 bits: VS c0-c255, PS c0-c255). `WriteConstantRunRaw` uses `ApplyRunRawDirty` (`me_pm4_runs.h`, same NEON loop plus the bits), `WriteConstantRun` and `WriteRegister` mark too. The bits travel in `SubmissionDraw::constants_dirty`. Draws side: `ConstantsByVector` in `masseffect_native_draws.cpp` with `ConstantBankTracker` (`me_constants_dirty.h`). |
| `masseffect_native_constants_dirty_verify` | 4096 | the first N decisions are also taken the old way (full compare of the bank with the CPU copy) and the bytes the draw binds are read back from the upload buffer and compared with the registers; after each upload the CPU copy is compared too. Any DIFFERENCE logs an error, turns the switch off for good and gives that draw a plain upload. |
| `masseffect_native_constants_dedupe` | false | with the switch above: a bank that must be uploaded is hashed (XXH3 of the bytes the shader reads) and looked up among the last 8 uploads of the same bank in this upload buffer; an equal one is bound instead of copied. The first N hits (same verify count) are compared byte by byte with the upload buffer. |

How a draw decides now (per bank, mode 0 = the constants come straight from the registers):

1. Take the ring's bits (OR into the tracker's pending mask, clear them in the ring). Check that every generation bump
   since the last draw came from a write that also set bits (`ConstantDirtyBits::bumps`); otherwise the tracker is out of
   sync and the older path runs.
2. If the last upload is valid (same upload buffer, it holds at least the bytes this shader reads): no pending bit in
   those bytes means reuse at once ("reused clean"). Otherwise only the pending vectors are compared with the CPU copy;
   if they are all equal it is reused too ("reused after comparing", the A -> B -> A case that
   `masseffect_native_constants_same_content` caught with its full compare). The decision is exactly the full compare's:
   a vector without a bit has not changed value since the upload.
3. Otherwise upload (or a dedupe hit) and bring the CPU copy up to date by copying only the pending vectors inside the
   old valid part plus the part beyond it. Pending bits are cleared.

The older path still handles: the first draws (until its first upload resyncs the tracker), the patched PS banks
(tone map override, motion blur fix: `constants_mode_ps != 0`, which desync the PS tracker), and everything after a
failed check. When the older path uploads a mode-0 bank, the tracker takes a full copy and is in sync again. The CPU copy
is the same array (`shadow_vs_`/`shadow_ps_`) the older content path reads, so switching between the two stays exact.

Not done, on purpose:

- Uploading only the changed 16-byte vectors into the upload buffer. Every draw binds one dynamic UBO offset per bank, so
  the GPU must find the whole bank at that offset; the earlier draws may still read the old block. Splitting the bank
  into several UBO bindings would change the shader layouts and add set-4 offsets (more NVK work per bind).
- Changing the batching or descriptor path: already one buffer per submission with dynamic offsets (section 1).

Report, every 10 s next to the `C6 set 4` lines:

```
[native] constants by vector (on): VS N decisions (a reused clean, b reused after comparing, c uploaded, d by the older
path), PS ...; x vectors compared per decision; CPU copy y bytes per upload instead of z; dedupe on: L lookups, H hits
(K KB not copied); lost sync VS 0 PS 0; checked 4096 (0 mismatches)
```

At startup with the switch on: `[native] constants by vector (masseffect_native_constants_dirty): ON ...`.
A failure: `[native] DIFFERENCE: VS constants by vector ...` (the switch goes off, the image stays correct).

Host tests (no console needed): `tests/cpu/test_native_pm4_runs.cpp` (ApplyRunRawDirty against the per-word loop:
same registers, same generations, exactly the changed vectors marked; 3,000,000 random runs) and
`tests/cpu/test_native_constants_dirty.cpp` (2,000,000 random draws with writes, A -> B -> A values, prefix sizes,
buffer resets and desyncs: the decision always equals the full compare and the bound block always equals the
registers). Both pass on the Mac (`tests/run_all.sh` picks them up).

## 3. Expected gain

Ring thread, heavy views (estimate from section 1):

| | core-% | ms per frame at 27 fps |
|---|---|---|
| `ConstantsSame` full compare gone (the test shows ~0.2 vectors compared per decision) | -1.8 to -2.0 | -0.7 |
| `ShadowConstants` full copy -> changed vectors only | -0.5 to -0.7 | -0.2 |
| extra cost in the ring sink (`vmaxvq` per 4 words and the bit set on changes) | +0.2 to +0.4 | +0.1 |
| **net, `constants_dirty` alone** | **about -2** | **about -0.8** |
| `constants_dedupe` | unknown: saves a write of the bank into the upload buffer (memory without CPU cache) per hit, costs one XXH3 of the bank per upload. Worth it only with a high hit rate; the report gives it. | |

`WriteConstantRunRaw` itself stays: its main cost is reading the guest words that the render thread just wrote on
another core (`vld1q_u32` load), which no change on this side removes.

## 4. What to check on the console

1. Production toml plus `masseffect_native_constants_dirty = true` (keep `masseffect_native_constants_same_content =
   true`; the dirty path replaces it where it can). Cold run, same route as the A/B baseline.
2. In the log: the `ON` line at startup; after 4096 checked decisions no `DIFFERENCE`; `lost sync` 0 (a non-zero value
   means a register write path bumps the constant generation without marking vectors: report it).
3. In the report: `reused clean` + `reused after comparing` should match what `same content` reused before, plus the
   draws whose generation did not change; `vectors compared per decision` well below 1; `CPU copy ... bytes per upload`
   well below the `instead of` number.
4. Image: identical screenshots against the baseline at the tour stops (the change is exact by construction; the
   check mode proves it on the first 4096 decisions).
5. Profile run (flag file): `ConstantsSame`/`EqualInLine` should vanish from the ring thread, the `ShadowConstants`
   memcpy should drop, `WriteConstantRunRaw` should rise by at most ~0.3 core-%.
6. Second run with `masseffect_native_constants_dedupe = true` as well: look at `hits` / `lookups` and the ring CPU %.
   Keep it only if the ring thread gets faster.
