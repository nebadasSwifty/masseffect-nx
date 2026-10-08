# Vulkan frame time outside the draws (NVK, GM20B)


> **Correction (main session, 2026-10-07):** the 7.8 ms "gap" is not CPU-bound idle. The game caps itself at 30 fps
> (Swaps never exceed ~299 per 10 s in any run, the host presents IMMEDIATE), so with a ~27 ms GPU frame the GPU idles
> until the next 33.3 ms slot. The cockpit average below 30 (28.8 fps) comes from single frames that miss the slot. The
> GPU savings below therefore matter for the frames that miss, for heavy scenes (Anderson, Citadel) and for 1280.
Date: 2026-10-07. Offline analysis only: console logs, the renderer source and the local Mesa tree
(`~/mesa-switch`, Mesa 26.2.2, the base of `mesa/mesa-switch-masseffect.patch`). Nothing was built or run on the
console. The GPU-time numbers come from these logs:

- `run/me1/hot3p`: 960x544, Normandy cockpit, normal profile. This is the main source.
- `run/me1/perdraw` and `run/me1/c_s960`: 960x544, the same route, used as cross-checks.
- `run/me1/prof_s1280`: 1280x720. This window is not the same scene content: its scene and post cost less than at 960.

Related documents: [gpu-cost-analysis.md](gpu-cost-analysis.md) (the earlier conversion analysis; several of its
proposals have since shipped), [scene-shader-cost.md](scene-shader-cost.md), [external-practices.md](external-practices.md),
and the backlog `mass-effect-recomp/docs/optimization-backlog.md`.

## 0. Summary

At 960x544 in the cockpit, a GPU frame is 34.7 ms of wall time per Swap. Of that, 7.8 ms is **gap**: the GPU sits idle
between submissions, waiting for the CPU. The GPU is busy for about **26.9 ms**. About 8.6 ms of the busy time is not
draws:

| category | ms/frame | what it is |
|---|---|---|
| edram_alias | 5.8 | about 2.6 for exponent-bias resolves, 1.0 for 7e3 → UNORM10 resolves, about 1.5 for f2 → f3 conversions, about 0.7 for the rest |
| copies | 1.5 | 17 resolve copies or clears per frame (vk_meta draws in NVK 26.2), plus resolved-image pool clears |
| edram_import / export | 0.6 / 0.3 | about 42 depth imports and 10 exports per frame, mostly the shadow and light slot at `5A0` |
| other | 0.4 | mostly the k_8_8_8_8 → k_16_16 word copy over 408 tiles |

Barriers and render-pass breaks are not a separate line item. Every one of them is a wait-for-idle on NVK. There are
roughly 150 to 250 such sync points per frame, but four earlier experiments that cut them measured no gain (section 3).

**What to do first** (all exact, ranked in section 5):

1. Stop converting f2 → f3 on the scene region.
2. At 1280 only: stop the partial-tile shadow-slot transfers (backlog E35). They are worth about 2.5 to 3 ms there.
3. Remove the zero clear when a resolved image is woken from the pool, or keep one image per format.
4. Run the 7e3 → UNORM10 resolve as a fragment pass.
5. Merge f0 and f4 into one mutable-format image.

Together these are worth about 2.5 to 4 ms at 960 and about 6 to 9 ms at 1280.

## 1. How to read the numbers

- `GPU per Swap` is already in real ms: raw timestamps × 1.627. `GPU time by pass` prints **raw** ms per 10 s, so the
  real cost per frame is raw × 1.627 / Swaps.
- A label belongs to the **first** operation of a category run. `MarkGpu` returns early while the category stays the
  same (`masseffect_native_targets.cpp` around line 2973), and `LabelMarkGpu` only fills an empty label. So
  "VS41 2.59 ms" means: the run that starts with an exponent-bias resolve, **including any alias work that follows it
  before another category begins**.
  - At 1280, VS41 is 2.54 ms for 5.5 resolves per frame. At 960 it is 2.59 ms for 4.5. The per-resolve cost does not
    grow with pixels, which suggests the 960 run carries trailing conversions.
  - To get exact attribution, run with `masseffect_diag_dump_marks_s = 60`. It logs the mark sequence of 6 work slots.
- Labels in category 12: VS40 is the 7e3 direct resolve (compute). VS41 is the bias resolve (fragment). VS42 is the bias
  resolve (compute). VS43 is the 32-bit word copy. VS16+n is a fragment colour conversion of class n, where 1 = to RGBA8,
  2 = from RGBA8, 3 = 16F → 16F, and the PS field is the tile count. VS(pipeline+1) is a compute conversion.
- `gap` is the time between the last mark of one submission and the first mark of the next (`CompleteA`, around line
  8300). There is about one submission per frame (`submissions since last report: 300`), so the gap is GPU idle time.
  That idle comes from the CPU (ring and game threads) plus the untimed output pass. **At 960 in this window, the frame
  rate (28.8) is set by the CPU, not the GPU.** Cutting GPU time here buys headroom for the heavy scenes (Anderson and
  Citadel, where alias was 17 to 26 ms, see gpu-cost-analysis.md) and for 1280.

## 2. Non-draw work per frame

### 2.1 At 960x544 (`hot3p`, 288 Swaps per 10 s; perdraw and c_s960 agree within about 0.5 ms)

| kind | ops/frame | ms/frame | guest pattern that causes it |
|---|---|---|---|
| **Exponent-bias resolve** (`ResolverWithBiasFrag`, label VS41) | 4.5, full screen | **~2.6** (label run) | The HDR chain resolves the 7e3 scene (`2D0`) into `15204000` (k_16_16_16_16_FLOAT, host RGBA16F) with bias −3 roughly every 30 draws (`30.5 draws per copy`). Each pass reads and writes 960x544 × 8 B, 8.4 MB in total, so about 0.55 ms at ~15 GB/s: it is bandwidth bound. VS9460/PS3474 (the full-screen opaque quad, 2.6 ms of draw time in category 3) appears to be the matching write-back. |
| **7e3 → UNORM10 resolve** (`ResolverDirect7e3`, compute, label VS40) | 1.3, full screen | **~1.0** | The same `15204000` resolved as k_2_10_10_10 (host A2B10G10R10). Thanks to `resolve_7e3_direct`, the tiles stay in f3. It is a storage-image compute shader with an ALL_COMMANDS barrier on each side. |
| **f2 → f3 colour conversions** (fragment, VS19) | ~1.5–1.9 full-screen equivalents (575 tiles/frame) in up to 18 runs | **~1.5** (0.98 for whole-target runs + 0.53 for small runs) | The light passes (VS2013/PS14702, VS2013/PS14489, VS15573/PS16235, VS11874/PS18583, additive ONE/ONE) draw into the 7e3 view (f3) over tiles that the UNORM10 view (f2) owns. **Only 40 tiles per frame go f3 → f2 through resolves**, so f2 must gain the remaining ~535 tiles some other way, most likely through proven-overwrite draws or clears in the f2 view (`proven-overwrite binds` 10.5/frame). Not yet confirmed by a tile trace (section 6). |
| other colour alias work | ~10 small runs | ~0.7 | C5A0 attenuation slot: 4x → 1x (8/frame, 13 tiles), D5A0 → C5A0 depth → colour (10/frame) |
| **Resolve copies** (`CopyInternal` → vkCmdCopyImage, category 6) | 17 | **1.5** | `14D2D000` 960x544 ×3.1 (post ping-pong, 1 draw per copy), `14F2B000` 864x864 ×2.8, `1F376000` 960x544 ×1, `1F574000` 256x138 ×3 (exposure), front buffer `1F5F8000` 1280x720 ×1 ("NOBODY READS IT": only the Swap reads it). Each copy has `BarrierBeforeCopyResolve` + `BarrierAfterCopyResolve`. |
| **Resolved-pool wake clears** (`WakeResolved`, inside category 6) | ~3.7 | part of the 1.5 | `15204000` switches format (32 ↔ 7) twice per frame. Each switch retires the old image and wakes another (pool `reuse` grows about 1,100 per 10 s), and **every wake clears the whole image to zero** with two ALL_COMMANDS barriers, even though the resolve that follows overwrites all 960x544 texels. |
| 32-bit word copy f0 → f4 (`conversion_copy_32`, label VS43, category 0) | 1 × 408 tiles | 0.44 | A k_8_8_8_8 target at `2D0` is reused as k_16_16 by the biggest scene pass (VS12867/PS26880). This is a pure bit reinterpretation of the same words. |
| Depth import (category 10) | ~42 (11 runs) | 0.6 | `D5A0` shadow and light slot: 4x (440x720 mx1my1) → 1x 880x880, 27 ops of 5 tiles; lazy-stencil depth-only imports |
| Depth export (category 11) | ~10 | 0.3 | D5A0 → C5A0/f7 (attenuation) |
| **Total non-draw** | | **~8.6** | plus the begin/end of render passes and timestamps hidden inside the draw categories (about 1 ms: category 640 is 16.8 ms, while the per-draw sum is 15–16 ms) |

### 2.2 At 1280x720 (`prof_s1280`, 290 Swaps)

| kind | ops/frame | ms/frame | note |
|---|---|---|---|
| bias resolve (VS41) | 5.5 | 2.5 | `14B48000` 1280x720, 40 draws per copy |
| f2 → f3 (VS19 PS720 and small runs) | 22 runs, ~1,330 tiles (1.8 screens) | ~1.4–1.9 | same light passes |
| 7e3 direct resolve (VS40) | 1.7 | 1.2 | |
| **import9: copy-engine stencil imports** | 16 (780 tiles) | **2.5** | Exists only at 1280. With pitch 1280 the scene region `2D0..5A0` plus the `C400/f3 1280x1280 my1` view reach the shadow and light slot at `5A0`. The shadow-slot clear rects (VS9828/PS10237, rect list, e.g. `400,0-432,432` and `0,176-320,182`) are not tile-aligned (a tile is 80 px wide), so they pull 1-tile colour → depth imports: 27 per frame `C400/f3 → D5A0`, plus 14 per frame `C400/f3 → C5A0/f7`. This is backlog **E35** (still todo). |
| copies | 18 | 1.4 | front buffer 1280x720 ×1, `144D7000` ×3.2, `15278000` ×2 (0 draws per copy) |
| import / export / other / clears | | 0.2 / 0.3 / 0.6 / 0.1 | |
| **Total non-draw** | | **~10–11** (6.7 alias + 2.5 + 1.4 + ...) | Earlier runs reported alias at 10.2 ms at 1280. Without matched content, use the ratio: alias grows ×1.75 with pixels, imports do not. |

## 3. Render-pass breaks, barriers and what NVK does with them

### 3.1 Count per frame (960, from the interval counters)

- There are **about 78 category switches** per frame (22,566 intervals / 288). Each one is at least one render-pass
  end/begin and one BOTTOM_OF_PIPE timestamp, which is a report semaphore that is released after all previous writes.
- Explicit `vkCmdPipelineBarrier` calls:
  - 2 per resolve copy (34);
  - 2 per compute resolve (2.6);
  - 2 per pool wake (7);
  - 1 pair per compute-conversion batch;
  - stencil copy-engine imports have their own pair, at 1280 only (32).
- Render-pass external dependencies: every draw pass (`CreatePass`, draws.cpp around line 9778), every fragment
  conversion (32 per frame), bias resolve (4.5) and depth import pass. dependency[1] is `ALL_COMMANDS` with
  `MEMORY_READ|MEMORY_WRITE` on the destination side.
