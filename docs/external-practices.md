# External practices: what other Xbox 360 emulators and recompilations do, and what applies here

Date: 2026-10-07. Desk research only: web sources plus the local checkouts of Xenia Canary
(`work/xenia-canary-upstream`, commit c332733, 2026-09-27) and of the Switch Mesa tree (`work/mesa-switch-main`, commit
c3cb40a). Nothing was built or run on the console. Estimates marked "unmeasured" are guesses to rank the work, not results.

The list skips what this port already has or has already tried (see [optimization-paths.md](optimization-paths.md),
[gpu-cost-analysis.md](gpu-cost-analysis.md), [mesa.md](mesa.md), [cpu-cost-analysis.md](cpu-cost-analysis.md) and
`mass-effect-recomp/docs/optimization-backlog.md`). Section 3 lists outside sources that support work already planned.

## 1. Ranked list of new techniques

| # | Technique | Benefit | Effort | Risk to the picture |
|---|---|---|---|---|
| 1 | Xenia's ME1 "Black Shading Fix" patch: one data byte turns off MSAA and predicated tiling | **high** | very low | changes the image: no 2x MSAA edges; may also fix the black-lighting bugs |
| 2 | Post-process AA (FXAA 3.11) to go with #1 | medium (keeps edge quality) | low | changes the image (a different AA) |
| 3 | Merge the predicated tiles into one pass while keeping 2x MSAA (an EDRAM address space bigger than 10 MB) | high at 1280 | high | none if exact |
| 4 | fp16 in NAK for GM20B (HADD2/HMUL2/HFMA2). Today NVK does not expose fp16 on Maxwell | medium | high | precision; needs an image check per shader |
| 5 | Check that the EDRAM views and resolved images really are compressed, and A/B `NVK_DEBUG=no_compression` | low to medium | low | none |
| 6 | Hand-written NEON for the hot SIMDe intrinsics in the generated code (`_mm_dp_ps`, `_mm_shuffle_epi8` byte swaps) | low to medium (CPU) | medium | none if bit-exact |
| 7 | Game-level knobs from the Xenia patch files: Gaussian blur radius 0, the game's own vsync target byte | low to medium | very low | blur: changes the image; vsync: none |
| 8 | Fragment shader interlock path (Xenia "fsi" / ROV): NVK exposes it on GM20B | uncertain | very high | none in theory |
| 9 | `VK_EXT_descriptor_buffer`, also exposed on Maxwell, as an alternative to push descriptors for S19 | low | medium | none |

### 1. Xenia's "Black Shading Fix" byte: no MSAA, no predicated tiling (top priority)

