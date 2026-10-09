# Best configuration: what helps, what does not

One place for the settings measured on the Switch (RU edition, CPU 1785 / GPU 768 MHz, Normandy route to the
cockpit, `tools/me1_anderson.sh` with `LONG=1`). Every entry links to the doc with the details. "Shipped" means it is
already on in `app/masseffect.toml`. Update this file with every console result.

## Kept (image identical)

| Setting | Effect measured | Status | Details |
|---|---|---|---|
| `masseffect_render_ring_kb = 4096` (hook) | black start fixed, 27/27 starts | shipped (default) | performance-history.md |
| `masseffect_scene_front_full` (crop) | black Shepard / title planet fixed | shipped (default) | do-not-break.md |
| `masseffect_native_extent_packed_formats` | less EDRAM work | shipped | gpu-cost-analysis.md |
| `masseffect_native_resolve_7e3_direct` | less EDRAM work | shipped | gpu-cost-analysis.md |
| `masseffect_native_resolve_bias_frag` | bias resolves as fragments | shipped | gpu-cost-analysis.md |
| `masseffect_native_conversion_copy_32` | f0<->f4 by image copy | shipped | post-chain.md |
| `masseffect_native_fold_texture_signs` | cheaper pixel shaders | shipped | scene-shader-cost.md |
| `masseffect_audio_dsp_native = 1` | CPU | shipped | |
| RU hook address fix of the D3D ring wait (sub_8222F888) | render thread 96 % -> 21 % CPU | code (RU) | cpu-cost-analysis.md |
| RU hot-function natives (27 hooks ported) | main/render thread CPU | code (RU), `masseffect_hot_guest = true` shipped | optimization-backlog |
| RU function_order.ld from a RU profile | slightly steadier | code (RU) | optimization-backlog |
| `masseffect_native_fold_vs_constants = true` | GPU draws ~15.8 -> ~12 ms/frame (cockpit) | **to ship** | vertex-shader-specialization.md |
| `masseffect_native_restore_into_7e3 = true` | GPU busy ~22 -> ~20 ms, alias 5.6 -> 4.2 ms | **to ship** | vulkan-frame-time.md s.8 |
| chunked pipeline-cache writes (default on in code) | worst warm hitches 1817/922 ms -> 883/383 ms | code default | cold-start-hitches.md |
| `masseffect_switch_text` (default true) | "Nintendo Switch" wording | default on | switch-branding.md |

| ring CPU exact set: `masseffect_native_fingerprints_skip_unused_sample`, `masseffect_native_load_memo`, `masseffect_native_load_memo_generation`, `masseffect_native_edram4_pair_keys`, plus `masseffect_native_texture_spread_phase` | heavy walk windows: 14.1k -> 15.6k draws/s (+11 %), worst texture recheck 27.5 ms per hitch frame; memo 0 differences | **to ship** | ring-cpu-per-draw.md |

| 1280x720: `edram4_rect_list_tiles` + `resolved_wake_no_clear` + `resolve_7e3_frag` together | GPU busy 38.6-41.2 -> 35.7-38.2 ms (-2.6 ms), late stencil fetches (import9) 5.9 -> 4.2 ms, alias 7.6 -> 6.5 ms; cockpit Swaps ~246 -> ~264 per 10 s | keep for 1280 (7e3_frag alone was slower at 960: split test pending) | vulkan-frame-time.md s.9 |

| ring CPU round 2: `masseffect_native_invalidate_textures_each_copy = false` (+ `masseffect_native_texture_copy_verify`), `masseffect_native_mismatch_log_info`, `masseffect_native_edram4_sync_lean` | heavy walk windows 26.9 fps, 17.1k draws/s (vs 14.3-15.6k with round 1); no verification mismatch logged | **to ship** | ring-cpu-per-draw.md (Round 2) |
| 1280x720: `masseffect_native_edram4_stencil_copy_rows`, `_stencil_known`, `_stencil_fetch_area` | GPU busy 34.6 -> 32.7 ms per frame (budget 33.3), late stencil fetches 1.8 -> 0.2 ms | keep for 1280 | vulkan-frame-time.md s.10 |
| `input_rumble = true` | no fps cost (heavy windows 26.9 fps both, draws/s same); feel to be checked by the user | pending the user's hands-on check | |

| Mesa NVK/NAK built at `-Doptimization=2` (was -O1) | heavy scenes 15.7k -> 16.6k draws/s (+5 %), 25.8 -> 26.4 fps, frames over 60 ms 160 -> 108 per route; same source, same image | **default** (mesa/build_mesa_docker.sh `MESA_OPT=2`; fallback `MESA_OPT=O1 tools/build_nro.sh`) | mesa.md, nvk-per-draw.md |
| `masseffect_present_immediate_threshold = 100` (swap callback hook; the game passes threshold 0, interval 2) | A/B/A/B: heavy walk windows 25.8/25.9 -> 27.7/27.1 fps, frames over 60 ms 209/195 -> 115/101 per route; cockpit stays capped (297-299 Swaps per 10 s) | **to ship** | frame-handshake.md P1 |
| `masseffect_vblank_adaptive = true` | 27.0-27.4 fps heavy, but the cockpit went to 300-304 Swaps per 10 s (extra VBlanks break the cap); superseded by the threshold | replaced | me_native_system.cpp |

