# EDRAM transfer shaders without IMUL, and the D3D Swap throttle

Status 2026-10-07: offline work only. Built into `run/me1/ru_imul.nro`; **not yet measured on the console**.
Follows [external-practices-3.md](external-practices-3.md) items 3 and 4.

## Part 1: IMUL / IMUL.HI / integer division in the EDRAM shaders

### Why

NAK on SM50 (`src/nouveau/compiler/nak/sm50.rs`, `impl SM50Op for OpIMul`) emits the legacy IMUL for every
32-bit multiply that is not by a power of two. That opcode is microcoded and has variable latency; `ptxas` never
emits it on sm_50, where XMAD (16x16+32) is the native multiply (Mesa MR !43389, not merged). Our NAK also has no
XMAD, so a "16x16 product" does not help: NIR turns it into the same IMUL. In the EDRAM shaders:

- `x / 80u` becomes IMUL.HI by 0xCCCCCCCD plus a shift, and `x % 80u` adds an IMUL by 0x50;
- `(y / 16u) * pitch`, `DivPitch(...) * pitch` and `... * 80u` are IMULs;
- `tile / pitch_tiles` and `tile % pitch_tiles` with a register pitch (the `Coordinate()` helpers of the raw64
  and 16F compute shaders) are NIR's full udiv lowering: MUFU.RCP plus 3-4 IMUL / IMUL.HI each.

### Tool

The host NAK cost tool of [scene-shader-cost.md](scene-shader-cost.md) only accepts the game's fragment-shader
descriptor interface. A copy that takes any stage and a set-0 layout built from the shader's own bindings
(combined image sampler, storage image, storage buffer) is in
`work/me2-codex-native-cost/out/mesa-gm20b-host-pregen/src/nouveau/vulkan-host/nak-any/`
(`nvk_host_entry_any.c`; rebuild with `cc.sh` then `ld.sh` from `out/mesa-gm20b-host-pregen`). Use:

    nak-any/cost.sh app/src/native/masseffect/shaders/me_edram_color_to_color.frag [outdir]

It compiles with `glslangValidator`, runs the source-matched NAK (SM53) and prints instruction count, NAK's
static cycle estimate and the number of `imul`, `imul.hi`, `f2i`, `i2f` and `mufu` in the assembly
(`outdir/<name>.asm`). Static cycles add both sides of every branch; they are not GPU time.

### Result (all 37 shaders of `app/src/native/masseffect/shaders`, before -> after)