- **Total: about 150 to 250 wait-for-idle-class sync points per frame.**

### 3.2 What NVK 26.2 emits (local source `src/nouveau/vulkan/nvk_cmd_buffer.c`)

- **Wait-for-idle on every barrier.**
  - `nvk_barrier_flushes_waits` (around line 780) sets `NVK_BARRIER_WFI` whenever the source stage lies inside the
    expanded ALL_COMMANDS set. That is every real stage.
  - `nvk_cmd_flush_wait_dep` then emits one `WAIT_FOR_IDLE` on the last subchannel (around line 936). It is one per call,
    however many barriers the call contains.
  - Execution-only barriers also wait.
- **L1 flush** (`INVALIDATE_SHADER_CACHES {data, flush_data}`, which is also a wait-for-idle) when the source access
  contains `SHADER_STORAGE_WRITE`, or `TRANSFER_WRITE` together with the COPY stage.
  - `BarrierBeforeCopyResolve` puts `TRANSFER_WRITE` with the TRANSFER stage in the source, so every resolve copy pays an
    L1 flush.
  - So do `ResolverWithBias` and `ResolverDirect7e3`, through `MEMORY_WRITE` with ALL_COMMANDS.
- **Invalidations without a wait** on the destination side (around line 1085):
  - `SHADER_SAMPLED_READ` → `INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI` (Maxwell and later);
  - storage or uniform → `INVALIDATE_SHADER_CACHES_NO_WFI`;
  - `TRANSFER_READ` with COPY → both.
  - `BarrierAfterCopyResolve` asks for all of them.
- **Copies are not on the copy engine any more.**
  - In 26.2, `nvk_CmdCopyImage2` (`nvk_cmd_meta.c` around line 494) runs colour copies as **vk_meta graphics draws**:
    begin rendering, save and restore state, then a draw.
  - Depth/stencil copies go to the copy engine (`nvk_cmd_copy_image_ce`).
  - `vkCmdClearColorImage` is a render pass with `CLEAR_SURFACE`.
  - So each resolve copy is a render pass plus two barriers. After `nvk_meta_end_gfx`, the next game draw re-emits the
    state, which is also a little CPU time.
- **Layouts are free.** NVK ignores image layouts apart from `UNDEFINED → *`, which initialises ZCULL. `GENERAL` everywhere
  costs nothing.
- **Compression.** Upstream nouveau enables compression only for Turing and later. **The Switch backend sets
  `has_compression = true`** (`nvkmd/switch/nvkmd_switch_pdev.c:483`). `nvk_image_can_compress` allows it for optimal
  colour, depth and storage images, and dedicated allocations get `compressed_pte_kind` (`nvk_device_memory.c` around
  line 197). Whether Horizon really backs these images with comptags has never been measured (external-practices.md
  item 5). NVK has no fast clear and no compression-aware resolve on Maxwell in any case.
- **Load and store ops.**
  - For tiled colour images, LOAD and DONT_CARE emit nothing (a non-tiler).
  - `storeOp DONT_CARE/NONE` saves nothing: the ROP writes through to L2 anyway.
  - Only ZCULL depth uses load/store (`LOAD_ZCULL`/`STORE_ZCULL`).
  - draws.cpp line 461 already measured that a depth LOAD costs nothing.

### 3.3 Why barrier work has not paid off so far

The backlog has four experiments against barriers: E14 (dropping duplicate barriers), E16 (narrow pass dependencies),
E17 (one barrier pair per sync batch) and E28 (Mesa fragment barrier instead of a wait-for-idle). All four measured
**no change**.

The conversions run at about 0.3 Gpixel/s and the resolves at about 15 GB/s. The time is in the bytes, not in the
drains. A wait-for-idle after a full-screen pass costs a 2-SM tail of perhaps 5 to 30 µs (inferred; no published
Maxwell figure was found). With about 200 of them, the upper bound is 1 to 4 ms, but the earlier experiments suggest the
real number is at the low end. Measure the upper bound once (candidate 8) before spending more effort here.

## 4. External findings

- **NVK barrier mechanics.** Wait-for-idle per call, an L1 flush only for storage and transfer writes, NO_WFI texture
  invalidates. See [nvk_cmd_buffer.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/nouveau/vulkan/nvk_cmd_buffer.c).
  The local tree matches main for these functions.
