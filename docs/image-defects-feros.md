# Image defects at Feros (Zhu's Hope dock): motion-blur "comb", popping, missing Shepard

Date: 2026-10-07. Offline analysis: nothing was run on the console. Sources:

- The user's two clips `mass-effect-recomp/run/me1/user_rec/2026100806290700-*.mp4` (clip A) and `2026100806292600-*.mp4`
  (clip B). They are about 20 s each and variable frame rate. They were resampled to 30 fps, so frame N is at t = (N-1)/30 s.
  Crops are in `run/me1/user_rec/analysis/`.
- Console log of the same session: `run/me1/loc_zhu_first/console.log` (Russian edition, `location.toml` = the shipped
  `app/masseffect.toml` plus the exec lines). The clips start at 06:29:07 (A) and 06:29:26 (B) on the log clock.
  The file names have one-second resolution, so a clip time and a log line can differ by up to about 0.5 s.
- The game's motion-blur and velocity shaders (PS15184, PS21181, PS24436, PS28926, PS24014), translated again with
  `out/tools/xenos_hlsl` in a private scratch folder, only to read them.

## 1. What the clips show

| clip | t (s) | frames | region (1280x720) | what happens | crops |
|---|---|---|---|---|---|
| B | 12.33-12.40 | 371-373 | 0,0,640,240 | blue-violet lens flare at full strength, gone for 1 frame (372), back | `glow_pop_off_B_*` |
| B | 12.70-12.80 | 382-385 | full | flare gone for 2 frames (383-384); Shepard also missing in 384 | `glow_pop_off2_B_sbs.jpg` |
| B | 3.30-3.47 | 100-105 | 0,0,640,240 | flare pops on in one frame (101), then off (105) | `glow_pop_on_B_sbs.jpg` |
| B | 12.80-13.00 | 385-391 | 0,0,480,200 | flare fades smoothly as the light leaves the screen (normal) | `glow_edge_fade_B_sbs.jpg` |
| B | 12.47-12.60 | 375-379 | 0,0,640,240 | camera still: flare steady | `glow_steady_still_B_sbs.jpg` |
| B | 12.23 | 368 (1 frame) | ~400,200,500,520 | Shepard missing; the hull, ramp and railing are drawn larger (camera pulled in) | `shepard_missing_B368_*` |
| B | 3.50-3.53 | 106-107 | full | Shepard missing, camera closer | `shepard_missing_B106_107_sbs.jpg` |
| B / A | 0.80 / 16.57-16.63 | B25, A498-500 | full | camera suddenly up against the "NORMANDY SR1" hull, Shepard missing | `camera_pullin_normandy_*` |
| B | 1.13 | 35 | full | the same one-frame pull-in | `camera_pullin_B35_sbs.jpg` |
| B | 4.20 | 127 | full | camera otherwise still, one frame from a different angle | `camera_jitter_still_B127_sbs.jpg` |
| B | 12.40 | 373 | head 580,290,160,120 | comb: fine vertical stripes on the hair (period ~3 px at 1280, ~2 px at 960x544) plus ghost copies of the torso 60-80 px to the left, along the turn | `comb_B373_full.png`, `comb_B373_head_x4_sbs.jpg`, `comb_B_moving374_vs_still376_sbs.jpg` |

Shepard is also missing in B 69, 113-114, 157, 207, 384 and A 322, 448, 492-493, 508-509.

Observations:

- **Texture flicker: none found in these clips.** With the camera still (A 0-10 s, B 126-141, 421-434, 518-530,
  553-574), no 32x32 block changes by more than 1.5/255 and comes back. No surface goes flat, black or to a lower mip.
  What looked like flicker is whole-view or whole-object pops on single-frame camera jumps.
- **Flare:** a lens flare from a ceiling light (top left). It pops off/on in one frame (strength 117 -> 18 -> 120).
  Each pop is on a frame where the camera jumps, and those frames also have no motion blur. That matches UE3 treating
  a big jump as a camera cut: the occlusion history is reset (the flare is hidden for a frame) and motion blur is
  skipped. With the camera still it is steady, and it fades normally at the screen edge.
- **Shepard missing** (1-3 frames): on every one of these frames the camera is closer (hull, ramp and railing drawn
  larger). That is UE3 camera collision pulling the camera in and hiding the pawn, which the game does on purpose. It
  happens in bursts during fast swings (B 101-118 cycles about every 7 frames).
- **Comb:** it appears only while the camera moves and is gone as soon as it stops (B376). The ceiling and walls also
  show the fine vertical stripes, not only Shepard.

## 2. Camera jumps, flare pops and the missing Shepard: long frames, not missing draws

- The log has no dropped draws: `FINAL VS selection audit: ... 0 unproven, 0 missing` and `native: ... 0 rejected` in
  every 10 s report. The many `VS identity mismatch` warnings (about 4 per 1000 draws, logged 1 in 256) are discarded
  **candidates**. The draw then uses the shader the ring loaded (`PreferVertexShaderCandidate` keeps `current`), and
  `draw-final` found a shader for every draw. The `(N without shaders)` count in the 10 s line grows by 20-50 k per
  10 s. Those are `PairDraw` results with no VS or no PS (`missing_draw_pairs_`, the `unresolved draw shader pairs`
  line, PS by address). The same count grows on the Normandy too, and the image there is complete. It is worth a
  separate look, but it does not match the symptom: the frames where Shepard is missing show a different camera
  position.
- The camera jumps line up with `[hitch] frame of 63-114 ms` lines (GPU only 27-32 ms per frame). B t=3.3-3.5 s
  (flare pop, Shepard missing 106-107, the 101-118 bursts) = 06:29:29.3-29.5. The log has five hitches there: 29.07,
  29.21, 29.34, 29.41 and 29.48 (63-100 ms each). B t=0.8/1.13 s matches 06:29:27.11. B t=12.2-12.8 s matches 38.80.
  A t=16.6 s matches 23.07. These long frames are on the CPU side: the ring spends 5-48 ms on textures
  (`fingerprints 8-12 ms`, `textures checked 15-30 MB` against a budget of 6 MB per frame). At 10-15 fps the game's
  camera (lag, spring and collision) takes a big DeltaTime step: it overshoots into the hull, hides the pawn, and
  UE3 counts the jump as a camera cut. So these three symptoms come from frame pacing, not from rendering. Removing the
  hitches removes them.

## 3. Motion-blur comb: the velocity path in our renderer

### 3.1 What the game does (from its shaders)

- **Velocity draws** (PS21181, PS24436; `IndividualVelocityScale`) write
  `oC0.xy = clamp(0.5 + 0.5 * v / max(|v|, 1), Scale.z, Scale.w)`, which is the screen velocity packed into [0,1] with
  0.5 = no motion. PS28926 and PS24014 (alpha-masked occluders) write 0. Render target: EDRAM 0x2D0 as `k_16_16`,
  blend ONE/ZERO, `RB_DEPTHCONTROL` 0x00700766 (depth test GEQUAL with depth write).
- **Motion blur PS15184** reads `VelocityBuffer.xy` and checks `x + y > 1e-4`:
  - **dynamic** pixel: velocity = `tex.xy * DynamicVelocityParameters.xy + .zw`;
  - **static** pixel: velocity comes from `SceneDepthTexture`, `ScreenToWorld` and `PrevViewProjMatrix` (camera
    reprojection), scaled by `StaticVelocityParameters` and clamped.

  It then takes 5 taps of `SceneColorTexture` along the vector (`i31.x = 4` loop iterations plus 1) and multiplies by
  `SCENE_COLOR_BIAS_FACTOR`. Shepard is drawn into the velocity buffer, so he takes the dynamic path. While the camera
  orbits him he should get almost no blur.

### 3.2 What our renderer does with it

| step | code | behaviour |
|---|---|---|
| render target `k_16_16` | `me_formats_color.h` `HostFormatTargetColor` -> `R16G16_UNORM`; PS output written unscaled; no `RB_COLOR_INFO` exponent bias | On the Xenos `k_16_16` is **fixed point -32...32** (`rexglue-sdk/include/rex/graphics/xenos.h:305-308`; Xenia stores SNORM and divides the output by 32, `vulkan/command_processor.cpp:6530`). The game's 0.5 is stored by the Xenos as 0x0200 (0.5/32 x 32767). We store 0x8000 (0.5 x 65535). **But see 3.6: the Xenos resolve converts this word back to the value, so the texture holds 0x8001 on both.** |
| content under the velocity draws | f0 (RGBA8, the light attenuation view) -> f4 whole-screen conversion, 1 per frame at 960 (`EDRAM mode4 pair C2D0/f0->C2D0/f4`, `ow-=1ops/408t`); `conversion_copy_32` makes it a `vkCmdCopyImage` | Bit-exact either way: the compute shader `me_edram_rgba8_to_r16g16.comp` and the image copy both move the raw 32-bit word. post-chain.md 1.4 step 7: at 1280 this conversion is **not** seen, so the velocity buffer's ownership history differs between 960 and 1280. |
| resolve | `resolved texture at 14B3F000, 960x544, format 25` | Same bits in a host `R16G16_UNORM` texture. The Xenos does not copy bits here (3.6). |
| fetch in PS15184 | `TextureFormatFor(25)` -> `R16G16_UNORM`; signs: only "biased" and "gamma" are applied (`shader_common.h`), "signed" is read as unsigned with a warning; **the fetch's exp_adjust (dword 3, bits 13-18) is ignored everywhere** | This log has no `signed textures` or `C3 sign:` lines, so the velocity fetch is not signed. Whether it has an exp_adjust is unknown. |
| texture-sign folding (today) | `masseffect_native_fold_texture_signs` | Replaces a constant with the same constant: exact. It cannot change the velocity path. |

What follows from this (written before the console facts; point 1 assumed a bit-copy resolve, which 3.6 shows is
not what the Xenos does):

1. If the game fetches the velocity texture unsigned with exp_adjust 0, the Xenos reads 0x0200/65535 = 0.0078 for
   "no motion" and we read 0.5, **64 times larger**. `DynamicVelocityParameters` was chosen for the Xenos value, so
   every dynamic object (Shepard) gets a large wrong velocity. With exp_adjust +6 both read about 0.5 and our path is
   right by accident. With +5 we are 2x off. The fetch-constant log below settles which case it is.