| Shader | IMUL | IMUL.HI | instrs | static cycles | I2F+F2I |
|---|---|---|---|---|---|
| `masseffect_edram_16f_to_16f` | 19 -> 0 | 7 -> 0 | 691 -> 671 | 5136 -> 3555 | 22 -> 36 |
| `masseffect_edram_7e3_to_rgba8` | 8 -> 0 | 1 -> 0 | 262 -> 279 | 2071 -> 1590 | 14 -> 23 |
| `masseffect_edram_rgba8_to_7e3` | 10 -> 0 | 1 -> 0 | 376 -> 397 | 2968 -> 2367 | 25 -> 36 |
| `me_depth_resolve_guestspace` (unchanged) | 0 | 0 | 58 | 531 | 0 |
| `me_depth_resolve_guestspace_msaa2` (unchanged) | 0 | 0 | 172 | 1976 | 0 |
| `me_edram_16f_to_r16g16` | 8 -> 0 | 1 -> 0 | 146 -> 163 | 1505 -> 1014 | 8 -> 17 |
| `me_edram_16f_to_raw64` | 14 -> 0 | 5 -> 0 | 378 -> 368 | 3174 -> 2043 | 12 -> 23 |
| `me_edram_color_stencil_to_buffer` | 18 -> 0 | 4 -> 0 | 810 -> 852 | 6160 -> 5377 | 44 -> 66 |
| `me_edram_color_to_color` | 5 -> 0 | 1 -> 0 | 325 -> 337 | 2328 -> 2099 | 21 -> 28 |
| `me_edram_color_to_depth` | 5 -> 0 | 1 -> 0 | 311 -> 323 | 2336 -> 2107 | 14 -> 21 |
| `me_edram_color_to_stencil` | 5 -> 0 | 1 -> 0 | 265 -> 277 | 2113 -> 1884 | 13 -> 20 |
| `me_edram_depth_1x_to_depth_msaa2` | 5 -> 0 | 1 -> 0 | 325 -> 337 | 3087 -> 2859 | 5 -> 12 |
| `me_edram_depth_msaa2_to_depth_1x` | 5 -> 0 | 1 -> 0 | 353 -> 365 | 3515 -> 3344 | 5 -> 12 |
| `me_edram_depth_msaa2_to_stencil_1x` | 5 -> 0 | 1 -> 0 | 350 -> 362 | 3317 -> 3146 | 5 -> 12 |
| `me_edram_depth_to_16f` | 8 -> 0 | 1 -> 0 | 588 -> 605 | 4804 -> 4313 | 13 -> 22 |
| `me_edram_depth_to_depth` | 5 -> 0 | 1 -> 0 | 255 -> 267 | 2026 -> 1797 | 7 -> 14 |
| `me_edram_depth_to_depth_msaa2` | 5 -> 0 | 1 -> 0 | 361 -> 373 | 3575 -> 3405 | 5 -> 12 |
| `me_edram_depth_to_raw64` | 8 -> 0 | 1 -> 0 | 401 -> 418 | 3531 -> 3040 | 8 -> 17 |
| `me_edram_depth_to_rgba8` | 8 -> 0 | 1 -> 0 | 263 -> 281 | 2465 -> 1977 | 11 -> 20 |
| `me_edram_depth_to_stencil` | 5 -> 0 | 1 -> 0 | 135 -> 147 | 1451 -> 1222 | 5 -> 12 |
| `me_edram_depth_to_stencil_msaa2` | 5 -> 0 | 1 -> 0 | 358 -> 370 | 3377 -> 3207 | 5 -> 12 |
| `me_edram_import` (unchanged) | 0 | 0 | 15 | 456 | 2 |
| `me_edram_r16g16_to_16f` | 8 -> 0 | 1 -> 0 | 146 -> 163 | 1457 -> 965 | 8 -> 17 |
| `me_edram_r16g16_to_rgba8` | 8 -> 0 | 1 -> 0 | 133 -> 151 | 1445 -> 957 | 6 -> 15 |
| `me_edram_r64_to_r64` | 5 -> 0 | 1 -> 0 | 75 -> 87 | 1022 -> 851 | 5 -> 12 |
| `me_edram_raw64_to_16f` | 14 -> 0 | 5 -> 0 | 277 -> 267 | 2554 -> 1423 | 8 -> 19 |
| `me_edram_raw64_to_depth` | 5 -> 0 | 1 -> 0 | 194 -> 206 | 1721 -> 1498 | 6 -> 13 |
| `me_edram_raw64_to_raw64` | 14 -> 0 | 5 -> 0 | 210 -> 200 | 2303 -> 1172 | 4 -> 15 |
| `me_edram_raw64_to_rgba8` | 14 -> 0 | 5 -> 0 | 189 -> 179 | 2208 -> 1077 | 8 -> 19 |
| `me_edram_rgba8_to_r16g16` | 8 -> 0 | 1 -> 0 | 133 -> 151 | 1445 -> 957 | 6 -> 15 |
| `me_edram_rgba8_to_raw64` | 14 -> 0 | 5 -> 0 | 232 -> 222 | 2545 -> 1414 | 12 -> 23 |
| `me_edram_stencil_msaa2_to_buffer` | 18 -> 0 | 4 -> 0 | 318 -> 360 | 3674 -> 2903 | 12 -> 34 |
| `me_edram_stencil_to_buffer` | 18 -> 0 | 4 -> 0 | 314 -> 356 | 3674 -> 2903 | 12 -> 34 |
| `me_resolve_7e3_to_unorm10` (unchanged) | 0 | 0 | 110 | 537 | 5 |
| `me_resolve_7e3_to_unorm10_frag` (unchanged) | 0 | 0 | 95 | 1015 | 7 |
| `me_resolve_exp_bias` (unchanged) | 0 | 0 | 39 | 214 | 1 |
| `me_resolve_exp_bias_frag` (unchanged) | 0 | 0 | 24 | 470 | 3 |

