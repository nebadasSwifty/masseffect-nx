# GPU cost analysis: Mass Effect 1 on Switch (NVK, GM20B)

Date: 2026-10-07. This is a read-only analysis of the console logs in `mass-effect-recomp/run/me1/*/console.log` and of
the renderer source. Nothing was built and nothing was run on the console.

Units: ms of GPU time per guest Swap, from the "GPU per Swap" lines. These runs use precise (BOTTOM_OF_PIPE) marks.
In every window used below, Swaps x total is about 10 s, so the GPU was busy for the whole window.

## 0. Measurement warning: the "960x544" Russian runs rendered at 1280x720

Four runs printed `internal resolution 960x544 (parts 0xef)` but did not actually render at that size:
`and_fix1`, `and_u_bil960`, `and_u_fsr800` and `and_u_bil800`.

- None of the resolution hooks logged anything: there is no `internal resolution: viewport/device/back buffer/scene render targets` line.
- Every EDRAM surface is `1280x1280`, and there is no `960x960` surface.
- Their numbers match `and_g_noscene` (no scene size set, so 1280) to within 1 %. The means of the GPU-bound windows are
  total 56.7 against 56.3 ms and edram_alias 32.7 against 32.3 ms.

So the bilinear and FSR upscaler comparisons in these runs compare 1280 against 1280. The real 960x544 Anderson
reference is `and_base` (English build, parts 0xff, `960x960` surfaces). The resolution hook path of the Russian build
should be checked before any further resolution A/B.

## 1. GPU ms per frame by category

At 960 the scene pass and the full-screen post-processing fall into the "640" column, because the category depends
on the target pitch. At 1280 they show up as "scene" plus "scene_no_z". Compare those columns as one sum.

### Anderson cutscene, matched heavy phase

Both runs do 7.35 full-screen color conversions per Swap in this phase.

| category | 960x544 (`and_base`, 215 Swaps/10 s) | 1280x720 (`and_g_p0`, 140 Swaps/10 s) | scaling |
|---|---|---|---|
| scene + post (scene, scene_no_z, 640) | 16.5 | 21.2 (14.0 + 5.8 + 1.4) | x1.28 |
| edram_alias | **26.3** | **45.7** | x1.74 (the pixel ratio is 1.76) |
| copies | 1.6 | 2.4 | x1.5 |
| other | 0.6 | 1.0 | |
| gap (GPU waiting for the CPU) | 1.5 | 1.5 | |
| **total** | **46.5 (21.5 fps)** | **71.9 (13.9 fps)** | x1.55 |

`and_fix1` reads 71.0 to 72.4 ms in the same phase, which is the 1280 value (see section 0).

### Means of all GPU-bound windows

These are the windows with gap < 3 ms and alias > 3 ms. They mix character creation with the cutscene.

| scene / run | resolution | total | scene + post | 320 | alias | import / export | copies |
|---|---|---|---|---|---|---|---|
| Anderson `and_base` | 960x544 | 40.6 | 17.4 | 1.5 | 17.1 | 0.6 / 0.4 | 1.8 |
| Anderson `and_g_p0` | 1280x720 | 56.2 | 17.8 | 1.0 | 32.2 | 0.3 / 0.3 | 2.2 |
| Normandy cockpit `ab_n_unified` | 960x544 | 41.1 | 20.0 | 2.8 | 14.3 | 0.9 / 0.5 | 1.2 |
| Citadel `ab_c_unified` | 960x544 | 40.5 | 16.2 | 2.5 | 16.9 (+import9 1.0) | 0.8 / 0.4 | 1.2 |

There is no 1280 Normandy run. Going by the Anderson scaling, it should cost about 41 + 0.74 x 14.3 + 0.28 x 20,
or roughly 57 ms.

### How each category scales with resolution

