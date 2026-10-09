# Hot-hook self-check guard

The hot guest hooks (`app/src/native/me_hot_guest.cpp`, natives in `app/src/native/hot/n_<addr>.h`) replace hot recompiled
functions with exact native versions. Each hook checks itself: the first `masseffect_hot_guard_calls` calls of every thread
and then 1 call in `masseffect_hot_guard_period` run both versions and compare them. A difference logs
`[hot] DIFFERENCE <hook>: ...` and switches that hook off for the rest of the run; `[hot] <hook>: guard OK after N checks`
is logged as before. Calls that are not checked run only the native version, unchanged by this document.

## The race in the old guard (fixed 2026-10-09)

The old `Check()` snapshotted the declared write ranges, ran the native version into **real guest memory**, copied its
result, wrote the old bytes back and then ran the original. Between those steps another guest thread could:

- read the native's bytes, or the rolled-back old bytes, of a buffer it shares (for example a package buffer that the async
  IO thread fills with `memcpy`, `sub_82AC4AF0`, while the loading thread parses it);
- store into those bytes and have its store overwritten by the rollback;
- update a shared global the native writes (the particle random seed of `sub_824DD848`, the two result globals of
  `sub_8256A1A0`) and lose that update.

Suspected consequence: the English start crash while loading `Engine.xxx` with garbage read from a package buffer, while the
IO thread's memcpy was being checked.

## How the guard works now

The guard never writes guest memory; the only writer is the original function itself, so other threads see exactly an
unhooked call. Code: `app/src/native/me_hot_shadow.h` (shared with the fuzzer), `app/src/native/me_hot_shadow.cpp`,
`Check()` in `me_hot_guest.cpp` (both editions).

1. `Writes()` of the hook gives the ranges the call may write, from the inputs (unchanged).
2. `ShadowBuild` copies those ranges (merged when they overlap or touch) into a private per-call buffer, with slack around
   each: 256 bytes (or `Writes::read_before`) in front, `clamp(length, 256, 4096)` behind. Guest memory is only read. The
   4 KB pages of the declared bytes are read directly (the original is about to write them); slack bytes on other pages
   are read through `RexGmShadowFor`, the always-mapped alias of a committed page, so the guard can never fault on an
   uncommitted page, commit a 4 MB chunk of physical memory, or take an emulated read on a GPU-watched page; an
   uncommitted page ends the slack there. (Non-Switch builds: no slack outside the declared pages.)
3. The **shadow build** of the native runs on a copy of the registers. `me_hot_shadow.cpp` compiles every `n_*.h` a second
   time inside namespace `me_hot_shadow_copy` with `ME_HOT_SHADOW_ACCESSORS`: `Raw()` and the `St*` stores of an address in a
   declared range (or the leading slack) use the private copy; the calling thread's own stack scratch
   (`[r1 - 4 KiB, r1 + 128)`) is used for real (thread-private, the original overwrites it anyway); any other scalar store is
   dropped and reported. Reads of everything else see guest memory, which is still in the pre-call state because the
   native runs first. The normal build of the natives (unchecked calls) is untouched: same code as before.