Every IMUL and IMUL.HI is gone; no integer division is left. The fragment shaders get about 12 instructions longer
(three to four conversions replace each multiply), the compute shaders with `Coordinate()` get shorter, and NAK's
static estimate drops everywhere (-7 % to -50 %). The cost of a microcoded IMUL on GM20B is not public, so only the
console can say how much of `edram_alias` (6.5 ms at 1280, 4.2 ms at 960) this saves. The hot ones:

- `me_edram_color_to_color.frag` (cat 12 VS19 and the other fragment conversions). In the common
  same-layout case (bit 17) it had 1 IMUL.HI + 1 IMUL per pixel; in the general case it had 6;
- `masseffect_edram_16f_to_16f.comp` and the R16G16 / raw64 compute conversions (VS(pipeline+1)): 9 to 26 per thread;
- the resolves (`me_resolve_7e3_to_unorm10*`, `me_resolve_exp_bias*`, VS40/VS44) had no IMUL and are unchanged.

### The rewrite

The same helper block is pasted into each shader (glslang `#include` would need a CMake dependency change):

    uint DivPitch(uint tile, uint pitch) { return uint((float(tile) + 0.5) / float(pitch)); }  // existing trick
    uint MulSmall(uint a, uint b) { return uint(float(a) * float(b)); }                      // a * b < 2^24
    uint ModPitch(uint tile, uint pitch) { return tile - MulSmall(DivPitch(tile, pitch), pitch); }
    uint Div80(uint x) { return uint(fma(float(x), 0.0125, 0.00625)); }                    // x < 2^20
    uint Mul80(uint x) { return (x << 6u) + (x << 4u); }
    uint Mod80(uint x) { return x - Mul80(Div80(x)); }
    // 64-bit views (r64_to_r64, raw64_to_depth): Div40 / Mul40 / Mod40, same forms

Mechanical substitutions: `(y / 16u) * pitch` -> `MulSmall(y >> 4u, pitch)`, `x / 80u` -> `Div80(x)`,
`x % 80u` -> `Mod80(x)`, `ModPitch(..) * 80u` -> `Mul80(ModPitch(..))`, `* (80u >> is_64bpp)` ->
`Mul80(..) >> is_64bpp` (exact: 80 m >> 1 = 40 m), `tiles_count * 80u` -> `Mul80(tiles_count)`, `tile % / pitch`
-> `ModPitch` / `DivPitch`, and the stencil buffer index `id.y * words_per_row` -> `MulSmall`. Multiplies and
divisions by 2, 4 and 16 were already shifts. The script that applied it is not kept; the diff is small and regular.

No cvar: the shaders are compiled into the NRO and selected by pipeline, so an A/B switch would mean a second set of
30 pipelines in `masseffect_native_targets.cpp` (which other work is changing). Instead the change is a pure codegen
improvement that is proven to give the same addresses:

### Proof: `tests/cpu/test_native_edram_imul_free.cpp`

Run with `tests/run_all.sh imul_free` (15 s). It checks, exhaustively:

- `DivPitch` / `ModPitch` against `/` and `%` for every tile < 2^16 and every pitch 1..2048 (real: tiles < 4096,
  pitches <= 160), with the reciprocal rounded to nearest, moved by 1 and 2 ulp either way (MUFU.RCP is not correctly
  rounded) and with a true division;
- `MulSmall` for every a < 8192, b <= 2048;
- `Div80` / `Mod80` / `Div40` / `Mod40` / `Mul80` / `Mul40` for every x < 2^20 (widest dispatch: 2048 x 80), FMA
  fused and unfused;

then the whole old and new per-texel maps (fragment: destination texel -> source texel or discard; compute:
invocation -> target and source texel, run and whole-target modes, 32 and 64 bpp) on every texel of 22 pitches
(1-9, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 159, 160) with 1x and 2x sample layouts, and on 16 million random
texels of random runs (pitches up to 2048, any start/count inside 2048 tiles, all sample shifts). A planted
off-by-one in `Div80` makes it fail (8836 mismatches). All other tests still pass, including the headless Vulkan
proofs (MoltenVK) of the three MSAA depth transfer shaders, whose checked-in SPIR-V fixtures
(`app/src/native/masseffect/me_edram_depth_*msaa2*.inc`) were regenerated.

One difference outside the proven ranges: with pitch 0 the old integer division and the new float one return
different garbage. The host never passes 0 (`PitchTilesEDRAM` of a real image; several shaders return early on 0).