- **edram_alias** is purely per-pixel: one full-screen conversion costs 1.7 to 1.85 ms at 960x544 and 3.0 to 3.3 ms
  at 1280x720. That is about 0.3 Gpixel/s, an order of magnitude below the GPU's fill and bandwidth limits.
- **Scene + post** grows only 1.28x, so a large part of it is vertex or draw bound.
- **Copies** grow about 1.5x. **Shadows, other and gap** do not depend on resolution.
- At 1280, alias is 64 % of the frame in the heavy phase. At 960 it is 57 %.

## 2. Why the alias conversions happen

### Formats

All the conversions are on the scene-color EDRAM region: base tile `2D0` (720), pitch 1280 (or 960), whole target,
720 (or 408) tiles per operation. The game views this one region through four guest formats, and the host keeps a
separate VkImage per storage class (`GetTarget`, keyed by `ClassEdramFORMAT`, in `masseffect_native_targets.cpp` around line 3085):

| class | guest format | host format |
|---|---|---|
| f0 | k_8_8_8_8 | RGBA8_UNORM |
| f2 | k_2_10_10_10 / AS_10_10_10_10 (UNORM10) | **RGBA16F** |
| f3 | k_2_10_10_10_FLOAT / AS_16_16_16_16 (7e3) | **RGBA16F** |
| f4 | k_16_16 | R16G16_UNORM |

Moving ownership between classes always goes through the 32-bit word. The fragment path is
`ConvertAliasColorFragEDRAM4` with `shaders/me_edram_color_to_color.frag`. The compute path is `ConvertAliasEDRAM`.

### Anderson: about 7.35 conversions per frame, at both resolutions

The sources are the `pair ... draws:` lines and transfer traces 1 to 12.

| pair (per frame) | triggered by | what the trigger does | avoidable? |
|---|---|---|---|
| f3 -> f2, x2 | a **resolve**, with no draw (`first: -`, cause `color-resolve/color->color`) | resolve of RT format 10 (UNORM10 view) into a k_2_10_10_10 texture, host A2B10G10R10 (`resolve conversion: guest RT 10 host 97 ... -> guest texture 7 host 64`) | **yes**: resolve straight from the f3 image |
| f2 -> f3, x2 | additive draws into f3-AS (VS6951/PS731, VS2013/PS29025, VS12100/PS27525, blend ONE+ONE) | ownership moved to f2 only because of the resolve above | **yes**, follows from the row above |
| f0 -> f3, x2 | VS9460/PS3474: 6 vertices, blend ONE/ZERO, mask F, no z or stencil test, full-screen scissor | an opaque full-screen quad that overwrites everything, but the extent estimator rejects it (`bounded draw extent: VS=9460 accepted=false reason=fetch-format`, `overwrite 0 est -1`) | **yes**, with an overwrite proof |
| f3 -> f0, x1.33 | VS22095/PS21267: 36 vertices, blend 0x00080008 (src x DstColor), stencil on | genuinely reads the destination | no, unless the PS is proven to output only 0 or 1 |

Labelled GPU time at 1280 (`GPU time by pass`, cat 12) splits as follows:

| label | meaning | ms per frame |
|---|---|---|
| VS19 | 16F -> 16F (f2 <-> f3) | 13.1 |
| VS18 | RGBA8 -> 16F (f0 -> f3) | 6.0 |
| VS17 | 16F -> RGBA8 (f3 -> f0) | 4.0 |
| VS0 | unlabelled, mostly "resolve bias" compute (about 6 to 7 per frame) | 5.1 |

The same labels at 960: 7.4, 3.3, 2.2 and 3.1 ms.

### Normandy at 960

Per frame: f0 -> f4 1.0, f4 -> f3 0.5, f0 -> f3 0.5, f2 -> f3 0.5, f3 -> f2 0.5.

- f0 -> f4 goes through the **compute** path (pipeline 10, a raw 32-bit copy through R32_UINT storage views).
  It costs 2.7 ms per 408-tile operation, which is 1.6x the fragment path.