2. Background pixels keep whatever bits the f0 -> f4 conversion brought. Their bits match the Xbox's, but any
   difference in what f0 holds at 960 (see the missing conversion at 1280) would turn noisy light-attenuation words
   into "dynamic" velocities. That would produce per-pixel, column-correlated noise in the blur.
3. A depth mismatch between Shepard's base pass and his velocity pass (separately translated vertex shaders: FMA
   contraction differs, and `MASSEFFECT_SHADER_PRECISE_POSITION` is opt-in) makes the GEQUAL test fail on some pixels.
   That leaves holes in his velocity coverage, and those pixels take the camera reprojection instead. It fits both
   the torso ghost along the turn direction and the 2-pixel stripes on the hair.

### 3.3 Ranking and regression status

1. **(most likely) Shepard's dynamic velocity is wrong or partial**, from mechanism 1 and/or 3. Evidence: the ghost
   copies of his torso go along the camera turn, so he is blurred as if he were static or had a large velocity, and
   on the Xbox he would be sharp. **Pre-existing:** `k_16_16` has been `R16G16_UNORM` with no range or exp_adjust
   handling since the native renderer began.
2. **Background velocity garbage under the velocity pass** (mechanism 2): it would explain the 2-pixel vertical stripes
   on the walls and ceiling too. Pre-existing as far as the code shows. `conversion_copy_32` (2026-10-06) is
   bit-identical to the compute path it replaced, so it is unlikely to be the cause, but the A/B below costs one run.
3. **Part of the look is inherent:** 5 taps over a whole frame of camera motion at 10-25 fps give discrete ghost
   copies. FSR's sharpening (RCAS) makes them crisper. The game skips motion blur on camera cuts, which is why some
   jump frames are clean.

### 3.4 Diagnostics added (cvar `masseffect_diag_velocity`, int bit mask, default 0, no change when 0)

Code: `masseffect_native_draws.cpp` (`Draw`, after the target loop) and `masseffect_native_targets.cpp`
(`ConvertAliasEDRAM`, the colour resolve). The motion blur PS is found by content since 2026-10-08
(`masseffect_diag_velocity_blur_ps = 0`, the default: the PS whose original container names both
`DynamicVelocityParameters` and `VelocityBuffer`; in the current package only n15187 does, it was n15184 before the
package gained 8 entries). A value > 0 forces that library number. The log line `velocity diag: motion blur PS
identified by content: n...` shows the match.

| bit | effect |
|---|---|
| 1 | Log, once per distinct value (at most 256 lines): `velocity diag: k_16_16 target draw VS PS color info ... exp bias` for draws into a `k_16_16` target, and `velocity diag: PS n15187 fetch r words ...: format, base, WxH, signs x y z w, exp_adjust, num_format, swizzle, filters` for every fetch of the motion blur PS and every `k_16_16` fetch. Since 2026-10-08 also: `velocity diag: motion blur PS n... DynamicVelocityParameters c11 ... StaticVelocityParameters c10 ...` (on every change, at most 160 constant lines), `velocity diag: velocity draw PS n... IndividualVelocityScale c1 ... depth control ...`, and `velocity diag: k_16_16 resolve ...: copy dest info (format, number, exp bias), command` (once per distinct value). No image change. |
| 2 | Skip the draws into `k_16_16` targets: dynamic objects get the camera velocity, like the background. |
| 4 | Skip the motion-blur draws. Reference picture without motion blur. |
| 8 | Draw into `k_16_16` targets with the depth test and depth write off (from a register copy). If Shepard's comb disappears, mechanism 3 is confirmed. |
| 16 | Replace each RGBA8 -> `k_16_16` conversion with a clear of the `k_16_16` image to 0, so uncovered pixels read "no dynamic velocity". If the stripes on the walls disappear, mechanism 2 is confirmed. It wipes the image's other tiles too; diagnostic only. |

### 3.5 Fixes, depending on the result

- **Mechanism 1** (the fetch has exp_adjust 0 or 5): emulate the Xenos `k_16_16` target. Add an output epilogue
  specialization for `k_16_16` slots, next to `kSpecTarget7e3`, that writes `bits = int16(round(clamp(v, -32, 32) *
  32767 / 32))` as UNORM `(bits & 0xFFFF) / 65535`. This is exact for ONE/ZERO blending, which is the only blend the
  velocity pass uses. Also honour the fetch exp_adjust (and "signed") for format 25 in the sampling helper, or as a
  folded per-fetch scale with the same machinery as `fold_texture_signs`. Both halves are needed together. The
  resolve and the conversions already move raw words, so they stay correct.
- **Mechanism 3**: build the shader package with `MASSEFFECT_SHADER_PRECISE_POSITION=1` (NoContraction on the
  position path of every VS), or add an exact-equality depth path for the velocity pass.
- **Mechanism 2**: trace what owns 0x2D0 just before the velocity pass at 960 (`masseffect_diag_trace_tile = "930"`,
  post-chain.md 2.B) and compare it with 1280. The clear the game expects is probably issued with a 1280-sized extent
  that the internal-resolution hooks do not cover.

### 3.6 Result (2026-10-08): what the Xenos does with k_16_16, and the emulation

**Console facts** (`masseffect_diag_velocity = 1`, `run/me1/manual6/masseffect_308.log`): every draw into the
`k_16_16` target has `RB_COLOR_INFO` 000402D0 (EDRAM base 0x2D0, exponent bias 0), mask F, blend 00010001
(ONE/ZERO/ADD). The motion blur PS (n15187) fetches it with words `87804802 14B2F099 0043E3BF 00001690 00000000
00000218`: format 25, 960x544, signs 0/0/0/0 (unsigned), exp_adjust 0, num_format 0 (fraction), swizzle xy01, point
filtering. Confirmed on the console: characters vanish for single frames during camera turns only with the game's
Motion Blur option on, and only on long frames (80-100 ms hitches).

**Xenos semantics** (sources: `rexglue-sdk/include/rex/graphics/xenos.h` 305-308; xenia-canary
`src/xenia/gpu/shaders/pixel_formats.xesli` `XeUnpackR16G16Edram`, `XePackR16G16Edram`, `XePackFixed`, the quoted
inferred-lighting paper above them; `resolve.xesli` `XeResolveLoad2RGBAColors` (unpack, `* dest_exp_bias_factor`);
`draw_util.cc` `GetCopyShader`: the raw "fast" resolve is used only when `IsColorResolveFormatBitwiseEquivalent`, which
is false for `k_16_16`; the texture formats `k_16_16_EDRAM` (13) and `k_16_16_16_16_EDRAM` (21) exist precisely to keep
the EDRAM representation through a resolve):

| step | Xenos | old host path |
|---|---|---|
| PS output v into a `k_16_16` RT | word = int16(round(clamp(v, -32, 32) * 32767 / 32)); 0.5 -> 0x0200 | UNORM16(v): 0.5 -> 0x8000 |
| clear, EDRAM aliasing (`k_8_8_8_8` <-> `k_16_16`) | the 32-bit word as is | the word as is (same) |
| resolve to a format-25 texture | value = max(int16 * 32 / 32767, -32), times 2^copy_dest_exp_bias, packed by copy_dest_number: 0 -> round(saturate(value) * 65535) | raw copy of the host texel |
| fetch format 25, signs 0, exp_adjust 0, num_format 0 | texel / 65535 | texel / 65535 (same) |

So for what the velocity draws write (values in [0, 1], ONE/ZERO, resolve exp bias 0, number 0) both paths give the
motion blur the same number: 0.5 written comes back as 0x8001 on the Xenos and 0x8000 here. The only differences are
the EDRAM quantization (steps of 32/32767, at most 33 UNORM16 steps, so small velocities below 0.00049 become 0) and
how foreign words left by the light-attenuation view are read (the Xenos reads them as signed fixed point and
saturates, the old path reads them as UNORM). **Mechanism 1 as written in 3.2 (a 64x error) does not happen if the
resolve converts, as xenia models it.** It would happen only if the resolve copied the word raw; the
`DynamicVelocityParameters` log line decides that on the console: `c11.zw / c11.xy` = -0.5 means the game expects the
value back (converting resolve); about -0.0078 would mean it expects the raw word.

**What the motion blur shader does with the buffer** (n15187, translated with `out/tools/xenos_hlsl`): it reads the
velocity texel, and if `x + y > 1e-4` uses `v = texel.xy * c11.xy + c11.zw` (dynamic) **without any clamp**, otherwise
the camera reprojection clamped to `StaticVelocityParameters`. Both are then scaled by `|v / c10.wz|^2` (a cubic
response) and the scene colour is averaged over 5 taps at -0.4, -0.2, 0, +0.2, +0.4 of the vector. The velocity
draws (n6414, n24443, n21185, ...) write `clamp(0.5 + 0.5 * d / max(|d|, 1), c1.z, c1.w)`, `d` = screen motion
times `IndividualVelocityScale.xy`; the occluder draws (n21076, n12118, n28619, n28934) write 0. A dynamic pixel whose
texel is far from 0.5 therefore gets an unclamped, cubically amplified vector, and 4 of its 5 taps land on the
background: the character "vanishes" and the background looks blurred and shifted, exactly the console symptom. On a
long frame the true per-frame motion `d` of a character is large, so this happens only on hitch frames. If that is
the cause, it is the game's own behaviour at 10 fps (the Xbox never ran it with 100 ms frames) and the remedy is to
remove the hitches (section 2), not the format.

**The emulation** (cvar `masseffect_native_velocity_16_16`, int, init-only, default 0 = old path):

- `1` = exact Xenos/xenia semantics. The host `k_16_16` image (still `R16G16_UNORM`) holds the EDRAM word. Every draw
  into a `k_16_16` slot gets a pixel shader epilogue (`me_fixed16_spirv.h`, `TransformFixed16Encode`, applied in
  `PipelineFor` after restore-into-7e3; specialization bits 4-7 = slots, never prewarmed) that stores
  `(word + 0.25) / 65535`, so a UNORM16 attachment stores exactly the word whether it rounds to nearest or toward
  zero. A resolve from such an image is a fragment pass (`shaders/me_resolve_fixed16_frag.frag`, variant 6 of the
  colour conversion passes, `ResolverFixed16Frag`) that unpacks the word, applies `copy_dest_exp_bias` and packs by
  `copy_dest_number` (exact 16-bit pattern into `R16G16_UNORM`; other normalized destinations for number 0). Clears and
  the EDRAM aliasing conversions already move the word and are unchanged. The fetch needs nothing for this game
  (format 25 unsigned, exp_adjust 0 = `texel / 65535`). A blended draw into `k_16_16` (none seen) is logged
  (`k_16_16 encode: ... blends with ...`): it would blend encoded words.