- **Copies moved from the copy engine to meta shaders in Mesa 26.2.**
  - [MR 42262](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/42262),
    [MR 42564](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/42564),
    [26.2.0 release notes](https://docs.mesa3d.org/relnotes/26.2.0.html).
  - The reason is in M. Henning's ["throw away your copy engine"](https://blog.darkrefraction.com/2026/throw-away-your-copy-engine.html):
    the copy engine cannot saturate bandwidth.
  - Locally, `NVK_COPY_ENGINE=1` was already measured slower (mesa.md).
- **ZCULL** landed in 26.1 ([MR 33861](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/33861)). Any STORAGE or
  TRANSFER_DST usage disables it. The scene is vertex bound, so it did not help here (S24).
- **Feature support.** NVK exposes `VK_EXT_attachment_feedback_loop_layout` and `VK_KHR_dynamic_rendering_local_read`. It
  does **not** expose `attachment_feedback_loop_dynamic_state`, `rasterization_order_attachment_access`,
  `fragment_shader_interlock` or `shader_stencil_export`
  ([features.txt](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/features.txt)).
  - Input attachments are lowered to texture fetches, so a feedback loop is a texture read of the bound target. It is
    coherent only after a wait-for-idle plus a texture-cache invalidate. There is no hardware framebuffer fetch.
  - So "read the scene colour in place instead of resolving" is not cheaper on this GPU, unless each draw reads only its
    own pixel **and** a barrier between draws is acceptable.
- **Memory aliasing of differently formatted optimal images** is undefined by the spec
  ([Memory Aliasing](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-memory-aliasing)).
  - In NVK every uncompressed 8–128 bpp colour format uses the same PTE kind. Depth formats use Z24S8/S8Z24/ZF32 kinds
    ([nil/image.rs](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/nouveau/nil/image.rs)).
  - So colour ↔ colour aliasing at the same bpp happens to work, but it is outside the spec. 32 bpp ↔ 64 bpp aliasing is
    a different GOB mapping, so it is not a reinterpretation. Colour ↔ depth aliasing is not safe.
  - This matches the local history: mode 2 ("same memory, other format") gave coloured bands.
  - **The legal way is `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`** with views of the same compatibility class: RGBA8 /
    R16G16 / A2B10G10R10 / R32_UINT are all in the 32-bit class. It costs nothing on Maxwell; Xenia uses it for its
    integer transfer views.
- **Xenia** ([render_target_cache.cc](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/render_target_cache.cc),
  [vulkan_render_target_cache.cc](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc)):
  - one host image per (base, pitch, MSAA, format), with an ownership map of 80×16 tiles;
  - transfers as fragment draws through integer bit-cast views;
  - `resolve_clear_cutout`, so a cleared rectangle is not transferred;
  - the transfer height comes from viewport and scissor, or from CPU execution of the vertex shader;
  - transfers sorted by shader key, with the source barriers gathered before the pass.
  - Mode 4 already does all of this except the cutout for partial tiles (E35).
- **UnleashedRecomp** ([video.cpp](https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecomp/gpu/video.cpp)):
  no EDRAM at all.
  - A resolve is deferred: the texture slot points at the source surface, and the copy runs only when the surface is
    re-bound for writing.
  - All barriers are flushed in one batch.
  - Depth resolves are skipped because the depth is transient.
  - The local equivalent is `masseffect_native_resolver_no_copy`, which is off. It was turned off because of a MoltenVK
    bindless problem, not an NVK one.
- **Mesa 25.1–26.2 for Maxwell.**
  - 25.2: "Maxwell+ conformant", root descriptors no longer doubled.
  - 26.1: ZCULL, early return from empty draws.
  - 26.2: meta copies, ZCULL fixes.
  - Still missing for Maxwell: fast clear, fragment shader interlock (FSI), stencil export.
  - Release notes: [25.1](https://docs.mesa3d.org/relnotes/25.1.0.html),
    [25.2](https://docs.mesa3d.org/relnotes/25.2.0.html), [26.1](https://docs.mesa3d.org/relnotes/26.1.0.html).
- **Batching.** One `vkCmdPipelineBarrier` with N barriers costs one wait-for-idle on NVK; N calls cost N. Use exact
  source masks, because a colour-attachment source costs less than a storage source
  ([NVIDIA barrier guidance](https://developer.nvidia.com/blog/advanced-api-performance-barriers),
  [Khronos pipeline_barriers sample](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html)).

## 5. Candidates, ranked (image identical unless stated)

Estimates are GPU ms per frame. "960" is the cockpit window above; "1280" is prof_s1280. The ranges are wide because
the label runs mix operations (section 1).

| # | change | 960 | 1280 | image risk | effort | where |
|---|---|---|---|---|---|---|
| 1 | **Stop f2 → f3 conversions on `2D0`.** First find the producer that hands about 535 tiles per frame to the UNORM10 view (tile trace, section 6). Then render that producer straight into the f3 RGBA16F image with an output epilogue: UNORM10 value → 32-bit word → 7e3 decode. That is exactly what the conversion computes afterwards, but done once, with no extra pass. It is valid only for opaque, full-mask producers (ONE/ZERO, mask F) or for clears (decode the clear word as 7e3, like E34). | **1.0–1.5** | **1.4–2.5** | Low when the guard is enforced (opaque, full mask, no blend); otherwise keep the conversion | M | `PrepareDrawEDRAM4` (~4930) and `PublishEDRAM4` (~3837) to retarget ownership; epilogue specialisation in pipeline creation (`masseffect_native_draws.cpp`); clear path in `CopyInternal` (~1700) |
| 2 | **E35: partial-tile shadow-slot clear rects** (`400,0-432,432`, `0,176-320,182`). Clear the covered part inside the current owner. For a colour owner, decode the depth+stencil word as the owner's format, as E34 does. Do not import 1-tile colour → depth with copy-engine stencil. Removes `C400 → D5A0` (27/frame) and `C400 → C5A0` (14/frame). | 0.1–0.3 | **2.5–3.0** (import9 2.5 + part of alias) | Low: the same word lands in the same pixels | M | `RedirectClearDepthEDRAM4` (~4503), `PartialEDRAM4` (~4449); declines show up as `partial tile: owner unsupported` |
| 3 | **No zero clear when a woken resolved image is fully overwritten.** Pass "full cover" (dx = dy = 0 and the extent equals the image) from `CopyInternal` to `GetResolved`/`WakeResolved` and skip the `vkCmdClearColorImage` and its two barriers. Better still, keep one live image per (base, host format) at `15204000`, so the 32 ↔ 7 alternation stops sleeping and waking images. That also removes `ForgetImage`/descriptor churn on the CPU. | 0.2–0.6 | 0.3–1.0 | None with the full-cover guard (the clear's texels are all rewritten) | S | `WakeResolved` (~7434–7485), `GetResolved` (~7495–7560), call site in `CopyInternal` (~1530) |
| 4 | **7e3 → UNORM10 resolve as a fragment pass.** Write the 32-bit word through an `R32_UINT` view of the A2B10G10R10 texture (MUTABLE_FORMAT, same 32-bit class). This is bit-exact by construction. Same render-pass pattern as `ResolverWithBiasFrag`. Storage-image stores (SUST) are slow on Maxwell, and the bias resolve gained about 1 ms from the same switch. | 0.3–0.5 | 0.4–0.6 | None (raw word); check once with a one-frame compare against the compute path | S | `ResolverDirect7e3` (~5487); port `me_resolve_7e3_to_unorm10.comp` to a `.frag` next to `shaders/me_resolve_exp_bias_frag` |
| 5 | **f0 and f4 as one mutable-format image.** RGBA8_UNORM and R16G16_UNORM views of one VkImage replace the 408-tile `conversion_copy_32` copy. It is the same bytes as `vkCmdCopyImage`, now with no copy at all. The ownership map treats f0 and f4 as one storage class (same bits). | 0.4 | 0.8 | None (spec-defined reinterpretation within a compatibility class) | M | `ClassEdramFORMAT` (line 468), `GetTarget` (~3332), copy-32 branch of `ConvertAliasEDRAM` (~7019) |
| 6 | **Deferred resolves (UnleashedRecomp style) and the front buffer.** Re-measure `masseffect_native_lazy_front = true` (the 1280x720 front copy that nobody reads, 0.3–0.4 ms) and `resolver_no_copy` on NVK for the post ping-pong `14D2D000` (3.1/frame, 1 draw per copy). Both were judged "not worth it" when the GPU was at 100+ ms, so the result was lost in the noise. | 0.4–0.8 | 0.7–1.3 | Medium: descriptor lifetime. lazy_front checks itself; no_copy needs per-slot descriptor snapshots | S to measure, M to fix | cvars at targets.cpp lines 94 and 242; `PostponeCopyFront`, `SwapWithResolved` (~7582) |
| 7 | **Bias resolve: fewer bytes.** (a) Count the union of draw rectangles between two bias resolves into `15204000`. If the lights are local, resolve only that rectangle; this needs candidate 3b, so the destination keeps its previous content. (b) When a 7e3 resolve and a bias resolve read the same source back to back, run one pass with two outputs (saves one 4–7 MB read). Measure first: the light passes use a full-screen scissor, so (a) only helps if the extent estimator bounds them. | 0–1.5 | 0–2.0 | None if the rectangle is a proven superset | M | `CopyInternal` exp-bias branch (~1620), `ResolverWithBiasFrag` (~5542) |
| 8 | **Barrier narrowing, with a bound measured first.** Add a probe cvar that drops the explicit barriers and pass dependencies of copies, resolves and transfers (the image may be wrong; memory note: measure image-changing variants too). If it gains less than 0.5 ms, close the topic. Otherwise: (a) a `BarrierBeforeCopyResolve` source without `TRANSFER_WRITE` when no transfer ran since the last barrier, which gives a wait-for-idle with no L1 flush; (b) `BarrierAfterCopyResolve` destination `SHADER_SAMPLED_READ` only; (c) one barrier pair for a run of back-to-back resolves; (d) `ResolverWithBias`/`Direct7e3` sources as `COLOR_ATTACHMENT_WRITE` instead of `MEMORY_WRITE`. | 0.1–1.0 | 0.1–1.0 | None for (a) to (d) | S | targets.cpp ~7730–7762, ~5515, ~5651; draws.cpp `CreatePass` ~9778 |
| 9 | **Compression A/B**: one run with `NVK_DEBUG=no_compression`, plus a log of `can_compress` and the PTE kind of the EDRAM and resolved images. Compression is on in the Switch backend but has never been verified. | ? (−1 to +2) | ? | None | XS | env only; external-practices.md item 5 |
| 10 | `masseffect_gpu_marks_categories = false` (backlog G4): about 78 report semaphores per frame fewer. M4 found TOP_OF_PIPE vs BOTTOM_OF_PIPE made no difference. | 0–0.3 | 0–0.3 | None | XS | toml |

Rejected for this frame, with the reason:

- **Aliasing images of different formats on one VkDeviceMemory** (as opposed to MUTABLE_FORMAT views): undefined by the
  spec, 32 ↔ 64 bpp does not reinterpret, colour ↔ depth uses different PTE kinds, and mode 2 already showed bands.
- **Attachment feedback loop or framebuffer fetch instead of resolves**: emulated as a texture read plus wait-for-idle on
  NVK. There is no rasterization-order access or FSI.
- **store/load op changes**: they emit nothing on Maxwell for colour.
- **The copy engine for copies**: measured slower, and upstream abandoned it.
- **Dynamic rendering by itself (E19)**: a pass break costs only state dwords; the barriers are the cost, see 8.
- **f2 as A2B10G10R10 instead of RGBA16F**: makes f2 ↔ f0 cheaper but not f2 ↔ f3, which is the pair that exists here
  (7e3 has no blendable Vulkan format).

### Expected result

- **960x544 cockpit:** candidates 1 and 3 to 6 save about 2.5 to 3.8 ms. GPU busy time goes from 26.9 to about 23–24 ms.
  The frame stays CPU-limited (gap 7.8 ms), so the fps change in this window will be small. The value is headroom for the
  Anderson and Citadel scenes, where the same conversion and resolve chain is 2–4 times larger.
- **1280x720:** candidates 1 to 6 save about 6 to 9 ms. Busy time goes from about 28.9 to about 20–23 ms in this window.
  The 39 ms heavy-scene figure would drop to roughly 30–33 ms, still with scene and post as the main cost.

## 6. Measurements to take first (one variable per run)

1. `masseffect_diag_dump_marks_s = 60` at 960 and at 1280. The exact per-operation GPU ms replaces the label-run
   estimates (bias resolve vs trailing conversions).
2. `masseffect_diag_trace_tile = 930` at 960. That is the middle of the `2D0` scene region: base 720, row 17 × 12 tiles,
   column 6. It shows which operation hands the tile to f2 before each f2 → f3 transfer, and so decides how candidate 1
   is implemented.
3. A counter for the union of extent-estimator rectangles between consecutive bias resolves of `15204000`. This decides
   candidate 7a.
4. A/B runs that are cvar or env only: `masseffect_native_lazy_front = true`, `NVK_DEBUG=no_compression` and
   `masseffect_gpu_marks_categories = false`. Run each cold, 60 s, on the cockpit route, and record each in the backlog.

## 7. Tile trace result (main session, 2026-10-07, `masseffect_diag_trace_tile = "930"`, cockpit, RU 960x544)

One frame of tile 930 (middle of the `2D0` scene region), in order:

1. f4 (k_16_16, velocity) -> f0 import at the first scene draw (VS12867/PS26880), then the opaque scene in f0.
2. For each shadowed light (two per frame here):
   - `VS22095/PS21267` (shadow projection) binds `2D0` as **f2** (k_2_10_10_10, host fmt 97);
   - `VS6906/PS21415`, a full-screen draw in the f2 view, overwrites the tiles: publish **f0 -> f2** (no conversion:
     proven overwrite). This is the scene-colour restore of UE3's Xenon path: the resolved 7e3 scene is drawn back
     into EDRAM as raw 10:10:10:2 bits through a UNORM10 target;
   - the light pass (`VS2013/PS14702` or `PS14489`, additive) binds the **f3** view (k_2_10_10_10_FLOAT): import
     **f2 -> f3** (the conversion that costs ~1.5 ms per frame), publish f2 -> f3.
3. `VS28697/PS7992`, then `VS2293/PS15729` (uber blend) in f0: publish f3 -> f0.

So the producer the doc asked for is `PS21415`, a full-screen raw-bits restore. Candidate 1 becomes: when a
full-screen proven-overwrite draw in the f2 view is followed by an f3 consumer of the same tiles, render it straight
into the f3 (RGBA16F) image with an output epilogue that rounds each UNORM10 output to its 10-bit code and decodes
the 7e3 value (alpha 2 bits likewise), so the f2 -> f3 import disappears. Must be bit-exact with the existing
conversion shader. Not started: the vertex-shader specialization agent is editing the same pipeline-key code.

## 8. Restore into 7e3 (candidate 1, implemented, `masseffect_native_restore_into_7e3`, default off)

Not measured on the Switch yet. Expected: about 1.0–1.5 ms per frame at 960x544 in the cockpit (the two f2 -> f3
imports per frame of section 7 disappear), more at 1280x720.

### What it does

The scene restore `VS6906/PS21415` (section 7) normally draws into the f2 (k_2_10_10_10, UNORM10) host image, and the
light pass then pulls the same tiles into the f3 (k_2_10_10_10_FLOAT, 7e3) host image through the conversion. Both
views are host RGBA16F images of the same size. With the cvar on, a qualifying draw binds the **f3 image** instead,
its pixel shader gets an output epilogue that computes exactly what "f2 store + conversion" compute, and the f3 view is
published as the owner of the tiles. The light pass then finds its tiles already owned and transfers nothing.

- `RestoreTargetEDRAM4` (`masseffect_native_targets.cpp`, called from `PrepareDrawEDRAM4`) picks the f3 image when all
  of these hold for the colour slot:
  - the view is the 32-bpp UNORM10 class, colour mask F, the pixel shader writes the output;
  - blending ONE/ZERO/ADD on colour and alpha, no alpha test, no alpha-to-mask, no shader kill, no exponent bias;
  - the draw is a proven full overwrite of the slot, and every tile it touches is wholly inside a proven rectangle
    (so no tile keeps old pixels that would reach f3 through another conversion);
  - the f3 image of the same base, pitch and sample layout already exists with the same geometry;
  - the site (VS, PS, base, slot) was seen at least twice with its restored tiles pulled next by the f3 view of the
    same base (learning, below), and it was never disabled;
  - `DrawsVulkan::SupportsRestore7e3`: the pixel shader output takes the epilogue.
- The draws code (`masseffect_native_draws.cpp`) reads `ContextTargets::RestoreInto7e3Mask()`, keys the slot with
  format k_2_10_10_10_FLOAT (so `TargetColor` returns the f3 image), sets specialization bits 10–13 (one per slot; the
  shaders do not test them) and builds the module with `ModuleRestore7e3` after the texture-sign fold. The slot is
  **not** in the existing 7e3 output clamp (`kSpecTarget7e3`), as today's f2 draw is not. Pipelines with bits 10–13
  are never prewarmed. If the epilogue cannot be built for the final module the draw is rejected (logged as an
  error), never drawn without it.

### The epilogue and why it is bit-exact

`me_restore_7e3_spirv.h`, `TransformRestore7e3`: every whole-vector `OpStore` to the output variable at the slot's
Location stores instead, per channel (x = the original value):

    q    = OpQuantizeToF16(x)                       models the f2 RGBA16F attachment store (RTNE)
    code = uint(Round(FClamp(q, 0, 1) * 1023))      the conversion's Pack32 for format 2 (same GLSL.std.450 ops)
    rgb  = code < 128 ? float(code) * 2^-9 : bitcast((code + (124 << 7)) << 16)      = From7e3(code)
    a    = float(uint(Round(FClamp(q, 0, 1) * 3))) * float(1.0 / 3.0)

`tests/cpu/test_native_restore_7e3_spirv.cpp` checks on CPU models of both chains:

- the closed-form decode equals the shaders' `From7e3` for all 1024 codes, every 7e3 value is exact in half and
  re-encodes (`A7e3`) to the same code; every 2-bit alpha survives f3 storage;
- all 1024 codes x = k / 1023 on every channel and all 4 alpha codes (what a point-sampled UNORM10 copy writes): the
  epilogue equals today's chain with the f2 attachment rounding to nearest-even **and** toward zero, so the real
  inputs do not depend on the ROP's float -> half rounding mode;
- a sweep of every 61st float bit pattern (70 M values, NaN, infinities, denormals), every half value and ±64 ulps
  around every code boundary: equal to today's chain with an RTNE attachment;
- the EDRAM word any later 32-bit consumer re-packs from f3 (f3 -> f0, f3 -> f2, f3 -> k_16_16, depth imports) equals
  the word it re-packs from today's f2 image, for every swept value;
- a negative control (the epilogue without the half step) is caught;
- the SPIR-V transform on a DXC-shaped module (one store rewritten, the expected operations, access chains and
  unknown Locations refused). Offline, 309 pixel shaders sampled from the package (every 97th entry) transform and pass
  `spirv-val --target-env vulkan1.2`.

Mesa facts used: `GLSLstd450Round` and `RoundEven` both become `fround_even`; NAK lowers `fquantize2f16` to F2F.F16
(nearest even, flush) and back. Half denormals flushed by the quantize step are below 2^-14, which rounds to code 0
on both sides.

What is not identical: a consumer that reads the **f2 host image itself** while f2 still owns the restored tiles (a
blend into f2, a resolve from f2) sees f16(x) today and code / 1023 with this path. They are equal for x = k / 1023,
which is what the plain copy writes. The learning guards it: once redirected, an import of the unchanged restored tiles
into a UNORM10 view disables the site for the session (one warning line), and it keeps the conversion.

### Learning (which consumer reads the restored tiles)

Each candidate draw (all conditions but the last two) stamps the tiles it published with its site, the owner, the
tile version and the owner's write count (`ImageNative::edram4_writes`, bumped by every writing publish, also through
the publish fast path that keeps tile versions). `SynchronizeEDRAM4` checks the stamps of every transfer it is about
to record: unchanged restored tiles pulled into the f3 view of the same base raise the site's confidence; pulled from
the f3 owner into a UNORM10 view, the site is disabled. A stamp is used once. With the cvar off nothing is stamped.

### Log lines

    [native] restore into 7e3: PS n21415 output 0 takes the epilogue (1 stores)
    [native] restore into 7e3: PS n21415 slots 1 module with the UNORM10 -> 7e3 epilogue (1 stores, N words)
    [native] EDRAM restore into 7e3: VS n6906 PS n21415 base 2D0 slot 0 tiles T drawn into the 7e3 image (no UNORM10 -> 7e3 import), draw D
    [native] EDRAM restore into 7e3, 10 s: D draws (T tiles) drawn into the 7e3 image; R f3 reads of restored tiles observed; sites L learned, X disabled (0 new); declined: [blending: n] [learning the consumer: n] ...

The first line appears once per pixel shader and slot, the third the first 16 times the path triggers, the fourth
every 10 s while the cvar is on. Success on the console: about 2 draws per frame in the 10 s line, and the
`C2D0/f2:... -> C2D0/f3:...` pair gone from `EDRAM mode4 top transfer pairs`. A `disabled` warning means the guard fell back.

## 9. Remaining exact candidates (2026-10-07, all default off, not measured on the Switch yet)

Four new cvars, each off by default. Turn them on one at a time on the cockpit route (960 and 1280), compare
`GPU per Swap` and the lines named below against the run without them.

| cvar | candidate | removes | expected 960 / 1280 |
|---|---|---|---|
| `masseffect_native_edram4_rect_list_tiles` | 2 (E35) | transfers into tiles a rectangle-list draw does not touch | 0.2-0.4 / 0.3-2.5 (see 9.1) |
| `masseffect_native_resolved_wake_no_clear` | 3 | the zero clear and its two barriers when a pooled resolved texture is woken for a full-cover resolve | 0.2-0.5 / 0.3-0.8 |
| `masseffect_native_resolve_7e3_frag` | 4 | the storage-image compute dispatch and its two barriers of the 7e3 -> UNORM10 resolve (VS40) | 0.3-0.5 / 0.4-0.6 |
| `masseffect_native_resolve_bias_probe` | (e) | nothing: diagnostic counters for the exponent-bias resolves | 0 |

`masseffect_native_resolve_7e3_frag` only acts when `masseffect_native_resolve_7e3_direct` is on (it replaces how
that resolve is recorded). Candidate 5 (one mutable-format image for f0 and f4) was not implemented, see 9.4.

### 9.1 E35 at 1280: what the logs really show, and `masseffect_native_edram4_rect_list_tiles`

The 1280 cost labelled E35 is not mainly the partial-tile depth clear. In `run/me1/prof_s1280` (per 10 s, ~299
Swaps):

- `partial tile: owner unsupported` is 299: one depth clear per frame (VS9828/PS10237, mode 5, rectangle
  `400,0-432,432` of `D5A0` 880x880) has a column of 27 partial tiles owned by `C400/f3`. With the stencil off
  (`dc 76`) the clear must keep the stencil byte, which is bits 0-7 of the 7e3 red channel there; no clear of the
  colour owner can write the other 24 bits alone, so this case cannot be redirected exactly. Drawn literally it
  imports the 27 tiles depth-only into one batched pass (`C400/f3 -> D5A0` 8,372 ops / 9,867 tiles, inside the
  0.2 ms `edram_import` line). Not worth more work.
- `edram_import9` (2.5 ms) is entirely `copy-engine stencil imports 4769 (233160 tiles)`, which equal the `late
  fetches 4769 (233160 tiles)`: 16 deferred-stencil fetches per frame, ~780 tiles, from the colour view `C400/f3`.
  At 1280 the 2x view `C400` (16 tiles per row) wraps past tile 2047 into the scene depth `D000` and covers the
  shadow slot `5A0`; at 960 (12 tiles per row) it does not, which is why the cost exists only at 1280.
- The shadow attenuation slot `C5A0/f7` is cleared every frame by VS9828/PS10237 as a **two-rectangle list**
  (`0,176-320,182` and the 2-pixel column `320,0-322,182`; at 960 `0,128-240,138` and `240,0-242,138`). The draw is
  multi-rect proven, but its sync and publish use the bounding box `0,0-322,182`: every tile of the box (~108 per
  frame at 1280, ~37 at 960) is imported from `D5A0` (a depth export, which first fetches the deferred stencil:
  9-pass at 960, copy engine at 1280) and from `C400/f3`, although the rectangles touch about 20 of them and the next
  draw (VS24994/PS24056, proven `1,1-321,181`) overwrites the interior anyway.

`masseffect_native_edram4_rect_list_tiles` syncs and publishes each rectangle's own bounds (clipped to the scissor)
instead of the box (`TouchRectsEDRAM4`, `SyncDrawEDRAM4`, `PublishDrawEDRAM4` in `masseffect_native_targets.cpp`;
the per-rectangle bounds come from `ProveRectangleList` in `me_native_system.cpp`, new field
`SubmissionDraw::edram_bounds_rects`).

Why it is exact: `ProveRectangleList` proves every rectangle by running the vertex shader on the CPU; each
rectangle's covered pixels lie inside its bounds, so a tile outside all of them receives no write from the draw.
On the Xbox its EDRAM bytes stay what they were; here the tile keeps its owner and that owner still holds exactly
those bytes, so every later reader (a draw's sync, a resolve's sync, a depth export) gets the same bits as before.
Tiles inside the union are synced and published exactly as before (the same `SynchronizeEDRAM4` per rectangle; a
tile shared by two rectangles is transferred once, the second sync finds it in the version cache). The ownership
claim (`edram4_own`) only ever shrinks to the last rectangle, so the fast paths stay conservative.

What it saves depends on whether the 780 fetched tiles at 1280 belong to these exports: the box tiles that move
from `D5A0` are exactly the deferred-stencil tiles that need a fetch before the export. Expected at 960: the
`D5A0 -> C5A0/f7` pair (10 ops, 37 tiles per frame) and the matching 9-pass imports (9 per frame) mostly
disappear, about 0.2-0.4 ms. At 1280: the `D5A0 -> C5A0/f7` and `C400/f3 -> C5A0/f7` pairs (29 ops, 108 tiles per
frame) drop to roughly a third; if the late fetches follow, `edram_import9` falls by up to 2.5 ms. The edge tiles of
the next draw (row 0 and column 0 of `C5A0`, not covered by either rectangle) are still imported once.

Log lines:

    [native] EDRAM rect list tiles: C5A0/f7:400x720 2 rectangles touch N of the M tiles of their bounding box 0,0+322x182 (VS n9828 PS n10237)
    [native] EDRAM rect list tiles, 10 s: D draws synced and published per rectangle (S slot syncs)

The first appears for the first 8 draws, the second every 10 s while the cvar is on. Success: in `EDRAM mode4 top
transfer pairs` the `D5A0/f0 -> C5A0/f7` and `C400/f3 -> C5A0/f7` op counts fall, and so do `late fetches` /
`copy-engine stencil imports` (1280) or the `9-pass imports` (960).

### 9.2 `masseffect_native_resolved_wake_no_clear`

`CopyInternal` computes, before `GetResolved`, whether the resolve covers the whole texture (`dx = dy = 0` and the
clipped request is at least the texture's width and height). `WakeResolved` then skips its zero clear and the two
barriers around it, and remembers the image as owing a clear. A guard object at the end of `CopyInternal` checks
that a resolve write was recorded (`copies_` grew: copy, blit, exponent-bias resolve or 7e3 resolve all count); if
not (a rejected format, an early return), it records the zero clear after all. Off when
`masseffect_native_lazy_front` is on (a postponed copy does not count as written).

Why it is exact: every texel the clear would zero is overwritten by the resolve that follows, before anything can
read the texture (resolved textures are written only by resolves). The barriers are not needed for ordering either:
the pool wakes an image only once the submission of its last use has completed (`ResolvedSleepingFinished`), and
the resolve write has its own barrier or render-pass dependency.

Log: the `resolved allocation pool` line gains `wake clears skipped (full-cover resolve)=N, recorded late (no
write)=M`. Success: N close to the `reuse` count of the same line, M = 0.

### 9.3 `masseffect_native_resolve_7e3_frag`

`ResolverDirect7e3Frag` records the 7e3 -> UNORM10 resolve as a fragment pass (new shader
`shaders/me_resolve_7e3_to_unorm10_frag.frag`, variant 3 of `EnsureConversionColorFrag`, the same load/store render
pass and dependencies as `ResolverWithBiasFrag`) into the A2B10G10R10 texture used as a colour attachment (it already
has that usage). The word math is the compute shader's, character for character. If the pass cannot be created, the
compute path runs (variant-3 failures do not disable the other fragment conversions). GPU label: `cat 12 VS44`
instead of `VS40`.

Why it is exact: both shaders compute the same word and output the same `vec4(code) / vec4(1023, 1023, 1023, 3)`;
the compute shader stores it through an rgb10_a2 storage image, the fragment shader through the ROP, both the Vulkan
float -> UNORM conversion. `tests/cpu/test_native_resolve_7e3_frag.cpp` shows that for every code, whatever way the
compiler evaluates the division (true division, reciprocal multiplication, either off by one ulp), the product with
1023 (or 3) is within 1e-3 of the code, and is exactly the code for a correctly rounded quotient; every half input
and mixed-channel words store back to the identical 32-bit word. Log: `resolve from the 7e3 owner as a fragment
pass: WxH ...` for the first 4 resolves.

### 9.4 Candidate 5 (f0 and f4 as one mutable-format image): not implemented

Legal (RGBA8_UNORM and R16G16_UNORM are in the same 32-bit compatibility class) and free on NVK: in the local Mesa
`nvk_image_can_compress` does not look at `MUTABLE_FORMAT`, and NIL picks the PTE kind by bits per pixel. It is not
worth it now:

- the renderer keys several caches by `VkImage` (`framebuffers_import_depth_edram_`, `framebuffers_conv_color_frag_`,
  `views_raw32_edram_`, `views_raw64_edram_`, `views_depth_edram_`, `views_raw64_rt_edram_`) and `Destroy` frees
  `image.image`; two `ImageNative` sharing one `VkImage` would need an alias concept in all of them;
- `masseffect_native_conversion_copy_32` already turns the f0 -> f4 conversion into a `vkCmdCopyImage` (post-chain.md:
  1.3-1.5 ms -> 0.25 ms at 960), and the pair is not in the 1280 top pairs. What is left is about 0.25 ms at 960.

### 9.5 (e) The exponent-bias resolves: can they be merged or elided?

From the logs: `14B48000` (1280) gets 2,074 resolves per 10 s, **all with draws before them** (36.2 draws per copy);
the light passes draw into the 7e3 scene between them. So an exponent-bias resolve normally reads a source that
changed since the previous one, and its result differs: nothing to elide. Two exact options remain:

1. **Repeat elision**: a resolve into the same texture with the same rectangle, bias and format from an unchanged
   source can be skipped (resolved textures are written only by resolves). Exact, but only worth it if it happens.
2. **Pair merge**: a bias resolve and a 7e3 -> UNORM10 resolve back to back from the same unchanged source could be
   one fragment pass with two colour outputs (one read instead of two). It needs a second render-pass format pair
   and both textures in one framebuffer; worth it only if pairs are frequent.

`masseffect_native_resolve_bias_probe` counts both, with a content signature over (owner, tile, version) of every
EDRAM tile of the resolved area (a version changes with every published write). One line every 10 s:

    [native] resolve probe, 10 s: bias resolves B (R repeat an identical earlier one from an unchanged source), 7e3 resolves C (S repeats); bias + 7e3 pairs from the same unchanged source P

Implement option 1 if R is a sizeable part of B, option 2 if P is close to C. With R = P = 0 the bias resolves are
as cheap as their bytes (one read and one write of the scene each, about 0.55 ms at 960) and the topic is closed.

## 10. 1280x720: the late stencil fetches (`edram_import9`) and what is left in `edram_alias` (2026-10-07)

Offline analysis of `mass-effect-recomp/run/me1/n1280_x/console.log` (RU, 1280x720, Normandy cockpit, with
`edram4_rect_list_tiles`, `resolved_wake_no_clear`, `resolve_7e3_frag`, `fold_vs_constants`, `restore_into_7e3`;
`r1280_x` for comparison). Steady window at the end of the log: 264 Swaps per 10 s, GPU busy 35.7-38.2 ms per Swap,
`edram_import9` 4.2 ms, `edram_alias` 6.3 ms. Raw `GPU time by pass` values are converted with x1.627 / 264.
Three new cvars, all default off; nothing was run on the console.

### 10.1 What `edram_import9` is at 1280

Category 13 is 686 raw ms per 10 s (4.2 ms per frame). The lazy-stencil lines give its two parts:

    lazy stencil: 7364 depth-only imports, ..., 1841 exports of deferred stencil, 196461 tiles inherited by
    depth-only draws, ..., 2104 late fetches (192516 tiles)
    9-pass imports: [C400:1280x1280:mx0my1->5A0:880x880:mx0my0 1841ops/3156t]; ...; copy-engine stencil
    imports 263 (189360 tiles)

1. **One copy-engine fetch of 720 tiles per frame (about 2.8-3.3 ms).** 263 per 10 s = one per Swap, 720 tiles each
   = the whole 1280x720 1x depth view `D000`. Category 13 has exactly 263 intervals of 1 ms or more (max 2.39 ms raw);
   solving the n/r logs for "a + b x tiles per 9-pass op, c per copy" gives c = 1.7-2.0 ms raw. Older logs with the
   per-trigger key name the pair: `D000:1280x1280:mx0my1 -> 000 (1x)`, trigger `slot0 dc00700263` (stencil EQUAL
   test, no stencil write, depth GEQUAL, no depth write). At 1280 the game draws the scene with 2x MSAA (the 2x view
   `D000 mx0my1`) and later uses the same EDRAM as a 1x depth buffer: the 1x view takes the tiles depth-only (lazy
   stencil: 744 tiles per frame inherited by depth-only draws), its stencil records point at the 2x view, and the
   first stencil-testing draw on the 1x view fetches all 720 tiles. The 1x view is `D32_SFLOAT_S8_UINT` (the
   half-range D24FS8 host format). In the local Mesa (`nvk_cmd_copy.c`, `nvk_remap_insert_aspect`), a buffer-to-image
   copy into the stencil aspect of `Z32_FLOAT_S8X24` is **two copy-engine launches per region**: a 1-byte remap into
   `stencil_copy_temp`, then a second launch marked `NON_PIPELINED`. `CopyStencilEDRAM4` makes one region per tile
   row, so the fetch is 45 regions = 90 launches, 45 of them serializing. This cost does not appear at 960 in the
   current runs (`copy-engine stencil imports 0`).
2. **Seven 9-pass fetches of 1-2 tiles per frame (about 0.9-1.4 ms).** At 1280 the 2x scene-color view `C400/f3`
   (16 tiles per row, 2x vertical) reaches past tile 2047 and covers the shadow-map slot `5A0`. The shadow-slot depth
   clear (VS9828/PS10237, mode 5, rect `400,0-432,432` of `D5A0`, a column of partial tiles) imports those tiles
   from `C400/f3` depth-only, so their stencil stays "deferred" in the color view. The attenuation-slot rect-list
   draw (VS9828 on `C5A0/f7`) then exports `D5A0 -> C5A0/f7`, and an export packs the stencil byte, so each export
   run first fetches the real stencil from `C400`: a color source counts as "all 8 bits may be set", the run is
   below `stencil_copy_min_tiles`, so it is a pass with a stencil clear and 8 bit draws, each in its own render pass
   (the export closes the sync's batch around it).

`VK_EXT_shader_stencil_export` is not an option: NAK has `UNREACHABLE("EXT_shader_stencil_export not supported")`
(`nak_nir.c`), and NVK does not expose it for any GPU. The 8 bit passes as full-screen draws cost about as much as
the copy engine (0.3-0.4 ms per full-screen pass measured earlier), and the 2x view's `edram4_bits_stencil` has all 8
bits set (old logs: "stencil bit passes skipped (bit never set): 0" with `D000` 9-pass imports), so bit skipping
does not help. Moving the shadow slot is not guest-invisible. The three changes below keep the fetch semantics and
change only how it is recorded.

### 10.2 New cvars (default off)

| cvar | what it changes | expected at 1280 | why it is exact |
|---|---|---|---|
| `masseffect_native_edram4_stencil_copy_rows` | the copy-engine stencil import writes consecutive whole tile rows with one `VkBufferImageCopy` (720-tile fetch: 45 regions -> 1, 90 copy-engine launches -> 2) | 0 to ~2.5 ms: all of it if the launches (and the 44 non-pipelined serializations) dominate, nothing if the per-byte remap throughput does | the merged region writes every texel from the same buffer byte (rows are `bufferRowLength` = rect width apart in both plans); `tests/cpu/test_native_stencil_copy_regions.cpp` checks it for every tile range of 7 view shapes (387,832 ranges), plus a negative control |
| `masseffect_native_edram4_stencil_known` | a late fetch whose source tiles all hold one stencil value set by a whole-tile stencil clear (redirected stencil clear, canonical stencil clear, or a redirected depth clear with stencil), with nothing written into the source image since, becomes `vkCmdClearAttachments` (stencil aspect) of the destination tiles to that value | up to ~3 ms if the 720-tile fetch qualifies (a stencil clear of 1280x720 instead of 90 copy-engine launches); 0 otherwise. The 9-pass fetches from `C400` never qualify (color source) | see 10.3 |
| `masseffect_native_edram4_stencil_fetch_area` | a stencil-using draw fetches only the tile rows/columns of its area (`edram4_draw_area_`: window scissor intersected with the proven rectangle) instead of its whole bound range; the other tiles keep their deferred record | 0 to ~2.5 ms, depending on how much of the screen the stencil-testing draws cover; whole-width areas still make one fetch, narrower ones one fetch per tile row | the sync and publish of the same draw already use exactly this area (tiles outside it are not transferred and receive no write); a tile outside keeps its record, so any later user (a stencil draw, an export, a plain copy) still fetches or carries the real stencil, as today for the tiles beyond `length_tiles` |

### 10.3 Why the known-value clear is exact

The import of a late fetch (copy engine or bit passes) sets the destination tiles' stencil to the source tiles'
**host** stencil (tile for tile, every sample) and leaves depth alone. `edram4_stencil_known_` holds, per depth image
and own tile, the value that every sample of that tile's host stencil holds, and only while that is certain:

- values are recorded only right after a recorded whole-tile stencil clear of that image (`RedirectClearStencilEDRAM4`,
  `PublishStencilClearEDRAM4`, `RedirectClearDepthEDRAM4` with a stencil value and a non-partial region; a partial
  region forgets the image);
- every other operation that can write the image forgets all of its values (`WrittenStencilEDRAM4`): binding it as
  the depth attachment of a draw (`PrepareDrawEDRAM4`, conservative: also read-only binds), `ImportColorDepthEDRAM4`,
  `CopyTilesEDRAM4`, `CopyStencilEDRAM4`, `ClearDepth`, content restores and destruction
  (`ForgetSynchronizedEDRAM4`), `CopyAliasEDRAMPitch`, `SynchronizeOverlapsEDRAM`, `CopyOfLapForResolver`. Depth
  images are written by nothing else: the draws code clears depth only when the targets code calls it, depth images
  never swap with resolved textures in mode 4, and resolves and exports only read them. A depth-only redirected
  clear keeps the values (it does not touch stencil).

So a value found at fetch time is exactly what the import would copy, and the clear writes the same value into the
same destination tiles (whole tiles, stencil aspect only). The destination's own values are forgotten after the
clear (no chain of recorded values through fetches). `edram4_bits_stencil` of the destination gets the value's bits,
a subset of what the import would add. With the cvar off nothing is recorded (the map stays empty and each forget is
one `empty()` test).

### 10.4 Log lines

With any of the three cvars on, every 10 s next to the lazy-stencil report:

    [native] EDRAM late stencil fetches, 10 s: [D000:1280x1280:mx0my1s1->000:1280x1280:mx0my0 import (no known value), copy engine 263ops/189360t] [C400:...->5A0:880x880:mx0my0 import (color source), bit passes 1841ops/3156t]; known-value clears K (T tiles); area-limited draws A (U tiles left deferred); copy rows merged M

(`sN` is the source's host sample count.) The first 8 known-value clears also log
`EDRAM known stencil: N tiles of 000:1280x1280 (from 000:... tile t, all holding stencil v since a clear) set by a
stencil clear instead of an import`.

What to check per run (cockpit, 1280, one cvar at a time on top of the current 1280 set):

- `stencil_copy_rows`: `copy rows merged` about 44 x 263 per 10 s; category 13 falls by what the launches cost.
- `stencil_known`: the 720-tile pair moves to `known value: clear`; `copy-engine stencil imports` drops by ~263 and
  `edram_import9` by ~3 ms. If the pair stays `import (no known value)`, the 2x view is written (stencil-writing scene
  draws) between its last stencil clear and the fetch, and this idea is closed.
- `stencil_fetch_area`: `area-limited draws` > 0 and `tiles left deferred`; `late fetches (tiles)` falls if the
  stencil-testing draws are local.
- Picture: the usual cockpit captures; the lights and the shadowed areas are where stencil matters.

### 10.5 `edram_alias` at 1280 (6.3 ms): what it is made of

| item (label) | per frame | ms | note |
|---|---|---|---|
| exponent-bias resolves, fragment (VS41) | 6.5 into `14B38000` 1280x720 RGBA16F, 38.9 draws per copy | ~4.2 | read 7.4 MB of the 7e3 image + write 7.4 MB each: ~96 MB per frame, ~23 GB/s, i.e. at the memory bandwidth limit |
| 7e3 -> UNORM10 resolves, fragment (VS44) | 1.16 into `15268000` | ~1.3 | 11 MB each; a VS44 run also carries any cat-12 op that follows it |
| cat 12 runs labelled VS0 | | ~0.6 | small conversions: `C5A0/f7 2x -> 1x` (11 ops, 18 tiles per frame, the DOF buffer border) and `C400/f3 -> C5A0/f7` (7 ops) |

The top transfer pairs at 1280 are all at `0x5A0`: `C400/f3 -> D5A0` (7364 ops/10 s, imports), `C5A0/f7 2x -> 1x`
(2893), `C400/f3 -> C5A0/f7` (1841) and `D5A0 -> C5A0/f7` (1841). Each is a whole render pass for 1-2 tiles whose
border pixels the DOF/bloom resolve (`1F540000`, crop 322x182) reads, so they must stay.

No exact removal is implemented for this category; the reasons, largest item first:

- **Bias resolves (4.2 ms)**: every one has draws before it (the light passes), so nothing repeats; the bytes are the
  cost. Exact reductions: (a) one pass for a bias resolve and a 7e3 resolve of the same unchanged source (saves one
  7.4 MB read per pair, ~0.3-0.5 ms; needs the bias resolve to be deferred until the next command, or written to a
  scratch image that is handed over, because the second resolve is only known afterwards); (b) dirty rectangles
  (the light passes use a full-screen scissor and the restore draw rewrites the whole region, so the union is the
  screen: expected nothing); (c) post-chain option C (host-side post targets, `post-chain.md` section 3, 2.5-3.3 ms
  at 1280, several days). Run `masseffect_native_resolve_bias_probe = true` at 1280 first: its `P` decides (a).
- **Restore elision is not exact any more** (post-chain.md option A): with `restore_into_7e3` the restore draw writes
  `From7e3(To7e3(x))` into the 7e3 image, while that image holds the unquantized FP16 sums of the additive light
  passes. Skipping the restore would keep the unquantized values and change the next light's result (by rounding).
  It is exact only if the 7e3 image is known to hold 7e3-representable values, which the renderer cannot prove.
- **The 0x5A0 small ops (~0.6 ms)**: needed for the 1-pixel border of the DOF buffer (post-chain.md option B needs
  a proof that the border is a known clear).

### 10.6 Test configuration

```toml
# on top of the current 1280 set (rect_list_tiles, resolved_wake_no_clear, resolve_7e3_frag, fold_vs_constants,
# restore_into_7e3); one per run
masseffect_native_edram4_stencil_copy_rows = true
# masseffect_native_edram4_stencil_known = true
# masseffect_native_edram4_stencil_fetch_area = true
```

Build: `run/me1/ru_1280gpu.nro`.

## 11. 1280x720 after section 10: per-frame budget, bias resolves, copies, pixel shaders (2026-10-07)

Offline analysis of `mass-effect-recomp/run/me1/s1280st_f/console.log` (RU, 1280x720, Normandy cockpit, the
`s1280st.toml` set: every change of sections 8-10 on) plus the host NAK cost tools. Nothing was run on the console.
Build with the new cvars: `run/me1/ru_1280b.nro` / `.elf`. Every new cvar is off by default.

### 11.1 Where the 32.5 ms go (last six 10 s windows, raw `GPU time by pass` x 1.627 / Swaps)

| item | ms/frame | per operation | note |
|---|---|---|---|
| scene (cat 2) | 13.7-15.3 | | geometry + lights; PS ranking pending (11.6) |
| scene_no_z (cat 8: post passes) | 4.4-5.2 | restore PS21415 ~0.7, PS3474 ~1.0 | |
| `edram_alias` VS41 exponent-bias resolves | 4.0-4.9 | 0.7-0.8 (5-6 per frame, full screen) | 14.7 MB each: 18-22 GB/s, the memory limit |
| `edram_alias` VS44 7e3 -> UNORM10 resolves (fragment) | 1.0-2.4 | **~1.0-1.1** (1-2 per frame) | only 11 MB each: 10-11 GB/s, half the bias resolve's rate, so not bandwidth bound (11.3) |
| copies (cat 6) | 1.7-2.3 | | 11.4 |
| 320 (cat 4: DoF/bloom quarter chain) | 2.3 | | |
| other (cat 0: start of each work slot) | 1.1 | | query resets and whatever precedes the first category change; the mark dump (11.7) names it |
| small cat-12 ops (VS0), clears, imports | ~1.1 | | the 0x5A0 border traffic (10.5) |

### 11.2 Exponent-bias resolves (4.2 ms): the three questions

1. **Merge several bias resolves of the same source into one pass?** No such pairs exist. Every resolve into
   `14B38000` has draws before it (`2001 copies, 2001 with draws, 38 draws per copy`), it is the only full-screen
   bias destination (the other fmt-32 texture, `1F540000`, is the 352x182 quarter buffer from `C5A0`), and the two
   pieces of a predicated-tiling resolve (`1280x512` + the `1280x208` segment at `0,512`) read disjoint tiles. The
   probe of section 9.5 has never been run; it is in `s1280pd.toml` (11.7) and settles it with numbers.
2. **Read the 7e3 data directly as RGBA16F instead of converting?** It already does: the host image of the 7e3 view
   is RGBA16F and `me_resolve_exp_bias_frag.frag` is one `texelFetch` and four `fmul` (NAK: 24 instructions, no
   conversion). A fragment pass over block-linear images reads every texel once; there is no cache-tiling gain left.
3. **Is the destination wider than needed?** No. The guest format is k_16_16_16_16_FLOAT (RGBA16F), the readers use
   all four channels (PS15729 reads `BlurredImage.w`), and the source must stay RGBA16F: the 7e3 image is blended
   (additive lights), and no blendable Vulkan format holds 7e3 (B10G11R11 has 6/5-bit mantissas and no alpha).

So the bias resolves cost their bytes and only structural changes remove them:

- **Dual output from the producing post pass** (exact, not implemented): when a full-screen ONE/ZERO post draw into
  the 7e3 view is followed by a bias resolve of the same rectangle, the draw gets a second color attachment (a scratch
  RGBA16F image handed to the resolved texture at the resolve) with the output `rop(clamp(x)) * 2^-3`. That removes
  the resolve's 7.4 MB read and its pass: ~0.33 ms per resolve, **~1.0-1.6 ms at 1280** for the 3-5 post resolves.
  Exactness needs the in-shader value to equal what the ROP stored in the 7e3 image: NAK's `OpQuantizeToF16` flushes
  half denormals, so the epilogue must emulate the ROP's float -> half conversion with integer code, and that
  conversion's denormal and rounding behaviour on GM20B must first be checked once on the console (a one-time
  self-test render). It also needs a learning site like `restore_into_7e3` and an extra attachment in the draw's
  render pass and pipeline key: `masseffect_native_draws.cpp` work, held back while that file is being split.
- **Hand-off instead of copy** (post-chain.md option C): 2.5-3.3 ms at 1280, several days. Folding the 2^-3 into
  the reader alone is not exact: the 7e3 image holds `f16(clamp(x))`, not 7e3-quantized values, so
  `f16(A / 8) * 8 != A` for `A < 2^-11`.

### 11.3 The 7e3 -> UNORM10 resolve (VS44, ~1 ms each): `masseffect_native_resolve_7e3_pack` (new, exact)

The fragment resolve moves 11 MB in ~1.05 ms, half the bias resolve's rate, so its ALU matters. NAK
(`nak-any/cost.sh`): 95 instructions per pixel. Two exact reductions:

| variant | NAK instrs | what changes |
|---|---|---|
| 0 (today) | 95 | |
| 1 | 83 | `A7e3`: the normal branch folds the exponent rebias into one add (bit 16 of `f32 - (124 << 23)` is bit 16 of `f32`); the denormal branch is `uint(roundEven(v * 512))` instead of mask/shift/min/shift; the branch test stays the original integer compare, so -0 and NaN take the same branch |
| 2 | 64 | variant 1, and the 32-bit word is written through an `R32_UINT` view of the texture (`MUTABLE_FORMAT`, same 32-bit class) instead of `vec4(code) / vec4(1023, 1023, 1023, 3)` through the UNORM conversion (no I2F, no multiplies) |

- Variant 1 is a specialization constant (`kPackFast`, pipeline variant 4) of `me_resolve_7e3_to_unorm10_frag.frag`;
  variant 2 is `shaders/me_resolve_7e3_word_frag.frag` (pipeline variant 5, render pass format R32_UINT). With
  value 2, A2B10G10R10 textures created from then on get `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` (`unorm10_mutable_`);
  a texture without it uses variant 1. Only with `masseffect_native_resolve_7e3_frag = true`.
- Why exact: `tests/cpu/test_native_resolve_7e3_pack.cpp` checks the cheaper `A7e3` against the original on the
  float32 image of all 65,536 half bit patterns (the texel fetch of an RGBA16F image returns exactly those, and any
  clamp output of a half is a half: 0, -0, 31.875, the value, or a NaN with a half payload), the normal branch for all
  2^32 inputs, and the word layout (R bits 0-9, G 10-19, B 20-29, A 30-31 = A2B10G10R10_UNORM_PACK32). The word
  output stores the same code the float path stores through the UNORM conversion (section 9.3 test). A negative
  control (truncating denormal branch) is caught. For float32 values that are not halves the denormal branches can
  differ (7 of 10.8 M sampled); the resolve never reads such values.
- Expected: up to ~0.3-0.5 ms per resolve if it is ALU bound (the bandwidth floor is ~0.5 ms), i.e. 0.3-0.8 ms per
  frame at 1280. Log: `resolve from the 7e3 owner as a fragment pass: ... variant 5, N with the word output`.
- Related A/B with no code: `s1280c7.toml` (`resolve_7e3_frag = false`). The compute path measured 0.7 ms per resolve
  at 1280 in `prof_s1280` against ~1.05 ms for the fragment pass now; at 960 the fragment pass was already slower.

### 11.4 Copies at 1280 (~2 ms): which full-screen ones can go

| texture | per frame | what | avoidable? |
|---|---|---|---|
| `1F5F8000` 1280x720 RGBA8 front buffer | 1 | `NOBODY READS IT` (only the Swap) | **yes, exact**: `masseffect_native_lazy_front = true` paints the Swap from the render target (self-checking). It used to switch `resolved_wake_no_clear` off (all ~34 wake clears per frame back); that exclusion is removed (11.5). ~0.3 ms |
| `144C7000` 1280x720 RGBA8 | 2-2.75 | light attenuation: clear, one shadow-frustum draw (PS21267, a 36-vertex box, multiply blend), resolve; read by the light passes | no; only a proven screen bound of the box would shrink it (its VS is not a rectangle list, the extent estimator fails: `est -1`) |
| `15268000` 1280x720 SceneDepth (k_24_8_FLOAT, guestspace compute from the 2x depth) | 2 | resolved right after the 7e3 resolve of each shadowed light (`0 draws per copy`) | **possibly**: the depth plane may be unchanged between the two lights (the shadow projection writes only stencil: `dc 00708571`, Z off, stencil REPLACE). `masseffect_native_resolve_repeat` (11.5) skips the second one if so |
| `1485F000` 864x864 shadow depth | 2-3 | one per shadowed light | no (new shadow map each time) |
| `1F540000` 352x182 | 3 | quarter DoF/bloom | small |
| pool wake clears | 0 | `wake clears skipped (full-cover resolve)=9728` per 10 s | already gone |

### 11.5 New cvars (all default off)

| cvar | what | exact because | expected at 1280 |
|---|---|---|---|
| `masseffect_native_resolve_7e3_pack` (0/1/2) | 11.3 | 11.3 | 0.3-0.8 ms |
| `masseffect_native_resolve_repeat` (0 off, 1 count only, 2 skip) | a bias, 7e3 -> UNORM10 or depth resolve that would write exactly what its texture already holds is not recorded | see below | 0 to ~0.8 ms (the second SceneDepth resolve if its depth plane is unchanged; color resolves are expected never to repeat) |
| (behaviour change, no cvar) `resolved_wake_no_clear` together with `lazy_front` | the wake clear is skipped with `lazy_front` on as well | a postponed front copy does not grow `copies_`, so the guard at the end of `CopyInternal` records the owed clear at once, before the deferred copy | lets `lazy_front` be measured without losing ~34 skipped clears per frame |

`masseffect_native_resolve_repeat` (`me_resolve_repeat.h`, `RepeatResolveEDRAM4` / `RepeatWrittenEDRAM4` in
`masseffect_native_targets.cpp`):

- Request key: kind, source view and image handle, source rectangle, destination offset and size, exponent bias,
  host and guest formats, sample select (depth), the 7e3 owner for the direct path.
- Source identity: for every EDRAM tile of the source tile rectangle (not only the touched ones): physical index,
  owner, owner image handle, owner tile, version, and the owner's `edram4_writes` (the publish fast path keeps tile
  versions but always bumps that count); plus the source view's handle and write count. A tile without an owner or a
  scaled / raster-grid / invalid-content view is not comparable.
- Depth plane only, for depth resolves: stencil-only changes (a draw publish with stencil writes and no depth write,
  the canonical stencil-only clear, the redirected stencil clear) get a new tile version as before, but
  `StencilOnlyVersions` remembers the version the tile had before a run of them, and `edram4_stencil_only_writes_`
  is subtracted from the write count. Late stencil fetches never changed versions.
- Destination: the resolved entry's `revision` (bumped by every write path: `ResolvedWritten`, guest-format change,
  re-creation, pool wake) must still be the one recorded right after the earlier hooked write, and the image handle
  the same. Up to 4 records per texture survive each other's writes when their rectangles do not overlap (the two
  predicated-tiling segments). A version-counter wrap invalidates every record.
- Then the texture holds, texel for texel, what the resolve would write: the same request on the same source content
  into an image nothing wrote since. `tests/cpu/test_native_resolve_repeat.cpp` checks the bookkeeping against a
  texel model (4 M random steps with unknown writers, pool wakes and two-segment resolves: 0 wrong of >100 k repeats;
  a revision-blind cache is caught) and the stencil-only versions against a depth-plane model (also with the cvar
  toggled mid-way).
- Log every 10 s: `[native] resolve repeat, 10 s: [bias: N resolves, R identical to the previous one, S skipped, U not
  comparable] [7e3: ...] [depth guestspace: ...] [depth copy: ...]`; the first 8 skips also log
  `resolve repeat: kind K into ADDRESS WxH skipped`. Run mode 1 first: if `depth guestspace` shows about one identical
  resolve per frame, mode 2 removes it.

### 11.6 Pixel shaders at 1280 (host NAK, after the texture-sign fold with every sign 0)

From a subagent run of `nak-gm20b-cost` over the library modules (prototypes in the session scratch folder
`ps1280/`; nothing in the repository):

| PS | base instrs | best exact | main source of the saving |
|---|---|---|---|
| 14702 | 298 | 240 | A + B + C |
| 2169 | 260 | 212 | A + B + C |
| 24056 | 673 (64 GPRs) | 591 | A (-64: the clamp immediates) |
| 7992 | 120 | 108 | |
| 15729 | 153 | 120 | |
| 3474 | 66 | 58 (34 with E) | |
| 23886 | 240 | 218 | |
| 26940 / 12720 / 18008 | 457 / 542 / 390 | 364 / 430 / 313 | |

- A. `clamp(x, -FLT_MAX, FLT_MAX)` costs two `mov` of 32-bit immediates per clamp (FMNMX takes only 20-bit
  immediates): read the bounds from two unused shared constant words instead (a renderer SPIR-V transform, no new
  pipelines). -3.4 % on 300 random shaders, ~0.16 ms at 1280.
- B. A combined image+sampler heap saves 7 instructions per texture (descriptor fetch): ~0.45 ms at 1280, a real
  renderer change. C. Drop the index mask once signs are folded: ~0.07 ms. D. Gamma decode with explicit selects
  (library rebuild): 0-0.2 ms. E. Fold PS3474's Gamma constant: <= 0.05 ms.
- Little or nothing: PS constants in compares (2 of 300 shaders), dead outputs (1-2 instructions), NoContraction /
  64-bit loads (none). After both folds PS14702 is pixel-bound, not vertex-bound (~130 M vs ~23-43 M lane
  instructions at 1280).
- Image-changing (separate): integer texel offsets in PS24056 (-8 instructions, 56 GPRs).
- None of these is implemented here: they live in `masseffect_native_draws.cpp`, which another change is splitting.
  Together ~0.7 ms at 1280.

### 11.7 Test configurations (`mass-effect-recomp/run/me1/`, base = `s1280st.toml`, build `ru_1280b`)

| toml | added lines | purpose |
|---|---|---|
| `s1280pd.toml` | `masseffect_native_stats_per_draw_s = 5`, `masseffect_native_resolve_bias_probe = true`, `masseffect_native_resolve_repeat = 1` | per-draw ranking at 1280 (`#n PS ... ms/frame` lines), bias probe and repeat counters; timing not valid |
| `s1280mk.toml` | `masseffect_diag_dump_marks_s = 320`, `masseffect_native_precise_marks = true`, `masseffect_gpu_marks_categories = true` | ordered marks of 6 frames in the cockpit: exact ms per resolve, per copy and the cat 0 "other" |
| `s1280pk.toml` | `masseffect_native_resolve_7e3_pack = 2` | 11.3 |
| `s1280rr.toml` | `masseffect_native_resolve_repeat = 2` | 11.5 (run `s1280pd` first) |
| `s1280lf.toml` | `masseffect_native_lazy_front = true` (replaces the `false` line) | 11.4 |
| `s1280c7.toml` | `masseffect_native_resolve_7e3_frag = false` (replaces the `true` line) | compute vs fragment 7e3 resolve |
| `s1280all.toml` | pack 2 + repeat 2 + lazy_front | everything of this section together |

```toml
# section 11, one per run on top of s1280st.toml
masseffect_native_resolve_7e3_pack = 2
# masseffect_native_resolve_repeat = 2
# masseffect_native_lazy_front = true
```

### 11.8 Summary

Implemented and exact (console numbers pending): 7e3 pack (0.3-0.8 ms), lazy front now usable with the wake-clear
skip (~0.3 ms), resolve repeat (0-0.8 ms). Not implemented, exact: bias-resolve dual output (1.0-1.6 ms), pixel
shader items A-C (~0.7 ms), hand-off (2.5-3.3 ms). Reaching 4-5 ms at 1280 without image change needs the dual
output (or the hand-off) in addition to this section's changes.

## 12. Removing the exponent-bias resolves: what is exact, and the measurement that decides (2026-10-07)

Task: remove the ~6.5 full-screen exponent-bias resolves per frame at 1280x720 (`14B38000`, ~4.2 ms, section 11.2)
either by a second output of the pass that produces their source ("dual output") or by handing the source image
over instead of copying ("hand-off"). Offline analysis of the code, the logs and the tile trace of section 7; nothing
was run on the console. Result: **neither is exact and broad at the same time**, so this section adds the
measurement that decides it (one console run) instead of a large blind change. Build: `run/me1/ru_nobias.nro` /
`.elf`. Both new cvars are off by default and change nothing in the picture.

### 12.1 What the resolve computes, and why every replacement needs care with tiny values

`ResolverWithBiasFrag` writes `rop(A * 2^-3)` for every texel `A` of the 7e3 host image (RGBA16F), where `rop` is the
GPU's float32 -> RGBA16F attachment conversion. `A` itself is `rop(x)` of whatever last wrote it (a blend result, a
restore, a post pass). For `|A| >= 2^-11` the result is exactly `A / 8` (a power-of-two scaling of a normal half is
a normal half). For `|A| < 2^-11` the quotient is a half denormal (step 2^-24) and the low bits of `A` are rounded
away. Every alternative must reproduce that rounding where it can happen:

- **Readers apply the 2^-3 themselves** (hand-off): a reader that samples `A` and scales gets `A / 8`, not
  `rop(A / 8)`. With point sampling the reader could emulate the rounding exactly (integer code on the fetched
  value); with bilinear filtering it cannot: the filter mixes four texels before the reader sees them, so the result
  differs whenever one of the four is below 2^-11 and has low bits. The difference is at most 2^-25 in the biased
  domain (2^-22 after the readers' x8 bias factor), far below one 8-bit step after tone mapping, but it is not
  bit-exact. The texture is also 1280x720 while the 7e3 view's host image is 1280x1280: readers use normalized
  coordinates, so the image cannot be bound in place without an aliased 1280x720 VkImage on the same memory
  (outside the Vulkan spec; the images are dedicated allocations) or a coordinate change in every reader.
- **A second output of the producing draw** (dual output): the producing draw would write `S = e(x)` into a scratch
  RGBA16F image next to `I = rop(x)`, with `rop(e(x)) == rop(rop(x) * 2^-3)` for every float32 `x`. For
  `|x| >= 2^-11` (and below the float16 overflow) `e(x) = x * 2^-3` is exact for *any* rounding mode: both sides
  round at the same relative position. Below 2^-11 (and at the overflow) `e` must model `rop` exactly: rounding mode
  and denormal handling of the attachment conversion. The Vulkan spec leaves both to the implementation, and NAK's
  `OpQuantizeToF16` flushes half denormals, so the model must be measured on the GM20B (12.4). A blended producer
  cannot be handled this way at all: `S` would have to blend in parallel (`S + s / 8`), which again differs in the
  half-denormal range of `S`, and costs a read and a write of `S` per blended pixel (as much as the resolve).

### 12.2 Which resolves each way could remove (from the frame structure)

The resolve's destination `14B38000` gets `1709` copies and `1491` reads per 10 s at 1280 (`n1280_x`, 263 Swaps):
6.5 copies but only 5.7 reads per frame. The pixel count (`1331.98 Mpixels` = 1445 full-screen equivalents) shows one
resolve per frame split in two segments (1280x512 + 1280x208), so there are ~5.5 *logical* resolves per frame and
~1.03 reads per resolve: practically every resolve is read (a lazy resolve that drops unread ones would remove
nothing). `38.9 draws per copy` with ~5.5 resolves means one long gap (the base pass and lights, ~200 draws) and short
gaps between the others.

The tile trace of section 7 (960x544, tile 930) gives the end of the frame: `VS9460/PS3474` (full-screen quad into the
7e3 view, mask F), `VS28697/PS7992` (material effect into the 7e3 view, **colour mask 7**: alpha kept), then
`VS2293/PS15729` (uber blend) into the RGBA8 view `f0` of the same base, `VS28697/PS16272` (mask 7) in `f0`; the DOF
gather `VS24994/PS24056` writes the quarter buffer `C5A0` between `PS7992` and `PS15729` (it reads `SceneColorTexture`
and the uber pass combines its result). Translucent draws (`PS12522` 44 draws per frame, `PS15127`, `PS13342`)
precede them in the 7e3 view with blending. The likely positions of the bias resolves, and what each way could do:

| resolve (inferred) | source last written by | readers until the next resolve | dual output | hand-off |
|---|---|---|---|---|
| after the lights / during translucency (~3-3.5 per frame at 1280) | additive lights, blended translucent draws | translucent draws that **blend into the source** | no (blended producer) | no (the reader writes the source: a copy is needed anyway) |
| after `PS3474` (if one is there) | `PS3474`, full-screen, mask F | `PS7992` (writes the source, mask 7) | **yes**, if `PS3474` is ONE/ZERO with a proven full rectangle | no |
| after `PS7992` | `PS7992`, mask 7: alpha from before | `PS24056` (quarter buffer), `PS15729` (`f0`) | only with the alpha carried from the previous resolve (the reader's own pixel of the previous texture) | yes, inexact for filtered reads (12.1) |

So the exact gain available is about one to two resolves per frame: ~0.35 ms each with dual output (it saves the
resolve's read of the source and its pass; the write moves into the producer as a second attachment), ~0.7 ms if the
source is also dead after the resolve (the producer then writes only the scratch image; not provable before the
resolve happens). The 2.5-3.3 ms of post-chain.md option C assumed post passes produce most of the resolves; the
counts above say most are in the scene phase, between blended draws.

### 12.3 The probe: `masseffect_native_bias_life_probe` (default false, diagnostic only)

`me_bias_life.h` (bookkeeping, `tests/cpu/test_native_bias_life.cpp`) and its glue in `masseffect_native_targets.cpp`
(`BiasLifeAfterDraw`, `BiasLifeResolve`, `BiasLifeClear`, `ReportBiasLife`). Nothing is recorded on the GPU except the
one-time test of 12.4. Per draw it reads the registers the targets code already has: for each colour slot that writes
an EDRAM view, its blend (ONE/ZERO on the written channels), colour mask, alpha test / alpha-to-mask / kill, render
target exponent bias, proven overwrite rectangle, tile span, and the image's publish write count; for each
pixel-shader sampler, the 2D fetch-constant base address (a read of a tracked texture) and its mag/min filters.

- **Producer class at each bias resolve** (the chain of draws into the source image since the last *full start*: a
  ONE/ZERO, mask F, no-kill draw with a proven rectangle): `strict` (the start alone covers the resolve rectangle),
  `chain` (start, then only non-blended draws, any mask), `blended`, `other` (another view, a clear, or an unseen
  write touched the source tiles), `small` (the start does not cover the rectangle), `no start`. `strict` + `chain`
  with `full cover` is what dual output can replace exactly. A **round trip** is a `strict` resolve whose start draw
  sampled the destination texture's current content (for example `PS3474` restoring the scene from the texture with
  `SCENE_COLOR_BIAS_FACTOR`): if that draw is a pure x8 copy, the resolve writes back exactly what the texture already
  holds, `rop(rop(8 T) / 8) = T` for every half `T`, and could be skipped without any new shader.
- **End of each resolve's life** (until the next resolve or copy into the same address): `unread`, `hand-off`
  (readers exist, none writes the source image, none reads after the source image was written again), `copy
  (reader writes the source)`, `copy (read after the source changed)`; whether all hand-off reads are point-filtered
  (then the hand-off could be exact); whether the source content is **dead** after the resolve (the first later write
  of its tiles, through any view, is a full ONE/ZERO overwrite or a clear of the whole rectangle).
- Approximations, all conservative for the exact classes: a draw's tiles are the whole tile rows of its area;
  redirected depth/stencil clears in `Draw` are not seen; a resolve's sync counts as an import into its source
  whenever that view is not the single owner of the area.

Log lines (every 10 s while the cvar is on):

    [native] bias life, 10 s: N bias resolves (F cover the whole texture); producer: strict S, chain C, blended B, other O, small Z, no start Q; lives ended: unread U, hand-off H (P with point reads only), copy (reader writes the source) W, copy (read after the source changed) K; source content dead after the resolve D / not E; dual output and dead X; round trips T; reader draws R (A lives with an exponent-adjusted fetch); n sources
    [native] bias life producers (chain start or last writer) [strict chain blended other small nostart]: VSa/PSb=[...] ...
    [native] bias life readers [hand-off copy-writer copy-changed]: PSn=[...] ...

`masseffect_native_bias_life_dump_s = T` (with the probe) logs, from T seconds after start, two frames in order: every
`BIAS RESOLVE` (source base/format, rectangle, destination, full cover, producer class), every draw that writes a
bias source (marked `*`) or reads a tracked texture (`VS/PS`, per slot: base, tiles, `direct`/`BLEND`, mask, kill,
proven rectangle; reads with their filter), every colour/depth clear of those tiles, and the end of each life. That is
the "capture B" of post-chain.md for exactly this question.

How to read the numbers (per frame = per 10 s / Swaps):

- `strict + chain` close to the number of resolves with `full cover` -> dual output is worth implementing
  (~0.35 ms each at 1280; ~0.7-1 ms each where `dual output and dead` also counts).
- `hand-off` large, `P` close to it -> an exact hand-off (point reads, emulated rounding) is possible; `hand-off`
  large but `P` small -> only the inexact hand-off of 12.1 (measure it as an image-changing variant).
- `unread` > 0 -> a lazy resolve that drops unread ones (exact, small) is worth it.
- `round trips` about one per frame -> the cheapest exact removal: skip the resolve when its start draw is a pure x8
  copy of the texture (guard: the same VS/PS, the draw's PS constants and vertex colours equal to the learned ones,
  verified by a GPU compare of a shadow resolve against the texture for the first N), ~0.7 ms each at 1280.
- Mostly `blended` / `copy` -> the bias resolves stay; the topic is closed.

### 12.4 The one-time ROP conversion test (runs with the probe)

`me_half_rop_test.h` (models, `tests/cpu/test_native_half_rop.cpp`: the IEEE model equals the host's `_Float16`
conversion on 4 M random values, every half round-trips under every model, the evaluator recognises an IEEE and a
truncating/flushing "GPU"), `RecordHalfRopTest` / `EvaluateHalfRopTest` in the targets code. At the first bias resolve
with the probe on, 20,480 float32 patterns (every half below 2^-11 with its midpoints and ±1 float32 ulp around them,
random full-mantissa normal values of both signs, the overflow boundary, infinities, NaNs, zeros) are uploaded into a
64x80 RGBA32F image and run through the **bias resolve pass itself** with bias 0 and bias -3 into two RGBA16F
targets; both are read back after the slot's fence. One line per pass:

    [native] bias life ROP test, float32 x 2^0 -> RGBA16F attachment: 20480 values compared (0 skipped), mismatches per model: RNE+denormals a; RNE+flush b; RTZ+denormals c; RTZ+flush d; RNE+saturate e; best M[; first mismatches of the best: ...]

A model with 0 mismatches in both passes is what a dual-output epilogue must emulate below 2^-11; no exact model
means the epilogue cannot be exact on this GPU without a further test.

### 12.5 Test configurations (`mass-effect-recomp/run/me1/`, build `ru_nobias`)

| toml | base | added | purpose |
|---|---|---|---|
| `nobias1280.toml` | `s1280all.toml` | `masseffect_native_bias_life_probe = true`, `masseffect_native_bias_life_dump_s = 320` | 1280 cockpit: probe counters, ROP test, ordered dump of 2 frames at 320 s |
| `nobias960.toml` | `tex_base.toml` | the same | the same at 960x544 |

A/B for overhead: the base toml with `ru_nobias` (probe off) against the probe toml; the probe costs a few register
reads per draw and is expected to be within noise. The image cannot change (nothing is recorded differently).

### 12.6 What was not implemented, and why

- **Dual output** (12.1): needs an extra colour attachment in the producing draw's render pass and pipeline (no free
  specialization bit is left: bits 0-31 are all used; the pipeline key is canonicalised for the prewarm list, so a
  marker in `blend[j]` would be lost), a SPIR-V transform that adds an output and stores `e(x)`, a scratch image
  swapped into the resolved texture at the resolve, learning of the producer sites, and a GPU compare of the scratch
  image against a shadow resolve. That is several days in `masseffect_native_draws.cpp` (which another change is
  editing) for at most ~0.35-0.7 ms per frame if 12.2 is right. Do it only if the probe shows `strict + chain`
  resolves, with the model from 12.4.
- **Hand-off**: not exact for filtered reads (12.1), needs an out-of-spec aliased image or a coordinate change in
  every reader, and saves only the resolves whose readers neither write the source nor come after it changed
  (probably the one before `PS15729`). It is an image-changing variant at best; the probe gives its size.
- **Lazy resolve that drops unread resolves**: reads per logical resolve are ~1.03, so it would remove nothing.
- **Round-trip elision**: depends on what `PS3474` really computes (texture x vertex colour x optional gamma x bias
  factor); it needs the dump to confirm the pattern first, then a guard on its constants and vertex colours.

### 12.7 Summary

The bias resolves are mostly in the scene phase: their sources are written by blended draws (lights, translucency)
and their readers blend into the same source, so a copy of the source is needed there whatever the mechanism; no
exact way removes them. Exactly removable at most: the resolve after a full non-blended producer (dual output,
~0.35 ms each), a round trip (~0.7 ms each), unread ones (none expected). The inexact hand-off would add the resolve
read by `PS24056`/`PS15729` (~0.7 ms, differences only from texels below 2^-11). One console run of `nobias1280.toml`
(and `nobias960.toml`) with `ru_nobias` gives the counts and the ROP conversion model; implement whichever of 12.3's
rules fires, otherwise close the topic.
