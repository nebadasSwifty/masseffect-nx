# Vertex shader specialization: compared constants and input remaps folded into the module

2026-10-07. Offline analysis (Mac, no console) of the vertex shaders of the per-light passes of the Normandy cockpit
(Russian edition, 960x544, GPU 768 MHz), and the change it led to: `masseffect_native_fold_vs_constants`
(default **off**). Nothing here was measured on the Switch yet.

## Starting point (console measurement)

One serialized diagnostic frame with a query per draw (total draw time 15-16 ms of a ~26 ms GPU frame):

| PS | ms/frame | draws | Mfrag | ns/frag | vertices / prims | VS |
|---|---|---|---|---|---|---|
| 14702 | 2.93 | 4 | 0.246 | 11.9 | 21640 / 13554 | n2013 |
| 2169 | 1.44 | 2 | 0.125 | 11.5 | 10820 | n22611 |
| 24056 | 1.33 | 1 | | | | DoF gather (16 + 16 fetches) |
| 26940 | 1.08 | 4 | 0.045 | 24 | 6800 | n2013 |
| 12720 | 0.70 | 4 | 0.031 | 23 | | n2013 |
| 18008 | 0.52 | | | | | n22611 |
| 1725 | 0.38 | | | | | n22611 |

The question was whether the skinned vertex shaders VS n2013 / n22611 (run again for every light) spend their
time on Xenos bool (`b#`) / loop (`i#`) constant branches, and whether specializing on them would help.

## Tools

* Shaders taken from the console's library (`masseffect_shaders.mesp`, 29,241 entries, the log's numbering;
  275 of them are vertex shaders) into a private scratch folder, translated again with `out/tools/xenos_hlsl`
  for reading.
* The host NAK cost tool is fragment-only. A vertex variant was built in the scratch folder from the same
  source (`mesa-codex-gm20b-host/src/nouveau/vulkan-host/nvk_host_entry.c` + `NAK_COST_STAGE=vertex`: stage
  `MESA_SHADER_VERTEX` for spirv_to_nir, no colour-target pass, no `nak_fs_key`), linked against the objects of
  `work/me2-codex-native-cost/out/mesa-gm20b-host-pregen`. Same profile as docs/scene-shader-cost.md (SM53,
  heaps 4096/16/64/512, robustness 1/1/0, null descriptors, specialization word 0x300). The fragment mode of
  the variant reproduces the original tool exactly (PS14702: 1122 instructions / 6258 cycles / 40 GPRs).
  The VS is compiled standalone, so outputs the pixel shader does not read are still computed (NVK links no
  varyings either; see `masseffect_native_vs_pruned_outputs` for the library v30+ answer to that).
* Counts are NAK's post-scheduling instructions; cycles are NAK's static estimate (both sides of every branch
  summed), not GPU time.

## Findings

### No bool or loop constants anywhere

* The translator (`shaders/XenosRecomp/shader_recompiler.cpp`) turns Xenos bool tests into
  `if ((g_Booleans & (1 << n)) != 0)`, `g_Booleans` being shared word 64 (VS bits 0-15, PS bits 16-31, read from
  the shared UBO, set 4 binding 2); loops become `for (aL = 0; aL < i#.x; aL++)`.
* But **no vertex shader of the library (0 of 275) and no pixel shader of a 400-shader random sample tests a
  bool constant or has a loop**. VS n2013 and n22611 have none either. Specializing on `b#`/`i#` would fold
  nothing.

### What the 126 `if`s of VS n2013 really are

* 84 `if (skipTo <= n)` blocks: the translator's straight-line form of forward Xenos jumps
  (`skipControlFlow`). The jumps are `if (!p0) skipTo = 19 / 47 / 77`, with `p0 = MaxBoneInfluences.x > 0, 1, 2, 3`:
  how many bones to blend, decided **three times** (position, tangent frame, light vectors).
  `MaxBoneInfluences` is a FLOAT constant (`c8` in n2013/n22611, `c5` in 11 shaders such as n12867, `c9` in 5).