- `2` = the same encode, but the resolve stays a raw copy: the alternative hypothesis (the game reads 0.5 as 0.0078).
  Use it only to compare pictures if the `c11` line says the game expects the raw word.
- Not implemented: honouring a non-zero fetch exp_adjust or a signed/integer fetch of format 25 (it would need
  `shader_common.h` in the shader package; the velocity fetch does not use them). Independent of the format, nothing was
  changed for mechanism 3 (depth mismatch): there is no console evidence for it yet; `masseffect_diag_velocity = 9`
  tests it.

Tests: `tests/cpu/test_native_fixed16_spirv.cpp` (every 16-bit pattern and a dense float sweep through the epilogue
and a UNORM16 store with both rounding modes; resolve model; the SPIR-V transform on a synthetic module). The
transform was also applied to the game's velocity pixel shaders n6414, n24443, n21076, n12118, n7105, n28934, n11213,
n12474, n21185, n28619 from the console package in a scratch folder: all accepted (1 store each) and `spirv-val`
clean (Vulkan 1.2).

**Console check** (cold start, the Feros/Eden Prime route with camera turns, Motion Blur on):

```toml
masseffect_diag_velocity = 1          # with each run below: read the c11 / c10 / c1 and resolve lines
masseffect_native_velocity_16_16 = 1
```

Look for: `k_16_16 encode: PS n... module with the fixed-point -32...32 epilogue` (several PS), `k_16_16 resolve
(fixed point -32...32 decoded): 960x544 ...`, `velocity diag: k_16_16 resolve ...: ... number 0, exp bias 0 ... decoded`.
No `epilogue failed`, `NOT decoded` or `blends with` lines. The picture should equal mode 0 (expected, per the table
above); if the vanishing characters change with mode 1, the resolve parameters or the foreign words matter and the log
lines say which. Then compare the `c11` values on a normal frame and on a hitch frame (the hitch lines are in the same
log): a change of `c11` with the frame time, or `c11.zw / c11.xy` far from -0.5, is the next lead.

### 3.7 Long frames and the motion blur: the math, and a frame-time fix (2026-10-08, not yet run on the console)

**Which constants carry the per-frame motion** (n15187 and the velocity PS n6414/n24443/n21185, translated with
`out/tools/xenos_hlsl`; shader-relative `cN`, words `4N..4N+3` of the PS bank):

| constant | meaning (UE3 `MotionBlurShader.usf` / `VelocityShader.usf`) |
|---|---|
| motion blur c2-c5 `ScreenToWorld`, c6-c9 `PrevViewProjMatrix` | static pixels: world = ScreenToWorld * (ndc.xy * z, z, 1); prev = PrevViewProj * world; `v = (ndc - prev.xy / prev.w)` |
| motion blur c10 `StaticVelocityParameters` | xy = NDC -> UV scale (UE3: 0.5 x MotionBlurAmount, -0.5 x MotionBlurAmount), zw = MaxVelocity in UV (UE3: `MAX_PIXELVELOCITY` 16/1280 x MaxVelocity, y times SizeX/SizeY). Static `v = clamp(v * c10.xy, -c10.zw, c10.zw)` |
| motion blur c11 `DynamicVelocityParameters` | texel -> UV: `v = texel.xy * c11.xy + c11.zw`, **no clamp in the shader**. UE3 builds it from the same MaxVelocity: (2Mx, -2My, -Mx, My), so the decode range is exactly +-c10.zw |
| velocity PS c1 `IndividualVelocityScale` | `d = (pos - prev pos)_ndc * c1.xy`, stored as `clamp(0.5 + 0.5 d / max(|d|, 1), c1.z, c1.w)`: the per-object motion normalized so that |d| = 1 is MaxVelocity. prev pos uses the previous frame's LocalToWorld and ViewProjection (VS constants) |
| motion blur c1 `MinZ_MaxZRatio`, c0 `SCENE_COLOR_BIAS_FACTOR` | depth linearization (pixels nearer than 14 units take depth 65504, i.e. rotation only), output scale |

Then both are multiplied by `|v / c10.zw|^2` (a soft threshold: small motion is cubically suppressed, motion at the
limit stays 1x) and the scene colour is averaged over 5 taps at -0.4, -0.2, 0, 0.2, 0.4 of that vector.

**What a long frame does.** Every input is a per-frame displacement: UE3 (as far as its source shows; ME1 is an
Xbox UE3 build of the same era) does not scale velocities by DeltaTime, which is why the comment in UE3 says
`MAX_PIXELVELOCITY` is "per 30 fps frame". A 100 ms frame has 3x the camera and character displacement of a 33 ms
frame. The encode normalization and the static clamp bound the result at MaxVelocity (dynamic: |v / c10.zw| <= 1;
static: per component, so the cubic factor is at most 2 and the tap span at most 0.8 x 2.83 x MaxVelocity), so with
consistent constants a long frame gives "maximum blur everywhere that moves", not an unbounded vector. A character
disappearing completely therefore means one of: (a) the game's MaxVelocity is large (then every hitch frame is a
max-blur frame: the camera sweep and the character's own sweep both saturate), (b) `c11` is not consistent with
`c10.zw` (unclamped dynamic decode), or (c) the velocity buffer is not rewritten on that frame (UE3 skips the
velocity pass when it resets the previous transforms, e.g. on a camera cut, see section 2) and the blur reads stale
or foreign words. The console line `velocity diag: motion blur PS ... c11 ... c10 ... (frame N ms)` decides between
them (c10.zw is MaxVelocity; c11.xy should be (2 c10.z, -2 c10.w)).

UE3 also has camera-cut handling: `FMotionBlurParams` RotationThreshold (45 degrees) / TranslationThreshold, and the
player controller's camera cut. On a cut the previous view transforms are reset (PrevViewProj = ViewProj), so static
velocity is 0 and nothing blurs. The cut is decided on the game thread; we see it only as c6-c9 equal to the current
view, which is fine: those frames are already clean (section 2: "some jump frames are clean").

**The 960x544 hypothesis is rejected.** Nothing in the chain is in pixels: positions are NDC, the vectors are UV, and
the taps add the vector to the UV. The velocity buffer and the scene colour are both fetched by UV at 960x544. The
only resolution-dependent number is the aspect in My = Mx x SizeX / SizeY: 960/544 = 1.765 against 1280/720 = 1.778,
0.7 %, whichever size the game uses. The internal-resolution hooks do not shrink or enlarge blur vectors.

**The fix** (cvar `masseffect_native_motion_blur_frame_fix`, int bit mask 0-7, default 0 = the game's constants; read
once per frame). The velocity draws (a `k_16_16` colour slot and a PS whose container names
`IndividualVelocityScale`) and the motion blur PS (container names `DynamicVelocityParameters` + `VelocityBuffer`)
get a patched copy of their PS constant bank, uploaded as if the guest had written it (`PatchMotionBlurConstants` in
`masseffect_native_draws.cpp`, used in the PS constant upload; constant mode 2 forces a fresh upload when switching
to and from a patched draw, the same way the tone-map override uses mode 1, and content reuse compares the patched
words). Frame time = the last guest Swap-to-Swap interval measured on the ring thread at `Present`
(`masseffect_native_targets.cpp`, steady_clock, the same value the `[hitch]` lines print; exposed as
`ContextTargets::SwapIntervalMs()`). While the ring is the bottleneck the game thread waits for it, so the game's
DeltaTime for the frame being drawn is about that interval (one frame of lag at the start and end of a hitch burst).

| bit | effect |
|---|---|
| 1 | Scale like a 30 fps frame: k = min(1, `masseffect_native_motion_blur_ref_ms` (33.3) / frame ms). Velocity draws: c1.xy *= k (the stored motion is the one a 33 ms frame would have, still normalized at MaxVelocity). Motion blur: c10.xy *= k (static/camera part, the clamp c10.zw unchanged). c11 is left alone (the dynamic part was scaled at the encode). Frames <= 33.3 ms are untouched. |
| 2 | Re-apply MaxVelocity to the dynamic decode: if 0.5 x |c11.x| > |c10.z|, c11.x and c11.z are multiplied by |c10.z| / (0.5 |c11.x|) (same for y/w). A no-op when the constants are UE3-consistent; logged when it acts. |
| 4 | Frame > `masseffect_native_motion_blur_cut_ms` (60) = camera cut: c10.xy = 0 and c11 = 0, every tap samples the pixel itself (the blur pass becomes a copy, x 5 x 0.2). Also covers a stale velocity buffer. |

Recommended: **5** (1 + 4): 33-60 ms frames blur like 30 fps frames, hitches above 60 ms are not blurred at all (on
the Xbox those frames would have been 33 ms frames anyway). Log: `motion blur fix: PS n... frame N ms, k K[, camera
cut][, c11 clamped to MaxVelocity]: c10 ... -> ..., c11 ... -> ...` (frames with k < 0.95, a cut or a clamp; at most
400 lines). The diag line now ends with `(frame N ms)` and shows the guest values before the patch.

Caveat: if the console log shows c10/c11 changing with the frame time on their own, the game already compensates
DeltaTime and bit 1 would double the correction; then use 4 (and 2 if c11 is inconsistent) only.

**Console check** (cold start, Feros/Eden Prime route with fast camera turns, Motion Blur on, one run each):

```toml
# 0. facts first: c10/c11 against the frame time (no image change)
masseffect_diag_velocity = 1
# 1. the spirit-of-30-fps scale only
masseffect_native_motion_blur_frame_fix = 1
# 2. the cut only (hitches unblurred)
masseffect_native_motion_blur_frame_fix = 4
# 3. both (recommended)
masseffect_native_motion_blur_frame_fix = 5
```

