# Scene shader cost on GM20B: texture signs folded into the pixel shader

2026-10-07. Offline analysis (Mac, no console) of the shaders behind the heaviest GPU intervals of the
Normandy cockpit run (Russian edition, 960x544, GPU 768 MHz), and the change it led to:
`masseffect_native_fold_texture_signs` (default **off**). Nothing here was measured on the Switch yet.

## What the "GPU time by pass" labels really are

The report line `GPU time by pass (category, first draw's shaders)` sums the time between two GPU marks.
A mark is written only when the pass **category** changes (`MarkGpu` returns early while the category stays
the same), and the label is the shaders of the **first draw** after the mark. At 960x544 the scene targets
have pitch 960, so the whole scene (geometry, lights, post) is category 3 ("640"). So:

| Label (cockpit, 10 s) | What the labelled draw is | What the time is |
|---|---|---|
| cat 3 VS12867 PS26880: 1403-1618 ms | VS12867: skinned position-only vertex shader (bone loop, no other outputs). PS26880: 10 NAK instructions, no texture, writes `oC0.x` and `clip()`s on a constant: a depth/stencil-style draw | the whole run of category 3 draws that starts with it (~5.6 ms/frame); not this shader pair |
| cat 3 VS2013 PS14489: 312-993 ms | VS2013: skinned VS with tangent frame and lights (126 `if`s, 29.7 k SPIR-V words). PS14489: a per-light additive pass (LightAttenuationTexture + 2 material textures, Phong specular) | the run of draws that starts with that light |
| cat 4 VS24994 PS24056: 483-536 ms | PS24056: 16 + 16 fetches of SceneColor and SceneDepth (a blur/DoF gather into a 320-pitch target) | mostly this pass |
| cat 3 VS2013 PS14702, PS5939 | per-light passes with 6 textures | |
| cat 3 VS2293 PS15729 | BlurredImage + SceneColor + SceneDepth composite (DoF) | |

`GPU intervals by category` in the same log: ~14 category-3 intervals per frame, 18.8 ms of the 33.4 ms
GPU frame. To rank the pixel shaders themselves, run once with `masseffect_native_stats_per_draw_s = 5`
(one frame every 5 s with one query per draw; prints `#n PS n<number> cat <c>: <ms>/frame, <draws>, Mfrag,
ns/frag` lines). There is no such ranking for ME1 yet.

## Where the pixel shader instructions go

Shaders were taken from the console's library (29,241 entries, same numbering as the log) into a private
scratch folder, translated again with `out/tools/xenos_hlsl` for reading, and compiled with the
source-matched host NAK (`work/me2-codex-native-cost/out/mesa-gm20b-host-pregen/src/nouveau/vulkan-host/nak-gm20b-cost`,
SM53, heaps 4096/16/64/512, robustness 1/1/0, null descriptors on, specialization word 0x300 = constants UBO +
1/size by constant, one colour target). Counts are NAK's post-scheduling instructions; cycles are NAK's
static estimate (it sums both sides of every branch and weighs texture latency), not GPU time.

Every texture fetch helper of `shaders/XenosRecomp/shader_common.h` ends in
`masseffectApplyTextureSigns(value, index >> 24)`: a loop over the four components that tests the 2-bit
TextureSign (2 = `x*2-1`, 3 = piecewise-linear gamma). The signs come from bits 24-31 of the descriptor index
in the shared constants, so they are uniform per draw but unknown to the compiler. NAK unrolls the loop and
keeps, per component, two uniform branches (each `ssy`/`bra`/`sync` with 13-cycle scheduling delays) and
both bodies. That is most of the shader:

| PS | fetches | base | signs folded, all 0 (unsigned) | all 0x2A (biased RGB) | all 0x3F (gamma RGB) | all 0xFF (gamma RGBA) |
|---|---|---|---|---|---|---|
| 26880 | 0 | 10 / 49 | unchanged | | | |
| 14489 | 3 | 631 / 3775 | **194 / 2012** (-69 %) | 212 / 1991 | 404 / 2703 | 430 / 2798 |
| 14702 | 6 | 1122 / 6258, 40 GPRs | **298 / 2214**, 32 GPRs (-73 %) | 333 / 2527 | 715 / 4389 | 741 / 4487 |
| 5939 | 6 | 1164 / 6695, 40 GPRs | **291 / 2686**, 32 GPRs (-75 %) | 329 / 3004 | 740 / 4488 | 766 / 4591 |
| 15729 | 3 | 546 / 3376 | **153 / 1358** (-72 %) | 167 / 1592 | 335 / 2090 | 361 / 2138 |
| 24056 | 32 | 3566 / 23605, 56 GPRs, 36 warps | **673 / 2008**, 64 GPRs, 32 warps (-81 %) | 797 / 7144 | 2333 / 15685 | 2333 / 15685 |
| 3474 | 1 | 259 / 1722 | **66 / 852** (-75 %) | 72 / 851 | 144 / 1226 | 169 / 1531 |