- f4 -> f3 (pipeline 11) is also compute.
- Labelled costs per frame: VS11 (f0 -> f4) 2.7, VS12 (f4 -> f3) 1.4, VS19 1.8, VS18 0.85, unlabelled 1.8.

### Design options

- **One VkImage with mutable-format views, the 32-bit word as the canonical store.** This makes f0 <-> f4 free, and
  f0 <-> f2 too if f2 moves to host A2B10G10R10_UNORM_PACK32, which is bit-exact with Xenos UNORM10. It does not help
  the Anderson cycle, because every pair there involves f3, and 7e3 has no Vulkan format with blending.
  Worth it for Normandy: f0 <-> f4 is about 2.7 ms.
- **Keeping f3 in RGBA16F and never leaving it unless a draw needs another view** removes most of the cycle. That is
  what proposals 1 and 2 below do.
- **Render-pass merging and barrier batching** will not help. E17 batched the barriers with no gain: the cost is
  per pixel, not per sync.

## 3. Top GPU reductions that keep the image identical

| # | change | where | ms saved per frame (960 / 1280) | risk | verify with |
|---|---|---|---|---|---|
| 1 | **Resolve straight from the f3 owner** (backlog G7). When a resolve reads base 2D0 as UNORM10 and the newest owner is the f3 RGBA16F image, run one fragment (or compute) pass. It reads f3, packs the 7e3 word (`Pack32` format 3 from `me_edram_color_to_color.frag`) and writes it bit-exact into the A2B10G10R10 texture (UNORM10 decode of the word, or an R32_UINT view). Do **not** move EDRAM ownership to f2; f3 stays the owner. This removes f3 -> f2 **and** the f2 -> f3 that follows. | `CopyInternal` (`SynchronizeEDRAM4` call before the copy, about line 1492) and the resolve-conversion branch (about line 1558) in `masseffect_native_targets.cpp`; new shader next to `me_edram_color_to_color.frag` | Anderson **7.4 / 13.0**; Normandy about 1.8 / 3.2 | Medium. A draw that later binds f2 must still convert; only the ownership move is skipped. | `top transfer pairs`: f3->f2 and f2->f3 disappear; `transfer causes`: `color-resolve/color->color` reaches 0; cat 12 VS19 is gone; same pixels in screenshots |
| 2 | **Overwrite proof for VS9460/PS3474**: add the fetch format it uses (log `rejected_format`; probably 16_16 / 16_16_FLOAT / 8_8_8_8) to the extent estimator. The estimator only accepts 32-bit float positions. The draw is opaque, full-mask, without depth or stencil, and full-screen, so the f0 -> f3 conversion becomes a proven overwrite. The same fix should cover VS17638 and VS19666 (also `fetch-format`) and Citadel's VS9737 (G6, 2 to 2.7 ms). | `me_native_draw_extent_estimator.h` lines 150 to 157 (`float4/float3/float2` only) | Anderson **3.3 / 6.0**; Normandy 0.85 / 1.5 | Low. A rejected proof falls back to today's path; the PS must have no kill. | `bounded draw extent: VS=9460 accepted=true`; the f0->f3 pair disappears; `proven-overwrite binds` goes up; VS18 drops |
| 3 | **Fragment path and an identity-layout shader for the remaining conversions.** (a) Route R16G16 pipelines 9 to 12 through the fragment pass: compute costs 1.6x per pixel here. (b) For f0 <-> f4, which is a raw 32-bit copy at the same base and pitch, use `vkCmdCopyImage` between size-compatible formats, or one mutable image. (c) Add a specialization for whole-target runs with the same base and pitch (source tile == target tile): `p = pixel`, with no runtime `/ %` by the pitch (emulated on Maxwell) and no `discard`. | `ConvertAliasEDRAM` / `ConvertAliasColorFragEDRAM4` (about lines 6387 to 6560), `me_edram_color_to_color.frag` | Normandy **2.5 to 3.5** (f0->f4 nearly free, f4->f3 1.4 -> 0.9); Anderson after #1 and #2, the remaining f3->f0: about 0.5 / 1 | Low, as long as the words stay bit-exact. | cat 12 VS11 and VS12 are gone or fall; VS17 drops |
| 4 | **"resolve bias" as a fragment pass, or merged into the resolve copy.** About 6 to 7 per frame, currently a compute storage-image RGBA16F pass between two ALL_COMMANDS barriers. Multiplying by exp2(bias) is exact. | `ResolverWithBias` (about line 5191), `shaders/me_resolve_exp_bias.comp` | about 1.5 / 2.5 (half of the VS0 share) | Low. | `operations since last report: resolve bias=` falls; cat 12 VS0 falls |
| 5 | (research) **The f3 -> f0 multiply (VS22095/PS21267).** If PS21267 is proven to output only 0 or 1 (multiply by 1 = identity, by 0 = zero bits = 0.0 in 7e3), run it on the f3 image directly with the same stencil test, and drop the f3 -> f0 conversion. | draw path, per-shader proof | 2.2 / 4.0 | Medium to high: it needs a shader-output proof. | f3->f0 pair is gone; pixel diff |