- **Source.** Xenia Canary game patches, Mass Effect:
  [4D5307E8 - Mass Effect (USA Rev 1).patch.toml](https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307E8%20-%20Mass%20Effect%20(USA%20Rev%201).patch.toml)
  (xex hash `8B5DAEAF86BFDD29`; patch "Black Shading Fix", description "Disables MSAA", one `be8` write of `0x00` at
  `0x82E5DCB7`, commented `# tilingcode`) and
  [4D5307E8 - Mass Effect.patch.toml](https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307E8%20-%20Mass%20Effect.patch.toml)
  (World, Europe, Germany and Italy xex files: the same byte at `0x82E5DCD7`). The ME2 file
  ([454108CE - Mass Effect 2.patch.toml](https://github.com/xenia-canary/game-patches/blob/main/patches/454108CE%20-%20Mass%20Effect%202.patch.toml))
  describes the same fix as solving "most dynamic lighting artifacts caused by an issue with light environments and MSAA tiling".
- **What it does.** The address is outside the code range (`REX_CODE_BASE 0x82210000` + `0xB7E8A8` ends at `0x82D8E8A8`),
  so it is a data byte, the engine's tiling or MSAA mode. Set to 0, the game renders the scene without 2x MSAA. At 1280x720 1x,
  colour plus depth is 7.4 MB, which fits in 10 MB of EDRAM, so the game has no reason to tile.
- **Why it helps here.** ME1 draws the scene with 2x MSAA in two predicated tiles at a 1280 pitch, with literal 1280x720 tile
  rectangles (see ME2 backlog R207, R210 and O10; ME1 has the same tables). The costs of this layout:
  - the scene's command stream is replayed once per tile, so vertex work runs twice, and the scene is vertex-bound (M7, M8);
  - EDRAM surfaces are 2x layouts (`1280x1280` surfaces in the logs);
  - the 2x to 1x depth restores and their conversions (E36, D000 2x to 1x, the depth-prepass problem B1) exist because of MSAA.

  Without MSAA these copies and restores have no reason to exist, and every per-pixel conversion on 2x surfaces covers half
  the samples.
- **Correctness bonus (to verify).** Two open bugs look like the Xenia symptom the patch fixes: "black Shepard / lost light"
  in character creation and Anderson's speech, and the black polygonal shards on Eden Prime terrain (B1, linked in the docs
  to the 2x to 1x depth round trip).
- **Benefit.** High, unmeasured. Plausibly 10 to 25 ms per frame at 1280x720 (the second tile's vertex work plus a large part of
  `edram_alias`), less at 960x544, where the second tile is mostly clipped.
- **Effort.** Very low. Write one byte into guest memory after the XEX is loaded and before the renderer is initialised,
  behind a setting (for example `masseffect_game_msaa_off`). Steps:
  - check which xex we have: our disc is "USA, Europe (Rev 1)", and the Rev 1 file has its own hash;
  - read the code that loads the byte to confirm what it means;
  - for the Russian edition, find the same variable by its code reference, not by the address.

  If the byte is in a read-only section, unprotect it as R210 had to (`LookupHeap(addr)->Protect`).
- **Risk.** It changes the image: edges lose 2x MSAA, which #2 can make up for. Measure it in any case (memory rule: test
  every variant, the user decides).

### 2. Post-process AA as a replacement for the game's MSAA