* Per-vertex `if (p0)` on the blend weights (`r5.y > 0`), real data branches.
* 7 `remapInput(value, g_InputRemap(location))` calls (vertex declaration swizzle fix-up, shared words 74-89):
  `if (code == 0xFFF) return value;` followed by a 4-component select loop. With
  `masseffect_native_normalized_remap` the code is almost always the identity, so the loop is skipped at run
  time, but it stays in the binary (selects, registers, branches).
* Bone matrices are read with a per-vertex index: `BoneMatrices(2 + a0)` = `g_UboVertex.v[9 + min(a0, 246)]`
  under `select(a0 < 247, ..., 0)`. NAK emits **119 indexed `ldc.b64`** (divergent within a warp: serialized
  per distinct address on Maxwell) plus their `imnmx`/`isetp`/`sel` bounds checks (453 `sel` in total). The
  three passes recompute `a0` from the same blend index, but the `skipTo` structure stops NIR from sharing the
  loads.

NAK numbers, base library SPIR-V:

| VS | instrs | static cycles | GPRs | warps/SM | indexed ldc | bra | sel |
|---|---|---|---|---|---|---|---|
| 2013 | 2687 | 12875 | 80 | 24 | 119 | 62 | 453 |
| 22611 | 2667 | 12836 | 80 | 24 | 119 | 61 | 451 |
| 12867 (position-only skinned, depth/shadow) | 611 | 3478 | 24 | 64 | 24 | 20 | 106 |

### Folding `MaxBoneInfluences.x` and the identity remaps (C++ transform below, same results as editing the HLSL)

VS n2013 (`r` = identity remaps folded, `m` = MaxBoneInfluences value folded, `x` = left dynamic):

| variant | instrs | static cycles | GPRs | warps | indexed ldc | bra | sel |
|---|---|---|---|---|---|---|---|
| base | 2687 | 12875 | 80 | 24 | 119 | 62 | 453 |
| r, m=x | 2073 | 8958 | 80 | 24 | 119 | 33 | 341 |
| m=4 only | 1695 | 7931 | **96** | 20 | 60 | 38 | 291 |
| r, m=4 | **1054** | **3507** | 80 | 24 | **60** | **9** | 179 |
| r, m=3 | 799 | 2650 | 72 | 28 | 42 | 6 | 129 |
| r, m=2 | 543 | 1806 | 64 | 32 | 24 | 3 | 79 |
| r, m=1 | 290 | 1484 | 40 | 48 | 6 | 0 | 29 |
| r, m=0 | 93 | 492 | 24 | 64 | 0 | 0 | 2 |

VS n22611: base 2667 / 12836 / 80 GPRs; r,m=4 1035 / 3545 / 72 GPRs (28 warps); r,m=1 264 / 1474 / 40 GPRs.
VS n12867: base 611 / 3478; r,m=4 281 / 1548; r,m=1 119 / 829 (24 GPRs throughout).

Folding only `MaxBoneInfluences` raises n2013 to 96-104 GPRs: the remaps must be folded with it (they are
in the same change). Library-wide (all 275 VS, every compared component = 4.0, all remaps identity): 209,247 ->
74,640 NAK instructions (-64 %), static cycles 1,117,439 -> 384,602; remaps alone: 106,193 instructions. All 275
folded modules pass `spirv-val` and compile with NAK; 5 shaders go from 48 to 56 GPRs (vs18353, 21429, 24914,
25031, 8435) and 1 from 72 to 80 (vs24635), i.e. one occupancy step lower, while losing ~57 % of their instructions.

### Expected gain (estimate, not measured)

The static counts overstate the run-time gain: at run time the base shader already skips the remap loops and,
with fewer bones, the unused slots. With four bones (the likely value for characters) the base shader executes
roughly the folded code **plus** the 59 duplicated indexed bone loads with their bounds checks, ~50 extra
uniform branches (13-cycle `ssy`/`bra`/`sync` each) and the `skipTo` bookkeeping: an estimated ~1700-2000
executed instructions per vertex against 1054 folded, i.e. **-35 to -45 % of the VS work**, and half the
divergent `ldc`s. With MaxBoneInfluences 1 or 2 (rigid or lightly skinned meshes) the folded shader is 3-6x
smaller than what runs today.