### Expected effect in the Anderson heavy phase

- **960x544:** 46.5 ms -> about 46.5 - 7.4 - 3.3 - 1.5 - 0.5 = **about 34 ms**. #5 would make it about 32.
  That is close to 30 fps, but scene + post (16.5 ms) is now the main remaining cost.
- **1280x720:** 71.9 ms -> about 71.9 - 13 - 6 - 2.5 - 1 = **about 49 ms**. Even with #5 (about 45 ms), 1280 at
  30 fps still needs about 12 ms of savings in scene and post. Without that, 1280 is not reachable from conversions alone.
- **Normandy 960:** 41.1 ms -> **about 34 to 35 ms**.

### Order of work

Do #2 first: it is the smallest change. Then #1, which saves the most. Then #3 and #4. After each step, compare
`GPU per Swap edram_alias` and the `top transfer pairs` against `and_base` (960) and `and_g_p0` (1280).

## 4. Already tried: do not repeat

Sources: `mass-effect-recomp/docs/optimization-backlog.md` (B), `masseffect-nx/docs/*`.

### EDRAM

| attempt | result |
|---|---|
| Mode 0, no aliasing (E25) | rejected: 7 fps ceiling, see-through characters |
| Mode 2, physical "same memory, other format" aliasing | colored bands |
| Mode 3 | flashes |
| E9: stencil-only bit-pass shader | no change |
| E10: import render area equal to the tile rectangle | no change |
| E14 / E15 / E16: barrier dedupe, copy engine only for runs of 32+ tiles, narrower dependencies | no measurable change |
| E17: batching one sync's transfers | GPU unchanged: the volume is the cost, not the waits |
| E28: Mesa fragment barrier instead of wait-for-idle | no change |
| E30: exact pixel bounds | never fired |
| G5: zeroing the f0/f2/f3 sibling views | 0 effect |

Kept, already in: E11/E12 (stencil through the copy engine), E33/E34/E36 (redirected clears), E37/E38 (overwrite
proofs), G3 (conversions as fragment passes).

### Resolution

| attempt | result |
|---|---|
| 960x540 instead of 544 | no gain |
| 800x450 and 800x448 | slower: they fall off the 16-pixel tile grid |
| 1120x624 | -6 fps |
| `-ResX/-ResY` on the command line and `StartupResolution` in the game config | do not work |
| Title planet at 960x544 | black (parked) |
| Post-processing passes at 960 | stay at 1280 inside the game's upscale, so they cannot be reduced exactly (E40) |

### Scene and post