- **Source.** Xenia presentation update,
  [xenia.jp 2022-01-29](https://xenia.jp/updates/2022/01/29/presenting-the-presentation-update-amd-fidelityfx-fsr-fxaa-cas-vrr.html):
  FXAA 3.11 (`postprocess_antialiasing = fxaa | fxaa_extreme`) in the presentation path.
- **What it does.** One full-screen pass before the HUD (it has to run on the scene image, not on the final output with the
  UI). The engine's tonemap pass is the natural place: run FXAA on its output.
- **Why it helps.** A 1x scene with FXAA is much cheaper than 2x MSAA plus two tiles, and FXAA costs one read and one write
  at 32 bpp. Unmeasured guess on GM20B at 1280x720: 1 to 2 ms.
- **Benefit / effort / risk.** Medium (image quality, not speed) / low / it changes the image.

### 3. Merge the tiles but keep 2x MSAA (exact alternative to #1)

- **Source.** The Xbox 360 predicated-tiling model
  ([Microsoft XNA docs, Predicated Tiling](https://learn.microsoft.com/en-us/previous-versions/windows/xna/bb464139(v=xnagamestudio.41)));
  UnleashedRecomp's approach of owning the render targets at the API level instead of emulating EDRAM
  ([UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), `gpu/video.cpp`: `SetFramebuffer`, `StretchRect`
  commands, MSAA resolve pipelines).
- **What it does.** The host has no 10 MB limit. Hook the game's tiling setup (ME1's version of BeginTiling, the static
  tile table, and the per-tile Resolve) so that it reports one tile covering the whole screen, and give mode 4 a virtual
  EDRAM bigger than 10 MB (for example 32 MB of tile addresses) so that the 1280x720 2x surface fits.
- **Why it helps.** It removes the second replay of the scene and the per-tile resolves, and keeps the exact 2x image.
- **Benefit / effort / risk.** High at 1280 / high: tile tables, scissors, resolve rectangles, EDRAM address arithmetic in
  all transfer shaders / low if done exactly, but a big change to mode 4. Do it only if #1 is rejected on image grounds.

### 4. fp16 on GM20B needs NAK work first

- **Sources.** Local NVK, `src/nouveau/vulkan/nvk_physical_device.c:447`: `.shaderFloat16 = info->sm >= 70`. NAK's
  `sm50.rs` has no `HADD2`, `HMUL2` or `HFMA2` encodings. The Tegra X1 has double-rate packed fp16 (1 TFLOPS fp16 against
  512 GFLOPS fp32 at 1 GHz; [AnandTech TX1](https://www.anandtech.com/show/8811/nvidia-tegra-x1-preview/2)), and
  [Chips and Cheese](https://chipsandcheese.com/p/nintendo-switchs-igpu-maxwell-nerfed-edition) found the compiler
  support limited. NAK on Maxwell: [mesa MR 30402](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30402).
- **What it means.** The plan item "RelaxedPrecision/fp16" (me1-30fps-no-cuts.md section 2.2) would do **nothing** with
  today's driver: NAK lowers everything to fp32. To get the gain:
  - add the SM53 half2 ALU encodings to NAK's SM50 backend, plus vectorisation of 16-bit NIR ALU into half2;
  - expose `shaderFloat16` for GM20B in the Switch build only;
  - then mark the post shaders (tonemap, blur chain, conversions) as mediump in the translator.
- **Benefit / effort / risk.** Medium, only for ALU-bound full-screen passes; the scene is vertex-bound and the conversions
  are not ALU-bound / high (compiler work) / precision: needs an image diff for each shader.

### 5. Framebuffer compression: check it is really on

- **Sources.** Local NVK: `nvkmd_switch_pdev.c:481` sets `has_compression = true` for Horizon. `nvk_image.c:789`
  (`nvk_image_can_compress`) allows it only for optimal-tiling, single-plane images with colour, depth/stencil or storage
  usage. `nvk_device_memory.c:198` uses `compressed_pte_kind` only for **dedicated** allocations. Chips and Cheese measured
  46 GB/s of L2 bandwidth and found the Switch GPU spills to DRAM early.
- **What to do.**
  - Log `can_compress` and the PTE kind of every EDRAM view and resolved image (the 64 bpp RGBA16F 7e3 views most of all).
    The render targets use `CreateDedicatedAllocationImage`. The texture pool deliberately does not (`nfsmw_nativo_texturas_pool.cpp:216`).
  - Run a frame A/B with `NVK_DEBUG=no_compression`.
  - If compression is off for the hot views (because of `MUTABLE_FORMAT`, aliasing or `STORAGE`), drop the flag that blocks it.
- **Benefit / effort / risk.** Low to medium: conversions run at 0.3 Gpixel/s, which suggests they are not bandwidth-bound,
  but the scene's 64 bpp traffic may be / low (logging plus one A/B) / none.

### 6. SIMDe overhead in the generated code (CPU)

- **Sources.** [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) uses [SIMDe](https://github.com/simd-everywhere/simde)
  on ARM64, and so does ReXGlue (`sdk/include/rex/ppc/intrinsics.h`). Counts in our generated code:
  - 38,544 `simde_mm_load_si128`;
  - 9,799 `simde_mm_shuffle_epi8`, mostly the vector byte swap after loads;
  - 4,355 `simde_mm_dp_ps`;
  - 1,506 `simde_mm_cvttsd_si32`.
- **What to do.** In the codegen (or a `tools/pch_*.py` pass):
  - replace `shuffle_epi8` with a constant byte-swap mask by `vrev32q_u8` (one instruction; SIMDe emits `tbl` plus a mask
    `and` and loads the constant);
  - replace `_mm_dp_ps` with a constant mask by `vmulq_f32` and `vaddvq_f32` (or a pairwise add) with the lanes fixed at compile time.
- **Why it helps.** The memory note says ME1 at 960x544 is CPU-limited in places (Normandy, CPU about 95 %). These run on
  every guest thread. This is a new item: SIMDe was never profiled in these docs.
- **Benefit / effort / risk.** Low to medium, unmeasured: first count the `tbl` and `dp` pc samples in the profile / medium /
  none if bit-exact. Fuzz with `tests/hot_fuzz`. Watch NaN and denormal behaviour against the `enableFlushMode` sites.

### 7. Game-level knobs in the same patch files

- **Source.** The same ME1 patch files:
  - "Upscaling Fix, Option C: Disable Gaussian Blur": forces the blur radius to 0;
  - "60 FPS": the byte at `0x82233BC3` is the game's vsync target, with 0 = unlimited, 1 = 60 and 2 = 30;
  - "Show Frametime": `0x82EAF10C`.
- **What to do.**
  - Blur radius 0 is an image-changing probe. It bounds the 320-class blur chain (about 1.8 ms in gpu-heavy-views.md).
  - The vsync byte is in code, so here it is a recompiled immediate. Read its function to see how the game paces itself at
    30, and make sure our present path matches that, for the fixed 30 fps pacing item.
  - The built-in frametime display can cross-check our profiler.
- **Benefit / effort / risk.** Low to medium / very low / blur: changes the image; others: none.

### 8. Fragment shader interlock path (Xenia FSI / ROV)

- **Sources.** Local NVK, `nvk_physical_device.c:260` and `:671`: `EXT_fragment_shader_interlock` and pixel/sample
  interlock exist for `MAXWELL_B <= cls < VOLTA_A`, which includes the GM20B. Xenia's Vulkan path is selected with
  `render_target_path_vulkan = any | fbo | fsi` (`vulkan_render_target_cache.cc:33`). The
  [Xenia ROV wiki](https://github.com/xenia-project/xenia/wiki/ROV) and the
  [2021 render target cache post](https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html)
  give its timings on a GTX 1070 (Halo 3 menu: host RTs 23.5 ms, ROV 52.5 ms; Sonic: 9.2 against 11.2 ms).
- **What it does.** Pixel shaders blend, test depth and stencil, and pack 7e3, float24, gamma and so on by hand, straight
  into an EDRAM storage buffer. There are no format views and no transfers at all.
- **Why it might help.** `edram_alias` is 26 to 46 ms of our frame. The catch: the depth test runs in the shader, so there
  is no early-Z and no ZCULL. On desktop Xenia this path is the slower one. The only realistic shape is a hybrid: host render
  targets for the depth-tested scene, FSI only for the 7e3/UNORM10/RGBA8 post chain where the conversions are.
- **Benefit / effort / risk.** Uncertain / very high (a second render back end) / none in theory. Keep it as a last resort
  after G7, G6 and #1.

### 9. `VK_EXT_descriptor_buffer` on Maxwell

- **Source.** Local NVK, `nvk_physical_device.c:246`: `EXT_descriptor_buffer = cls_eng3d >= MAXWELL_A`.
- **What it does.** Descriptors are written straight into a buffer and set binds become offsets. This is an alternative
  to the open S19 (push descriptors, fewer non-push binds), which matters for the per-draw front end on pre-Turing GPUs.
- **Benefit / effort / risk.** Low (the frame is GPU-bound in the heavy scenes) / medium / none.

## 2. Things checked that do not apply, or are already done

- **8888 and 8888_GAMMA in one storage; 2_10_10_10 and its `_AS_10_10_10_10` in one storage** (Xenia
  `GetColorResourceFormat`, `render_target_cache.h:728`): already done (`nfsmw_nativo_destinos.cpp:938-940`).
- **7e3 stored as RGBA16F on host RTs**: Xenia does the same (`k_2_10_10_10_FLOAT_AS_16_16_16_16`). Its blog accepts the
  small precision difference.
- **Async compute to hide transfers**: Maxwell does not run graphics and compute at the same time, and the docs already
  measured engine switches as wait-for-idle. Keep transfers as fragment passes (G3).
- **Xenia depth cvars** (`depth_float24_convert_in_pixel_shader`, `depth_transfer_not_equal_test`): the first must stay off
  (our `float24_ps_mode` lesson); the second is backlog E23.
- **Xenia's 256 KB invalidation granularity, readback options** (`readback_resolve`, `readback_memexport`): not relevant,
  because the native renderer does not watch guest memory.
- **Anisotropy patch** (Xenia ME1 "16x AF"): the opposite direction. Our default is already 1.
- **Skip-intro-movies patch** (`0x8223B3F8`): already done through `Coalesced.ini`.
- **XenonRecomp register-as-local options** (`skip_lr`, `ctr/xer/cr/reserved/non_argument/non_volatile_as_local`, `skip_msr`):
  done or measured here (C47, C48, diet).
- **XenonRecomp FPU/VMX denormal switching**: the same scheme in ReXGlue. The FPCR analysis is in cpu-cost-analysis.md
  section 5.

## 3. External support for work already planned

| Planned item | Outside evidence |
|---|---|
| G7: resolve straight from the 7e3 owner, without moving ownership | Xenia's `VulkanRenderTargetCache::Resolve` (`vulkan_render_target_cache.cc:1088`) never changes ownership on a resolve. It dumps the owners' raw words for the resolve rectangles only (`DumpRenderTargets`, `GetResolveCopyRectanglesToDump`), and the copy shader decodes the format the resolve asks for and applies the exponent bias in the same pass. That covers G7 and gpu-cost #4 (fold the bias). |
| E27, G3: transfers as quads, one pass per destination | 2021 Xenia post: going from CopyTextureRegion plus compute to per-format-pair pixel shaders, copying only exact ownership ranges, made host RTs 2.1x to 3.4x faster. |
| E32c: shipped pipeline list compiled during the logos | UnleashedRecomp embeds `pipeline_state_cache.h` and runs `PrecompilePipelines` tasks during the intro logos without blocking. It also compiles a level's pipelines as part of asset loading, so gameplay never stutters. |
| C10, C20: API-level hooks, a separate recording thread | UnleashedRecomp replaces the game's D3D calls with a render queue (`moodycamel::BlockingConcurrentQueue g_renderQueue`) consumed by a render thread, plus a separate copy queue. |
| FSR1, dynamic resolution, 30 fps pacing | Xenia does FSR (EASU+RCAS) or CAS at presentation (`postprocess_scaling_and_sharpening`); UnleashedRecomp renders to an intermediary back buffer when the viewport size differs from the swap chain. For ME1, scale before the HUD (the R2 design), or the UI gets blurred too. |

## 4. Suggested order

1. #1 on the console: one run at 960x544 and one at 1280x720, Anderson and Eden Prime, captures of Shepard's lighting and the
   terrain. Compare `GPU per Swap` categories, scene draw counts per Swap, and the `top transfer pairs`.
2. If #1 is kept, add #2 and measure its cost. If #1 is rejected on image grounds, weigh #3 against G7 and G6.
3. #5 (logging plus one A/B) and the #7 probes: cheap.
4. #6 after a profile shows SIMDe samples on the main or render thread.
5. #4 and #8 only if 1280x720 at 30 fps is still out of reach after the above.

## Sources

- Xenia, "Leaving No Pixel Behind" (render target cache, 2021): https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html
- Xenia, presentation update (FSR, CAS, FXAA, 2022): https://xenia.jp/updates/2022/01/29/presenting-the-presentation-update-amd-fidelityfx-fsr-fxaa-cas-vrr.html
- Xenia ROV wiki: https://github.com/xenia-project/xenia/wiki/ROV
- Xenia Canary game patches: https://github.com/xenia-canary/game-patches (ME1 files `4D5307E8 - Mass Effect*.patch.toml`, ME2 `454108CE - Mass Effect 2.patch.toml`)
- Xenia Canary source (local checkout c332733): `src/xenia/gpu/render_target_cache.{h,cc}`, `src/xenia/gpu/vulkan/vulkan_render_target_cache.cc`, `src/xenia/gpu/pm4_command_processor_implement.h:409` (bin predication)
- UnleashedRecomp: https://github.com/hedge-dev/UnleashedRecomp (renderer `UnleashedRecomp/gpu/video.cpp`)
- XenosRecomp: https://github.com/hedge-dev/XenosRecomp
- XenonRecomp: https://github.com/hedge-dev/XenonRecomp
- ReXGlue SDK: https://github.com/rexglue/rexglue-sdk
- SIMDe: https://github.com/simd-everywhere/simde
- Microsoft, Predicated Tiling (XNA): https://learn.microsoft.com/en-us/previous-versions/windows/xna/bb464139(v=xnagamestudio.41)
- Chips and Cheese, Switch iGPU: https://chipsandcheese.com/p/nintendo-switchs-igpu-maxwell-nerfed-edition
- AnandTech, Tegra X1 architecture: https://www.anandtech.com/show/8811/nvidia-tegra-x1-preview/2
- Mesa MR 30402, NAK on Maxwell: https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/30402
- Mesa Switch tree (local c3cb40a): `src/nouveau/vulkan/nvk_physical_device.c`, `nvk_image.c`, `nvk_device_memory.c`, `nvkmd/switch/nvkmd_switch_pdev.c`, `compiler/nak/sm50.rs`
