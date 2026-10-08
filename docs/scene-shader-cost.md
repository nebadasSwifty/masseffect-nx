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

## Per-draw ranking measured on the Switch (2026-10-07, RU, 960x544, cockpit, `masseffect_native_stats_per_draw_s = 5`)

One serialized diagnostic frame every 5 s; the draws sum to 15-16 ms of a ~26 ms GPU frame (the rest is EDRAM
conversions, copies, resolves, clears and the present). Typical cockpit frame (192 draws, 5.0 Mfrag):

| PS | ms/frame | draws | Mfrag | ns/frag | vertices | VS |
|---|---|---|---|---|---|---|
| 14702 | 2.93 | 4 | 0.246 | 11.9 | 21640 | 2013 |
| 2169 | 1.44 | 2 | 0.125 | 11.5 | 10820 | 22611 |
| 24056 | 1.33 | 1 | 0.033 | 40.8 | 4 | 24994 (DoF gather) |
| 26940 | 1.08 | 4 | 0.045 | 24.0 | 6800 | 2013 |
| 12720 | 0.70 | 4 | 0.031 | 23.1 | 5704 | 2013 |
| 7992 | 0.62 | 1 | 0.522 | 1.2 | 4 | 28697 (full screen) |
| 15729 | 0.57 | 1 | 0.522 | 1.1 | 4 | 2293 (uber blend) |
| 18008 | 0.52 | 2 | 0.025 | 21.0 | 3400 | 22611 |
| 23886 | 0.51 | 2 | 0.226 | 2.3 | 2064 | 8275 |
| 3474 | 0.48 | 1 | 0.522 | 0.9 | 4 | 9460 |

The skinned per-light passes (VS2013, VS22611) cost 10-24 ns per fragment against ~1 ns for full-screen
passes: they are vertex-bound (~6.5 ms per frame together). Next step: specialize those vertex shaders
(docs/vertex-shader-specialization.md, in progress).

## Descriptor and clamp rewrites (A, B, C), 2026-10-07

Offline follow-up on the pixel shaders once their texture signs are folded: what is left per fetch is the
descriptor handling (index load, `& 0xFFFFFF`, clamp, handle `ldc` for the texture AND for the sampler, combine)
and, per clamp, the two 32-bit bounds of `clamp(x, FLT_MIN, FLT_MAX)` that SM50 FMNMX cannot encode (20-bit float
immediates only), so NAK emits a `mov` for each. Three exact rewrites, each behind its own setting (default **off**),
all requiring `masseffect_native_constants_ubo`. SPIR-V transforms: `app/src/native/masseffect/me_ps_descriptors_spirv.h`;
renderer side: `masseffect_ps_descriptors_members.inc` (+ `masseffect_ps_descriptors_cvars.inc`). Nothing here was
measured on the Switch yet.

| | setting | what the shader does instead | why it is exact |
|---|---|---|---|
| A | `masseffect_native_ps_flt_bounds_ubo` | the FClamp/FMin/FMax/NClamp/NMin/NMax operands `0xFF7FFFFF` / `0x7F7FFFFF` become loads (once per function) of shared words 122 / 123 (bytes 488 / 492, the unused tail of the declared 31-float4 block) | the renderer always writes exactly those bits there (`kSharedWords` 122 -> 124) |
| B | `masseffect_native_ps_combined_heap` (read at start-up for the layout) | every access of the 2D heap goes to a COMBINED_IMAGE_SAMPLER heap at **set 5**; `OpSampledImage(image of 2D word w, sampler of word 48 + w)` becomes the combined descriptor; other image uses get `OpImage` of it | the draw writes into 2D word w the combined index of (its 2D slot, its sampler slot); the combined descriptor is that slot's view + that slot's sampler, rewritten whenever the 2D slot is (`WriteImage` mirror) |
| C | `masseffect_native_ps_no_index_mask` (needs `masseffect_native_fold_texture_signs`) | the `& 0xFFFFFF` of the descriptor words of the folded registers is removed | only when the signs of **every** sampled register are in the key; the draw then writes those words (2D, 3D, cube) with the sign byte cleared |

Soundness checks (forward taint of the shared words, `TaintSharedWords`): for B every read of words 0-15 must end in
`>> 24` (sign byte, kept) or, through `& 0xFFFFFF` / copies / phis, in a 2D heap index; for C every read of a cleared
word must end in its mask (a `>> 24` left unfolded or any other use refuses). An access to the block without constant
indices refuses both. A sampler that cannot be proven to be the one of the same register keeps its separate sampler
(the image still comes from the combined heap: still exact). The vertex shader must not read descriptor words 0-47
(checked per VS; 0 of the 275 library vertex shaders do). Decisions are made per draw from a per-PS plan computed once
on the library module (`PsDescriptorsPlanFor`: the same transforms are run on the plain and on the signs-folded + C
module); a transform that fails at pipeline creation for a key with B or C rejects the pipeline (logged as an error,
not expected), a failed A is skipped.

### Renderer changes

* **Key**: bits 24-26 of `PipelineKey::signs_heaps` (`kKeyPsFltBounds`, `kKeyPsCombined`, `kKeyPsNoMask`); no new
  fields. Prewarm list version 4 -> 5 (one "list from another version" at the first start). Prewarm records with a bit
  whose setting is off (or B without set 5) are skipped. The async specialized pipelines keep A/B in the generic key;
  keys with C are never deferred (the generic module would read the cleared sign bytes).
