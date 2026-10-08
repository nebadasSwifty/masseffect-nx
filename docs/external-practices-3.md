# External practices, part 3: occlusion latency, render-thread sync, NAK on SM50, correctness leads

Date: 2026-10-07. Desk research only. Sources: public web pages, the Mesa GitLab API (public merge requests and issues),
the local read-only checkouts of Xenia Canary (`work/xenia-canary-upstream`, origin
`github.com/xenia-canary/xenia-canary`) and of the Switch Mesa tree (`work/mesa-switch-main`, base Mesa 26.2.3,
head `c3cb40a`), a shallow clone of deko3d in the scratchpad, and the generated code in
`mass-effect-recomp/generated/default`. Nothing was built or run on the console. Every "win" below is an unmeasured
estimate used only for ranking.

Parts 1 and 2 ([external-practices.md](external-practices.md), [external-practices-2.md](external-practices-2.md))
already cover: Xenia's ME1 patches (MSAA off, blur, vsync byte), FXAA, tile merging, fp16 in NAK, compression checks,
SIMDe `shuffle_epi8`/`dp_ps`, FSI/ROV, descriptor buffers, the dead `Coalesced.ini` keys, real occlusion queries
(steps A-C), the one-frame-lag hook, the ring size, `MinDesiredFrameRate`, GC, the texture pool, audio, skeletal and
light-environment throttling, memory clock, PhysX step and the location list. They are not repeated here, except where
this part adds a new source or a new detail.

Labels: **[confirmed]** = read in the source or the cited page. **[speculation]** = inference, needs a console test.

## 1. Occlusion queries: Xenia's 2026 "fast" (speculative) mode

- **What it is [confirmed].** Xenia Canary implemented real `EVENT_WRITE_ZPD` occlusion queries in May 2026 and
  rewrote them in September 2026 as a running sample counter. The default cvar is `occlusion_query = "fast"`:
  - `fake`: a fixed result without asking the GPU (what our mode 0 does);
  - `fast` (default): at parse time it writes a **guess** into guest memory, so the guest never sees the pending
    sentinel. The guess is the last resolved delta for the same report address if it was non-zero, otherwise "visible"
    (one sample). When the GPU result lands, Xenia patches the real value into guest memory and re-bases later guesses;
  - `fast-alt`: same, but also replays cached zeroes. The source comment says this helps some titles' effects but
    "stale zeroes tend to break occlusion culling tests, resulting in popping primitives";
  - `strict`: waits for the real result (what our mode 2 does, with a 50 ms timeout).
  - When the query pool is exhausted, the fast modes report at least one passing sample instead of waiting.