Whether this moves the frame depends on the VS share of these passes, and the "ns per fragment" figures do not
prove the passes are VS bound: PS14702 runs 1122 static NAK instructions per fragment (with texture signs not
folded) on 0.246 Mfrag, i.e. ~2-5x the lane-instructions of its ~21.6-40.7 k VS invocations. Rough budget:
VS ~20-40 % of the VS2013/VS22611 per-light passes (7.05 ms/frame in the table) -> **~0.5-1.5 ms/frame** saved,
more if the divergent `ldc` replays dominate (they are not visible in static counts). Plus the depth/shadow run
headed by VS n12867 (5.6 ms/frame category-3 interval, 611 -> 281 instructions with four bones).
Only the console A/B can tell.

## The change

`masseffect_native_fold_vs_constants` (bool, default false, re-read once per frame) and
`masseffect_native_fold_vs_constants_max_values` (int, default 4, 1-64).

* **Analysis** (`app/src/native/masseffect/me_vs_constants_spirv.h`, `AnalyzeVertexConstants`, once per VS on the
  ring thread): the float register components `K * 4 + c` of the vertex block (set 4, binding 0) that are a direct
  operand of an `OpFOrd*`/`OpFUnord*` comparison (traced through `OpPhi` / `OpLoad` / `OpCopyObject` / constant
  `OpAccessChain`), at most 3; and the input locations whose remap word (shared words 74-89) the module reads.
  On the library: 84 of 275 VS compare exactly one component (`MaxBoneInfluences.x` in 42, `ScreenAlignment.x`
  in 39, an unnamed `c255.z` in 3), none compares more.
* **Key** (`masseffect_native_draws.cpp`, `FoldVsConstantsInKey`): only with `SPEC_CONSTANT_CONSTANTS_UBO` and not
  for rectangle lists. The raw bits of each compared component (`r[0x4000 + K * 4 + c]`, the value the UBO
  receives) go in `PipelineKey::vs_values[0..2]`, provided the draw uploads that register (`constants_bytes`)
  and the value is an ordinary float (no NaN, infinity or denormal). `PipelineKey::vs_fold` = bit L for each read
  location whose remap code is `0xFFF`, bits 16-17 = number of components. Specialization bit 19
  (`kSpecVsFolded`, left to the app by shader_common.h) marks it. The key grew from 88 to 104 bytes (no
  padding; `has_unique_object_representations` still holds).
* **Budget**: each VS may be specialized for at most `max_values` distinct value sets; the next new set switches
  that VS's components back to dynamic for the rest of the session (its identity remaps stay folded: they follow
  the vertex input, which is already in the key through the entry fingerprint).
* **Module** (`FoldVertexConstants`, `ModuleVsConstants`): every `OpCompositeExtract %float` of a folded component
  becomes `OpCopyObject %float <constant>`, every `OpBitcast %uint` of an identity remap word becomes
  `OpCopyObject %uint 0xFFF`. Exact by construction (the value is the one the shader would read). A failed or
  empty fold returns the plain module (same image). Modules are cached per (library entry, values, fold word);
  ring and prewarm use the same function.
* **Prewarm list** version 3 -> 4 (record size changed). The first start with the new build logs
  "list from another version or damaged: starting from scratch" once and writes a new list. Records with folded
  VS constants are skipped when the setting is off.
* **Tests**: `tests/run_all.sh vs_constants` (synthetic module). On the real library: all 275 VS folded with
  `spirv-val` clean and NAK-compiled; the C++ transform gives the same NAK counts as editing the HLSL.

## Log lines

* Once per change of the setting: `[native] VS constants: compared float constants and identity input remaps
  folded into the vertex shader (in the pipeline key)`.
* First 64 VS with something to fold: `[native] VS constants: VS n2013 compares c8.x; reads the input remaps
  of locations 061F`.
* Every distinct value set per VS (first 256 lines overall; this is the "distinct constant combos per shader"
  report): `[native] VS constants: VS n2013 value set #1: c8.x=4 (40800000)`.
* Budget: `[native] VS constants: VS n.. has more than 4 value sets; its compared constants stay dynamic from
  now on` (warning; not expected for MaxBoneInfluences / ScreenAlignment).