4. The original runs for real.
5. Comparison: registers and FPCR as before; every declared byte of the private copy against guest memory after the
   original (same `memory ... (+k of range ...) native=.. original=..` line); then two new DIFFERENCE reasons: `native
   stored to X, outside its declared write ranges (store dropped)` and `native wrote X next to its declared write ranges`
   (both mean the hook's `Writes()` is wrong).

A call whose `Writes()` reports overflow (too large, unbounded, guest code with side effects) runs only the original, as
before. A declined call (the native returns false) runs only the original, as before.

Cost of one checked call: two copies of the window (declared bytes plus slack) instead of two copies of the declared bytes,
and a window lookup per guest access of the shadow native. Only checked calls pay it.

## Per-hook handling

EN / RU = guest addresses of the two editions (`editions/ru/overlay/app/src/native/me_hot_guest.cpp`). "Shadow" = the generic
private-copy run described above; every hook in the table is race-free (the guard writes no guest memory for any of them).

| Hook (EN / RU) | Function | Declared writes (`Writes()`) | Guard handling | Notes |
|---|---|---|---|---|
| `82219258` / `82219048` | D3D constant block copy (VS) | `[dst & ~15, N*16)`, dirty mask (8 B) | shadow | a source just above or below an overlapping destination is covered by the slack |
| `82219360` / `82219150` | D3D constant block copy (PS) | same | shadow | as above |
| `824F9D10` / `824FA968` | D3D set one float4 constant | three small device ranges | shadow | |
| `826545D0` / `82655008` | bone matrices | array header 12 B, matrix range | shadow; allocator path: overflow (original only) | |
| `82654030` / `82654A68` | particle sprite quicksort | the array (in place) | shadow | the native re-reads its own swaps from the private copy |
| `822631E8` / `82262EC0` | 4x4 matrix inverse | destination 64 B | shadow | **changed:** the NaN path now declines (returns false) instead of calling the original from inside the native; otherwise the shadow run would have executed the original into guest memory. Unchecked calls behave the same (the hook runs the original on decline) |
| `8256A1A0` / `8256AB90` | ray vs slab clip | globals `kG1` (12 B), `kFlag` (4 B), `[r9]`, `[r10]`, two caller-stack outputs | shadow | the globals are shared: the old rollback could race with other threads |
| `8256AFD8` / `8256B9C8` | ray clip (sqrt / divide) | `[r7]` 4 B, `[r8]` 12 B | shadow | |
| `82270C78` / `82270788` | `Cast<T>` | none | shadow (read-only) | lazy class creation path: overflow (original only); the singleton never returns to 0, so the shadow run never reaches the in-native original call |
| `822E3158` / `822E3058` | `Cast<T>` | none | shadow (read-only) | as above |
| `822B9200` / `822B8F50` | `Cast<T>` | none | shadow (read-only) | as above |
| `8230D5F0` / `8230D568` | hash table lookup | none | shadow (read-only) | |
| `8267C000` / `8267CBA8` | swept interval test | `[r10]`, two caller-stack outputs | shadow | |
| `8225CA80` / `8225C688` | iterator advance | iterator 20 B | shadow | |
| `824DD848` / `824DE638` | particle distribution lookup | random seed global (4 B), output array | shadow | the seed is shared by threads: the old rollback could lose another thread's update |
| `8264C7C0` / `8264D1F8` | sprite vertex / index fill | vertex range, index range | shadow | input particle records just in front of the output are covered by the leading slack |
| `8262CFC0` / `8262DBB0` | box vs plane groups | none | shadow (read-only) | |
| `82BAFF58` / `82B87B50` | chained hash index lookup | none | shadow (read-only) | |
| `82BB0748` / `82B89988` | hash find-entry wrapper | `[r3]` 8 B | shadow | |
| `82210970` / `822108B8` | object iterator advance | `[r3 + 4]` 4 B | shadow | |
| `8230F620` / `8230F598` | object hash lookup | none | shadow (read-only); declines when the key is not built | |
| `82B5F0E8` / `82B5A928` | bounding volume overlap | none | shadow (read-only) | its two virtual getters are guest code and run on real memory (read-only getters; their frames are thread-private stack) |
| `82AC4AF0` / `82973160` | CRT memcpy | `[dst, n)` | **dedicated** `ShadowMemcpy`: the expected bytes are the pre-call source bytes, read directly from guest memory into the private copy | declines exactly where the native declines (`dst > src` overlap), also for wrapping ranges; the generic shadow cannot do this hook (a source inside the destination range would be read past the end of the private copy) |
| `82AC4520` / `82972B90` | CRT wcscmp | none | shadow (read-only) | |
| `82AC3790` / `82971E00` | CRT wcsicmp | none | shadow (read-only) | |
| `826EAF70` / `826EB9C8` | object hash | none | shadow (read-only) | |
| `8264ADA0` / `8264B7C8` | skin cache rebind | headers, data ranges, object fields | shadow; virtual call or allocator path: overflow (original only) | |
| `8245FF18` / `82460BD0` | `TFieldIterator<UProperty>::IterateToNext` | iterator 8 B | shadow; class construction path: overflow (original only) | the virtual `GetSuperStruct` is guest code on real memory (a read-only getter in the game) |
| `8264E178` / `8264EBB0` | sprite emitter Render | modes 0 / 1 / 2: overflow (original only); other modes: nothing (stack scratch only) | shadow for the other modes | every path that calls guest code (virtual calls, DrawRichMesh) stays unguarded as before |
| `827D2A00` / `827D3388` | LZO1X decompressor | output range, `[r6]` 4 B, stack `r1 - 16` | shadow | **changed `Writes()`:** the measuring decode records how far match sources reach in front of the output (`Writes::read_before`), and the private copy starts that far below, so such a copy run reads one coherent buffer |
| `82AAFB90` / `82B2D4F0` | XAudio resampler (mono) | voice state 52 B, output range | shadow | |
| `82AAFE20` / `82B2D780` | XAudio resampler (stereo) | voice state, both planes (merged when they overlap) | shadow | |
| `82AC4A50` / `829730C0` | CRT memset | `[dst, n)` | shadow (the memset goes to the private copy) | |

No hook is left unguarded by this change. The calls that were already not compared (overflow: unbounded write sets, guest
calls with side effects; declines) stay so.

## Residual risks

- A false DIFFERENCE (hook switched off, the original keeps the state correct) when a native reads through one pointer
  across the edge of a private copy: a read run starting more than the leading slack in front of a declared range, or
  running more than the trailing slack past its end, while the native has already changed those bytes. None of the fuzz
  cases does this with the slack above (all aliasing generators included). If the neighbouring page is not committed
  (fuzzer `ME_GUARD_PAGES=declared` models it), overlapping copies that cross a page end give such a false DIFFERENCE in
  about 1 of 2000 fuzz inputs for the D3D constant copies, and the LZO streams that match in front of their output (a
  generator corner case; on the console those pages are committed because the original reads them) in 22 %. Real game
  inputs do not alias like that.
- A native storing through a raw pointer to an address outside every declared range would still write guest memory (as
  before; the scalar `St*` stores are caught and dropped). Every current raw store targets a declared range, and the
  fuzzer checks that the shadow run writes nothing outside the stack scratch.
- Guest code called from a shadow run (the virtual getters of `8245FF18` and `82B5F0E8`) executes on real memory. In the
  game these are read-only getters; any other guest call path is declared overflow.
- Another thread changing the declared bytes between the original and the comparison gives a false DIFFERENCE, as before.

## Verification

- Host fuzzer `tests/hot_fuzz` (`python3 tests/hot_fuzz/build.py <case>`): `build.py` now also generates `shadow_all.cpp`
  (the shadow build of the case's natives, like `me_hot_shadow.cpp`), and the guard replay in `main.cpp` runs the console
  algorithm with the shared `me_hot_shadow.h`: shadow run (must not change the arena outside the stack scratch), original,
  registers / FPCR, private copy against memory (NaN-payload words tolerated like the cases' own comparison), dropped
  stores and slack. Cases that widen `Writes()` for their host stubs give the console write set in `Case::guard_writes`
  (`8264ADA0`, `8264E178`, `8245FF18`).
- 2026-10-09, EN generated code, 20000 iterations per case: 34 of 37 cases 0 failures (`827D2A00` also at 100000); the
  three others (`822B9200`, `822E3158`, `8245FF18`) do not link against the current generated code, unchanged from
  before this work. RU (scratch tree as in audio-cpu.md, RU generated code): `822631E8`, `8245FF18`, `82AAFB90`,
  `82AAFE20`, `82AC4A50` 0 failures; `8264E178` does not link (pre-existing, same on the previous commit).
- Mutation checks: a shadow memcpy that also writes guest memory is reported on the first iterations ("the shadow run wrote
  guest memory"); a `Writes()` that misses a byte is reported by the existing coverage check.
- Console: not run yet. Expect the usual `[hot] ... guard OK after N checks` lines and no `[hot] DIFFERENCE` with
  `masseffect_hot_guard_period = 1` (every call checked) for the enabled hooks; the EN start crash while loading
  `Engine.xxx` should not come back with `masseffect_hot_crt_memcpy` on.