- **Sources.**
  - [fbd620c "Implement ZPD occlusion queries"](https://github.com/xenia-canary/xenia-canary/commit/fbd620c22b44638b66a70bba80d6f30d55a10924) (2026-05-01).
  - [3d233a5 "Rewrite ZPD as a running sample counter"](https://github.com/xenia-canary/xenia-canary/commit/3d233a5b2e94b940825847b70c788951e364bb33) (2026-09-04).
  - `src/xenia/gpu/command_processor.cc` lines 53-67 (cvar text) and `QueueZPDReport` (around line 895).
  - The Xenia compatibility report for ME1 carries the `gpu-occlusion-query` label:
    [xenia-canary/game-compatibility#154](https://github.com/xenia-canary/game-compatibility/issues/154).
  - Epic documents that UE reads occlusion results back one frame later, accepting "pop" on fast camera turns:
    [Visibility and occlusion culling](https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine).
- **Why it applies here.** Our mode 2 ([occlusion-queries.md](occlusion-queries.md)) is the `strict` design: the end
  structure keeps the sentinel until the slot's fence signals, and the UE3 render thread spins in `sub_826E7C98`.
  Our ring runs about 800 records behind the game and submits at the Swap, so a strict query is usually not done when
  the render thread asks. That is why mode 2 needs early submissions (`query_flush_us`) and a timeout. Both cost CPU
  and submissions, in a frame that already has an ~8.7 ms CPU gap. A fast mode removes the spin completely. Culling
  still happens: UE3 reads a query a frame or more after issuing it, and by then the real value has usually been
  patched in.
- **Estimated win.** CPU: it removes the GetData spin and the early submissions. Before the 500 us wait hook, the
  poll was about 9 % of the render thread at stock, which is about 1 ms per frame at 1785 MHz. Mode 2 may add more.
  GPU and ring: the same draw reduction as mode 2 in interiors, without its latency cost. **[speculation]**: 1-3 ms
  CPU, plus whatever mode 2 saves in draws.
- **Risk to the image.** Low. Unknown results mean "visible". A primitive is only hidden when a real 0 arrives before
  the game reads the result. Do not replay zeroes (`fast-alt`): Xenia reports popping with that.
- **Next step.** Add `masseffect_native_query_mode = 3` in `me_native_system.cpp` / `masseffect_native_targets.cpp`.
  - At the end ZPD: compute the guess from a small map keyed by the end-structure guest address (last non-zero
    resolved count, else 1000). Write it at once through `WriteOcclusionCounts`, which clears the sentinel.
  - Keep the per-draw Vulkan queries of mode 2. In `Complete`, overwrite the guest words with the real count unless
    the query was reissued (the "superseded" logic exists already), and update the map.
  - No early submission and no timeout in this mode.
  - A/B against mode 0 with skip boxes. Watch the draw count per Swap, the render thread's samples in `sub_826E7C98`,
    and pop-in during fast camera turns in the Citadel and Normandy.

## 2. Game/render thread sync: outside confirmation for the one-frame-lag hook

- **What it is [confirmed].**
  - Microsoft's Xbox 360 multicore guidance describes the standard design: the update thread runs one frame ahead of
    the render thread, so input shows up two frames later
    ([Coding for multicore on Xbox 360 and Windows](https://msdn.microsoft.com/ko-kr/library/ee416321.aspx)).
  - UE's `FFrameEndSync` "allow[s] one frame of lag between the game and the render thread by using two events in
    round robin fashion" ([UE 4.26 API](https://docs.unrealengine.com/4.26/API/Runtime/Engine/FFrameEndSync/index.html)).
  - UE5's `r.OneFrameThreadLag` (on by default) keeps the game thread from getting more than one frame ahead of the
    render thread. The page notes that in the old model "the game thread synced with the rendering thread at the end of
    the frame" ([Low latency frame syncing](https://dev.epicgames.com/documentation/en-us/unreal-engine/low-latency-frame-syncing-in-unreal-engine)).
  - `FRenderCommandFence` is the primitive: `BeginFence`, then `Wait` or poll `GetNumPendingFences`
    ([Rendering thread](https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine)).
- **Why it applies.** `sub_822FE760` (`while (*counter > n) Sleep(0)`) has exactly the shape of
  `FRenderCommandFence::Wait`. The two-fence round robin is what `FFrameEndSync` does when lag is allowed. If ME1's
  frame-end call waits for the fence it has just issued (n = 0), the 2007 build runs fully synced, and the main thread
  (~18.6 ms) and the render thread (~11.6 ms) take turns instead of overlapping. No public UE3 source was found that
  states the 2007 default. This is the part 2 item 1.2. The new information is the outside evidence that one frame of
  lag is the designed, shipped behaviour of this engine family **[confirmed for UE4/5; speculation for ME1's build]**.
- **Estimated win.** If ME1 is fully synced today: up to min(main, render) ≈ 8-11 ms of overlap in CPU-bound views.
  In practice, the part of the 8.7 ms CPU gap that comes from the main thread waiting.
- **Risk to the image.** Low to medium: one more frame of input latency; possible one-frame flicker if 2007 game code
  touches render data on the assumption of a full sync.
- **Next step.** Read-only first: find the call sites of `sub_822FE760` and log `n` and the fence counter at each
  call once per second. If the frame-end site waits for the newest fence, add a hook that keeps two fence slots and
  waits for the older one (the `FFrameEndSync` pattern), behind `masseffect_frame_lag = 1`.

## 3. UnleashedRecomp's present path: wait on the frame before last, late

- **What it is [confirmed].** UnleashedRecomp `UnleashedRecomp/gpu/video.cpp`
  ([source](https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecomp/gpu/video.cpp)):
  - `NUM_FRAMES = 2` (line 302). `Video::Present` waits only for the command fence of the frame slot it is about to
    reuse (`g_queue->waitForCommandFence(g_commandFences[g_frame])`, around line 2835), not for the frame just
    submitted;
  - the swap-chain wait is deferred. `WaitOnSwapChain` is called only when the next frame first needs the back buffer
    (comment block before line 2773);
  - all D3D calls go into a `moodycamel::BlockingConcurrentQueue` consumed by a render thread (part 1 already notes
    this).
- **Why it applies.** Our UE3 render thread spends 6-8 ms (stock) in the guest Swap `82234000`, waiting on the ring
  (`8222C768`). The ring retires fences at parse time (C25), so this wait is ring throughput, not GPU completion. The
  pattern says what to wait on, and when:
  - the guest Swap should block only when the ring is more than one whole frame behind;
  - any back-buffer or present wait should happen when the next frame needs the image, not at the Swap.
- **Estimated win.** It does not reduce work; it turns a serial wait into overlap. **[speculation]**: 0-4 ms of the
  CPU gap, only in views where the ring thread is not already the bottleneck.
- **Risk.** None to the image; +1 frame of latency at most.
- **Next step.** Log, per Swap, the time spent in `8222C768` and the ring's record lag at entry. If the wait covers
  more than one frame of records, give the Swap wait a "one frame in flight" target, together with the part 2 ring
  size test (`RBSecondarySize`).

## 4. NAK on SM50: microcoded IMUL in our EDRAM transfer shaders

- **What it is [confirmed].** Mesa MR [!43389 "nak/sm50: XMAD integer-multiply lowering"](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43389)
  (closed, not merged, 2026-09-18; successor of [!42977](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/42977)):
  - "the legacy IMUL opcode NAK currently emits is microcoded, slow, and variable-latency. `ptxas` never emits IMUL on
    `sm_50`." Maxwell's native integer multiply is XMAD (16x16+32);
  - measured on a GM108: a compute VP9 decoder went from 19.9 to 35.0 fps (1.76x), bit-exact;
  - shader-db: compute shaders -10 to -23 % static cycles, but "typical non-compute-heavy vertex/fragment shaders" +15
    to +24 %, because three dependent XMADs cost more than one IMUL when multiplies are rare.

  Our local NAK (`src/nouveau/compiler/nak/sm50.rs:1406`, `impl SM50Op for OpIMul`) still emits IMUL (opcodes
  0x5c38/0x3838/0x4c38/0x1fc0), and nothing in the tree has XMAD.
- **Why it applies.** `edram_alias` runs at about 0.3 Gpixel/s, an order of magnitude below the GPU's fill rate
  ([gpu-cost-analysis.md](gpu-cost-analysis.md) section 1). That points to ALU or latency, not bandwidth.
  `shaders/me_edram_color_to_color.frag` avoids the runtime division (`DivPitch` in float), but per pixel it still has:
  - `physical.x / 80u` and `physical.x % 80u`. Division by a constant becomes an `umul_high`, which is IMUL.HI on SM50;
  - `(physical.y / 16u) * c.pitch_tiles_target`: a register IMUL;
  - `ModPitch(...) * 80u`, `DivPitch(...) * 16u` (the 16 becomes a shift) and `DivPitch(...) * pitch`: IMULs.

  The compute variants (`masseffect_edram_*.comp`, `me_edram_*.comp`) likely have the same shape **[speculation:
  not disassembled]**.
- **Estimated win.** Unknown until the shaders are disassembled. If each conversion has 3-5 IMUL/IMUL.HI per pixel and
  is ALU-bound, removing them could save a sizeable share of the 14-26 ms `edram_alias` **[speculation]**. If the
  passes are latency-bound on texel fetches, the gain is near zero.
- **Risk to the image.** None: integer math stays exact.
- **Next step (cheap, no compiler work).**
  1. Dump the NAK disassembly of the transfer shaders (`NAK_DEBUG=print`, or `NVK_SHADER_STATS=1` for counts) on the
     Mac host build of Mesa, and count IMUL.
  2. Rewrite the shader-side math without integer multiplies:
     - `x * 80` as `(x << 6) + (x << 4)`;
     - `x / 80` with the same exact float trick as `DivPitch` (`x < 2^16` here);
     - the run's `pitch_tiles` products precomputed per row in the vertex shader, or passed as push constants per run;
     - or the run's base tile passed in, so the per-pixel tile index needs only adds.
  3. Only if IMUL is still hot: port !43389 behind an env var (for example `NVK_SWITCH_XMAD=1`), and apply it only
     to our transfer shaders, given the +15-24 % regression for ordinary VS/FS.

## 5. NAK on SM50: operand reuse, dual issue, register banks (unmerged)

- **What it is [confirmed].** Mesa MR [!43390](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43390)
  (closed 2026-09-19) adds three SM50 passes:
  - set the Maxwell operand-reuse flags. The field exists but "nothing ever set it". In our tree, `InstrDeps::add_reuse`
    is `#[allow(dead_code)]` (`ir.rs:8745`) and `sm50.rs:246` encodes an always-zero mask;
  - dual-issue ALU+LSU pairs (NAK "always stalled at least one");
  - spread co-read values over the 4 GPR banks.

  The author measured **no** change on a compute VP9 workload.
- **Why it might apply.** The scene is vertex-bound (M7, M8). The translated Xenos shaders are long and ALU-dense with
  many constant reads, which is where register bank conflicts and the lack of dual issue hurt most. That workload is
  different from the one measured.
- **Estimated win.** Unknown, probably small (0-5 % of shader time) **[speculation]**.
- **Risk.** Compiler correctness. Dual issue and the reuse flags must follow Maxwell's control-code rules, so a
  bit-exact image test is needed.
- **Next step.** Low priority. Cherry-pick the three commits from the author's fork (`postadelmaga/mesa`, branch
  `nak-sm50-scheduling-v1`) onto the Switch tree behind an env var, and measure scene ms in `GPU per Swap` on Eden
  Prime grass.

Related, also not merged: [!43373 "bound textures Maxwell/Pascal"](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43373)
and issue [mesa#13998](https://gitlab.freedesktop.org/mesa/mesa/-/issues/13998). Pre-Turing texture handles come from
the descriptor cbuf, and a bound path needs all textures in one cbuf. The shader-db effect in the MR is tiny (-0.8 %
GPRs in affected shaders), so skip it.

## 6. NVK on Maxwell: checked, nothing new to do

- **ZCULL** landed upstream in Mesa 26.1 ([Phoronix](https://www.phoronix.com/news/NVK-ZCULL-Merged), MR !33861:
  +3 % in Horizon Zero Dawn, BG3 31 to 33 fps). Our tree has ZCULL, and S24 measured no gain because the scene is
  vertex-bound. Do not retry.
- **UBO constants on pre-Turing.** `nvk_nir_lower_descriptors.c` (`build_cbuf_map`) does not promote non-push
  `UNIFORM_BUFFER` descriptors to hardware cbufs before Turing, except in Switch fragment shaders. Our draw constants
  use `UNIFORM_BUFFER_DYNAMIC` (`masseffect_native_draws.cpp:7632`), and those **are** promoted (`NVK_CBUF_TYPE_DYNAMIC_UBO`).
  So the vertex shaders already read their constants with LDC, not global loads. Keep it that way. Any new UBO binding
  for vertex shaders should be dynamic or push, not a plain descriptor. NVIDIA also notes that structured or storage
  buffers are slower than constant buffers on Pascal and older
  ([NVIDIA, Advanced API performance: descriptors](https://developer.nvidia.com/blog/advanced-api-performance-descriptors)).
- **Compression / ZBC reference.** deko3d (the native Switch homebrew API,
  [github.com/devkitPro/deko3d](https://github.com/devkitPro/deko3d), `source/maxwell/image_formats.cpp`
  `pickImageMemoryKind`) maps compressed images to explicit PTE kinds:
  - colour: `C32_2CRA`, `C32_MS2_2CRA`, `C32_MS4_2CBR`;
  - depth: `Z24S8_2CZ` and its MS variants;
  - `Z16_2C` when zero-bandwidth clear is wanted.

  It also decompresses a compressed image before presenting it (`dk_swapchain.cpp`). Use these as the expected values
  when doing the part 1 #5 check (log `pte_kind`/`compressed_pte_kind` of the EDRAM views, 64 bpp and MS2 ones most of
  all).
- **Maxwell tiled caching.** Maxwell rasterises in screen tiles, and the tile size adapts so that the tile's pixel
  output fits an on-chip buffer ([AnandTech](https://www.anandtech.com/show/10536), [PCPer on RealWorldTech's test](https://pcper.com/2016/08/tiler-exposed-in-maxwell-pascal-architectures/)).
  Fatter attachments (RGBA16F + D24S8 at 2x) mean smaller tiles and more DRAM traffic. This is one more argument for
  the part 1 #1 MSAA-off variant, not a separate action **[speculation]**.

## 7. Xenia render target cache, 2026 changes worth reading

All in the local Xenia checkout, commits on `github.com/xenia-canary/xenia-canary`:

| Commit | What | Why it matters here |
|---|---|---|
| [9da6934](https://github.com/xenia-canary/xenia-canary/commit/9da693480d0995326b81c3a14f8b1a4c226066eb) "EDRAM bits respected for color/depth aliases" | When a colour target aliases the depth/stencil EDRAM range, Xenia keeps depth enabled if the colour writes do not overlap the tested bits, and tracks which host RT owns the depth bits per range. It fixes "clouds and other sprites staying visible through geometry" in 4D530A26. | **Correctness lead** for the black shards on Eden Prime terrain (B1), which are tied to the 2x/1x depth round trip. Check whether ME1 writes colour into the scene depth range (stencil byte) between depth passes. |
| [437a728](https://github.com/xenia-canary/xenia-canary/commit/437a7280cf95310d518a2f68087aab61403956ac) "single sample addressing scheme" | 1x/2x/4x views share one sample layout. For 2x: `u = (x & ~2) \| (s << 1)`, `v = ((y & ~1) << 1) \| (y & 1) \| (x & 2)`. Transfers are built for all six direction pairs from canonical coordinates. | Compare with our `me_edram_depth_msaa2_to_depth_1x` / `1x_to_depth_msaa2` layouts. A mismatched 2x/1x sample mapping gives exactly the "shards" shape. |
| [3ff230d](https://github.com/xenia-canary/xenia-canary/commit/3ff230d23be454b39a9a2904ad0d8a5156e3aa10) "extended-range float16" | Xenos 16-bit float with exponent 31 is finite (up to 131,008), not Inf or NaN. Packing used to clamp, and unpacking returned Inf. | Applies only if ME1 uses `k_16_16_16_16_FLOAT` targets. Our 7e3-as-RGBA16F path is unaffected (7e3 max is 31.875). Check the 16F formats of the light accumulation and bloom targets. |
| [74db632](https://github.com/xenia-canary/xenia-canary/commit/74db632ab32e179dd0d6858b51caae07fc444949) `draw_resolution_scale_threshold` | Resolution scale per surface, chosen by pitch: small-pitch post chains render at 1x while large surfaces scale. | The same idea as our 960 scene with full-pitch post. A reference for the 1280 dynamic-resolution plan: scale chosen per surface class, and ZPD counts normalised per segment. |

## 8. Mass Effect 1 specific

- **Light environment NaNs (correctness) [confirmed on PC].** SilentPatch for ME1 traced the PC "black blobs" on
  **Noveria and Ilos**:
  - `D3DXMatrixInverse` (SSE2 path) gave slightly different results per CPU, and could leave its output uninitialised
    on near-singular input;
  - the resulting NaNs reached the pixel shader constants `UpperSkyColor` / `LowerSkyColor` (c10, c11), which drive
    the light environment;
  - the fix swaps in DirectXMath `XMMatrixInverse`.

  Sources: [Silent's blog](https://silentsblog.com/2020/07/19/silentpatch-mass-effect/),
  [SilentPatchME](https://github.com/CookiePLMonster/SilentPatchME),
  [OSnews summary](https://www.osnews.com/story/132094/fixing-mass-effect-black-blobs-on-modern-amd-cpus/).

  **Why it applies:** the light environment code is shared with the 360 build, and on our port its VMX/FPU math runs
  through SIMDe with explicit flush-mode switches. A different denormal or rounding outcome in the 360's inverse
  routine could produce the same NaN, and black characters or terrain are already on our bug list. **Next step:** in
  a debug build, check the PS constant upload for NaN or Inf on registers c10 and c11 (and log the shader and draw),
  then walk through Noveria (`BIOA_ICE00`) and Ilos (`BIOA_LOS00`). If NaNs show up, find the guest matrix-inverse
  routine by its callers and compare its output against the Mac build. Cost: none at runtime when disabled. Risk: none.
- **Virmire FPS collapse after the salarian prisoners [reported].** Players report the frame rate dropping to about
  8 fps after killing the indoctrinated salarians, "also on the original ME1 Xbox", recovering only after leaving the
  area. A corpse model is suspected
  ([EA forum thread](https://forums.ea.com/discussions/mass-effect-franchise-discussion-en/bug-me1-framerate-drop-after-killing-docile-salarians-on-virmire/9081489/replies/9081493)).
  For the acceptance sweep: if Virmire's lab dips below 25 fps, first reproduce on the reference (Xenia or a console
  video) before treating it as a port regression. The corpse-cleanup keys in part 2 are the natural lever.
- **Scaleform UI.** For ME2, BioWare says they rewrote "the pre-vis phase... in Scaleform", among the inefficient
  processes they found ([MCV, Epic diaries: Mass Effect 2](https://mcvuk.com/development-news/epic-diaries-mass-effect-2/)).
  ME1's UI therefore likely has the slower version. Watch for UI-heavy screens (galaxy map, inventory, dialogue wheel)
  in the main-thread profile **[speculation]**.
- **Xenia patch list re-checked.** The current ME1 patch file has only: 60 FPS, Black Shading Fix, Skip Intro Movies,
  16x AF, Disable Coalesced hash check, Show Frametime
  ([patch file](https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307E8%20-%20Mass%20Effect%20(USA%20Rev%201).patch.toml)).
  All are covered in part 1. The ME2 file adds lens flare, bloom, DOF and motion-blur toggles, which have no ME1
  addresses.

## 9. Static recompilation on ARM64: cost facts for part 1 #6

- **Cortex-A57 costs [confirmed, LLVM scheduling model].** From
  [AArch64SchedA57.td](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AArch64/AArch64SchedA57.td):
  - `TBL v16i8` with one register: 6 cycles, 3 V-pipe uops; with two registers: 9 cycles, 5 uops. This is what
    SIMDe's `_mm_shuffle_epi8` becomes; a byte swap is a single `REV32`;
  - `FADDP v4f32`: 9 cycles, 3 uops. This is the reduction step of `_mm_dp_ps`;
  - `FDIV v4f32` and `FSQRT v4f32`: 34 cycles each.

  So the 9,799 `shuffle_epi8` and 4,355 `dp_ps` sites in our generated code are the expensive ones on this core, as
  part 1 #6 suggested.
- **`vrefp` / `vrsqrtefp` are exact here [confirmed].** `rexglue-sdk/src/codegen/builders/vector.cpp:128-141` emits
  `1/x` and `1/sqrt(x)` as `simde_mm_div_ps` (and `sqrt_ps`), about 68 cycles for a vector rsqrt. There are only
  ~55 sites, so the CPU gain is small. Keep it exact: the light environment NaN story above argues against swapping
  in a 12-bit estimate.
- **Fences.** The generated code has 529 `atomic_thread_fence(seq_cst)` and 120 `acq_rel`. Both are `dmb ish` on
  AArch64. They are low priority unless a profile shows `dmb` samples in a lock-heavy path.
- **XenonRecomp notes [confirmed].** The register-as-local options "reduced [frame times] by several milliseconds" in
  Unleashed. XenonRecomp marks every load and store volatile, which ours no longer does. It handles VMX on ARM64 through
  SIMDe ([README](https://github.com/hedge-dev/XenonRecomp)). No public Xbox 360 recompilation ships an ARM64 build
  yet, so there is no outside ARM-specific lesson to copy. The ReXGlue docs list `-ffp-model=strict` on all
  platforms ([mintlify](https://mintlify.wiki/rexglue/rexglue-sdk/advanced/optimization)). Our GCC Switch build uses
  `-ffp-contract=off` instead (`app/CMakeLists.txt:121`), which keeps FP optimisation intact. Do not switch to a
  strict model.

## 10. Ranked top 10

| # | Item | Section | Expected win | Image risk | First step |
|---|---|---|---|---|---|
| 1 | Speculative ("fast") occlusion results, mode 3 | 1 | CPU 1-3 ms plus mode 2's draw cut, without its stalls | low (visible bias) | map of the last non-zero count per guest address; write at parse, patch on completion |
| 2 | One-frame lag at the frame-end fence (outside confirmation) | 2 | up to 8-11 ms overlap in CPU-bound views | low-medium | log `n` at the `sub_822FE760` call sites |
| 3 | Remove IMUL / IMUL.HI from the EDRAM transfer shaders | 4 | unknown, potentially several ms of `edram_alias` | none | NAK disassembly of the transfer shaders; shift/add and float-exact rewrites |
| 4 | Swap waits for "one frame in flight", not for the ring to drain | 3 | 0-4 ms of the CPU gap | none | log the time in `8222C768` and the ring lag per Swap |
| 5 | NaN guard on PS c10/c11 + Noveria/Ilos walk | 8 | correctness (black characters/terrain) | none | debug check on the constant upload |
| 6 | Xenia colour/depth alias bits and the 2x sample layout vs our depth transfers | 7 | correctness (B1 black shards) | none | compare the 2x<->1x layouts with commit 437a728 |
| 7 | XMAD lowering only for our transfer shaders (port !43389 behind an env var) | 4 | depends on #3 | compiler risk | only if IMUL is still hot after #3 |
| 8 | SIMDe `shuffle_epi8` -> `REV32`, `dp_ps` -> fixed-lane NEON (now with A57 numbers) | 9 | low-medium CPU | none if bit-exact | profile `tbl`/`faddp` samples first |
| 9 | NAK operand reuse / dual issue / bank spreading (!43390) | 5 | 0-5 % of shader time | compiler risk | cherry-pick behind an env var, image diff |
| 10 | Acceptance sweep notes: Virmire salarian dip exists on the 360; Scaleform UI screens | 8 | test hygiene | none | reproduce on the reference before blaming the port |

Checked and not worth doing: NVK ZCULL (S24 already measured, no gain), UBO cbuf promotion (already right for our
dynamic UBOs), bound textures pre-Turing (!43373, tiny), approximate `vrefp`, a strict FP model.

## Sources

- Xenia Canary ZPD commits: https://github.com/xenia-canary/xenia-canary/commit/fbd620c22b44638b66a70bba80d6f30d55a10924 , https://github.com/xenia-canary/xenia-canary/commit/3d233a5b2e94b940825847b70c788951e364bb33
- Xenia Canary RT cache commits: https://github.com/xenia-canary/xenia-canary/commit/9da693480d0995326b81c3a14f8b1a4c226066eb , https://github.com/xenia-canary/xenia-canary/commit/437a7280cf95310d518a2f68087aab61403956ac , https://github.com/xenia-canary/xenia-canary/commit/3ff230d23be454b39a9a2904ad0d8a5156e3aa10 , https://github.com/xenia-canary/xenia-canary/commit/74db632ab32e179dd0d6858b51caae07fc444949
- Xenia ME1 compatibility: https://github.com/xenia-canary/game-compatibility/issues/154
- Xenia ME1 patches: https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307E8%20-%20Mass%20Effect%20(USA%20Rev%201).patch.toml
- Epic, visibility and occlusion culling: https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine
- Epic, low latency frame syncing: https://dev.epicgames.com/documentation/en-us/unreal-engine/low-latency-frame-syncing-in-unreal-engine
- Epic, rendering thread: https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine
- Epic, FFrameEndSync (UE 4.26): https://docs.unrealengine.com/4.26/API/Runtime/Engine/FFrameEndSync/index.html
- Microsoft, coding for multicore on Xbox 360: https://msdn.microsoft.com/ko-kr/library/ee416321.aspx
- UnleashedRecomp video.cpp: https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecomp/gpu/video.cpp
- Mesa MRs: https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/43389 , /42977 , /43390 , /43373 ; issue https://gitlab.freedesktop.org/mesa/mesa/-/issues/13998
- NVK ZCULL: https://www.phoronix.com/news/NVK-ZCULL-Merged
- NVIDIA, descriptors: https://developer.nvidia.com/blog/advanced-api-performance-descriptors
- deko3d: https://github.com/devkitPro/deko3d
- Maxwell tiled caching: https://www.anandtech.com/show/10536 , https://pcper.com/2016/08/tiler-exposed-in-maxwell-pascal-architectures/
- SilentPatch ME: https://silentsblog.com/2020/07/19/silentpatch-mass-effect/ , https://github.com/CookiePLMonster/SilentPatchME , https://www.osnews.com/story/132094/fixing-mass-effect-black-blobs-on-modern-amd-cpus/
- Virmire FPS drop: https://forums.ea.com/discussions/mass-effect-franchise-discussion-en/bug-me1-framerate-drop-after-killing-docile-salarians-on-virmire/9081489/replies/9081493
- MCV, Epic diaries ME2: https://mcvuk.com/development-news/epic-diaries-mass-effect-2/
- LLVM A57 scheduling model: https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AArch64/AArch64SchedA57.td
- XenonRecomp: https://github.com/hedge-dev/XenonRecomp
- ReXGlue optimization docs: https://mintlify.wiki/rexglue/rexglue-sdk/advanced/optimization