* Modules: `[native] VS constants folded: VS n2013 (N modules so far), 12 compared reads and 7 remap reads
  folded, values 40800000 00000000 00000000, fold 1061F` for the first 32, then every 256th.
* Failures (not expected): `[native] VS constants: VS n.. not folded: <reason>` / `cannot be analyzed`.

## How to test on the console

1. A/B with the same build: `masseffect_native_fold_vs_constants = false` / `true`, cold (`COLD=1`), cockpit route,
   ideally with `masseffect_native_stats_per_draw_s = 5` to get the per-PS lines of the table above.
2. Read the value-set lines: they give the real MaxBoneInfluences values (and so which row of the table applies).
3. Compare `GPU per Swap`, the per-PS ms of PS14702 / 2169 / 26940 / 12720 / 18008 / 1725, the category-3 total,
   fps, the pipeline count and the creation time / hitches.
4. Screenshots must be identical (any difference is a bug).
5. Worth combining with `masseffect_native_fold_texture_signs = true` (the pixel side of the same passes).

## Risks

* **Pipeline count**: one pipeline per (VS value set x the rest of the key). For the shaders found, the values
  are per mesh/material (bone count, sprite alignment mode), so 1-3 sets per VS are expected; the budget bounds
  the worst case at `max_values` per VS. Identity-remap masks follow the vertex declaration and the VS, already in
  the key, so they should not add pipelines; if a VS is drawn with both identity and non-identity remaps for
  the same entry fingerprint it would get two.
* **Compile hitches**: every new key compiles on the ring (cold start, first visit of an area). The folded
  modules are smaller, so each compile should be cheaper, but there are more of them. Check the creation time.
* **NVK shader cache / prewarm**: folded modules are new SPIR-V; the first cold run compiles all of them. The
  prewarm list of earlier builds is discarded once (version 4).
* **Occupancy**: a few shaders gain one GPR step (48 -> 56, 72 -> 80) while losing 55-60 % of their instructions;
  n2013 stays at 80, n22611 goes 80 -> 72.
* **Memory**: one module per (VS, value set); no copy of the source SPIR-V is kept (the library entry is the key).
* Values that are not plain floats, registers outside the uploaded range, and rectangle-list draws stay dynamic.

## Reusing skinned positions across light passes (feasibility only, not implemented)

* NVK exposes `VK_EXT_transform_feedback` (transformFeedback, geometryStreams) and
  `vertexPipelineStoresAndAtomics` on this GPU (mesa-switch `nvk_physical_device.c`), so both a capture by
  transform feedback and a VS/compute that writes skinned vertices to a storage buffer are possible.
* What it would take: (1) a skin-only variant of each skinned VS (position + tangent frame, without the
  light-dependent part) — the translator works on the Xenos microcode and has no notion of "light part", so this
  means a per-shader split of the 42 MaxBoneInfluences shaders, or a hand-written skinning kernel that reproduces
  their exact math; (2) detecting, per frame, that consecutive draws are the same mesh with the same bones
  (vertex buffer address, index range, bone-constant generation); (3) a light-pass VS that reads the captured
  attributes. Transform feedback writes per emitted vertex (no index reuse: up to 3 x 13,554 vertices for the
  PS14702 draws, ~2 MB/frame at ~13 floats each), a compute pre-skin pass writes each vertex once.
* Upper bound of the gain: the skinning share of the VS work of the 2nd..Nth light pass of each mesh. After the
  fold above the skinning part is ~60-70 % of n2013's 1054 instructions, and the VS is an estimated 20-40 % of
  these passes, so ~10-25 % of 7 ms minus the capture cost. Feasible on NVK, but a translator-level project; do
  the fold (and measure) first.

## Follow-ups not done

* Bone matrices through a storage buffer or texel fetch instead of indexed UBO loads (divergent `ldc` serializes;
  the texture path caches per lane). Needs a library change.
* The `select(INDEX < 247, ...)` bounds check per bone load (≈ 4 `sel` + `isetp` + `imnmx` per load) cannot be
  removed exactly without knowing the blend indices are in range.
