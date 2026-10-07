# The scene-color and post-processing chain: what it is, what to capture, how to replace it

Date: 2026-10-07. Read-only research. Nothing was built and nothing was run on the console for this page.

Sources:

- Console logs of the current build (Russian edition, Normandy route: Anderson cutscene, then the cockpit):
  `mass-effect-recomp/run/me1/long_final/console.log` (960x544, shipped toml without `conversion_copy_32`),
  `long_copy32` (960x544, the same with `conversion_copy_32`), `long_1280/console.log` (1280x720, `full1280.toml`),
  `long_bias` (960x544, an older build: before the packed-extent proof and the 7e3-direct resolve).
- The renderer source: `app/src/native/masseffect/masseffect_native_targets.cpp` (`CopyInternal`, `ResolverWithBias`,
  `ResolverDirect7e3`, `SynchronizeEDRAM4`, `PublishRangeClearEDRAM4`, GPU marks), `masseffect_native_draws.cpp`.
- The shader library on the Mac (the user's own `masseffect_shaders.mesp`, same numbering as the log). The shaders named
  below were read back by index and translated again with `out/tools/xenos_hlsl` in a private scratch folder, only to read
  their constant and texture names. Nothing from the game was copied into the repository.

"Steady" numbers below are the cockpit windows at the end of each run (960: 282-312 s after the log starts; 1280:
292-322 s), per guest Swap. The cutscene (200-260 s) is heavier at 1280 (GPU 77-96 ms, `edram_import9` up to 10.5 ms).

## 0. Summary

1. Most of the remaining EDRAM cost is one UE3-on-Xbox-360 pattern, used both in the lighting and in the post chain:
   **resolve the scene color (7e3 EDRAM at base 0x2D0) to a texture, reuse the same EDRAM for something else in another
   format, then draw the texture back ("restore")**. The 7e3/UNORM10/RGBA8/16_16 round trips come from that pattern, not
   from the post effects' own math.
2. In the steady cockpit, per frame: 960x544 `edram_alias` 7.4-7.7 ms before `conversion_copy_32` (5.7-6.3 ms with it,
   now in `shipped.toml`) + import/export 0.9 ms; 1280x720 `edram_alias` 10.1-10.8 + `edram_import9` 5.9 + import/export
   0.8 = **~17 ms**. The 5.9 ms of `edram_import9` at 1280 is **not** the post chain: it is the shadow-map depth region
   0x5A0 pulling stencil from tiles the 2x scene-color view (C400) last owned (section 1.6).
3. The shaders' own constant names identify the post passes: DOF/bloom gather (PS24056), uber blend with the color
   controls (PS15729), motion blur (PS15184), velocity buffer (PS21181/PS24436), distortion apply (PS12145), material
   post-process effects (PS16272, PS7992), copy shaders (PS3474 with gamma and bias factor, PS21415 plain copy).
4. Two exact facts from the counters: the number of 7e3-direct (UNORM10) resolves per frame equals the number of
   whole-screen f2->f3 conversions per frame at both resolutions (1.5 and 1.5 at 960, 2 and 2 at 1280), and f2 becomes
   the owner without any transfer being logged. So something writes the whole screen through the UNORM10 view after each
   raw resolve. That is either a raw restore draw or the distortion apply. Section 2 has the capture that decides which.
5. Recommended order: one capture session (no code) → **restore elision**, exact and in the renderer only (about 1-2 ms at
   960 and 2-4 ms at 1280) → host-side post targets with zero-copy resolves (about 2 more ms at 960 and 3 more at 1280)
   → a native fused post chain (about 1-2 more ms at 960 and 3-4 more at 1280). Hooking the game's UE3 post functions is
   the last resort.

## 1. What the logs and the code show

### 1.1 EDRAM surfaces

| base / class (log name) | guest format | host format | size (960 / 1280) | what it is |
|---|---|---|---|---|
| C2D0/f3 | k_2_10_10_10_FLOAT (7e3), also AS_16_16_16_16 (fmt 12) for additive blending | RGBA16F | 960x960 / 1280x1280 views, 960x544 / 1280x720 used | HDR scene color |
| C2D0/f2 | k_2_10_10_10 (fmt 2) and AS_10_10_10_10 (fmt 10) | RGBA16F (UNORM10 values) | same | raw 32-bit "bit copy" view of the scene color |
| C2D0/f0 | k_8_8_8_8 (fmt 0) | RGBA8 | same | light attenuation buffer (shadow projection, see 1.2); LDR targets |
| C2D0/f4 | k_16_16 (fmt 4) | R16G16_UNORM | same | velocity buffer for motion blur |
| C5A0/f7 | k_16_16_16_16_FLOAT, pitch 320 / 400 | RGBA16F | 240x136 / 320x180 | quarter-res DOF/bloom buffer. It **shares base 0x5A0 with the shadow-map depth D5A0** (880 pitch) |
| C5A0/f7 mx1my1 | same, 2x layout | | 160x720 / 200x720 | 2x view of the quarter buffer: 8 small 2x->1x imports per frame (13 tiles) |
| C400/f3 mx0my1 (1280 only) | 7e3, 2x vertical | | 1280x1280 | 2x scene-color view at 1280. It overlaps 0x5A0 |

### 1.2 Resolved textures per frame (steady cockpit)

| texture (960 / 1280) | Xenos texture format | per frame 960 / 1280 | draws per copy | role (from the readers' constant names) |
|---|---|---|---|---|
| 15204000 / 14B48000 | alternates fmt 7 (k_2_10_10_10, raw) and fmt 32 (16_16_16_16_FLOAT, exp bias -3) | 5.5 / 7.0 | 28-45 | `SceneColorTexture`: 4 / 5 biased resolves (`resolve bias=`) + 1.5 / 2 raw resolves (`resolve from 7e3=`) |
| 14D2D000 / 144D7000 | fmt 6 RGBA8 | 3.0 / 3.0 | 1.0 | probably `LightAttenuationTexture` (one shadow projection draw per copy) |
| 1F376000 / 15278000 | fmt 23 k_24_8_FLOAT | 1.0 / 2.0 | 0 | `SceneDepthTexture` |
| 1F574000 (256x138, crop 242x138) / 1F540000 (352x182) | fmt 32 | 3.0 / 3.0 | 1.7 | quarter DOF/bloom (`BlurredImage`): gather + 2 blur passes |
| 14F2B000 / 1486F000 (864x864) | fmt 22 k_24_8 | 2.0 / 2.0 | 0 | shadow depth |
| 1F5F8000 1280x720 | fmt 6 | 1.0 / 1.0 | 1.0 | front buffer (kept 1280x720, see do-not-break.md rule 1) |
| 1413F000 / k_16_16 | fmt 25 | cutscene windows only | 2-3 | probably the velocity buffer (`VelocityBuffer`) |

### 1.3 Shader roles

The index is the library number used in the log. The role comes from the shader's own constant and texture names (UE3
keeps them in the constant table). Times are `GPU time by pass` per frame in the steady windows: an interval runs from the
labelled draw to the next category change, so it is an upper bound for that pass. At 960 the full-res post passes are
category 3 ("640", the same as the scene). At 1280 they are category 8 (`scene_no_z`).

| VS / PS | inputs (names) | likely UE3 role | target | ms 960 / 1280 |
|---|---|---|---|---|
| VS24994 / **PS24056** | SceneColorTexture, SceneDepthTexture, BloomScale, MinMaxBlurClamp, PackedParameters; 32 taps | DOF + bloom gather / downsample to quarter res | C5A0/f7, scissor (1,1)+240x136 | cat 4 (the whole quarter-res run incl. blurs): 2.9-3.0 / 4.9 |
| (unknown) | | 2 blur passes of the quarter buffer | C5A0/f7 | inside cat 4 |
| VS2293 / **PS15729** | BlurredImage, SceneColorTexture, SceneDepthTexture, SceneShadowsAndDesaturation, SceneMidTones, SceneInverseHighLights, GammaColorScaleAndInverse | uber post blend: DOF/bloom combine + color controls (the "tone mapping" of this UE3 generation) | 2D0 full-res | 0.6 / 1.1 |
| VS1788 / **PS15184** | SceneColorTexture, SceneDepthTexture, VelocityBuffer, PrevViewProjMatrix, ScreenToWorld | motion blur (camera moves and cutscenes only) | 2D0 full-res | ~1.0 / ~1.7 when present |
| VS28697 / **PS16272**, **PS7992** | SceneColorTexture (+ Texture2D_0), UniformVector_n, SCENE_COLOR_BIAS_FACTOR | post-process **material** effects (per level; PS7992 is the one the code calls `kMeFingerprintTonemap`) | 2D0 full-res | 0.4 + 0.37 / 0.7 + 0.65 |
| VS9460 / **PS3474** | Texture, Gamma, SCENE_COLOR_BIAS_FACTOR | SimpleElement copy: texture x vertex color, optional gamma, x bias factor. Restore of the scene color into 7e3 (fmt 3), and probably final copies | 2D0 fmt 3 | 0.9-1.8 / 0.6 (cat 8) + share of cat 2 |
| VS6906 / **PS21415** | Texture only | plain copy | full-res, cat 8 at 1280 | 0.4-0.55 / 0.8-1.0 |
| VS6905 / **PS12145** | AccumulatedDistortionTexture, SceneColorTexture | distortion apply (raw bits through the UNORM10 view) | 2D0 fmt 2 (f2), rect list | rare in these windows |
| VS3014, VS7284 / **PS21181, PS24436** | IndividualVelocityScale | velocity buffer draws | 2D0 fmt 4 (f4) | inside cat 3 |
| VS12867 / PS26880, VS7960 / PS28926, PS24014 | | first draws of the f4 pass (skinned, `oC0.x` + clip) | 2D0 f4 | f0->f4 import 1/frame |
| VS22095 / **PS21267** | ShadowDepthTexture, SceneDepthTexture, ScreenToShadowMatrix, RandomAngleTexture | shadow projection into the light attenuation buffer (blend src x DstColor) | 2D0 fmt 0 (f0) | |
| VS22095 / **PS27703** | the same + ShadowModulateColor, LightPosition | modulated shadow | 2D0 fmt 12 (f3) | |
| VS2013 etc. / PS14702, PS14489, PS5939, PS16235, PS731, PS1701, PS1896, PS2552, PS29025, PS18583 | LightAttenuationTexture, LightColorAndFalloffExponent | per-light additive passes (ONE+ONE) | 2D0 fmt 12 (f3) | the f3 consumers that pay f2->f3 |
| VS9828 / PS10237 (rect list, depth ALWAYS) | | shadow-map and quarter-buffer rect clears at 0x5A0 | D5A0 / C5A0 | source of the 0x5A0 traffic |

### 1.4 The frame, in order (as far as the logs show)

"Proven" means the logs show it directly. "Inferred" means it fits every counter but the order has not been seen; section
2 captures it.

| # | step | source -> target | EDRAM operation it causes | measured cost per frame (960 / 1280) | status |
|---|---|---|---|---|---|
| 1 | base pass, sky, emissive | 2D0 f3 | none in steady state (earlier builds: f4->f3, f0->f3, removed by packed-extent proofs) | (scene) | proven |
| 2 | per shadowed light: **raw resolve** of the scene color through the UNORM10 view (RT fmt 10) to 15204000 fmt 7 | f3 -> texture | `ResolverDirect7e3` (label VS40), 1.5 / 2 per frame | 0.6-0.9 / 1.2-1.4 | proven (counts, label) |
| 3 | light attenuation: clear the RGBA8 view, shadow projection PS21267 (multiply), resolve to 14D2D000 | 2D0 f0 | clear redirect; rare f3->f0 | small | inferred (3 RGBA8 resolves per frame with 1 draw each) |
| 4 | **restore** of the scene color into 2D0 **through f2** (raw bits), or the distortion apply PS12145 | texture -> 2D0 f2 | f2 becomes the owner. No transfer is logged, so the write is a proven whole-screen overwrite or a clear | the draw itself | **unknown which draw: capture B** |
| 5 | additive lights (fmt 12) | 2D0 f3 | **f2 -> f3 whole screen** (label VS19, 408 / 720 tiles), 1.5 / 2 per frame, the same count as step 2 | 0.8-1.0 / 1.6-1.8 | proven (pairs: first draw VS2013/PS14702, PS14489 ...) |
| 6 | per post effect that reads the scene color: **biased resolve** (x 2^-3) to 15204000 fmt 32 | f3 -> FP16 texture | `ResolverWithBias`, fragment (label VS41), 4 / 5 per frame | 1.3-1.4 / 2.3-2.5 | proven (counts, label) |
| 7 | velocity buffer: draws into 2D0 as k_16_16 | 2D0 f4 | **f0 -> f4 whole screen**, 1 per frame at 960 (compute VS11 1.3-1.5 ms; image copy VS43 ~0.25 ms with `conversion_copy_32`). Not seen at 1280 | 0.25 / 0 | proven at 960 |
| 8 | DOF/bloom gather PS24056 into the quarter buffer, 2 blurs, 3 resolves to 1F574000 | 2D0 tex -> C5A0/f7 | 2x->1x imports of C5A0 (8 ops, 13 tiles), depth<->color between D5A0 and C5A0 (10 exports, 6 imports per frame); at 1280 also C400/f3 -> C5A0/f7 (14 per frame) | cat 4 2.9 / 4.9 + export 0.3 / 0.5 + import 0.6 / 0.3 + part of the small alias ops | proven (pairs) |
| 9 | uber blend PS15729 | textures -> 2D0 | none if a proven overwrite | 0.6 / 1.1 | proven (pass list) |
| 10 | motion blur PS15184 (when the camera moves) | textures -> 2D0 | biased resolve before it | ~1.0 / ~1.7 | proven in cutscene windows |
| 11 | material effects PS16272, PS7992 | resolve -> 2D0 | a biased resolve before each | 0.4 + 0.4 / 0.7 + 0.65 | proven (pass list) |
| 12 | final copy (PS21415 / PS3474), UI, front buffer resolve 1F5F8000 | 2D0 -> front | copy (cat 6) | copies 1.4 / 2.0 in total | proven (resolve list) |

The order of 6 to 11 matches UE3's post chain (DOF/bloom, uber, motion blur, material effects), but these logs do not
prove it.

### 1.5 EDRAM cost per frame, by label

`GPU time by pass`, category 12 (`edram_alias`), steady cockpit. Labels: VS17 = 16F->RGBA8, VS18 = RGBA8->16F, VS19 =
16F->16F (f2<->f3), VS20 = variant, VS40 = resolve from the 7e3 owner, VS41 = biased resolve (fragment), VS42 = biased
resolve (compute), VS43 = 32-bit word copy, VS(n+1) = compute conversion pipeline n; the "PS" of a conversion label is
its tile count.

| item | 960 | 1280 | part of the post chain? |
|---|---|---|---|
| VS41 biased resolves (4 / 5 per frame) | 1.3-1.4 | 2.3-2.5 | yes (the HDR chain) |
| VS40 raw resolves (1.5 / 2) | 0.6-0.9 | 1.2-1.4 | lighting (and distortion) |
| VS19 f2->f3 (1.5 / 2 x full screen) | 0.8-1.0 | 1.6-1.8 | lighting restore (or distortion) |
| VS11 -> VS43 f0->f4 velocity import | 1.3-1.5 -> 0.25 | not seen | yes (motion blur) |
| not in the top 16 (many small ops, each paying a pass and a wait-for-idle) | 2.5-3.0 | 4.5-5.0 | mostly the 0x5A0 quarter-buffer traffic (the `color alias` count is ~10 ops per frame, 8 of them the C5A0 2x->1x) |
| **edram_alias total** | **7.4-7.7 (5.7-6.3 with copy32)** | **10.1-10.8** | |
| edram_export / edram_import | 0.3 / 0.6 | 0.5 / 0.3 | 0x5A0 shadow <-> quarter buffer |
| edram_import9 | 0 | **5.9** | no: shadow depth, see 1.6 |

### 1.6 The 1280-only item: `edram_import9` 5.9 ms

`9-pass imports: [C400:1280x1280:mx0my1->5A0:880x880:mx0my0 2610ops/10440t]` and `lazy stencil: ... 2784 late fetches
(135720 tiles)`: at 1280 the shadow-map depth view at 0x5A0 takes tiles that the 2x scene-color view C400/f3 last owned.
A color source counts as "all 8 stencil bits may be set", so every late stencil fetch runs the 8 bit passes (780 tiles
per frame). The shadow clears there are rect lists (VS9828/PS10237, depth ALWAYS, stencil off) and are declined with
`partial tile: owner unsupported` (174 per 10 s). This is outside the post chain. The cheapest exact fix is to accept a
color owner in the partial-tile clear redirect, or to treat the shadow view's stencil as inert when no shadow draw tests
stencil. It is worth about 5 ms at 1280 and nothing at 960. Listed here because it is a third of the "17 ms".

### 1.7 Not known yet

- Which draw makes f2 the owner after each raw resolve (step 4): a raw restore (UE3 copies the resolved texture back
  through the integer 10:10:10:2 view), or the distortion apply PS12145, or a clear in the UNORM10 view.
- Whether PS3474 / PS21415 draws are restores of an unchanged scene color (the texture they sample was resolved from the
  f3 image, and the f3 image has not changed since).
- The two blur shaders of the quarter chain, and the source of the C5A0 2x view (probably a clear of the quarter buffer
  issued while the 2x MSAA state is still set).
- At 960, the per-pass split inside category 3, and whether part of the post chain still runs at 1280 inside the game's
  upscale (backlog E40, t154). The current 960 logs show `scene_no_z 0.0`, so check the pass sizes in capture A.

## 2. What to capture on the console

Use the normal Normandy route (`tools/me1_anderson.sh`, or `me1_anderson_timed.sh` for a picture that does not fill the
screen). Start from `run/me1/shipped.toml` (960) or `run/me1/full1280.toml` (1280) and **append** the lines below. Times
are seconds after the renderer starts: in `long_final` the cutscene is around 200-260 s and the steady cockpit around
280-320 s. Run each capture separately: B makes the log large and slows the frame, so its times are not valid.

### A. Ordered GPU marks of 6 frames, with sizes and formats (no image change, negligible cost)

```toml
masseffect_diag_dump_marks_s = 290
masseffect_gpu_marks_categories = true
masseffect_native_precise_marks = true
```

Once, about 290 s in, the log gets 6 lines (one per work slot, normally one per frame):

`[native] GPU marks of one slot (N marks): [i:cC VSa PSb WxH fF/dD n draws ms] ...`

- `c` is the category (2 scene, 3 "640" = 960 targets, 4 quarter res, 6 copies, 8 full-res without depth, 10/11/12/13
  EDRAM import/export/alias/import9). `VS PS` is the label: the shader numbers of the first draw for passes, or the
  conversion and resolve labels of section 1.5. `WxH fF/dD` is the last render pass begun in that interval: size, host
  color format (97 = RGBA16F, 37 = RGBA8, 64 = A2B10G10R10, 77 = R16G16) and depth format. Marks shorter than 0.05 ms are
  left out. The label of a copy (cat 6) is 0.
- What to look for: the order of the VS40 / VS41 marks against the passes of PS21267, PS3474, PS21415, PS12145, PS24056,
  PS15729, PS15184, PS16272, PS7992; where each VS19 (f2->f3) sits; the WxH of the post passes at 960 (960x544, or 1280x720
  as E40 said); how many cat 12 marks fall between two cat 4 marks (the quarter-buffer small ops).
- Do it at 960 and at 1280. At 1280 the full-res post passes are category 8, so they are not merged with the scene.

### B. Ownership trace of one scene-color tile (log heavy, timing invalid)

```toml
# 960x544: tile at row 17, column 6 of the 0x2D0 surface (screen x 480-559, y 272-287): 720 + 17*12 + 6
masseffect_diag_trace_tile = "930"
masseffect_diag_trace_from_s = 290
```

At 1280x720 use `"888"` (720 + 10*16 + 8; it stays below the C400 view). For the quarter buffer and its border, a second
run with `"1440"` (the first tile of 0x5A0). The trace starts at `trace_from_s` and continues until the game is closed
(up to 1024 events per frame), so close the game about 10 s later.

Lines (`[native] EDRAM TILE TRACE ...`):

- `draw-request frame=F nextDraw=D vs=.. ps=.. surface=.. depthinfo=.. ...` when the draw state changes;
- `sync-bind ... target=[... guestfmt=G ...] owner=[... guestfmt=H ...]`: a conversion is needed (H -> G);
- `publish ... before=[...] after=[... guestfmt=G ...]`: a draw made view G the owner;
- `clear-publish ... after=[... guestfmt=G ...]`: an EDRAM clear (resolve command) made G the owner;
- `draw-result`, `import-attempt`, `stencil-only-preserve`.

What decides step 4 of 1.4: the event just before each `sync-bind target=[...guestfmt=3...] owner=[...guestfmt=2...]`.

- A `publish ... after=[guestfmt=2]` preceded by a `draw-request` with `ps=12145` means it is the distortion apply.
- With another PS (look for 21415, 3474 or an unknown copy shader), it is a raw restore. Note its `vs/ps` for the
  allowlist of section 4.
- A `clear-publish ... after=[guestfmt=2]` means it is a clear issued through the UNORM10 view.

Resolves are not traced. If they are needed in the same stream, add one `TILE TRACE resolve` line in `CopyInternal`
right after `area_edram_` is set: source view class, `direct_7e3 != nullptr`, the exponent bias, `clear_color`,
destination address and format, rect. It is a diagnostic only and changes nothing.

### C. Per-pixel-shader GPU cost (one frame every 5 s, serialized per draw)

```toml
masseffect_native_stats_per_draw_s = 5
```

Lines: `[native] per-draw GPU time (N diagnostic frames, serialized): ...` and up to 25 lines
`#k PS n<number> cat <c>: <ms>/frame, <draws> draws, <Mfrag>, <ns/frag>, ... costliest draw ... with VS n<number>`.
The times are serialized and scaled x1.627, so they are larger than in normal play. Use them to rank, not to add up.
What to look for: the cost of PS24056 against the cat 4 total (the rest is the two blurs: note their PS numbers), and
PS15729, PS15184, PS16272, PS7992, PS3474, PS21415, PS12145 at 960 and 1280. This gives the per-pass budget of section 3.

## 3. Replacing the chain: options from least to most invasive

The savings are per frame, steady cockpit, against the current shipped configuration (copy32 on). They are estimates
built from the measured labels above, not measurements. "Exact" means the same EDRAM words, hence the same image bit for
bit.

| # | option | what changes | ms saved 960 / 1280 | exact? | risk |
|---|---|---|---|---|---|
| A | **Restore elision** (renderer only, game draws kept) | a whole-screen copy draw that writes back a texture resolved from the f3 image, while that image is unchanged since the resolve, is skipped; f3 is published as the owner again. That also removes the f2->f3 conversion and, when the raw resolve has no other reader, the resolve itself (made lazy) | 1.1-2.3 / 2.0-3.9 (VS19, the restore draws, and VS40 if lazy) | yes, by construction (under the conditions of section 4) | low to medium: wrong conditions mean stale content, so a verify mode comes first |
| A' | if capture B shows a **clear** through the UNORM10 view instead | apply the clear to the f3 owner (same 32-bit word decoded as 7e3) and publish f3 | 0.8-1.0 / 1.6-1.8 | yes | low |
| A'' | if it is the **distortion apply** PS12145 | "write-through": render it into the f3 image with an output epilogue that packs the UNORM10 result to its 32-bit word and decodes it as 7e3 (blend ONE/ZERO, so no read of the destination) | 0.8-1.0 / 1.6-1.8 | yes (it is what EDRAM does) | medium: a new output variant in the module transform, like the existing "7e3 output" variant |
| B | **Quarter-buffer separation** | the k_16_16_16_16_FLOAT views at 0x5A0 (pitch 320 / 400) get their own host image when the trace proves the border is a known clear; no ownership exchange with the shadow depth D5A0 or with C400 | 0.6-1.5 / 1.0-2.5 (export, imports, the small cat 12 ops) | yes if the border clear is proven; otherwise a 1-pixel border of the bloom buffer can differ | medium |
| C | **Host-side post targets and zero-copy resolves** (game draws and shaders kept) | from the first biased resolve after the last light pass (or the first draw of a known post PS) until the front-buffer resolve, 2D0 is rendered into a ping-pong pair of RGBA16F host images (plus RGBA8 for LDR passes). A biased resolve becomes a hand-off of the image just written, with the 2^-3 folded into the reader's texture fetch (a per-fetch scale specialization, the same machinery as `masseffect_native_fold_texture_signs`) or into the writer's output when 2D0 is fully rewritten after the resolve. Plain resolves become hand-offs | +1.6-2.2 / +2.5-3.3 on top of A and B (VS41, the post part of the copies, the velocity import) | yes, if each hand-off is proven: full overwrite by the next pass, no EDRAM read of old content, 7e3 quantization kept at the same points | medium to high: the chain varies (DOF on or off, motion blur only when moving, distortion, per-level material effects, UI). Any unknown draw forces a fallback that writes the host image back into EDRAM, which costs one conversion |
| D | **Native fused post chain** (recognised by PS fingerprints) | gather + 2 blurs as one compute pass; uber (PS15729) -> material effects (PS16272, PS7992) -> final copy fused into one full-screen pass at output resolution; the game's post draws and resolves are skipped; constants are taken from the guest registers at the skipped draws | +1-2 / +3-4 on top of C (fewer full-screen read/write passes: `scene_no_z` 6.3-6.6 ms at 1280 could drop to about 3) | only for pointwise passes, and only if the 7e3 / FP16 / RGBA8 quantization between the original passes is reproduced in the fused shader; the gather is exact only with the same 32 taps | high: per-level material PS must stay unfused (fallback), 1-2 weeks |
| E | **Hook UE3 post functions** in guest code | the game emits no post commands; native passes take the parameters from the effect objects | about D + a little CPU (the post command emission and the ring work) | depends on how good the reimplementation is | highest: reverse engineering per edition (English and Russian addresses, overlays and codegen rules 4-5 of do-not-break.md); not recommended before C and D have data |

Cumulative, if everything exact lands: 960 about 3.5-6 ms (A+B+C), 5-8 ms with D; 1280 about 5.5-9.5 ms (A+B+C),
9-13 ms with D, plus about 5 ms from the separate 1.6 fix. With 37 ms now at 960, A+B+C reaches about 31-33.5 ms. The
1280 goal needs all of it plus the 1.6 fix: 64 -> about 46-50 ms. It is not reachable from the post chain alone; the scene
itself (about 24-27 ms at 1280) is the rest.

Image-changing variants worth measuring later (the user decides): a 16-tap instead of 32-tap gather, or a separable
compute DOF/bloom (cat 4: -1.5 / -2.5 ms); running the post at the internal resolution and upscaling once at the end
(only matters if capture A shows post passes still at 1280 in 960 mode).

Already done or tried, do not repeat: G7 (7e3-direct resolves), fragment biased resolves, packed-extent overwrite proofs
(VS9460/PS3474 f0->f3), `conversion_copy_32` (f0->f4), batching barriers (E17: no gain, the cost is per pixel and per
pass), turning post or motion blur off in the game's config (no effect: not read from Coalesced).

### How to prove equivalence on the console

1. **Shadow compare (the main tool, a diagnostic cvar):** in the frames the diagnostic picks, run the old path and the
   new path side by side into separate images, reduce the difference on the GPU (a compute pass writing count of
   differing texels and max |d| per pass into a small buffer), read it back with the work-slot fence, and log
   `post compare: step k (VS/PS ..), differing texels N, max |d| ..`. Options A to C must give N = 0. This works with a
   moving camera, film grain and animation, where screenshots cannot be compared pixel for pixel.
2. **Screenshots:** the usual route with c01..c30 and the contact sheet, A/B with the same build and only the cvar
   changed, cold start. Look at the bloom edges, DOF, distortion, motion blur in the cutscene camera moves, and the
   lit faces (do-not-break.md: black Shepard, lighting).
3. **Counters:** `top transfer pairs` (C2D0 f2->f3 gone for A), `operations since last report` (`resolve from 7e3=` and
   `resolve bias=` drop for A/C), cat 12 labels VS19 / VS40 / VS41 drop, `GPU per Swap edram_alias`, `edram_export`,
   `edram_import`, and fps. Check that the internal-resolution hook lines are present (do-not-break.md 2a) and that the
   uploaded toml was verified (rule 6).

## 4. First step (under a day, after capture B)

**Restore elision behind a cvar, starting with a detect-only mode.** It removes the per-light f2->f3 round trip, the
cleanest and best-proven piece of the 7e3/UNORM10 cycle, without touching any shader.

Cvar: `masseffect_native_restore_elision` (int, default 0): 0 = off; 1 = detect and log only; 2 = skip the draw.

1. **Remember each resolve** in `CopyInternal`: guest texture address, rect, the image whose content was copied (the f3
   owner for the 7e3-direct path, `target_render` otherwise), that image's content generation, exponent bias, guest
   texture format. Add a `content_generation` counter to `Image`, incremented on every write into it: draw publish,
   conversion or import into it, clear, resolve-clear. The publish/sync paths of `masseffect_native_targets.cpp` are the
   only writers.
2. **At a draw**, before `SynchronizeEDRAM4`, a candidate must meet all of these:
   - color slot 0 at the base of the remembered resolve, a whole-tile rect equal to the resolve rect (the existing
     overwrite proof, `edram_overwrite_rect`);
   - blend ONE/ZERO, color mask 0xF, no alpha test or kill, depth and stencil not written;
   - the (VS, PS) fingerprint pair is in an allowlist taken from capture B (expected: the raw restore pair, and
     VS6906/PS21415, the plain copy);
   - fetch constant 0 points at the remembered texture address with the remembered format, the texture has the size of
     the rect, and the sampler is point (or linear exactly at texel centers);
   - the remembered image still has the remembered `content_generation`, and its class matches what the draw would
     produce: raw bits through f2 from a fmt 7 texture that came from f3, or fmt 3 with an identity scale.
3. **Mode 1** logs one line per candidate (frame, VS/PS, base, rect, source image, generations, accepted or the first
   failed condition) and changes nothing. One console run tells how many restores qualify per frame. The expected
   answer is 1.5 at 960 and 2 at 1280, the VS19 count.
4. **Mode 2** skips the draw and publishes the remembered image as the owner of the rect's tiles with a new version (as
   if the draw had written it). Ownership then never moves to f2, so the next additive light pays no conversion.
5. **Verify:** mode 2 against mode 0 with the same build. VS19 should drop to about 0 and `edram_alias` by 0.8-1.0 ms
   (960) / 1.6-1.8 ms (1280), plus the skipped draws. Screenshots of the cockpit lights and of the Anderson cutscene faces.
   Before enabling it by default, the shadow compare of section 3 on the skipped draws (re-run the draw into a scratch
   image and compare it with the f3 image).

If capture B shows a clear (A') or the distortion apply (A'') instead, do that variant first. A' is the smallest: in the
`clear_color` branch of `CopyInternal`, when the view is class 2, every tile of the clear span is owned by a class-3
image, and the span covers whole tiles, clear that image with the same 32-bit word decoded as 7e3 and publish it.

Out of scope for the post chain but cheap and large at 1280: section 1.6 (`edram_import9`, about 5 ms).