| attempt | result |
|---|---|
| ZCULL (S24) | no gain |
| Early-Z for alpha-tested geometry (S25) | at most -2.3 ms |
| FMA rewrite of HLSL (S23) | no gain |
| Shadow scale, flags, radius, no-load (S9, S13, t186, t208) | no gain |
| Motion blur or post off in the game config | no effect |
| Submitting every N draws | worse |
| TOP_OF_PIPE against precise marks (M4) | same fps: the marks do not inflate the frame |

### Not tried yet, from the docs

- G6 (estimator for VS9737) and G7 (resolve from the f3 view). These are proposals 2 and 1 above.
- FSR1 and dynamic resolution. The `and_u_*` runs did **not** test them; see section 0.
- Skipping the stencil fetch for inert-stencil draws.

## 5. Eden Prime after the Saren/Nihlus cutscene (2026-10-08)

Sources: `mass-effect-recomp/run/me1/prof_eden/masseffect_382.log` and `prof_eden2/game.log` (RU, 960x544, build
ru_glob19). Offline analysis; nothing was run on the console for this section. Raw `GPU time by pass` values are
converted with x1.627 / Swaps (measuring.md); `GPU per Swap` is already in ms.

### 5.1 Where the GPU time goes

| window (prof_eden) | Swaps / 10 s | total | gap (GPU idle) | busy | "640" (scene + post) | edram_alias | copies |
|---|---|---|---|---|---|---|---|
| 04:36:51 | 291 | 34.4 | 7.0 | 27.4 | 17.6 | 5.0 | 2.4 |
| 04:37:11 | 266 | 37.7 | 6.1 | 31.6 | 21.3 | 5.3 | 2.7 |
| 04:37:21 (heaviest) | 175 | 57.4 | 20.5 | 36.9 | 27.8 | 4.7 | 2.2 |

The heavy windows have a large gap: the frame there is limited by the CPU (ring and game thread, see best-config.md
"Eden Prime"), so a GPU saving shows up in fps only in the windows where the gap is small.

Labels per frame (window ending 04:37:01, 294 Swaps; raw ms x 1.627 / 294):

| label | what | per frame | ms per frame |
|---|---|---|---|
| cat 3 VS20772/PS9551 (and PS21081 in the heavy window) | scene base pass | | 7.7 (heavy window: 10.6) |
| cat 12 VS41 | exponent-bias resolves into `15214000` (960x544 RGBA16F) | 5.7 (1671 / 294) | **3.3** (0.58 each) |
| cat 6 | copies (all non-EDRAM resolves) | | 2.1 |
| cat 4 VS26396/PS25414 | quarter-res DOF/bloom gather and blurs | | 1.4 |
| cat 12 VS40 | 7e3 -> UNORM10 resolves (compute) | 1.3 (380 / 294) | 0.9 |
| cat 12 VS0 | small conversions (C5A0 2x -> 1x border tiles) | 8 | 0.3 |
| cat 0 VS43 | f0 -> f4 word copy (`conversion_copy_32`) | 1 | 0.4 |
| cat 10 / 11 / 13 | depth imports / exports / 9-pass stencil imports, all at 0x5A0 | 43 / 10 / 8.5 | 0.3 / 0.2 / 0.0 |

The 1.9 ms quoted for VS41 in the task was the raw value: with the timestamp factor it is 3.1 to 3.3 ms per frame,
the largest single EDRAM item in this scene.

### 5.2 The shadow-map traffic at 0x5A0

The shadow map is D5A0 (D24S8, pitch 880, 1x, 864x864 used: four 432x432 slots; the shadow draws use a 422x422 scissor
at 5,5 inside a slot). The game clears a slot through the 4x alias of the same EDRAM (`D5A0 440x720 mx1my1`, a collapsed
single-sample host image) and also keeps its quarter-res DOF buffer at the same base (`C5A0/f7`, k_16_16_16_16_FLOAT,
pitch 320, 64bpp: EDRAM tiles 0x5A0..0x5E8). Per frame (pairs and decline reasons, both logs):