* **Shared block**: the signs of the key are now taken from the block before `PlanPsDescriptors` rewrites it (B: 2D
  words = sign byte | combined index; C: sign bytes cleared), and both happen before the upload.
* **Layout (B only, start-up)**: `layout_pipeline_` gets a sixth set (`comb_set_layout_`, 16384 entries requested,
  halved until `vkGetDescriptorSetLayoutSupport` accepts, UPDATE_AFTER_BIND + PARTIALLY_BOUND like the other heaps),
  bound right after sets 0-3 (`BindCombinedSet`). With the setting off at start-up the layout is exactly the old one.
  Pairs are allocated on first use and never freed (a pair follows its 2D slot through `WriteImage`); when the heap
  is full the new pairs' draws simply keep the separate heaps (warning once).
* **Modules**: `ModulePsDescriptors` after `ModuleTextureSigns` (ring and prewarm), order C, B, A; cached per (source
  code, bits, cleared words) and registered in `codes_modules_depth_` for the later transforms.

### Offline results (host NAK, same profile as above, SM53)

Hot pixel shaders of the cockpit, NAK instructions with the signs folded to 0 (`s`); B measured with set 0 declared
COMBINED_IMAGE_SAMPLER (`nak-cost-comb`, `NAK_COST_COMBINED=1`; the descriptor handle path is the same as set 5):

| PS | s | s + C | s + B | s + C + B | s + C + B + A |
|---|---|---|---|---|---|
| 14702 | 298 | 292 | 256 | 250 | **240** |
| 2169 | 260 | 255 | 225 | 220 | **212** |
| 24056 | 673 | 669 | 659 | 655 | **591** |
| 26940 | 457 | 447 | 387 | 377 | **364** |
| 12720 | 542 | 531 | 465 | 454 | **430** |
| 18008 | 390 | 381 | 327 | 318 | **313** |
| 15729 | 153 | 150 | 132 | 129 | **120** |
| 14489 | 194 | 191 | 173 | 170 | **160** |
| 23886 | 240 | 238 | 226 | 224 | **218** |
| 7992 | 120 | 119 | 113 | 112 | **108** |
| 3474 | 66 | 65 | 59 | 58 | **58** |

The C++ transforms reproduce the earlier prototype counts exactly (scratch prototype: 252 -> 202 instructions on
average over a 300-shader sample with A+B+C, -20 %; estimated ~0.45 ms/frame at 1280x720 for B, ~0.16 ms for A,
~0.07 ms for C; static estimates, not GPU time). On 723 library pixel shaders (two random samples plus the hot ones)
the chain signs fold -> C -> B -> A succeeds on every module and every output passes `spirv-val --target-env
vulkan1.2`; 2481 2D fetches combined, 0 kept separate; 2497 masks removed; 4728 clamp bounds moved.

### Tests

`tests/run_all.sh ps_descriptors`: a synthetic DXC-shaped module (validated with `spirv-val` when it is on the PATH):
A's operands are the loads of v[30].z / v[30].w and the module constants are the bits the renderer writes; B keeps the
same index id into the combined heap, combines the matching pair, keeps the mismatched sampler, refuses a 2D word with
another use; C removes the mask only after the signs fold, refuses an unfolded `>> 24`, leaves other words alone; C,
B, A chain. `ME_PS_LIBRARY_DIR=<folder of library PS .spv> out/tests/cpu/test_native_ps_descriptors_spirv` runs the
whole chain on real modules and validates each output (no game data in the repository).

### How to test on the console

1. Build `ru_psdesc` (this change). Same build, cold, usual route; one run per toml (all on top of `s1280all.toml` /
   `tex_base.toml`, which already fold the texture signs): `run/me1/psd1280_{a,b,c,abc}.toml`,
   `run/me1/psd960_{a,b,c,abc}.toml`, against the base toml itself.
2. Log lines: `[native] PS descriptors: clamp bounds ... (A) ON ...` once; with B
   `combined image+sampler heap at set 5, N entries`; `PS n.. (k analyzed): A yes (n bounds), B yes (n 2D fetches), C
   yes` for the first 48 PS; `PS n.. rewritten (...)` for the first 32 modules. Not expected: `rewrite .. failed`,
   `combined heap full`, `VS n.. reads descriptor words`.
3. Compare GPU per Swap, category 3/4 totals, fps, pipeline count and creation time (A, B, C do not add pipelines:
   every key gets the same bits for a given PS; C only when the signs already are in the key).
4. Screenshots must be identical (exact by construction; any difference is a bug).

### Risks

* B adds a descriptor write per new (texture slot, sampler) pair and per 2D slot rewrite of a slot that has pairs
  (ring thread CPU), and one more `vkCmdBindDescriptorSets` per command buffer. Per draw: a short scan per sampled
  register.
* B's pipeline layout differs from the usual one: with B on, the NVK cache entries of the old layout do not match
  (first run cold for all pipelines).
* C gives up silently (bit clear) for pixel shaders with more than eight sampled registers or a register whose signs
  are not folded.

### D (not done): gamma decode constants

With the signs folded the gamma decode (sign 3) is still ~25 NAK instructions per component, mostly the immediates
of its three compares and of the scale/offset selects materialised with `mov`. Loading those ten constants up front
(or from shared words, `gammaubo` in the prototype) gave 0-0.2 ms in the offline estimate. It needs either a
`shader_common.h` change and a full library rebuild (risky: every shader changes) or a SPIR-V patch that also grows
the declared shared block (31 -> 34 float4) of every module. Left for later; most hot scene shaders sample unsigned
render targets and are unaffected.