## Rejected or no gain

| Setting | Result | Details |
|---|---|---|
| `masseffect_native_query_mode = 2` (real occlusion queries) | fewer fps (200-250 vs ~290 Swaps/10 s), 2-4 % culled | occlusion-queries.md |
| `masseffect_native_query_skip_boxes` | no gain (~17 queries/frame) | occlusion-queries.md |
| `masseffect_wait_ring_max_us = 200` | no change in the heavy walk windows | cpu-cost-analysis.md |
| `masseffect_wait_blocking_ring = false` (spin) | no change in the heavy walk windows | cpu-cost-analysis.md |
| clock logging through clkrst in the ring report | froze the console | reverted |
| `masseffect_native_waitregmem_spin_us` 300 / 2000 | no change: the time is one wait per frame on guest memory 0x1FC9B006 (the game's VBlank/flip handshake, 13-27 ms per frame in heavy windows), not wake-up latency | me_native_system.cpp |
| `masseffect_exec = "set PrimitiveComponent bUseAsOccluder False"` (no guest depth prepass) | A/B/A/B: draws per frame lower (peak 664-750 vs 934-1018) but heavy windows 25.8/26.0 vs 27.3/27.7 fps and more long frames (210-214 vs 148-154): the game also uses occluders for its own culling | draw-count-options.md |
| `masseffect_frame_lag = 2` | game crashes at start (guest access violation), 2 of 2 launches | me_ring_wait.cpp |
| `masseffect_exclusive_core = -1` or `22` (vs 32) | no change in the heavy walk windows (22-24 fps either way) | cpu-cost-analysis.md |

## Image-changing options (measured, the user decides)

| Setting | Effect | Details |
|---|---|---|
| `masseffect_game_msaa = 0` | less GPU at 1280 | performance-history.md |
| 800x448 + FSR | ~21.5 ms GPU at the cockpit | performance-history.md |
| `present_effect = "fsr"` at 960x544 -> 1280x720 | edge strength of cockpit shots 3.15 -> 6.07 (bilinear -> FSR); cockpit Swaps unchanged (287-300 per 10 s). **Chosen by the user: shipped** (app/masseffect.toml; tools/build_nro.sh now builds UPSCALE_SHADERS=ON by default) | run/me1/sharpness/ |
| `present_effect = "cas"` at 960x544 | edge strength 3.15 -> 3.73; Swaps unchanged | run/me1/sharpness/ |

| `masseffect_native_fingerprints_sample_min_interval = 4` (sampled texture rechecks) | heavy windows 18.1k draws/s (+28 % vs base), worst recheck 13.8 ms; no visible difference on the route; a changed texture can show up to 32 frames late | ring-cpu-per-draw.md |

## Pending console tests

| Setting | Expected | From |
|---|---|---|
| `indirect_dispatch_hot_cache = true` (SDK, exact; builds with `MASSEFFECT_INDIRECT_DISPATCH` 1/2 only, SDK rebuild, no codegen). First run also `indirect_dispatch_hot_cache_verify = true` (expect no `hot cache DIFFERENCE`), then A/B without verify on the prof_eden route (Saren/Nihlus cutscene + open area) | game thread -0.2-0.4 ms per frame, render thread -0.15-0.3 ms (the `indirect::Find` self time, 0.58 / 0.43 ms per frame in prof_eden, should drop to about half); check the `indirect_dispatch: hot-miss cache on` line | cpu-cost-analysis.md (Eden Prime game-thread profile) |
| `masseffect_hot_audio_resampler = true`, `masseffect_hot_audio_resampler_stereo = true`, `masseffect_hot_crt_memset = true` (new exact natives of the XAudio voice resamplers and the CRT memset, RU `sub_82B2D4F0` / `sub_82B2D780` / `sub_829730C0`; need `tools/edition.sh ru all` and `en all`: the hooked set changed; not covered by `masseffect_hot_guest`). First run with `masseffect_hot_guard_period = 1` and `masseffect_audio_dsp_native = 2` (expect `[hot] ... guard OK`, no `[hot] DIFFERENCE`, `0 mismatches`), then A/B on an audio-heavy route (Citadel spacewalk, combat) | audio thread ~34 % -> ~25 % of a core in the heaviest blocks; audio unchanged | audio-cpu.md |
| `masseffect_hot_lzo = true` (new native of the LZO1X decompressor, RU `sub_827D3388`; needs `tools/edition.sh ru all`: the hooked set changed; not covered by `masseffect_hot_guest`). First run: `masseffect_hot_guest = false`, `masseffect_hot_lzo = true`, `masseffect_hot_guard_period = 1` (every LZO call checked against the original; expect `[hot] sub_827D3388: guard OK ...`, no `[hot] DIFFERENCE`), then A/B: cold start time to the menu, level load / streaming hitches, streaming threads' CPU | LZO self time of the streaming threads several-fold lower (0.47 ms per frame on average in prof_eden, much more while streaming/loading); fewer streaming hitches, faster loads; picture identical | cpu-cost-analysis.md (Eden Prime game-thread profile) |
| ring CPU round 3: `masseffect_native_cache_textures_between_frames = true` + `masseffect_native_texture_inval_by_address` + `masseffect_native_constants_same_content` + `masseffect_native_dedupe_hash_after_copy` (exact, self-checking; measure with `masseffect_native_report_texture_uploads`) | textures stage ~12 -> ~4-6 us, uploads ~8.6 -> lower by the constant reuses; tomls run/me1/tex_base.toml, tex_ab.toml; build ru_tex.nro | ring-cpu-per-draw.md (Round 3) |
| `masseffect_native_edram4_rect_list_tiles` (960: no measurable change; 1280 together with the other two: see kept table) | 0.2-0.4 ms at 960, up to 2.5 ms at 1280 | vulkan-frame-time.md s.9 |
| `masseffect_native_resolved_wake_no_clear` (960: no measurable change; 1280 together with the other two: see kept table) | 0.2-0.8 ms | vulkan-frame-time.md s.9 |
| `masseffect_native_resolve_7e3_frag` (960 cockpit: label VS44 ~250 ms/10 s vs VS40 compute ~160-220: slower; recheck at 1280) | 0.3-0.6 ms | vulkan-frame-time.md s.9 |
| `masseffect_native_pipelines_async_specialized` | fewer cold-start hitches | cold-start-hitches.md |
| `masseffect_native_pipelines_shipped_list` | fewer cold-start hitches | cold-start-hitches.md |
| `masseffect_exec` + `tools/me1_location.sh` | location travel via `AT <map> <start>` | location-tests.md |
| `masseffect_native_query_mode = 3` (latency-1 real occlusion queries; `_max_age = 4`, `_hidden_after = 2`, `_history_key = 0`, then key 1) in Port Hanshan vs mode 0 | **measured 2026-10-08 (q3_hanshan): no gain**, 254-258 Swaps/10 s in the GPU-bound window like mode 0 (255-258); only ~3 % of box results are 0 because the boxes test a depth buffer without the skipped prepass (section E). Next: the row below | occlusion-queries.md sections D, E |
| `masseffect_native_query_occlusion_depth = true` with `masseffect_native_query_mode = 3` (`_max_age = 4`, `_hidden_after = 2`, `_history_key = 0`; `masseffect_native_skip_prepass` stays true), Port Hanshan (`tools/me1_ab_loc.sh`, same route as q3_hanshan) vs mode 0 and vs mode 3 alone | boxes test the prepass depth: zero results well above 3 % in the GPU-bound window, Swaps between 255 (mode 0) and 300 (mode 1) if the hidden draws outweigh the prepass drawn again (new `occlusion_depth` GPU category). Check the `[native] occlusion depth (...)` line (prepass draws per Swap recorded vs dropped, boxes on it vs left on the scene depth, clears noted/applied) and pop-in on fast turns; picture must be identical to mode 0. Not compiled yet | occlusion-queries.md section E |
| image-risk variant: `masseffect_native_skip_prepass = false` with `masseffect_native_query_mode = 3` (no new code) at 960x544 | at 960 the game has no MSAA, so the prepass, boxes and base pass share one 1x depth view and the B1 2x-to-1x mismatch cannot occur; boxes get real depth and the base pass gets early-Z, the prepass costs GPU and ring CPU. Watch for black shards (VS position differences between prepass and material shaders) | occlusion-queries.md section E |
| `masseffect_native_edram4_clear_alias_raw64 = true` (new, exact by construction; Eden Prime after the Saren/Nihlus cutscene, same route as prof_eden2, A/B against the same toml without it) | the slot clear drawn through the 4x alias is redirected (log `clear alias raw64, 10 s: N ...`, N about 1 per frame); pair `D5A0/f0:440x720:mx1my1g0->D5A0/f0:880x880` (27 ops/frame) gone; GPU 0.2-0.5 ms per frame plus ring CPU; picture identical (shadows, bloom edge) | gpu-cost-analysis.md s.5.3 |
| Eden Prime, measurement only (no image change): `masseffect_native_bias_life_probe = true`, `masseffect_native_bias_life_dump_s = <s of the heavy window>`, `masseffect_native_resolve_repeat = 1` | decides whether any of the 5.7 VS41 bias resolves per frame (3.1-3.3 ms) can be removed exactly (dual output / round trip) and whether shadow-depth resolves repeat | gpu-cost-analysis.md s.5.5, vulkan-frame-time.md s.12 |
| Eden Prime at 960: `masseffect_native_resolve_7e3_frag = true` + `masseffect_native_resolve_7e3_pack = 2` (existing, exact) | VS40 (0.9 ms/frame here) lower; was slower alone in the 960 cockpit, so compare VS40/VS44 labels | gpu-cost-analysis.md s.5.4, vulkan-frame-time.md s.11 |

## Postponed (user decision, 2026-10-07)

- Last-resort image-changing options, only if nothing else helps: MSAA off (`masseffect_game_msaa = 0`, re-measure at
  960 and 1280 on the current build) and the blur/drop-detail knobs (Gaussian blur radius 0, `MinDesiredFrameRate`).

- FSR to 1920x1080 in the dock.
- Full NSP built by the installer (program + RomFS with the user's own game data, signed with the user's own keys in the
  browser): only after 30 fps everywhere, when the program no longer changes (user decision).

## Candidate best toml (additions to app/masseffect.toml; shipped 2026-10-08, see "Shipped defaults 2026-10-08")

```toml
masseffect_native_fold_vs_constants = true
masseffect_native_restore_into_7e3 = true
masseffect_native_fingerprints_skip_unused_sample = true
masseffect_native_load_memo = true
masseffect_native_load_memo_generation = true
masseffect_native_edram4_pair_keys = true
masseffect_native_texture_spread_phase = true
masseffect_native_invalidate_textures_each_copy = false
masseffect_native_mismatch_log_info = true
masseffect_native_edram4_sync_lean = true
masseffect_present_immediate_threshold = 100
```

## Status of the three internet research lists (2026-10-07)

external-practices.md (list 1):
| # | Item | Status |
|---|---|---|
| 1 | Xenia "Black Shading Fix" byte (no MSAA / predicated tiling) | done as `masseffect_game_msaa = 0`; measured only at 1280 before today's GPU work (cockpit 18-29 fps, ~39 ms GPU; the 1280 MSAA reference run froze); not measured at 960; image-changing, user decides |
| 2 | FXAA to go with #1 | not done |
| 3 | merge predicated tiles keeping 2x MSAA | not done |
| 4 | fp16 in NAK for GM20B | not done |
| 5 | check render-target compression, `NVK_DEBUG=no_compression` A/B | not done |
| 6 | NEON for hot SIMDe intrinsics | not done |
| 7 | blur radius 0 / the game's vsync byte | not done (no console run found) |
| 8 | fragment shader interlock (ROV) path | not done |
| 9 | `VK_EXT_descriptor_buffer` | not done |

external-practices-2.md (list 2):
| # | Item | Status |
|---|---|---|
| 1 | occlusion queries (skip boxes / real) | done, rejected (no gain) |
| 2 | one frame of lag | measured (already n = 1); lag 2 crashes: rejected |
| 3 | larger D3D ring (`RBSecondarySize`, `RBSegmentCount`) | not done |
| 4 | `MinDesiredFrameRate` drop-detail | not done (no console run found) |
| 5 | GC hitches (`TimeBetweenPurgingPendingKillObjects`, `MaxObjectsNotConsideredByGC`) | not done |
| 6 | texture pool size | not done |
| 7 | audio update rate / voices | not done |
| 8 | native particle distribution `824DD848` | done (hot natives, also RU) |
| 9 | skip skeletal update of meshes not rendered | not done |
| 10 | throttle light environment updates | not done |
| 11 | memory clock (EMC) | user decision, not done |
| 12 | PhysX step 50 -> 30 Hz | not done |

external-practices-3.md (list 3):
| # | Item | Status |
|---|---|---|
| 1 | speculative occlusion results (mode 3) | implemented 2026-10-08 as latency-1 history keyed by box content (pooling-safe); measured: no gain while the boxes test a depth without prepass; occlusion depth (section E) pending |
| 2 | one-frame lag | see list 2 #2 |
| 3 | remove IMUL from EDRAM transfer shaders | partly (DivPitch float trick); remaining IMULs not audited |
| 4 | Swap waits only for "one frame in flight" | not done |
| 5 | NaN guard on PS c10/c11 (black characters) | not done |
| 6 | Xenia colour/depth alias bits, 2x sample layout (black shards) | not done |
| 7 | XMAD lowering for transfer shaders | not done (after #3) |
| 8 | SIMDe NEON | not done |
| 9 | NAK operand reuse / dual issue | not done |
| 10 | acceptance-sweep notes | noted |

## 2026-10-08 Feros (Zhu's Hope, tools/me1_ab_loc.sh zhu, walk 90 s, build run/me1/ru_glob1)

| Variant | Swaps per 10 s after arrival (mean / min) | Frames over 100 ms / over 250 ms | Notes |
|---|---|---|---|
| g1_base: manual_best.toml (new switches off) | 289 / 248 | 39 / 8, worst 993 ms | |
| g1_a: + log_nonblocking, heap_free_bitmap (+verify), texture_coherency = 2 (measure), diag_missing_shader_draws | 300 / 280 | 4 / 0, worst 154 ms | coherency: clean+CHANGED 0, clean+same 2988 MB hashed per 10 s (what mode 1 would skip); 12+ pixel shaders MISSING from the package (draws dropped) |
| g2_b: build ru_glob2 (indirect dispatch mode 2 + verify, dcbt prefetch, natives field_iter/sprite_render via hot_guest), log_nonblocking, heap_free_bitmap, texture_coherency = 1 | 300 / 300 | 0 / 0 | dispatch verify: 1,000,000 checks, 0 differences; coherency switched itself off at start (DIFFERENCE on a 1280x720 k_8_8_8_8 texture written without an event) so it did not contribute |
| g3_c: build ru_glob3 + shader package out/shader-missing2 (30160 shaders, Feros geometry complete), glob_b + motion_blur_frame_fix 5, guest_memory_large_pages 1, io ghost, coherency 2 | 300 / 288 | 2 / 0, worst 249 ms | 0 missing shader draws (was 35 PS missing); TLB benchmark 64 MB: 4 KB pages 179.5 ns/load vs 2 MB blocks 139.7 (large pages mode 1: window 2 MB aligned); minigame diamond patched; io ghost: 32 MB cache 23 %, 128 MB 32 % session hits; motion blur: c11 = 2*c10.zw (clamp consistent), c10.xy is per-frame; bit 1 also scales normal 35 ms frames by ~0.94, so use 4 (cut on frames over 60 ms) for an identical normal image |

## 2026-10-08 audio: XMA zero-end loop (build run/me1/ru_glob7)
- `audio_xma_loop_zero_end_off = true` (default): contexts with loop count 255 and loop start = end = 0 are decoded as "no loop". Console (Citadel Wards, user listening): the constant 344.5 Hz buzz is gone and the music that was replaced by it now plays. Pre-existing defect (also in the morning build ru_loc2). Keep as default; port to EN.

## 2026-10-08 build ru_glob8 (Citadel Wards, user)
- `masseffect_native_resolved_cpu_overwrite = true` (default): all letters present in the express terminal text, dialogues and the "Сохранение" autosave label (pre-existing defect: a resolved GPU copy kept being used after the CPU rewrote that memory for a new Scaleform glyph page). 2 "resolved texture rewritten by the CPU" events in the session.
- XMA: 0 repeat/one-frame-loop lines (buzz fix holds).
- DLC (`dlc_enable = true`, out/dlc_content + out/shader-dlc package): both packages listed and mounted, AutoLoad.ini found for both.
- Repack note: `Layer0/Movies/BWLogo.bik` of the Russian repack is a repack instruction card ("ДИСК 1 ... смените Диск 1 на Диск 2", a VK link, "Сборку образа выполнил Платов Д. 2014г"), not the BioWare logo.

## Shipped defaults 2026-10-08 (English and Russian editions)

One `app/masseffect.toml` is shipped with both editions (tools/edition.sh copies `app/` into the RU tree; the
installer downloads the same file). The EN edition now has every RU code change of 2026-10-08, so the same toml applies
to both. Not yet run on the console for EN: the first EN build should be checked with the same route as the RU builds.

Set in `app/masseffect.toml` (new or changed today):

| Key | Value | Evidence |
|---|---|---|
| `present_effect` | `"fsr"` | user choice 2026-10-07 (image-changing table) |
| `log_nonblocking` | `true` (writer priority `log_nonblocking_priority` = 0x2C, code default) | Feros g1_a: 289 -> 300 Swaps per 10 s, frames over 100 ms 39 -> 4 |
| `heap_free_bitmap` | `true` (`heap_free_bitmap_verify` stays false) | same run |
| `masseffect_native_motion_blur_frame_fix` | `4` (frames over 60 ms are camera cuts) | image-defects-feros.md 3.7; 4 keeps normal frames identical |
| `masseffect_native_fold_vs_constants` | `true` | kept table |
| `masseffect_native_restore_into_7e3` | `true` | kept table |
| ring CPU set: `fingerprints_skip_unused_sample`, `load_memo`, `load_memo_generation`, `edram4_pair_keys`, `texture_spread_phase`, `edram4_sync_lean`, `mismatch_log_info` (all `masseffect_native_*`) | `true` | ring-cpu-per-draw.md rounds 1 and 2 |
| `masseffect_native_invalidate_textures_each_copy` | `false` (was `true` in the toml; code default false, `texture_copy_verify` 4096) | ring CPU round 2 |
| `masseffect_present_immediate_threshold` | `100` | frame-handshake.md P1 |
| `masseffect_switch_text`, `masseffect_switch_minigame_layout` | `true` (also the code defaults) | switch-branding.md; minigame diamond seen patched in ru_glob3 |
| `dlc_enable` | `false` (the installer sets it to true when it packs DLC) | dlc.md |

Code defaults, listed as comments in the toml: `audio_xma_loop_zero_end_off = true` (buzz fix, ru_glob7),
`masseffect_native_resolved_cpu_overwrite = true` (missing letters fix, ru_glob8).

Left off (commented in the toml as experimental): `masseffect_native_texture_coherency` (mode 1 switched itself off on
Bink planes in ru_glob2; the fix is not measured yet, so 0), `guest_memory_large_pages`, `masseffect_native_velocity_16_16`,
the IO block cache (`masseffect_io_bcache_*`, wrong data in ru_glob3), `input_rumble` (pending the user's check;
turned on later in the shipped toml: no fps cost, Therum 2026-10-08),
the 1280x720-only EDRAM options.

Build-time defaults (not toml keys):

| Option | EN | RU | Notes |
|---|---|---|---|
| `dcbt_prefetch` (codegen, perf_overrides.toml) | `true` (was false) | `true` | needs codegen with the current generator |
| `MASSEFFECT_INDIRECT_DISPATCH` (build env) | `2` | `2` | ru_glob2: verify build, 1,000,000 checks, 0 differences; the CMake default stays 0, so pass it |
| hot natives `masseffect_hot_field_iter` (EN sub_8245FF18), `masseffect_hot_sprite_render` (EN sub_8264E178) | registered, on through `masseffect_hot_guest = true` | same | EN: fuzzed against the EN generated code, 200,000 iterations each, 0 failures (tests/hot_fuzz/cases/case_8245FF18.inc, case_8264E178.inc) |
| `app/function_order.ld` | RU order translated to EN names (1657 entries, 30 without an EN match dropped) | RU profile | stale entries are harmless |
| `masseffect_shader_dump_dir` (debug cvar) | added | had it | |

## Shipped defaults 2026-10-09 (both editions)

Added to `app/masseffect.toml` and made the code defaults (commit "native: console-verified night reworks on by
default"), evidence in "2026-10-09 night" below: `masseffect_native_constants_dirty`, `masseffect_native_vertex_arena`,
`masseffect_hot_audio_resampler`, `masseffect_hot_audio_resampler_stereo`, `masseffect_hot_crt_memset` (all `true`;
A/B on the Citadel spacewalk with 0 verification differences, then all five on together in ru_integ7 with
`run/me1/tour_base_night.toml`). Not yet run on the EN build: check the first EN build with the same route.
Their self-checks stay on (`masseffect_native_constants_dirty_verify` 4096, `masseffect_native_vertex_arena_verify_n`
4096 and 1 in `_verify_every` 64, hot guards): a DIFFERENCE switches the feature off for the session.

## 2026-10-08 locations on build ru_glob8 (shipped-default set + DLC on)
| Location | Swaps per 10 s while playing | Frames over 100 ms | Notes |
|---|---|---|---|
| Asteroid X57 (Bring Down the Sky, Mako) | ~263 mean, ~240 low | 18 (worst 2.2 s at zone loads) | 2 VS dropped by identification (fetch-swizzle patching), fix in progress |
| Therum (Mako, user driving and fighting) | ~297 mean (whole session 293), low 266 | 22 incl. loading | 0 missing shader draws; rumble on (input_rumble = true) |

## 2026-10-08 all RU locations on build ru_glob12 (glob_j.toml, bot walk, video capture off)
Run with tools/me1_ab_loc.sh (new game + AT travel), ~230 s per location after arrival. 300 swaps per 10 s = locked 30 fps.

| Location | Swaps per 10 s mean / min | Frames over 100 ms | Notes |
|---|---|---|---|
| Normandy | 291 / 224 | 3 | |
| Eden Prime | 296 / 246 | 5 | |
| Citadel Presidium | 296 / 245 | 7 | |
| Citadel Wards | 289 / 223 | 1 | |
| Citadel Tower | 293 / 257 | 6 | |
| Feros, Zhu's Hope | 300 / 289 | 1 | |
| Feros (feros route) | 284 / 211 | 12 (worst 349 ms) | drops to 211-244 in the last 90 s |
| Noveria, Port Hanshan | 270 / 248 | 3 | ~255 for the first 140 s, then 300: GPU-bound view |
| Noveria, Aleutsk | 295 / 273 | 1 | |
| Noveria, Hot Labs | 300 / 297 | 1 | |
| Virmire, beach | 300 / 300 | 0 | |
| Virmire, bomb | 297 / 212 | 0 | only the first window low |
| Ocean (Noveria/Peak 15 route) | 300 / 290 | 0 | |
| Therum | 301 / 300 | 0 | |
| Trench | 289 / 215 | 0 | dip in the middle (215-280) |
| Ilos archives | 296 / 259 | 0 | |
| Citadel plaza | 295 / 254 | 4 (worst 719 ms) | |
| Spacewalk | 291 / 161 | 13 (worst 1.13 s) | last window 161: likely a load/cutscene |
| Uncharted world (Mako) | 300 / 295 | 0 | |

Overnight series stopped at 04:55 by an Atmosphere fatal in overlayDisp (0x10801) during bot video capture; rerun with ME1_RECORD_EVERY=0 had no fatal.
Open items: Port Hanshan GPU view, Feros route late drops, spacewalk and plaza long frames.

## 2026-10-08 shipped prewarm list, cold (build ru_glob12, run/me1/glob_k.toml)
List: 1956 records extracted (tools/extract_prewarm_list.py) from the console's cache after the all-location series,
next to the NRO; `masseffect_cold_startup = true` (cache/cold.bin deleted before each run), prewarm_threads = 2,
async_specialized = 2. Prewarm finished ~110 s after launch (1221 real compiles, 215 s of compile time), before arrival.

| Location | Before (warm cache, no list) | Cold + shipped list |
|---|---|---|
| Citadel plaza | 295 / 254, 4 frames > 100 ms, worst 719 ms | 300 / 294, 0 frames > 100 ms, worst 87 ms |
| Spacewalk | 291 / 161, 13 frames > 100 ms, worst 1131 ms | 296 / 282, 0 frames > 100 ms, worst 81 ms |

Kept: ship masseffect_prewarm_list.bin with the build (installer + NSP RomFS) and turn these settings on by default.
Done 2026-10-08: `app/masseffect.toml` sets `masseffect_native_pipelines_shipped_list = "masseffect_prewarm_list.bin"`,
`masseffect_native_pipelines_prewarm_threads = 2`, `masseffect_native_pipelines_async_specialized = 2`; the lists live in
`app/prewarm/masseffect_prewarm_list-<ru|en>.bin` (EN still to be made), are release assets, and the installer puts the
edition's one next to the NRO (zip) or into the NSP RomFS (cold-start-hitches.md, C "Shipping").

## 2026-10-08 occlusion queries at Port Hanshan (route stands ~150 s in the Maeko Matsuo conversation)
Build ru_glob14/15, toml glob_j + the lines below. Baseline mode 0: ~255 swaps/10 s in the conversation window.

| Variant | Conversation window swaps/10 s | Zero results | Notes |
|---|---|---|---|
| mode 1 (all hidden, wrong image) | ~300 | 100 % | upper bound |
| skip_boxes | ~255 | - | no gain |
| mode 2 (real, waits) | ~180 | - | stalls |
| mode 3 (latency-1), skip_prepass on | ~255 | ~3 % | boxes tested against an empty depth (prepass skipped) |
| mode 3 + query_occlusion_depth | ~247 | 62 % | only 16 % of answers hidden: identities move (pooled), prepass drawn twice |
| mode 3 + skip_prepass = false | ~246 | 62 % | no black shards in the screenshots at 960; same identity problem |
| mode 3 + occlusion_depth + history_key = 1 | 212-260 | 43 % | worse; 6110 begins without end; flickering light shafts/flares in the user's video |

Rejected for now: the culling the game gets back does not pay for the prepass, and the history answers cannot follow
UE3's pooled query objects. Port Hanshan's slow window is the conversation close-up, not the port.

## 2026-10-08 Eden Prime, Saren/Nihlus cutscene and the area after it (user playing, build ru_glob19)
Same save and route twice. Analysis of run 1 (prof_eden): ring ~37 ms/frame (XXH3 texture checks 15 %, constants and
vertex copies 13 %, 175-319 occlusion boxes/frame), game thread up to 37 ms/frame, GPU busy up to 37 ms.
Run 2 adds (run/me1/glob_prof2.toml): texture_coherency = 1 (2.8 GB/10 s not hashed, stayed on), ring CPU round 3
(cache_textures_between_frames, texture_inval_by_address, constants_same_content, dedupe_hash_after_copy),
query_skip_boxes = true (all boxes skipped), textures_binding_thread = true.

| Window (same scene) | Run 1 swaps/10 s | Run 2 swaps/10 s |
|---|---|---|
| cutscene start (loading) | 259 | 252 |
| heavy 1 | 243 | 256 |
| heavy 2 | 208 | 234 |
| end (large area) | 175 | 200 |

About +10 % in the heavy windows; still 20-23 fps there. Image: user saw no change. Remaining limits: GPU (scene pass
VS20772/PS21081/PS9551 6-8 ms, ~5 shadowed lights, edram_alias 4-5 ms) and the game thread itself.
Run 3 (build ru_glob20, glob_prof3.toml = run 2 + indirect_dispatch_hot_cache with verify on + edram4_clear_alias_raw64):
heavy windows 266, 226, 229, end 179/191 swaps per 10 s: no measurable change (verify checks every hit, 67 M checks,
0 differences; clear alias fired 189-287 clears per 10 s). The large area after the cutscene stays at ~18-23 fps,
GPU busy 34-37 ms there.

## 2026-10-09 night: the three reworks, A/B on Citadel spacewalk (bot walk, build ru_cdirty / ru_integ1 / ru_integ2)
The route holds ~30 fps already, so fps barely moves; ring savings show as lower ring CPU in heavy scenes (profile
pending). Swaps per 10 s mean / min, GPU per Swap total / gap (ms), guard/verify result.

| Variant | Swaps | GPU / gap | Verification |
|---|---|---|---|
| cA constants old path | 297 / 291 | 33.7 / 14.0 | - |
| cB masseffect_native_constants_dirty | 300 / 295 | 33.4 / 13.8 | 4096 decisions checked, 0 differences, lost sync 0; 0.68 vectors compared per decision, CPU copy 147 B instead of ~3600 B |
| vM vertex arena measure only | 299 / 284 | - | 1.0-1.5 GB per 10 s copied, ~95 % clean+same, clean+CHANGED 0 |
| vB arena + measure | 285 / 244 | - | 0 differences in 46 k verified reuses (the measurement's full hashes cost more than the arena saves) |
| vC masseffect_native_vertex_arena | 300 / 295 | 33.4 / 13.6 | 6.2 GB per 10 s not copied, 50 k verified reuses, 0 differences, 0 undeclared changes |
| aA audio old | 300 / 292 | 33.5 / 13.7 | - |
| aB audio natives (resampler mono/stereo, memset) | 300 / 299 | - | mono and memset guard OK; stereo "DIFFERENCE" was a guard bug (overlapping ranges), fixed in 6aa6847 |

Found on the way: hot native sub_82262EC0 (matrix inverse) differed in the sign of NaN results (15 guard hits in
older runs); fixed by falling back to the original when NaN appears (fix/hot-82262EC0).
Rerun on build ru_integ3 (guard fix 6aa6847), spacewalk, profiles fetched: aA3 300/294, aB3 300/296; guards OK for
sub_82B2D4F0 and sub_82B2D780, no DIFFERENCE. Average CPU per thread (68 profile blocks): ring 60.4 -> 60.5 %,
game thread 48.1 -> 48.3 %, audio thread (third XThread) 11.8 -> 11.2 % (this route is light on audio; the 32 % audio
windows were in the location tour).

## 2026-10-09: memory fixes (build ru_integ5 tour, ru_integ7 spacewalk)
- Location tour on ru_integ5 (pipeline-cache save and texture staging fixes, emergency reserve, `[mem]` report),
  83 min over Eden, Normandy, Presidium and Wards: never-taken heap 551 -> 207 MB (fast at first, ~2 MB/min later),
  0 out-of-memory drops, emergency reserve never released. Stopped by request before the reserve was reached.
- ru_integ7 (integ5 + kernel waits + ring no-alloc + interned/dropped shader variant code, commit 5d18398), spacewalk,
  base toml run/me1/tour_base_night.toml: Swaps 300 / 297, 1 frame > 100 ms (129 ms), picture unchanged.
  `[mem]` 3 min after arrival: variant source copies 0 KB (were ~107 MB), 3719 idle transformed codes dropped
  (90.7 MB), 30 remade (3 ms total, max 583 us), 0 mismatches; never-taken heap 876 MB.
  Next memory target: driver module SPIR-V, 4839 modules 117 MB (Mesa keeps a copy per VkShaderModule).

## 2026-10-10: start reliability (launch loops, bot via HOME forwarders)
| Build | Edition | Launches OK | Failure seen |
|---|---|---|---|
| ru_integ7 | RU | 4 / 5 | frozen intro-movie frame (display queue refused, NWindow poisoned) |
| ru_integ8 (+ Mesa WSI unpoison) | RU | 24 / 25 | 1 abort: shader package read failed in a prewarm thread |
| ru_integ9 (+ read retry, private-copy hot guard) | RU | 19 / 20 | 1 black screen after a recovered display refusal |
| ru_integ10 (+ presenter keeps the swapchain) | RU | 20 / 20 | - |
| en_integ10 | EN | 3 / 6 | Disc Read Error, frozen BioWare logo, console network stalls (cold: no pipeline cache) |
| en_integ11 (+ 64 MB heap to the kernel), prewarm off | EN | 8 / 8 | - |
| en_integ12 (+ masseffect_native_pipelines_prewarm_delay_s = 25) | EN | 6 / 6 held 75 s, 3 / 3 fully cold | - |
| ru_integ12 | RU | 1 / 1 fully cold, 6 / 6 warm | - |

Cause: a cold start compiled the whole prewarm list in the first seconds while the game loaded its startup packages;
the kernel refused resources (svc::ResultLimitReached 0x10801) to the display queue, the file system and even sysmodules.