(instructions / static cycles; a column applies the same sign byte to every fetch, the real draw has one
byte per texture.) Library-wide, 150 random pixel shaders: 945 instructions on average, 246 with all signs 0
(-74 %), 586 with gamma on RGB everywhere (-38 %). Render-target textures (scene colour, depth, light
attenuation, blur buffers) are unsigned, so post passes like 24056 and 15729 should land in the first column.

What is left once folded: the gamma decode itself is ~25 NAK instructions per gamma component (three
compares, selects of immediates materialised with `mov`, `trunc`); the descriptor index handling is ~10 per
fetch (mask, clamp, `ldc` of the bindless handle). Nothing else in these shaders stood out: no loops other
than the sign loop, no dynamic branches except the constant-buffer `if (MaxBoneInfluences ...)` chain in the
skinned vertex shaders, no redundant fetches (24056 really samples 32 distinct offsets). FP16 was not
evaluated.

The vertex shaders cannot be measured with this tool (fragment only). They have the same pattern for the
vertex input remap (`remapInput(value, g_InputRemap(n))`, a 4-component loop on a per-draw code), but the
identity code takes a fast path already (`masseffect_native_normalized_remap`); folding it would need the
remap codes in the key (they are not part of `VerticesEntry::fingerprint`).

## The change

`masseffect_native_fold_texture_signs` (bool, default false, re-read once per frame):

* **Key** (`masseffect_native_draws.cpp`): after the sampler loop of a draw, for the first eight fetch
  registers (< 16) the pixel shader samples, in ascending order (`SignedRegistersPS`), the sign byte
  (`shared[heap * 16 + reg] >> 24`) and the heap it is bound to go in `PipelineKey::signs_low`,
  `signs_high` and `signs_heaps` (the old explicit padding word `fill` became `signs_low`; the key is
  88 bytes). Specialization bit 23 (`kSpecSignsFolded`, a bit shader_common.h leaves to the app) marks it.
  Only with `SPEC_CONSTANT_CONSTANTS_UBO`; without the setting all three words are 0 and the bit is clear,
  so keys are what they were.
* **Module** (`me_texture_signs_spirv.h`, `ModuleTextureSigns`): on the selected material module (plain,
  7e3, alpha-only or early-Z), before the FragCoord and depth transforms, every
  `OpShiftRightLogical %uint %index %24` whose `%index` traces back (OpPhi / OpBitcast / OpLoad /
  OpCopyObject / constant OpAccessChain) to a word of the shared block (set 4, binding 2) that the key
  carries becomes `OpCopyObject %uint <constant>`. NVK's NIR then folds the loop and the branches. The value
  is what the shader would have read, so the image is identical by construction. Untraceable shifts and
  registers beyond the eighth stay dynamic. A failed or empty fold returns the selected module (same image).
  Modules are cached by (source code, sign table). Ring and prewarm use the same function.
* **Prewarm list** version 2 -> 3 (record size changed). The first start with the new build logs
  "list from another version or damaged: starting from scratch" once and writes a new list. Records with
  folded signs are skipped when the setting is off.
* **Test**: `tests/run_all.sh texture_signs` (synthetic module, no game data). On the real library: 399
  random pixel shaders folded with `spirv-val` clean, every `>> 24` traced (0 untraceable), the 7 that fold
  nothing sample no texture; the C++ transform gives the same NAK counts as the HLSL with the sign helper
  removed.

## How to test on the console

1. Build as usual, then an A/B pair with the same build: `masseffect_native_fold_texture_signs = false`
   and `= true` in `masseffect.toml`, cold (`COLD=1`), the usual cockpit route.
2. Log lines: `[native] texture signs: folded into the pixel shader (in the pipeline key)` once;
   `[native] texture signs folded: PS n.. (N modules so far), K shifts folded, ...` for the first 32 modules,
   then every 256th. No `texture signs: ... not folded` warnings expected.
3. Compare `GPU per Swap`, `GPU time by pass` (the category 3 and 4 totals), fps, the number of pipelines and
   the pipeline creation time / hitches (more pipelines: one per distinct sign set per pixel shader).
4. Screenshots must be identical (bit-exact by construction; any difference is a bug).

## Risks

* **More pipelines.** A pixel shader drawn with different texture formats (different signs) now needs a
  pipeline each. Most materials always bind the same kinds of textures, but every new key is a compile on
  the ring (cold start, first visit of an area). Check the pipeline count and creation time on a cold run.
* **NVK shader cache.** Folded modules are new SPIR-V: the first cold run compiles all of them.
* **Occupancy.** PS24056 goes from 56 to 64 GPRs (36 -> 32 warps/SM) while losing 81 % of its instructions;
  the others keep or lower their GPR count.
* **Partial fold.** Pixel shaders with more than eight sampled registers keep the loop for the rest.
* The prewarm list of earlier builds is discarded once (version 3).

## Follow-ups not done

* A cheaper exact gamma decode in `shader_common.h` (needs a library rebuild): with signs folded it is the
  biggest remaining sign cost (~25 instructions per gamma component).
* Per-PS ranking on the console (`masseffect_native_stats_per_draw_s`) to know which pixel shaders dominate
  the category 3 runs.
* Folding the vertex input remap codes into the vertex shader (needs them in the key).