### What to measure on the console

`GPU time by pass` cat 12 (VS19, VS11/12/17/18, compute VS(pipeline+1)) and `GPU per Swap edram_alias` against the
previous build, same view, 960 and 1280. If edram_alias does not move, the passes are latency/bandwidth bound and
the next step is fewer conversions, not cheaper ones.

## Part 2: the Swap wait (item 3)

### What the guest waits for

`sub_8222C768` (RU `sub_8222C558`) is `WaitForFence(device, fence, reason)`:

- `[device+10780]` is the next kick's fence value (starts at 3, +2 per kick, `sub_8222C338`);
- `[[device+10768]]` is the fence word the ring writes back (each kick ends with `EVENT_WRITE_SHD` of the counter). Our
  ring parser writes it at parse time (`me_native_system.cpp`, C25), so it means "parsed", not "GPU finished";
- the loop spins in `sub_8222FA98` while `counter - fence < counter - written` (the fence is not reached yet).

The reason code `r5` tells the callers apart. Reason 3 is the Swap throttle and only comes from the Swap (EN `82234000`,
call at `0x82234684`; RU `82233DF0` at `0x82234474`) and from the segment function it calls (EN `8222BD48`, RU
`8222BB38`). Swap N reads `r30 = [device+10780]`, kicks, waits (reason 3) for `[device+14564]` (the fence of Swap N-1),
then stores `r30` there. **So D3D already allows exactly one frame in flight.** The 6-8 ms the render thread spends there
is the ring thread still parsing frame N-1: ring throughput, not a deeper drain. Ring and segment memory have their own
waits (reasons 1 and 2, separate functions, on the read pointer and segment write-backs); resources have their own
fences (reasons 4-15). `[device+16184]` counts Swaps.

### Cvars (in `app/src/native/me_ring_wait.cpp` and the RU overlay copy)

- `masseffect_swap_wait_stats` (default false): every reason-3 wait is timed; every 10 s the log line
  `[swap_wait] last 10 s: N Swap waits, K had to wait, R relaxed to N-2, T ms total (ms/Swap, max), ring lag at entry
  A kicks avg, M max`. The lag is `(counter - written) / 2`, the kicks issued but not yet parsed when the Swap starts
  waiting.
- `masseffect_swap_frames_in_flight` (default 1 = the game's own, range 1-2): 2 makes the reason-3 wait use the fence of
  Swap N-2 (the previous wait's target, only if that wait was in the Swap right before; never newer than the game's own
  target; not when the GPU is flagged hung).

### Why 2 is safe for ordering, and what it does not prove

- The guest's command stream, its order and every packet stay the same; only the time at which Swap returns changes.
- The guest cannot overwrite unread ring or segment memory: those waits (reasons 1/2) are untouched.
- Resources with fences (reasons 4-15, e.g. lock of a busy texture or vertex buffer) still wait for their own fence.
- Unlike `masseffect_frame_lag = 2` (which crashed: it let the **game** thread run two frames ahead of the render thread,
  i.e. mutate UE3 render-thread data early), this does not touch `FRenderCommandFence` / `sub_822FE760`: game and
  render threads stay in lockstep exactly as before. Only the render thread runs ahead of the ring parser.
- Not proven: guest data reused per frame **without** a fence, relying on Swap's implicit "frame N-1 is parsed" (for
  example a double-buffered dynamic buffer pool). With our parse-time retirement the parser still reads each packet
  before the guest can see its fence advance, but a per-frame buffer could be rewritten while the parser has not yet
  reached the draw that reads it. Symptom: wrong geometry or constants for one frame, not a crash. Hence default 1.
- Gain: only if the ring thread is bursty (sometimes ahead, sometimes behind). If it is simply the bottleneck, fps stays
  the same and input latency grows by one frame. Measure `masseffect_swap_wait_stats` first: if `had to wait` is close to
  the Swap count and the lag is about one frame of kicks, the ring thread is the limit and 2 frames will not help.

### Build note

The RU callers call `__imp__sub_8222C558` directly until the codegen sees the new hook; `tools/edition.sh` regenerates
when the hooked set changes. A `MASSEFFECT_D3D_TRACE_ALL` build wraps the same function, so the hook is compiled out there.