What to look for: Shepard and NPCs no longer vanish on hitch frames (with 4/5 the hitch frames look sharp; with 1
they keep a 30 fps amount of blur); the comb on normal 30-40 ms frames is unchanged or slightly weaker (k 0.83-1);
`motion blur fix:` lines appear in the same seconds as the `[hitch]` lines; no `DIFFERENCE: PS constants reused`
line. In run 0, read `c10.zw` (MaxVelocity in UV: 0.0125 is UE3's default) and check `c11.xy = (2 c10.z, -2 c10.w)`;
if not, add bit 2 (`= 7`). If the characters still vanish with 4 on frames the log marks as cut, the cause is not the
blur vector, and the next test is `masseffect_diag_velocity = 5` (blur off) on the same route.

### 3.8 One-frame vanishing of whole meshes on normal frames (2026-10-09, Port Hanshan videos)

**Sources.** Build with the shipped defaults (`motion_blur_frame_fix = 4`, `velocity_16_16 = 0`, query mode 0), Port
Hanshan, handheld. Console videos `scratchpad/flicker2/2026100902033000-*.mp4` (clip a), `...02034000-*.mp4`,
`...02040300.mp4` and the older `scratchpad/flicker/v.mp4` (clip v). They are variable frame rate: every game frame is
stored once with its own timestamp, so the native frames (`ffmpeg -fps_mode passthrough`) and their pts give each
game frame's display time. Index 156 of the user's `fps=30` extraction of clip a is native frame 175.

**What the bad frames show.**

| clip, native frame | camera | interval before / shown | missing | still there |
|---|---|---|---|---|
| a 175 (30 fps idx 156) | turning | 34 ms / 33 ms | Shepard, both crate stacks, the small box, the crane arms and the ship's dark structures in the window | floor, wall and railing, Kaira (smeared by the camera blur), ceiling lights, HUD |
| v 450 | **still** (phase correlation 449 -> 451: 0 px) | 50 / 33 ms | Shepard, the "00" docking ramp, the orange crates, the crane head, a ceiling beam, the machinery at the right edge | floor, both concrete blocks, the crane pole, far hangar, Normandy hull, the blue light shaft, HUD |
| v 473, 541, 543-544, 568-569, 592; a 410, 415-417 | turning | 33-50 ms | the same kind of set | the same kind of set |

- The frames are **not long**: a 175 follows a 34 ms interval and is shown 33 ms. `motion_blur_frame_fix = 4` (cut
  above 60 ms) never sees them. Section 3.7's hitch explanation does not cover this defect.
- Where the meshes were, the image shows the **geometry behind them, correct for this frame's camera**: the floor
  seams run on through Shepard's place without any trace of him (contrast-stretched crops), the ceiling lights
  behind the ramp are sharp dots, the far hangar wall is complete.
- **Everything else is unchanged.** With the camera still (v 449/450/451) the static regions have the same mean,
  percentiles and spread to within 0.3 levels (floor, concrete block, ceiling), and the difference mask contains only
  the missing meshes. So: no exposure jump (eye adaptation), no other tone curve (a 7e3 scene read as UNORM10 would
  change every pixel), no fog change, no camera pull-in. The frame is 12-15 levels brighter on average only because
  the bright foggy background replaces dark meshes ("the window drawn over them").
- **The motion blur vector cannot do this.** Section 3.6/3.7: static vectors are clamped to MaxVelocity (c10.zw =
  0.0125 x 0.0071 UV), dynamic ones decode a [0, 1] texel with c11 = (0.025, -0.0142, -0.0125, 0.0071) (console log),
  and the 5 taps span at most 0.8 x 2 x MaxVelocity, about 20 px at 960x544. A 130 px wide Shepard would leave his
  centre tap (20 %) visible; there is no trace of him, and v 450 has no camera motion at all. So the scene colour the
  blur sampled (`15204000`, `k_16_16_16_16_FLOAT`, resolved from `C2D0/f3`) **did not contain those meshes**, or they
  were never drawn that frame. Kaira in a 175 is semi-transparent: she was in the scene colour and is smeared by the
  camera-motion (static) path, so she is not a velocity object there.
- The missing set looks like the `query_mode = 1` picture (`run/me1/ab/occ_q1_hanshan/film`: meshes gone, BSP and
  the bright fog stay), but in mode 0 every finished query reports 1000 samples minus a begin structure the ring
  zeroes: the game cannot see a 0. User's earlier test (2026-10-08): with the game's Motion Blur option off the
  vanishing does not happen.

**Console logs** (`run/me1/ab/od1_hanshan`, `np_hanshan`, `q3_hanshan`, `n12_*`; automated runs, mostly still
camera, not the user's session): no per-frame warning that matches. `15204000` is resolved about 6.4 times per frame,
alternating `k_2_10_10_10` (UE3's raw scene save before each shadowed light, 3 lights per frame here) and
`k_16_16_16_16_FLOAT`, so the entry changes image twice per frame through the allocation pool (`resolved texture
changes at 15204000` lines). Ruled out from the logs: CPU-overwrite of resolve destinations (0 hits), mid-frame
submissions for a full upload buffer (0), rejected draws (0), the EDRAM overwrite proof on depth-tested draws (its
`depthstencil` reject counter is 0 and the `C2D0/f4 -> f3` switch after the velocity pass transfers all 408 tiles),
partial resolves into another entry (no line).

**Root cause: not identified offline.** What is proven is the shape (the blur's input lacked a set of meshes on a
normal-length frame). The candidates, in the order the console should separate them:
1. **The blur samples an older scene than the frame drew** (renderer): a stale binding of `15204000`, or the scene
   resolve taken before some draws into `C2D0/f3`, or recorded out of order (deferred recording), or a resolve of the
   other format still served at the blur.
2. **The meshes were not submitted or not recorded that frame** (game or ring): the frame's draw count drops.
3. **The `C2D0/f3` image lacked them although they were drawn** (EDRAM views): they went through another view or a
   redirect that did not reach the 7e3 image (`masseffect_native_restore_into_7e3 = true` is new in the shipped toml,
   `resolve_7e3_direct`, `conversion_copy_32`, the overwrite proofs).

**Diagnostic: `masseffect_diag_blur_source` (int, default 0, no image change).** The targets keep a per-frame
journal: every resolve (`address#n:format<view@draws`: n-th write into that address this frame, the EDRAM view it
was read from, the draws recorded into that view so far in the session), the draws into `k_16_16` views, and the
frame's draw count. For each 2D texture of the motion blur draw it logs which resolve wrote the image it samples
(`write #k/K`, or `N frames ago`), the draws that reached the source view after it and how many of them came before
the velocity pass, the velocity draws after it, and `STALE BINDING` if the sampler caches gave a slot other than the
one a fresh preparation gives now. One line per frame with a blur draw, at the Swap:

```
[native] blur source frame F (33.4 ms): draws R recorded of C, velocity draws V; resolves (...): 15204000#1:f7<C2D0/f2@..
  ...; blur: PS n... t0 15204000 fetch f32: f32 write #2/2 from C2D0/f3 (+0 draws into it since), velocity draws since
  V, 0 of them before the velocity pass; PS n... t1 1F376000 ...; PS n... t2 14B2F000 ...
```

`N` = how many blur frames are logged in full (300 = about 10 s at 30 fps); after that only flagged frames are logged,
as `blur source SUSPECT ... -- <reasons>` (warning level, at most 2000): a texture written in an earlier frame, a fetch
served by a resolve of another format, scene-view draws between the scene resolve and the velocity pass, a stale
binding. A summary every 10 s: `blur source (10 s, ...)`. Cost when 0: one branch per draw and per resolve.

**Guard: `masseffect_native_motion_blur_source_guard` (bool, default true).** For the one motion blur draw per frame,
every 2D texture served from a resolve is prepared once more without the sampler caches; if that gives another slot
than the one the loop bound, the fresh slot is bound and the cache entries that gave the old one are dropped (log
`motion blur source guard: ... bound again`). Exact: in a frame without a stale binding the slots are equal and
nothing changes; the cost is three map lookups per frame. It covers candidate 1's stale-binding case only; the other
cases are what the diagnostic is for. (The extra preparation adds one to the `reads` of those addresses in the
`faces resolved` report.)

**Console check** (same route, handheld, turning from the dark corner to the bright window, record a video):

```toml
# run 1: image unchanged, journal for the first 900 blur frames (about 30 s) plus every flagged frame
masseffect_diag_blur_source = 900
```

Find the flash frames in the video (`ffmpeg -fps_mode passthrough`, the frame whose mean jumps 10+ levels for one frame),
convert their time to the log clock (the clip name is its start time; the log line has a wall-clock prefix) and read the
`blur source` lines of those frames against their neighbours:
- `SUSPECT` with `received N draws between its resolve and the velocity pass` or `served by a f7 resolve` or `written
  1 frames ago` or `STALE BINDING` on a flash frame: candidate 1, and the reason names the mechanism.
- No flag, but `draws R recorded of C` clearly lower than the neighbours (hundreds fewer): candidate 2 (compare `C`:
  if `C` drops too, the game did not submit them; if only `R` drops, the renderer dropped them).
- No flag and normal counts: candidate 3. Then one run each, cold, same route, with the journal still on:
  `masseffect_native_restore_into_7e3 = false`, then `masseffect_native_resolve_7e3_direct = false`, then
  `masseffect_native_deferred_recording = false`; the run without flashes names the switch.

#### 3.8.1 Console result (2026-10-09, `scratchpad/flicker3`, `masseffect_diag_blur_source = 900`)

Clip `2026100903033700-*.mp4` (20.2 s; the album name is the clip END: clip start = 03:03:16.2 on the log clock).
Flashes in the video at 7.75, 14.27, 16.28 and 18.87 s (Shepard, crates, crane parts gone; same signature as above).
Their spacing (6.52, 2.01, 2.58 s) matches four log frames exactly (03:03:23.966 frame 4643, 30.508 frame 4816,
32.624 frame 4875, 35.211 frame 4950: spacing 6.54, 2.12, 2.59 s). Every one of them, against its neighbours:

| | neighbours (e.g. 4640-4642) | flash frame (e.g. 4643) |
|---|---|---|
| draws submitted by the game (Draw calls) / recorded | 950-985 / 500-520 | **691 / 326** (about 30 % fewer, same recorded share) |
| velocity draws | 6-11 | **2-3** |
| shadowed-light cycles (shadow map, light attenuation, raw scene save `15204000:f7`) | 5 | **0** (one shadow-map resolve only) |
| blur's scene colour | `15204000` f32, last write of the frame from `C2D0/f3`, 0 draws after it | the same |
| stale binding, old write, format mismatch | none | none |

So the renderer recorded faithfully what it got and the blur read the current scene: **candidate 2**. The game itself
submitted a reduced frame: no shadowed-light passes and a set of meshes missing (the movable/occlusion-tested set:
Shepard, crates, crane heads, the docking ramp). About 20 such frames occur in the 20 s window, often in pairs 2-3
frames apart (4643/4646, 4816/4819, 4999/5002, 5032/5034, 5104/5106); the visible flash is the one where the dropped
meshes cover a large area. In the same 10 s windows the ring sees more occlusion-query BEGINs than ENDs (29 s window:
27978 begun / 27330 ended; 39 s window: 23881 / 22259; "begins without end" grows 3186 -> 3834 -> 5456). Per 10 s
window (end time: begun - ended): 02:59 330, 03:09 2856, 03:19 0, 03:29 648, 03:39 1622, 03:49 1856, 03:59 0; the
windows with a deficit are exactly the ones containing reduced frames (02:58.9-03:00.2, 03:02.3-03:07.2,
03:23.9-03:27.1, 03:30.5-03:38.2, 03:39.3-03:42.2), the balanced ones (03:09-03:19, 03:49-03:59) contain none. Everything the game culls this way (lights and primitives) is occlusion-tested
in UE3, so the next suspect is how the game's occlusion reads end (mode 0 answers 1000 at every END the ring parses;
a query whose END never comes keeps D3D's sentinel, and the render thread's poll `sub_826E7C98` gives up after 100000
iterations, now with `masseffect_wait_occlusion_us = 500` ring-progress waits instead of 100 us sleeps).

(The journal flagged every frame as `scene draws between the scene color resolve and the velocity pass` with a huge
number: a bug of the diagnostic, a snapshot not reset on a new write; fixed. In ME1 the velocity pass comes first in the
frame, before the scene resolves.)

**Next diagnostic: `masseffect_diag_occlusion_poll` (bool, default false, no behaviour change)** in
`me_ring_wait.cpp` (EN and the RU overlay): counts the Sleep(0) iterations of the occlusion poll and logs
`[ring_wait] occlusion poll burst: N iterations in M ms` when more than 5000 come within 100 ms (a query the ring has
not answered), plus a 10 s total. A burst in the second before a flash frame means the game culled because a query
read gave up.

**Follow-up runs** (same route, video, one run each, cold):
```toml
# A: journal + poll diagnostic, nothing else changed
masseffect_diag_blur_source = 900
masseffect_diag_occlusion_poll = true
# B: the game's own Sleep(0) in the poll (100 us sleeps, the poll can no longer run out quickly)
masseffect_wait_occlusion_us = 0
# C: late frames presented at the next VBlank again (frame pacing as the game expects)
masseffect_present_immediate_threshold = -1
```
If B removes the flashes, the fix is in the poll wait; if A shows no bursts and B/C change nothing, compare the
`occlusion queries (mode 0)` begun/ended lines and capture which query addresses lose their END.

#### 3.8.2 Cause: the ring takes an occlusion-query END for a BEGIN and zeroes its result (2026-10-09)

**Console A/B of 3.8.1** (user runs, `../mass-effect-recomp/run/me1/flick/uA.log`, `uB.log`, `uC.log`, counted with
`../mass-effect-recomp/tools/me1_reduced_frames.py`):

| run | change | reduced frames per 1000 gameplay frames | occlusion poll bursts |
|---|---|---|---|
| A | shipped defaults (query mode 0) | 16.8 | 0 |
| B | `masseffect_wait_occlusion_us = 0` (the game's own Sleep(0) in the poll) | 17.0 | 0 |
| C | `masseffect_present_immediate_threshold = -1` | 11.9 (fewer frames, same order) | 0 |

Neither the poll wait nor the present timing is the cause. In every run the `occlusion queries (mode 0)` line shows
`begun - ended` per window equal to the growth of "begins without end" (uA, 03:46:59: 82078 - 80862 = 1216 = 20274 -
19058), and "draws inside" equals "ended" (one box per query).

**What the code says** (English addresses; Russian in brackets, from `editions/ru/address_map.json`):
- D3D `Issue` `sub_82228F58` [`sub_82228D48`]: for an occlusion query (type 9) the **BEGIN** path stores the sentinel
  `0xFFFFFEED` into words 0..3 of every tile's end structure **on the CPU** (then `dcbf`) and emits the begin
  `EVENT_WRITE_ZPD` (`sub_822289C8`, `RB_SAMPLE_COUNT_ADDR` = end + 0x20). The END path only emits the ZPD at the end
  structure and stores the kick fence in `[query+20]`. (occlusion-queries.md said the END stores the sentinel; it is the
  BEGIN.)
- D3D `GetData` `sub_82229158` [`sub_82228F48`], occlusion branch: `S_FALSE` (with `*out = 1`) while the ring has not
  written back the fence `[query+20]` (it kicks the segment if that fence is the current one) or while all four words
  still hold the sentinel; otherwise `S_OK` with `end.ZPass_A + end.ZPass_B - begin.ZPass_A - begin.ZPass_B`.
- Its **only caller** is UE3's blocking poll `sub_826E7C98` [`sub_826E8490`]: `while (GetData == S_FALSE) Sleep(0)`,
  up to 100000 times; on give-up it returns false and the callers leave their state unchanged. The poll is called 8
  times, all from `sub_82392F28` [`sub_82393828`] (light and primitive visibility): a successful read with **0
  samples** clears the visibility bit, removes the light from the shadowed/visible light counts (`[r21+1724]`,
  `[r21+1728]`) and its interactions. So there is no non-blocking reader and `S_FALSE` cannot cull; a **0** can.
- Our ring (`me_native_system.cpp`, `PM4_EVENT_WRITE_ZPD`) decided END vs BEGIN by looking for the sentinel in the
  structure at parse time; a "begin" is zeroed. In modes 0, 1 and 3 the END writes the result at parse time, which
  clears the sentinel.

**The race.** When the ring lags (heavy frames: turning from the dark corner to the bright window), the game can issue
the same query object again (UE3's query pool hands it to another primitive or light) before the ring has parsed the
previous END:
1. CPU: `Issue(BEGIN)` of issue N+1 stores the sentinel into the end structure (already holding issue N's sentinel).
2. Ring: parses END of issue N, writes 1000 samples: **the sentinel of issue N+1 is gone**.
3. Ring: parses BEGIN N+1 (zeroes the begin structure), then END N+1: no sentinel, so it is taken for a BEGIN and the
   **end structure is zeroed**. Counters: `query_unended_` +1 here and +1 at the next real BEGIN; `begun` +1, `ended`
   -1, which is exactly the `begun - ended = growth of begins without end` seen in every window.
4. Game: reads issue N+1 (fence passed, no sentinel): 0 - 0 = **0 samples**: the primitive or light is culled for one
   frame. A culled shadowed light takes its shadow-map pass with it; a lost light/primitive set is the reduced frame.

The fence check does not protect the read: `[query+20]` holds the fence of END N+1, which the ring passes while
zeroing the structure.

**Fixes** (both default on, image unchanged otherwise):
- `masseffect_native_query_pair_by_address` (bool, default true, init only, `me_native_system.cpp`, address-free:
  both editions). A ZPD whose address + 0x20 is the open query's begin address is its END, sentinel or not. Only that
  query's END can sit there (a BEGIN at that address would need another query structure overlapping this one); with predicated tiling
  D3D sets the register per tile and emits one ZPD, the begin and end of an issue use the same tile. Applies to all
  modes: in modes 0/1/3 the END now gets its answer; in mode 2 it is queued as a real query (if a late GPU result of
  issue N already erased the sentinel, the game may read issue N's count for N+1: a stale count, never a forced 0;
  mode 2 is not shipped). The 10 s `occlusion queries` line gets `; N ends whose sentinel was already erased (paired
  by address)` (counted also with the option off, then they are the ENDs that were zeroed).
- `masseffect_query_getdata_visible` (bool, default true, init only, `me_ring_wait.cpp`, English and Russian overlay).
  Query mode 0 only: the occlusion branch of GetData answers `S_OK`, 1000 samples at once, without reading guest
  memory and without waiting for (or kicking) the ring. Mode 0 writes 1000 at the END anyway, so the value is the
  same; the game no longer waits in the poll for the ring and can never read a zeroed structure. The ring's later
  write (1000) stays harmless: nothing reads it. Modes 1-3, other query types (8, 10), a GPU marked hung and the
  D3D-trace build call the original. Log every 10 s: `[ring_wait] occlusion GetData (10 s, mode 0): A answered 1000
  at once; the original would have answered R 'not ready' (ring behind), O with another count, Z of them 0
  (culled)`. Z > 0 on a run with the option on is direct proof of the cause (each one was a culled object).

**How to verify on the console** (same route, handheld, video, cold, `masseffect_diag_blur_source = 900`):

```toml
# 1. both fixes (defaults): target near 0 reduced frames per 1000
masseffect_diag_blur_source = 900
# 2. ring fix alone (the game waits for the ring as before)
masseffect_query_getdata_visible = false
# 3. reference, the old behaviour: expect ~17 per 1000 and ends "taken for begins" in every window with flashes
masseffect_query_getdata_visible = false
masseffect_native_query_pair_by_address = false
```

Run `../mass-effect-recomp/tools/me1_reduced_frames.py console.log` on each log. Expected: run 1 and 2 near 0 per
1000 (residual hits should be real scene changes: check them in the video); in run 1 the GetData line's `Z` counts
the reads that would have culled, `R` how often the ring was behind; in run 2/1 the `occlusion queries` line shows
`begins without end` no longer growing and `begun = ended`. Also compare `Swaps per 10 s` of run 1 and 2: run 1
removes the poll's waits for the ring on the render thread (possible fps gain).

## 4. Popping / flicker: other candidates, ranked

1. **Camera jumps on CPU hitches** (section 2), not a texture problem. Remove the hitches: the per-frame texture
   recheck volume (15-30 MB against a budget of 6 MB) shows rechecks piling up in the same frames.
2. **Lens flare and fake occlusion queries** (`masseffect_native_query_mode = 0`: every query reports 1000 samples).
   The flare does not flicker with the camera still, so the queries are not the trigger. They do keep it visible
   behind geometry. Mode 2 would make it fade like the original.
3. **Stale texture content after streaming reuses an address**: 64 `ME stable texture changed ... after interval 4`
   lines in this session, many of them during the clips. That content is old for up to 4 frames (2 after the third
   late change). It looks like a surface showing another texture briefly. The clips do not show it, but it is the
   most plausible renderer-side popping.
4. EDRAM ownership or resolved-texture invalidation: no evidence in the log or the clips.

## 5. Console A/B list (append to the shipped toml, one line or block per run, cold start, same route)

```toml
# 1. velocity facts (no image change): read the "velocity diag" lines
masseffect_diag_velocity = 1
# 2. Shepard holes (depth mismatch)
masseffect_diag_velocity = 9
# 3. background garbage under the velocity pass
masseffect_diag_velocity = 17
# 3b. exact k_16_16 emulation (docs 3.6); 2 = raw-word resolve hypothesis, picture comparison only
masseffect_native_velocity_16_16 = 1
# 4. dynamic velocity off / motion blur off (references)
masseffect_diag_velocity = 3
masseffect_diag_velocity = 5
# 5. regression check of yesterday's copy path
masseffect_native_conversion_copy_32 = false
# 6. comb sharpness from the upscaler
present_effect = "bilinear"
# 7. hitches -> camera jumps, flare pops, missing Shepard
masseffect_native_texture_spread_phase = true
masseffect_native_fingerprints_skip_unused_sample = true
# 8. flare occlusion like the original
masseffect_native_query_mode = 2
# 9. stale streamed textures
masseffect_native_texture_interval_max = 2
# 10. long frames and the motion blur (docs 3.7): 1 = 30 fps scale, 4 = cut above 60 ms, 5 = both
masseffect_native_motion_blur_frame_fix = 5
# 11. one-frame vanishing of meshes on normal frames (docs 3.8): journal of the blur's inputs, no image change
masseffect_diag_blur_source = 900
```

## 6. Overheat bar missing (combat HUD)

Date: 2026-10-07. Offline analysis, nothing run on the console. Sources: the user's screenshot
`mass-effect-recomp/run/me1/manual1/2026100807393000-*.jpg` (console clock 07:39:30) and the log of that session,
`run/me1/manual1/s303.log` (RU edition, `run/me1/manual_best.toml`, 960x544).

Symptom: in combat the "ПЕРЕГРЕВ" (overheat) label is drawn next to the weapon icon above the squad panel, but the
heat meter itself is not.

### 6.1 How the HUD draws it

The HUD is a Scaleform GFx movie. GFx tessellates shapes on the CPU and draws them with a handful of table-less
Direct3D shaders that the game builds at run time (they are not in any Unreal package, so the shader package only
has them through `shaders/runtime_containers`). The pixel shaders of that family, read from their microcode:

| PS (microcode hash) | words | what it computes | in the console package |
|---|---|---|---|
| `706F57115BB33790` | 9 | `oC0 = c0` (solid colour) | yes |
| `6C85C130EB1E02FE` | 15 | `oC0 = vertex colour * c2 + c3` (colour with a cxform) | yes |
| **`C5840B564B1D37B0`** | 18 | `oC0 = tex(s0, uv) * c2 + c3` (**bitmap or gradient fill with a cxform**) | **no** |

Solid shapes and text (squad health and shield bars, labels) use shaders that are present. A gradient or bitmap
fill goes through `C5840B564B1D37B0`: GFx renders gradients as small textures (the session has several 256x1
format-6 textures that change, `ME stable texture changed at ... 256x1 format 6`). The heat meter is a gradient bar.

### 6.2 Evidence in s303.log

- `shaders: unidentified pixel shader: 18 words, fingerprint C5840B564B1D37B0 (23 containers of that type and
  length)` (07:35:14), and `PS identity mismatch ... loaded_host_hash=C5840B564B1D37B0 loaded_words=18 selected_n=4507
  selected_host_hash=AD3FB87D67622D22 selected_words=15` (the address lookup offers the solid-colour shader and the
  identity guard correctly refuses it).
- The draw is then dropped (`me_native_system.cpp`, "draws without shaders"). The cumulative counter of
  `unresolved draw shader pairs ... (VS=0000000000000000,PS=C5840B564B1D37B0)` (VS found, PS missing) grows by about
  one draw per frame exactly while in combat: 211 at 07:39:01, 436 at 07:39:11, 628 at 07:39:31 (the screenshot
  interval: +191 in 10 s at ~25 fps), then +91, +187, +252, +243, +209 ... up to 5501 at 07:44:42. Outside combat
  it does not move.
- The microcode was dumped before (`run/shader-discovery-maps-01/raw/ps_c5840b564b1d37b0.ucode`, 2026-10-01) and
  wrapped into the v24 package (stage3-native.md, "v24 package"). The old lineage still has it
  (`run/vs-remap-v27/masseffect_shaders.v27-vs-remap.nfsp`), but the current package built by the installer
  pipeline from `shaders/runtime_containers` (`~/Downloads/masseffect-nx/masseffect_shaders.mesp`, 29,241 entries,
  the one on the console) does not: those wrapped containers were never copied into `shaders/runtime_containers`.
  A byte search of the console package finds neither the 18-word program nor its 15-word twin.
- A diff of the two packages: 951 v27 containers have microcode that the console package lacks. Almost all are
  material shaders of the other edition; the 8 table-less (runtime Direct3D/GFx) ones are the regression:

| file added to `shaders/runtime_containers` | from v27 container key | what |
|---|---|---|
| `ps_c5840b564b1d37b0.bin` | `023357a9d6c1eefb` | 18-word textured cxform PS (the heat bar) |
| `ps_28e67c688099f91c.bin` | `f3b8ba902a16d537` | the same program as a 15-word load; rewrapped from its exact microcode with the complete 16-interpolator header (`wrap_raw_shader.py`, template `ps_6c85c130eb1e02fe.bin`), because the v27 container stopped at the shader header |
| `ps_704e1d4ddb6f6a33.bin`, `ps_35921998fe465046.bin`, `ps_c7fe7f15eee279d3.bin`, `ps_f5e61df11e7dbc58.bin` | `a752b7c441cf91ce`, `bcfb5231feb986e9`, `d71b55068d5f1ab2`, `812ca4fd9f8bcd17` | other table-less PS of the same family |
| `vs_a7863db9d76ac293.bin`, `vs_cc4bf1218fb29544.bin` | `bd2175e9c8de9573`, `cbd7b99691287572` | table-less Direct3D VS |

  All 8 translate with `out/tools/xenos_hlsl` and compile with DXC + `spirv-val` (the flags of
  `compile_spirv_one.sh`). `ps_c5840b564b1d37b0` translates to
  `oC0 = tfetch2D(s0, r0.xy) * c2 + c3` plus the alpha-test clip.
- Other draws dropped for a missing **vertex** shader in the same session: `VS=BF7D911CF3F3D02D` (183 words, about
  one draw per frame in other combat stretches, 07:39:41-07:44:42), `CD057930742AFE84` (120 words, pause menu),
  `94C3653D88899DEA`, `ACF2098369FE014A`. Their microcode has never been dumped; the diagnostic below logs it.
- Not related: 8 `draw rejected before completion` at 07:41:58 (a resolved 256x256 crop with an unproven layout),
  no stencil, scissor or primitive-type rejects in the session (`native: ... 0 rejected` in every 10 s report).

### 6.3 Ranked hypotheses

1. **(confirmed by the log, fix ready) The GFx textured-cxform pixel shader is missing from the shader package**, so
   every bitmap/gradient fill of the HUD is dropped. Fix: a package with the 8 recovered containers.
2. **A missing VS (`BF7D911CF3F3D02D`)** drops another combat draw (could be another HUD part or an effect). Needs its
   microcode: run with `masseffect_diag_missing_shader_draws = true`, then wrap or repair it
   (`wrap_raw_shader.py` for a table-less VS, `repair_vertex_variant_declarations` for a patched package VS).
3. If the bar comes back but looks stale or transparent for a few frames after a change: the 256x1 gradient texture
   recheck (`ME stable texture changed ... 256x1 format 6 after interval 4`); A/B with
   `masseffect_native_texture_interval_max = 1`.
4. GFx stencil masks, scissor, alpha test: lowest. The renderer implements stencil reference/masks/ops and nothing
   in the log points at them; only worth a look if 1-3 do not explain what remains.

### 6.4 What was changed

- `shaders/runtime_containers/`: the 8 containers above; `installer/wasm/runtime_containers.json` regenerated (the
  288 existing entries are byte-identical, 8 added), so the next installer build includes them.
- A ready package: `out/shader-overheat/masseffect_shaders.mesp` + `.idx` (29,249 entries =
  console package + 8; `me_pack_shaders --replace ~/Downloads/masseffect-nx/masseffect_shaders.mesp <8 containers>
  <their SPIR-V>`; the packer reloaded it and found all 8). No app rebuild is needed for the fix.
- `app/src/native/me_native_system.cpp`: cvar `masseffect_diag_missing_shader_draws` (bool, default false, log only).
  For draws dropped for a missing VS/PS it logs the state once per distinct state (at most 64 lines):
  `[native] missing shader draw: VS <hash> found|MISSING (<n> words) PS <hash> ... | prim P count N mode M | color mask
  .. blend0 .. colorcontrol .. alpha ref .. | depthcontrol .. stencil .. | color info .. surface .. | tex0 format F
  WxH base B words ... | draw D`, and the full microcode of each missing program once (at most 16):
  `[native] missing shader microcode VS|PS <hash> <n> words [<first>]: <16 words per line>`. Needs an app rebuild;
  syntax-checked on the Mac host flags, not built.

### 6.5 Console test (one run each, cold, combat on Eden Prime)

1. Upload `out/shader-overheat/masseffect_shaders.mesp` and `.idx` to `/switch/masseffect-nx/` (both files; the
   index is bound to the package). Same NRO and toml. Expect: the heat meter is drawn; the log line
   `shaders: library with 29249 shaders`; `unresolved draw shader pairs` no longer lists `PS=C5840B564B1D37B0`; no
   `unidentified pixel shader: 18 words, fingerprint C5840B564B1D37B0`.
2. With a rebuilt NRO, add `masseffect_diag_missing_shader_draws = true`. Look for `missing shader draw:` lines
   (with the old package, the `PS C5840B564B1D37B0 MISSING` line should show `blend0 07060706`-style alpha blending,
   the 960-pitch HUD target and a small texture such as `tex0 format 6 256x1`) and copy every
   `missing shader microcode` block; those words go through `wrap_raw_shader.py` into `shaders/runtime_containers`.

### 6.6 Feros: 35 material pixel shaders missing from the package (2026-10-07)

Source: `mass-effect-recomp/run/me1/ab/g1_a/console.log` (Zhu's Hope, `masseffect_diag_missing_shader_draws = true`,
console package = `out/shader-overheat`). Offline analysis; nothing run on the console yet.

**What was missing.** 64 `missing shader draw` lines, all with the VS found and the PS missing; 35 distinct PS
(`unidentified pixel shader: N words, fingerprint ...` for each), no missing VS. They are Unreal material shaders
(30-249 words, hundreds of package containers of each length), not D3D/GFx runtime shaders. The largest cumulative
`unresolved draw shader pairs` at the end of the run: `F9EF2E6AF4FB009F` 418,969 draws, `D087FC3E486BAA24` 185,837,
`01F330CCFC8C9926` 159,642, `989854653DAFD42E` 94,919, `44F23DF8B1131CDF` 53,566, `9A69BD6AFD7FE0D3`,
`5098B939F658A8F1`, `7771CE33CF5C721F`, `0493886EB80CA50F`, `90FE5C5D17649BA8`, `2FDFDC5CAD252DCB`, `8D829693448DFFA9`
(the large-count ones from the request, e.g. `8FF2EDF8CC2A2D42` count 3255, `35D1FFFB5CF843C8`, `9D8098D1BC791533`
count 4890, are in the same set). Most of the Zhu's Hope world geometry and props were simply not drawn.

**Microcode dumps.** The log has 185 `missing shader microcode` lines = 16 programs (the old cap). All 16 are
complete (every word index present) and their XXH3 over the host-order words equals the logged hash. Copies:
`out/shader-missing2/ucode-from-log/ps_<hash>.ucode` (little-endian host words). No other log under
`run/me1/**` has such lines.

**Why they were missing: the package was built from the truncated Feros maps.** The console package
(`~/Downloads/masseffect-nx/masseffect_shaders.mesp`, 2026-10-06 00:08; `out/shader-overheat` = it + 8) was made
from the game root whose `BIOA_WAR*` packages were truncated by the 2-disc merge. The user replaced 290 packages on
2026-10-07 (238 in `Layer0/Maps`, mostly `BIOA_WAR*`, and 52 `Layer1/Maps/BIOA_UNC*`). Scanning exactly those 290
with `out/tools/ue3_shader_scan` gives 6,672 distinct containers; 923 have microcode the console package does not
have (921 PS, 2 VS), and **all 35 missing PS are among them with their original containers** (full header and
constant table). So these are neither runtime-patched variants nor a fingerprint/fetch-masking problem:

- Nearest same-length package PS for each of the 16 dumped programs differs in 43-211 words (one, the 30-word
  `430373595B859F5C`, in 3 words: another material's literal), so no selection change could match them.
- The two `PS identity mismatch` families on these hashes (`loaded_host_hash=9809BF13A761D9D0` x36 offered
  `18638D1826BA26D3`, 156 of 189 words differ; `0D959E5EBF2B16B0` x29 offered a 201-word `99EB74D76A041C1D`) are
  `identify-address` hits on a recycled address; the guard refused them correctly. No selection-logic change.
- Because the original containers exist, nothing was wrapped with `wrap_raw_shader.py` (an empty-CTAB wrap would be
  a worse copy) and nothing was added to `shaders/runtime_containers` or `installer/wasm/runtime_containers.json`
  (that folder is for shaders that exist in no `*.xxx`; these are disc data, and the installer's own package scan
  picks them up when it runs on the repaired game root).

**New package: `out/shader-missing2/masseffect_shaders.mesp` + `.idx`** (30,160 shaders = 29,249 of
`out/shader-overheat` + 911 new; 925,395,968 bytes, index 48,557,332 bytes). Steps:
1. `ue3_shader_scan <dir> <the 290 packages newer than the console package>` (list:
   `out/shader-missing2/scanned_packages.txt`).
2. Keep containers whose microcode XXH3 is not in the base package (base extracted with
   `extract_shader_package`): 923.
3. `translate_all.py` (out/tools/xenos_hlsl): 913 translated, 10 failed (their header does not parse, scanner false
   positives, none of them a missing hash); `compile_spirv_all.sh` (DXC + `spirv-val`, the flags of
   `compile_spirv_one.sh`): 911 SPIR-V, 2 failed (`vs_664739fc5097d095` = loaded hash `093FB7223F611ADC` and
   `vs_e17c82a1e9171000` = `2DD35B45B15E59FA`: duplicate `BLENDWEIGHT0` input, the known translator failure class;
   neither shows up in this log).
4. `me_pack_shaders --replace out/shader-overheat/masseffect_shaders.mesp <911 containers> <spirv> out/shader-missing2/...`
   (0 old entries replaced, all packed ones found after reload). `validate_shader_package.sh`: 30,160 shaders,
   `invalid=0`. All 35 missing hashes are present by microcode in the extracted new package.
The 911 containers are kept in `out/shader-missing2/containers/` (game data, local only).

**Diagnostic caps are now cvars** (`app/src/native/me_native_system.cpp`, needs an NRO rebuild; syntax-checked with
the mac-host flags):
- `masseffect_diag_missing_shader_programs_max` (int, default 0 = no limit): each distinct missing VS/PS gets its
  microcode dumped once per run; the old fixed cap was 16.
- `masseffect_diag_missing_shader_states_max` (int, default 64, 0 = no limit): distinct `missing shader draw` lines.

**What still lacks microcode** (19 of the 35; irrelevant now, since their original containers are in the new
package): `0D959E5EBF2B16B0`, `258657FF4DCBB879`, `2F6827B35C7DD88C`, `2FDFDC5CAD252DCB`, `31BDB29FF386D912`,
`3DBF38935D902FA8`, `48A75EC64D4371E9`, `5B603AFF77E1AD44`, `65E1E6F23E3B7CE7`, `6B6AF4766157A083`,
`7771CE33CF5C721F`, `8D829693448DFFA9`, `9809BF13A761D9D0`, `9D8098D1BC791533`, `BBDCA7FF7742F3F1`,
`CB0697C40E5DDCC3`, `D087FC3E486BAA24`, `D121C30109FC3A48`, `E7CCEB7B93AF3C55`.

**Console test.** Upload both `out/shader-missing2/masseffect_shaders.mesp` and `.idx` to `/switch/masseffect-nx/`
(same NRO, same toml, with `masseffect_diag_missing_shader_draws = true` still on). Expect `shaders: library with
30160 shaders`, no `unidentified pixel shader` for the 35 hashes, and the Zhu's Hope geometry drawn. Any remaining
`missing shader draw` lines are a new set (another map, or packages still truncated) and, with a rebuilt NRO, come
with complete microcode. Note: the long-term fix is to rebuild the whole package with the installer from the
repaired game root (other maps may also have changed shaders); a truncation checker for all packages is still to do.

### 6.7 Vertex shaders dropped although the package has them (2026-10-08)

Not a package problem: every `missing shader draw: VS ... MISSING` line with dumped microcode under `run/me1/**`
(4 programs) is a library program that Direct3D patched for its vertex declaration. The exact lookup
(`ShadersNative::Identify`) keeps the fetch destination swizzle, the patched words carry another one, and the draw
was dropped. Programs, checked offline with the real identity code against `out/shader-dlc-vs` (281 VS):

| loaded VS | words | where (log) | what Direct3D changed | library entry |
|---|---|---|---|---|
| `CD057930742AFE84` | 120 | Eden Prime (`ab/g3d_eden`, 2524 dropped draws; the pause-menu VS of 6.2) | position `688 -> A88`, and the `r3.zw`/`r3.xy` fetches of instructions 11-12 swapped | `894453492fbd1d03` |
| `30458CCA865ABD51` | 192 | Normandy, Wards (`ab/g3d_normandy`, `ab/g3d_wards`) | position `688 -> A88` | `bdec8003b46754c7` |
| `83D232C56BD49D57` | 129 | BDtS X57 (`x57`) | position `688 -> A88` | `d85a6766b65e299a` |
| `0D0AB386D5018592` | 177 | BDtS X57 (`x57`) | position `688 -> A88`, texcoord5-7 `E88 -> E0A` (D3DCOLOR) | `47960bda7104b5b9` |

`BF7D911CF3F3D02D`, `94C3653D88899DEA` and `ACF2098369FE014A` (6.2) have no microcode dump yet; they are probably the
same class.

Fix (app rebuild needed; syntax-checked with the mac-host flags, CPU tests pass): a second-stage lookup in
`ShadersNative::Identify` (cvar `masseffect_native_vs_identify_patched`, default true) and the same-register fetch
permutation in the VS identity (cvar `masseffect_native_vs_fetch_permutation`, default true). Exactness: every
ALU/CF word identical, declared fetch words identical under the loader masks, every swizzle change representable by
the existing input remap (the `C6` remap codes apply it), reordering only inside one exec clause between
back-to-back fetches into the same register with disjoint components. Details and log lines: `docs/dlc.md`, section
4, "Shaders". Counter line every 10 s: `[native] VS patched-fetch lookup: N draws rescued ...`.

Console check (Eden Prime pause menu, Normandy, any run with `masseffect_diag_missing_shader_draws = true`): the
four hashes above no longer appear in `missing shader draw` or `unresolved draw shader pairs`; one
`identified by the patched-fetch lookup` line each (`CD057930742AFE84` with `same-register fetches reordered`);
the rescued counter grows; no `FINAL VS unproven` and no new `rejection causes` 21 (`ME ambiguous fetch`). Look at
what those draws show (pause menu backdrop on Eden Prime, the X57 asteroid geometry) for wrong vertex colors or
UVs: that would point at the remap, not at identification.

## 7. Missing letters in UI and dialogue text (2026-10-08, Citadel Wards, build ru_glob3)

**What was seen.** `run/me1/manual8/2026100810261100-...jpg` (config `glob_e.toml`): "Экспресс  итадели" twice in the
same line, only the letter "Ц" blank, its advance kept. `2026100810092000-...jpg` (earlier run with the faulty IO block
cache on): most common letters blank (е, с, к, п, и, о, т, н, х) while rarer ones (Э, Ц, ж, й, ы, б, ю) are drawn. The
layout is right in both, so the strings and glyph metrics are right and only glyph images are missing.

**Where the glyphs live.** The UI library rasterizes each glyph on the CPU the first time a string needs it into a
glyph-cache page and draws the string at once. In the logs these pages are k_8 (single channel, `VkFormat 9`
R8_UNORM) 256x256 textures of 64 KB at several addresses (`160D3000`, `15BD4000`, `14A8F000`, `14A7F000`, `1F304000`
...): they are created empty ("C3 reuse by content: new texture 160D3000 ... same content as 1F33D000") and change
later ("ME stable texture changed at 160D3000 256x256 format 2 after interval 4 ... late changes 1, sample full",
followed by "ME texture update ordering: split ... before replacing 160D3000"). Only the few tiles of the new glyph
change.

**How a glyph can stay blank in the native renderer** (`app/src/native/masseffect/masseffect_native_draws.cpp`,
PrepareTexture):

1. *Recheck interval.* A stable texture is rechecked every `interval` frames (doubling up to
   `masseffect_native_texture_interval_max`, 4 in the shipped toml; 32 is the code default), so a new glyph shows up
   to `interval` frames late. The adaptive cap brings a page that changed late down to 4 then 2 frames, but only after
   it has changed.
2. *Sampled recheck.* From interval 8 (`masseffect_native_fingerprints_sample_min_interval`) a stable texture may be
   rechecked by a sample of its bytes (first and last 4 KB block plus 1 in 8). A glyph written into an unsampled block
   is invisible until the next full recheck, 1 in `masseffect_native_fingerprints_sampling` = 8 rechecks later: at
   interval 32 about 9 s. This path is unreachable with `glob_e.toml` (interval max 4 < 8): every late change in the
   2026-10-08 logs says "sample full". It does apply with the code defaults (interval max 32).
3. *Lost upload.* When a recheck finds new bytes, the raw hash is updated, the data is laid out and the texture is
   queued in `textures_to_upload_` for the current draw. If that draw stops before its upload stage (a resolved
   texture fetch failing in another sampler, "draw larger than the upload buffer", a failed send), the list is cleared
   by the next draw and the texture keeps `needs_upload = true` with its hash already current: every later recheck
   finds "same bytes" and the image keeps the old content until the guest changes those bytes again. For a glyph page
   that means a glyph missing for as long as the page is not rewritten. Of the three, only this one leaves a glyph
   blank for good with `glob_e.toml`.

**Changes** (all on by default):

| Switch | What it does | Exact? |
|---|---|---|
| `masseffect_native_texture_requeue_uploads` | PrepareTexture queues again a texture whose upload was dropped (its data is still held); log `ME lost texture upload requeued` | yes |
| `masseffect_native_texture_sample_after_change` | a texture with a late change is never rechecked by its sample alone | yes (removes a non-exact shortcut) |
| `masseffect_native_texture_atlas_cap` = 1 | a single-channel texture up to 512x512 with a late change (glyph-cache page) is rechecked every frame it is used; such pages are also kept out of the coherency skip (`masseffect_native_texture_coherency` = 1) because glyph writes may come without a coherency event; log `ME CPU-updated atlas ...` | never staler; not frame-identical (fresher) |

The cost of the cap is one XXH3 over 64 KB (about 10-20 us on the A57) per glyph page and frame, for the handful of
pages the logs show. 0 restores the adaptive 4 / 2.

**What to check on the console.** Same build route as `glob_e.toml`, Citadel Wards, open the Citadel express terminal
and several dialogues with new letters (first appearance of a rare letter: Ц, Щ, Ъ, Ё, Ф). Expect every letter drawn
from the first frame or the next one. In the log: `ME CPU-updated atlas` lines for the k_8 256x256 pages; any
`ME lost texture upload requeued` line proves mechanism 3 happened (and that it is now repaired). If a letter is still
missing with no requeue line, set `masseffect_native_texture_interval_max = 1` for one run: if that fixes it, the page
is not being classified as an atlas (look at its format/size in the "stable texture changed" lines); if not, the glyph
is not in guest memory at all (CPU side, e.g. a hot native or the IO path, as in the 10:09 run with the faulty block
cache).


### 7.1 Root cause found: stale resolved textures at reused addresses (2026-10-08, second pass)

**New facts.** The morning build (`run/me1/ru_loc2.elf`, `manual_best.toml`, before the switches above) shows the same
missing letters in the Citadel express terminal, so the defect is old. `run/me1/manual8/2026100810554600-*.jpg`
(log `run/me1/manual9/masseffect_326.log`, with the switches above on) misses "р у ю б ы :" in both the cyan dialogue
line and the yellow wheel option. Every glyph page that log names (`160D3000`, `15BD4000`, `14A9F000`, `14A8F000`) took
the guest-memory path and was rechecked every frame; no "lost texture upload requeued" line. The user also saw letters
missing from the short autosave label "Сохранение".

**How the UI fills its glyph pages (guest code, RU addresses).** No GPU resolve ever targets a glyph page (the log's
resolves are 960x544, 864x864, 1280x720, 256x138 and 256x256 k_8_8_8_8 targets, none at a page address). Scaleform's
texture object (vtable `0x820C8C90`) has no partial update: `InitTexture(GImage*)` = `sub_82251280` copies the whole
image (CRT memcpy `sub_82973160`), consumes the GImage and enqueues `InitResource`; on the render thread
`InitDynamicRHI` = `sub_82251628` does `CreateTexture` (`sub_82224318`, XPhysicalAlloc) and
`D3DXLoadSurfaceFromMemory` (`sub_82AE9B80`: format A8 `0x04900102`, linear source `0x04900002`), which tiles the texels
on the CPU with `XGTileSurface` (`sub_82811698`, also through the CRT memcpy). Old textures are released one frame later
by the `ScaleformReleaseDeferredTextures` render command (`sub_82246D78`, two release lists swapped every frame). So
every page change is a new D3D texture whose memory is a freshly allocated physical block, and the allocator hands out
blocks that were just freed: the same address comes back (hence "stable texture changed at 160D3000"), and so do the
addresses of render-target textures the game resolved into and freed.

**Where the glyphs were lost.** The native renderer never writes resolves into guest memory: a resolve fills a GPU image
kept in `resolved_[base]` (masseffect_native_targets.cpp), and `PrepareTexture` asks `ResolvedTexture(base)` first for
every 2D fetch. Those entries were never dropped when the game freed the target and reused the memory: they lived until
another resolve of another shape hit the same base. A glyph page created at the base of an old resolve destination
(a UI render-to-texture, a save thumbnail, a 256x256 face) was therefore sampled from that old render target (wrong
format and content: blank letters) for the rest of the session, while its real texels sat in guest memory unused. On the
360 the CPU write replaces the resolved bytes and the fetch reads them. It fits every observation: whole pages fail
(all letters that went to one page; "Ц" alone when a new page started with it), the set changes from run to run (it
depends on which freed block each page gets), it is worse when allocations churn (the autosave label: a thumbnail target
freed right before the label's glyphs were rasterized), and none of the old diagnostics could see it (the resolved path
logs nothing and never reaches the atlas code of section 7).

**Fix: `masseffect_native_resolved_cpu_overwrite` (default true).** Our resolves leave guest memory untouched, so the
bytes at a resolve destination change only when the CPU (or a DMA) writes them. After the last resolve into an entry a
fingerprint of its destination bytes is taken at a present (the footprint is pitch x height aligned to 32 x 32 at the
guest format's texel size; full XXH3 up to 256 KB, above that 256 bytes of every 4 KB plus the first and last 4 KB;
destinations up to 512 KB at the first present after the resolve, larger ones once a whole frame passed without a
resolve). `ResolvedTexture` rechecks it every 1, 2, 4, 8 presents while unchanged, and at once (once per present) for a
fetch whose format or size differs from the resolve's. Changed bytes mean the CPU rewrote the memory after the resolve:
the entry is marked, `ResolvedTexture` returns nullptr and the fetch reads guest memory like any other texture. The next
resolve into that base clears the mark. Read-backs of resolved texels into guest memory re-take the fingerprint (they
are our writes, not the CPU's). Exact in what it changes; the only gap is a CPU rewrite within the frame of the last
resolve of a large (> 512 KB) target, which keeps the old behavior. Cost: one XXH3 per small resolve destination per
frame it is resolved in, and the scheduled rechecks; reported every 600 presents. `false` restores the old behavior.
Files: `app/src/native/masseffect/masseffect_native_targets.cpp` (Resolved fields, `ResolvedFootprint`,
`GuestFingerprint`, `NoteResolvedContent`, `CaptureResolvedFingerprints`, `CpuOverwroteResolved`, hooks in
`GetResolved`, `ResolvedWritten`, the present and the read-back loop), `masseffect_native_draws.h` (`ResolvedTexture`
takes the fetch constant), `masseffect_native_draws.cpp` (passes it).

**Diagnostic: `masseffect_native_glyph_trace` (default false, `masseffect_native_glyph_trace_lines` = 2000).** For
glyph-page textures (single-channel 2D, up to 512x512) it logs every source of their texels: `ME glyph trace: new page`
(first content, nonzero texel count), `ME glyph trace: CPU bytes changed` (which 32x32 tiles changed, as `[x,y wxh]`
rectangles, and the nonzero texels before and after), `ME glyph trace: upload ... recorded` / `upload requeued`, and
`ME glyph trace: k_8 fetch ... served by a RESOLVED image` (a k_8 fetch answered by a resolve's GPU image: the defect
itself when the fix is off). Independently of it, the fix logs `ME resolved texture rewritten by the CPU at <addr>` (first
64; `masseffect_native_resolved_cpu_overwrite_log`) and a cost line every 600 presents.

**What to check on the console.** Cold start, same route: Citadel Wards express terminal, a dialogue, then let an
autosave run and read the "Сохранение" label.
1. Run A (fix on, trace on: `masseffect_native_glyph_trace = true`). Expect every letter drawn. In the log: one or more
   `ME resolved texture rewritten by the CPU at X ... fetch 2 256x256 (another shape)` lines (X = a glyph page address
   that also appears in `ME glyph trace: new page X`), and `ME resolved CPU-overwrite check` lines with a small MB figure
   (a few MB per 600 presents is expected). No `served by a RESOLVED image` line for a page that is also logged as
   rewritten after that point.
2. Run B, only if A still misses letters: `masseffect_native_resolved_cpu_overwrite = false`, trace on. A
   `served by a RESOLVED image` line for the page with the missing letters confirms this cause; if there is none and the
   page's `CPU bytes changed` lines show the tiles of the missing glyph never changing, the glyph is not in guest memory
   (CPU side, see the previous paragraph).
3. fps must not move (compare the cost line; with the trace off the fix only hashes small resolve destinations).