| pair or event | per frame | triggered by |
|---|---|---|
| `D5A0 4x -> D5A0 1x` (depth-repr-switch) | 27 ops, 136 tiles | the shadow draws VS16864/PS6118, PS24934, PS13880 (stencil off) pulling tiles the 4x view owns |
| 9-pass imports `D5A0 4x -> 5A0 1x` | 8.5 ops, 26 tiles | late stencil fetches before the D5A0 -> C5A0 exports (bits skipped: the 4x stencil bits are 0) |
| `D5A0 1x -> C5A0/f7` (depth->color) | 10 ops | the quarter-buffer rect clear VS10373/PS10803 (`ow 0,128-240,138`) |
| `C5A0/f7 -> D5A0 1x` (color->depth) | 6-8 ops of 1 tile | the 1x strip clears VS10373/PS10803 mode 5 (`ow 400,0-432,432`): partial tiles over C5A0 |
| `C5A0/f7 2x -> 1x` | 8 ops, 13 tiles | the DOF gather VS26396/PS25414 (border tiles) |
| depth clear redirect declined: `target C5A0/f7 ... (clear of 5A0/f0 mx1my1 depth 1 stencil -1)` | 1 | a depth-only clear (to 1.0) of a slot drawn through the 4x alias: some of its whole tiles are owned by the 64bpp C5A0 view |
| declined: `partial tile: owner unsupported` | 1 | the 1x strip clears above (their partial tile column 5 is owned by C5A0) |
| declined: `depth not proven constant (depth info 000005A0)` | 1-4 | not a constant-z draw (no change possible without a trace of that draw) |

Mechanism of the 27 repr switches: the redirect of the 4x clear (E4/E5: clear the tiles in the 1x alias, which becomes
their owner) refuses the whole clear as soon as one tile is owned by a 64bpp color view, because a 64bpp view is not a
lazy stencil source. The clear is then drawn on the 4x view, which becomes the owner of the whole slot, and every
shadow draw of the next light pulls its rows back 4x -> 1x (one import run per tile row; backlog E39).

### 5.3 Implemented: `masseffect_native_edram4_clear_alias_raw64` (default false)

In `RedirectClearDepthEDRAM4` (`masseffect_native_targets.cpp`): for a **depth-only** clear (no stencil write) drawn
through a **collapsed single-sample 4x view** that has a known 1x alias, a whole tile owned by a 64bpp color view is
now handed to the 1x alias like the tiles the 4x view owns itself: the depth of the tile is cleared in the alias, and
the alias becomes the owner. Partial tiles are not touched (still declined as before).

Why the picture cannot change: the redirect reproduces what the literal path leaves behind, tile by tile.

- Depth: the literal path clears the whole tile in the 4x view (one collapsed value per pixel, the constant) and the
  next 1x draw imports it; the redirect writes the same constant into the 1x alias directly. This is the existing
  E4/E5 redirect, unchanged; only the owner test is wider.
- Stencil: the literal clear does not write stencil. Its publish (`PublishEDRAM4`) never takes a 64bpp previous owner as
  a lazy stencil source, so the 4x view's own host stencil counts as real for those tiles, and the 4x -> 1x import
  records "stencil in the 4x view, same tile". The redirect records exactly that source for the alias tile
  (`edram4_stencil_source_[alias][tile] = {4x view, tile}`) and marks the 4x view's stencil of that tile as real.
  Every later stencil user (the late fetch before the D5A0 -> C5A0 export, a stencil-testing draw) fetches the same
  bits from the same image as before. The 4x view is not written in between (nothing else draws through it; a later
  4x draw first imports the tile back from the alias, and that import keeps the 4x stencil because the record points
  at the destination itself).
- Ownership and versions follow the existing reassign path (new tile version, epoch bump, stamps for the learned
  consumer), so every other view syncs from the alias exactly as from any redirected tile.

Fallbacks (the old path runs): the cvar off, `edram4_clear_alias` or `edram4_stencil_lazy` off, a clear that writes
stencil, a drawn view that is not a collapsed 4x view, a drawn view not prepared or swapped away (its stencil must stay
fetchable), no known 1x alias yet (the first frames), any other decline reason of the same clear (a partial tile, a
non-constant z): the whole clear is drawn as before.

Proof that it fires, every 10 s while the cvar is on:

    [native] EDRAM mode4 clear alias raw64, 10 s: N depth clears drawn through a 4x alias were redirected although a 64bpp color view owned some of their tiles; T such tiles cleared in the 1x alias (stencil kept in the 4x view, as the drawn clear leaves it)

What to compare against `prof_eden2` (same route): N about one per frame (about 290 per 10 s at 29 fps); the
`target C5A0/f7 ... (clear of 5A0/f0 mx1my1 ...)` decline reason gone; the `D5A0/f0:440x720:mx1my1g0->D5A0/f0:880x880`
pair (27 ops per frame) gone or much smaller; `redirected depth clears` up by about one per frame; the 9-pass import
line now names `D5A0:440x720:mx1my1->5A0:880x880` only through deferred records (same count as before).

Expected saving: small in GPU time, 0.2-0.5 ms per frame (the 27 import runs are cheap per pixel, cat 10 is 0.3 ms;
the rest is the literal 4x clear pass and up to 27 breaks of the shadow render pass, each a reload of the depth
attachment), plus ring CPU (27 synchronizations and pass restarts per frame), which matters more in the CPU-bound
heavy windows. Risk: low; the image is the same by construction. Screenshot check: shadows of the characters and the
bloom edge (the quarter buffer shares the tiles).

### 5.4 Resolves straight from the host image (item 2 of the task): nothing left in this scene

`transfer causes` in both logs list only `draw/*` and `depth-resolve/color->depth` (54 single tiles per 10 s): no
resolve converts EDRAM ownership any more. The scene-colour round trip of section 2 is already gone here: the raw
resolves read the 7e3 owner (`resolve_7e3_direct`, VS40, shipped) and the restore draw renders into the 7e3 image
(`restore_into_7e3`, shipped; `380 draws ... drawn into the 7e3 image` per 10 s). The remaining colour aliases are the
f0 -> f4 velocity copy (VS43, one word copy per frame, 0.4 ms) and the DOF border tiles (`C5A0 2x -> 1x`, 0.3 ms), both
real data dependencies (post-chain.md options B and the velocity buffer). Existing exact options for the VS40 part
that have not been measured at 960 in this scene: `masseffect_native_resolve_7e3_frag` + `masseffect_native_resolve_7e3_pack = 2`
(vulkan-frame-time.md section 11, 0.3-0.8 ms there).

### 5.5 VS41 exponent-bias resolves (item 3): no exact cut without the probe

5.7 full-screen bias resolves per frame (960x544, read 4.2 MB of the 7e3 image, write 4.2 MB, about 0.58 ms each including
the barrier). The pass is a plain `texelFetch x 2^bias` into a color attachment (`me_resolve_exp_bias_frag.frag`); on
an immediate-mode GPU the load op costs nothing, so the shader cannot be made cheaper and only fewer resolves help.
vulkan-frame-time.md section 12 shows why neither a hand-off nor a second output is exact in general (blended sources,
readers that write the source, FP16 denormal rounding below 2^-11), and dirty rectangles do not help because the
publish area of an ordinary draw is its scissor, the whole screen. Post-chain plan C (host-side post targets) is a
multi-day change with fallbacks per pass and is only exact per proven hand-off. The decision data is one console
run with the existing probe (`masseffect_native_bias_life_probe = true`, `masseffect_native_bias_life_dump_s` at the
heavy window): it counts `strict`/`chain` producers (dual output, ~0.6 ms each here), `round trips` (skip, ~0.6 ms
each) and `unread` resolves. Nothing was implemented for VS41 in this round.
