# Performance history

This page tells how Mass Effect went from a few frames per second on the Nintendo Switch to about 25 at the console's
stock clocks (its normal speeds, without overclocking), in the order it happened. Read it to see which changes paid
off, which ones did not, and how much each was worth, before you plan work on this port or on another game. The
reasoning behind each step is in the other documents; this page is the timeline. Every idea, with its exact result, is
catalogued by area in [optimization-paths.md](optimization-paths.md). How the numbers were taken is in
[measuring.md](measuring.md), and what is still wrong is in [known-issues.md](known-issues.md).

## Ground rules for the numbers

- Every number comes from the real console, never from the PC. The PC build was used to check that things work, and
  it misled more than once: it hid CPU costs that the Switch's slow cores made visible.
- "Stock clocks" means the CPU at 1020 MHz with three usable cores (the fourth belongs to the system), and no
  overclock. Two overclocked diagnostic runs exist (t210 and t211 below); they are marked and never counted as results.
- The test scene is the Eden Prime save: the opening level, a route walked automatically. "fps" is the game's own
  frames per second, read from the profiler log in 10-second intervals, not from an on-screen overlay.
- Runs have names `t1` ... `t313`, numbered in order. They are the ones quoted in the backlog of the development
  repository; this page quotes them so a result can be traced.
- The frame budget for 30 fps is 33.3 ms. With three cores, the CPU budget is about 100 core-ms per frame
  (see [core-ms](glossary.md#core-ms-per-frame)).
- The GPU clock is a point where the sources disagree: see "Where the sources disagree" at the end.

## In short

| Step | Result |
|---|---|
| 1. The emulated GPU (stage 2, 29 Sep) | logo movies about 30 fps after fixes, menus 1.5-2.5 fps, in-game never reached (guest crash) |
| 2. The first native renderer on the console (1 Oct) | about 1 fps in the game (2003 ms per frame), then 28.8-30 fps in menus once the GPU profile and the work slots were fixed; in-game still 0.7-1.0 fps |
| 3. Early-Z restored, EDRAM transfer bookkeeping fixed (2 Oct) | 1.5 fps then 6.0 fps |
| 4. Cheaper EDRAM transfers, vertex outputs pruned, redirected clears | 6.7 fps then 11-14 fps (GPU 163 ms then 85 ms per frame; the two figures come from different routes) |
| 5. Internal resolution 960x540 and black shards fixed | 15-18 fps |
| 6. Cheaper guest code and thread priorities | 21-25 fps |
| 7. Code generator options, ring thread, SDK overhead | about 26-28 fps (98.6 core-ms per frame) |
| 8. Today | about 25 fps on average (intervals 19-31), the CPU is the limit |

## 1. The emulated GPU: no usable game (stage 2)

The first Switch builds used the GPU backend that comes with the SDK. It imitates the Xbox 360's GPU
([Xenos](glossary.md#xenos)): it translates the game's GPU commands while the game runs and emulates the
[EDRAM](glossary.md#edram), the GPU's 10 MB of embedded memory. To know when the game writes to memory the GPU has
already read, the SDK watches pages of guest memory.

| What was measured | Result |
|---|---|
| First boot | the memory allocator ran out of kernel memory blocks after about 7 s (the SDK committed memory 64 KB at a time) |
| After the fix (commit memory in whole 2 MB granules) | logo movies run, but the game stalled at 0.1-0.3 fps |
| Cause of the stall | about 47,000 emulated reads per second (a read of a watched page faults on the Switch: the page is unmapped) |
| Logo movies at their worst | 1.2-2.6 fps with about 390,000 emulated reads per second |
| After unwatching hot pages | logo movies about 30 fps, menus 8-10 emulated reads per second, 1.5-2.5 fps in the menu phase |

The game then crashed in a collision function on a pointer held in one NEON register. The measured cause: while a
thread is inside its user exception handler (the SDK's code that emulates a faulting read), the Horizon kernel saves
only half of the vector registers when it preempts the thread, so some come back stale. A probe measured 3.6 % of
emulated reads corrupted under forced preemption (1.2 % with a first fix). The window cannot be closed from user
mode; the only lever is the number of emulated reads. So the decision was the same as for other ports of this
family: stop watching memory by replacing the GPU emulation with a native renderer
(see [native-renderer.md](native-renderer.md)).

Never reached in game: the emulated path crashed after 1 to 10 minutes before a gameplay number could be taken.

## 2. The native renderer, first console numbers (1 Oct)

The native renderer reads the list of commands the game's own [Direct3D](glossary.md#direct3d) layer writes for the
GPU (the [PM4 ring](glossary.md#pm4-ring)) and records Vulkan commands from it on its own thread. It was built on the
PC first, where correctness was the difficulty, not speed: days went into matching the Xbox 360's depth, MSAA and
EDRAM behaviour. The first console measurement:

| Measure | Value |
|---|---|
| Total frame time | 2003.2 ms (about 1 fps) |
| Time the ring thread spent asleep in `vkWaitForFences` | 1795.5 ms (the frame was 100 % GPU bound) |
| CPU per draw | 11.92 µs over about 1,133 draws (13.5 ms) |
| GPU per frame, largest parts | scene 732.5 ms, "reflection" class 464.4 ms, idle gap 207.4 ms |

Four causes were found and fixed that day:

1. The GPU stayed at its 307.2 MHz handheld default, because the system's performance mode was never requested. The
   port now asks for the official 460.8 MHz handheld profile.
2. One work slot and synchronous output serialised the CPU and the GPU. Three slots and asynchronous presentation cut
   the idle gap from 207 ms to 2.3-2.7 ms. Menus, loading and transitions then ran at 28.8-30.1 fps.
3. Several cheap-draw optimisations (stripping the fragment stage of draws that write no colour) were switched off
   and have to be on.
4. A setting meant as an experiment (`masseffect_native_float24_ps_mode`) was on by default. It made every depth-writing
   shader write the depth by hand, which on this GPU disables [early-Z](glossary.md#early-z) and
   [ZCULL](glossary.md#zcull). Every fragment of about 2,700 draws per frame was shaded. It is now off (0) and must stay
   off. With the first three fixes in game rendering was still 0.7-1.0 fps (1236-1551 ms per frame); this fourth one is
   the one that made the scene drawable.

The next recorded in-game number is 1.5 fps at the start of the next day. The sources do not contain a number for the
frame between these two points.

## 3. EDRAM transfers: 1.5 to 6.0 fps (2 Oct)

Mass Effect keeps its colours in several formats inside the same EDRAM words, so the port keeps one Vulkan image
per format and moves data between them when a draw needs a format that is not the owner's
("[mode 4](glossary.md#edram-mode-4)"). Every move is a [transfer](glossary.md#transfer). At first these dominated the
frame, on the CPU and then on the GPU.

| Step | Idea (backlog id) | Result |
|---|---|---|
| a | Area-bounded tile loops (E1): the bookkeeping scanned about 900 tiles twice per draw, with 4300 draws per frame | 1.5 to 2.6 fps (it was CPU, not GPU) |
| b | O(1) fast paths with an ownership epoch (E2) | 99 % of tile visits gone, no fps change (now GPU bound) |
| c | Proven rectangle bounds the area (E3), clears of shadow maps redirected into the owner's view (E4, E5) | 2.6 to 3.0 fps; imports -40 % |
| d | Lazy stencil: depth-only imports, per-tile stencil source (E6) | 3.0 to 3.7 fps |
| e | Stencil through the copy engine instead of masked passes (E11) | 3.7 to 5.7 fps. Maxwell cannot export stencil from a shader; five imports of 720-1440 tiles cost 133 ms per frame |
| f | Newer NVK driver build merged (S8) | 5.7 to 6.0 fps |
| g | Cached format queries, shader identity reuse, stencil from colour sources, multi-rectangle proofs (C1, C2, E12, E13) | about 5.8 to 6.7 fps |

At 6.7 fps the frame was about 163 ms of GPU (scene 66, EDRAM import 22 plus stencil copy 17, export 20, alias 15) and
the ring thread was at 100 % (about 35 µs per draw, about 4300 draws per frame). Without any EDRAM transfer (mode 0)
the ring thread alone capped the game near 7 fps, and characters became see-through, so that mode was rejected (E25).

## 4. The scene itself: vertex outputs

The scene pass (about 4200 draws) cost 78 ms at first.

| Step | Idea | Result |
|---|---|---|
| a | Vertex shaders without the strict "no contraction" flag except on the position path, input remap with `select()` (S1, S2) | scene 78 to 66 ms |
| b | A vertex shader writes only the outputs the pixel shader of that pipeline reads (S6; NVK links no varyings) | scene 66 to 36.6 ms; the frame became CPU bound (GPU idle 32 ms) |

Two probes (M6, M7) showed why. With zero-area scissors (no fragments) the scene dropped from 66 to 45.6 ms, and with
one triangle per draw to 3.8 ms: about 40 ms had been vertex work and only about 4 ms per-draw state. Shader tricks
that targeted pixel work (S15: no branches in the texture-sign helper) moved the scene by 0.8 ms: the pixel shaders were
not the problem.

## 5. Redirected clears and overwrite proofs: 7 to 14 fps

The per-frame EDRAM traffic was a chain of full-screen conversions of the same words. Tile traces showed why: clears
and full-screen passes were being drawn through aliases of other views. The fixes make the renderer prove that a
clear or a full-screen pass overwrites everything, and then skip the transfer.

| Run | Change | Result |
|---|---|---|
| t75 | Batch the transfers of one sync (E17) | no change (132 ms), the cost is volume, not waits |
| t76 | Stencil clear redirect, exact bounds (E33, E30) | 128.9 ms; E30 never fired |
| t78 | Depth and stencil clear redirected (E33b) | 120.5 ms |
| t81 | Clear redirected into a colour owner by decoding the EDRAM word (E34) | 109.1 ms (from 120.9), image correct |
| t97 | Clears sent straight to the consumer learned per site (E36) | export 19.8 to 0.8 ms; 109.5 ms; 9.2-11.8 fps |
| t103 | Blocking fence wait woken by the fence write (C31) | 105.0 ms; CPU 286 to 264 % |
| t114 | Estimator accepts ALU with constants, indexed two-triangle quads, clip crop, OpenGL pixel centres, no-KILL proofs (E37, E38) | **92.4 ms**, 11.6-14.3 fps; image correct |
| t117 | Vertex remap codes normalised (S22) | scene 41.9 to 36.9 ms |
| t119 | Direct calls between recompiled functions (C34; 157,617 calls in 190 files) | 11.9-13.8 fps, steadier; GPU 85.5 ms |

The depth-restore pass (E37) and the tone-map pass (E38) were the last full-screen conversions that could be removed
exactly. What remains are real data dependencies (see "Current bottleneck").

## 6. Cold start: the compile cost

A first run (no caches) must build every pipeline. Run t74 (cold) showed 1.8 fps in one interval; run t115 had 211
pipelines at 137 ms each on the ring thread.

| Run | Change | Result |
|---|---|---|
| t115 | cold baseline of the t114 code | in game 7.0 11.3 5.4 13.8 14.1 9.9 fps; load 2.5 s |
| t116 | NVK per-stage shader cache keys (E32a) | compile per new pipeline 137 to 81 ms (-41 %); in game 14.2 9.1 11.7 14.0 10.3 12.6 |
| t121 | asynchronous pipelines (one worker) | 4515 draws skipped (pop-in) so asynchronous compile stayed off |
| t152 | cold, 960 mode, no prepass | in game 17.3 16.3 14.6 17.1 17.2 13.9; load 7.0 s |

The per-stage key is on in the best configuration (`masseffect_nvk_cache_per_stage`). A shipped list of pipeline keys
so a background thread can compile while the game starts (E32c) was never decided; see [known-issues.md](known-issues.md).

## 7. Resolution and the black shards: 960x540

At 1280x720 the scene pass was the biggest GPU cost. Making the game itself think the screen is 960x540
(`video_mode_width` and `video_mode_height` in the configuration; the game sizes its render targets from it, so the
EDRAM stays exact) was a change of configuration, not code.

| Run | Change | Result |
|---|---|---|
| t138 | reference at 1280x720 | 10.3-13.9 fps |
| t139 | 960x540 | 15.0-17.3 fps; GPU 70 ms including 20 ms idle; scene 37 to 27 ms; image softer |
| t182, t188 | 800x450 and 800x448 | worse (12.5 and 12.7 fps): off the 16-pixel tile grid, fast paths are lost |

The black polygonal shards on terrain (B1, present since the first Switch runs) were found by bisecting with a series of
runs (t138-t148). They are gone without the scene depth prepass: the prepass is drawn into the 2x view and its depth,
brought back to 1x, disagrees with the material pass on slopes. `masseffect_native_skip_prepass` (default on) skips
it; the material pass writes its own depth (t148 at 1280, t150 at 960: no shards on any of 7 legs; t150 16.6-18.0 fps,
GPU 66.9 ms of which about 52 busy). Exact 2x to 1x depth is still open.

## 8. Cheaper guest code and the first CPU wall

The probes of step 4 and the overclock diagnostics showed the game had become CPU bound. Guest code is the recompiled
Xbox 360 code of the game; making it cheaper helps the main thread directly.

| Run | Change | Result |
|---|---|---|
| t153 | guest memory accesses without `volatile` in the generated code (C38) | 16.4-22.3 fps against 14.5-18 (+15-20 %), image correct on 7 legs |
| t156 | shader identification memo by microcode (C39) | ring thread 46 to 44.5 ms per frame |
| t187 | lock-free queue from the game thread to the ring | no gain; heavy views are GPU bound |
| t201, t203 | deferred recording on a worker thread (C20); start-up work (U1, U2) | ring busy about 62 to 40 %; fps unchanged (GPU/game bound); title after 28 s instead of 48 |
| t210 (diagnostic) | CPU 2397 MHz, GPU 1305.6, memory 2400 | about 30 fps most of the time, dips 20-27; not an accepted configuration |
| t211 (diagnostic) | CPU 2397 MHz, GPU stock 768 | about 22 fps: with a fast CPU the stock GPU is the limit; both needed about 1.6x less work |
| t216 | internal resolution 960x544 (viewport, targets, back buffer) | avg 19.1 fps; GPU busy 41-53 to 28-36 ms, idle 21-30 ms: now CPU bound (about 295 % of 3 cores) |
| t219 | non-volatile registers as C++ locals (C45) | +9 % (21.3 avg), image correct |
| t225 | UE3 render thread raised to priority 0x2B | +1.8 fps (23.0 avg) |
| t227 | PhysX result poll yields with core migration | +1 fps (24.0 avg); best at stock clocks that day |
| t232-t233 | vertex fingerprint limit, same-frame index cache | 24.9 avg, within noise |

Why 960x544 and not 960x540: 544 is a multiple of 16, the EDRAM tile height. Run t262 tried 960x540 on the best
configuration: 27.4 fps against 27.7, no gain, so 544 stays.

## 9. Code generator options, ring thread, host overhead

From t247 the metric changed to [core-ms per frame](glossary.md#core-ms-per-frame), because fps in a single run moved by
more than the changes did. The baseline was about 107-110 core-ms total, main thread 32.4 ms, ring thread 28.0 ms.

| Run | Change | Main thread | Ring thread | Total |
|---|---|---|---|---|
| t247-t249 | baseline | 32.4 | 28.0 | 107-110 |
| t250 | registers r0/r2/r11/r12/f0 and vector registers as locals (`non_argument_as_local`) | 31.7 (-2 %) | | |
| t250 | plus `skip_msr` and `reserved_as_local` | 31.3 (-3.4 %) | | |
| t251 | host-overhead agent, all switches | 32.4 | 27.2 | 105.8 |
| t253 | ring thread agent, all switches plus fast vertex copy | 32.0 | 25.7 | 103.3 |
| t255 | "diet": no link-register stores, D-form split, inline `fctiwz`, fast-math flags | **29.1 (-10 %)** | 28.2 | 103.0 |
| t256-t258 | integration of all branches | 30.0-30.5 | 26.4 | 101.0 (26.5 fps) |
| t261 | best configuration with 11 verified native hooks | 29.4 | 26.2 | **98.6 (27.7 fps)** |
| t263 | arguments in registers on the old base | 30.4 (-6 % vs 32.4) | 27.7 | 104.0 |
| t264 | arguments in registers on top of everything | 30.0 | 26.6 | 100.1: **no gain** over t261 |

Per-interval fps of t261: 28.9 25.8 40.6 30.9 22.9 29.8 21.6 26.7 26.4 23.0 28.9. Thirty fps is reached in light views;
the dips to 20-23 are the heavy views (grass fields).

## 10. Probes: where the ceiling is

| Probe | What it does | Result |
|---|---|---|
| t179 (M8) | one triangle per scene draw (960 mode) | scene 34 to 2.4 ms, game only 18.9 to 19.7 fps |
| t180 | scene draws with no fragments | scene 12 ms, game 20.3 fps; the ring thread was at 99.8 % |
| t238 | scene draws dropped on the Vulkan side (parse and shader pairing still run) | about 29 fps: **even with the whole scene free on the GPU, the game reaches only about 29 fps**; the main thread stays at about 90 % of its core (31 ms per frame) |

This is the evidence behind the current analysis: a stable 30 fps needs the main thread about 10 % cheaper as well.

## 11. Latest measurements

| Run | What | Result |
|---|---|---|
| t266-t269 | location sweep (seven maps) | NOR00 27.4, ICE00 29.3, JUG00 32.5, LAV00 31.4, PRO00 28.1, STA00 23.6, WAR00 28.8 fps; rendering correct where the map loaded |
| t271 | saved in-game graphics options (Intermediate display, motion blur, film grain) | 25.7 fps, 103.1 core-ms: these options cost about 1.4 fps (and are the profile on the SD card for later runs) |
| t303 | resolution A/B, same route, back to back | 960x544: 25.4 and 27.1 fps; 1120x624: 20.8 and 19.7 fps (+34 % pixels cost about 6 fps) |
| t310, t311 | colour conversions as fragment passes (`masseffect_native_conversion_frag`) | off 27.2 / 24.5 / 24.8, on 27.2 / 24.3 / 27.9 fps: about +1 fps average on the Eden route, +2.9 fps in Citadel; run-to-run noise about 3 fps |
| t281-t284 | 13 more native hot functions | no main-thread gain (33.4 against 33.6 ms) |

The route has two regimes: a fast one near 27 fps (GPU 33 ms per swap) and a slow one near 24 fps (GPU 54-60 ms per
swap).

## 12. Start-up time

| Run | State | HOME to title |
|---|---|---|
| t187 | before start-up work | about 48 s |
| t203 | shader package loaded by index only (U1: 13.3 s to 1.0 s) and a cached directory index (U2: 8.7 s to 206 ms) | about 28 s |
| t311 | warm caches, from the process's first log line | title visible at 15-16 s, in game at about 44 s |

The remaining time is the game's own loading: 8.6 s of one CPU-bound thread with no I/O, then 128 KB package reads.
The ranked options are in the start-up section of [optimization-paths.md](optimization-paths.md).

## Table of the big wins

| Win | Effect | Section |
|---|---|---|
| GPU at the official 460.8 MHz profile, 3 work slots, async output | idle gap 207 to 2.5 ms; menus 1 to about 29 fps | 2 |
| Early-Z and ZCULL restored (depth export off) | scene drawable at all | 2 |
| Copy-engine stencil (E11) | 3.7 to 5.7 fps | 3 |
| Vertex outputs pruned (S6) | scene 66 to 36.6 ms | 4 |
| Redirected clears (E33, E34, E36) | GPU 120.9 to 109.5 ms; export 19.8 to 0.8 ms | 5 |
| Overwrite proofs (E37, E38) | GPU 109 to 92.4 ms | 5 |
| Vertex remap normalised (S22) | scene 41.9 to 36.9 ms | 5 |
| Direct calls (C34) | steadier 11.9-13.8 fps; GPU 85.5 ms | 5 |
| NVK per-stage cache keys (E32a) | cold compile 137 to 81 ms | 6 |
| Guest video mode 960x540 | 10.3-13.9 to 15.0-17.3 fps | 7 |
| Skip the depth prepass (B1) | black shards gone | 7 |
| Guest memory without `volatile` (C38) | +15-20 % | 8 |
| Start-up (U1, U2) | title 48 to 28 s | 8 |
| Non-volatile registers as locals (C45) | +9 % | 8 |
| Render thread priority (t225), PhysX poll (t227) | +1.8 fps, +1 fps | 8 |
| "Diet" code generator options | main thread 32.4 to 29.1 ms | 9 |
| Ring thread switches | ring thread 28.0 to 25.7 ms | 9 |
| All together (t261) | 98.6 core-ms, 27.7 fps | 9 |

## Table of what gave no gain, or was rejected

| What | Result | Why it did not help |
|---|---|---|
| Link-time optimisation of everything (C35, t122) | 226 core-ms per frame against 218-226 | builds far longer; no gain, option `MASSEFFECT_LTO` stays off |
| Profile-guided optimisation (C36, t160-t162) | 19.6 average but the same-session pair 16.7 against 16.6 | the first number was session noise; plumbing kept (`MASSEFFECT_PGO`) |
| NVK fragment barrier instead of wait-for-idle (E28, t101) | 109.0 ms, unchanged | helps only 3D to 3D; compute and copy-engine switches stay waits |
| Batching one sync's transfers (E17, t75) | 132 ms, unchanged | per-operation waits are not the cost; volume is |
| Dropping duplicate barriers, narrow pass dependencies (E14, E15, E16) | no change | render pass dependencies already wait for idle |
| Constant-register run fast path (t125) | no measurable change | register writes are not the cost |
| Lower resolution on a CPU-bound frame (800x448 t235, 800x450 t182) | no fps change or worse | the frame is CPU bound; off the tile grid the fast paths are lost |
| Higher resolution 1120x624 (t303) | -6 fps | +34 % pixels |
| Mesh LOD range, shadow filter and resolution, decals, motion blur set in the game's configuration file (t175, t185, t186, t208, t236, S13) | no gain | some options are not read, and shadows are not the cost; the 69 to 60.3 ms GPU drop of t186 did not show in fps and was attributed to view noise |
| Branch-free texture-sign helper (S15) | scene 66.5 to 65.7 ms | the scene is bound by per-draw fixed cost |
| "No signs" fast path (S16, t205) | no gain | most textures carry gamma signs; kept off (`masseffect_native_no_signs`, removed) |
| Fused multiply-add in vertex shaders (S23, t151) | scene 28.2 against 28.4 ms | no gain; package v31 not kept |
| ZCULL on the scene depth (S24, t173) | scene 35.3 against 34.7 ms | scene is vertex bound |
| Early-Z for alpha-tested depth writers (S25, t174) | at most -2.3 ms (a wrong-image probe) | not worth it |
| Vertex dedupe off (C3, C41, t155) | 12.7 fps, ring 72 ms per frame | dedupe still saves a lot |
| Exact tile bounds for pixel-aligned rects (E30) | never fires | no pixel-exact rects in the real stream |
| Native replacements of memcpy and wcsstr (t245) | within noise | kept as option, off |
| Native hot functions, 13 more (t281-t284) | main thread 33.4 against 33.6 | too little time in each function |
| D3D state setters natively (t246) | would save about 0.9 ms | the setters cost 0.25 ms; the draw bodies hold the cost |
| PhysX Fix A (t244) | rejected | the step does not run every frame, so the wait times out |
| Smaller generated code (`-Os`, t234) | 21.8 against 24.9 fps (worse) | main thread at 98-102 % of its core |
| Hot function ordering (C40, t158) | 18.7 against 18.9 | kept, no measurable gain |
| Ring wake with a libnx event (C46, t224) | 18.7 fps, ring at 100 % | the wait never slept; reverted |
| Game-thread wait sleeps (t126, t229) | no change | sleep granularity is not the bottleneck |
| Early blocking wait on the ring (C23, t72) | 6.6-9 fps, same | wrong predicate; the fence-word version (C31) was adopted at t103 |
| Official CPU boost (t243) | 3.2-4.5 fps | the system throttles the GPU to 76.8 MHz while it is on |
| Arguments in registers (C47, t264) | no gain over diet plus locals | overlaps with what those already removed; kept as a switch, off |
| Asynchronous pipeline compile (t121) | 4515 draws skipped | pop-in; off |
| Submitting to the GPU every N draws (`masseffect_native_send_each`, removed; t206, t207) | 17.4 and 17.1 against 19.1 | no gain; the frame has only about 2 submissions anyway |
| k_16_16 render targets (t285, t286) | water still absent, planet still disappears | was not the cause; 22.3 fps |
| Four-core mask (docs) | never run | the user decided not to change the console's system configuration |
| Overclock | never accepted | goal is stock clocks |

## Current bottleneck

Two regimes alternate along the route and across levels:

| Regime | fps | CPU | GPU per swap |
|---|---|---|---|
| light views | 27-30 and above | 98-107 core-ms per frame | about 33 ms, with 2.5-3 ms idle |
| heavy views (Eden Prime grass) | 20-24 | 142 core-ms per frame (2.99 of 3 cores), t271 | busy 42 ms, idle 9.5-12 ms |
| Citadel (STA00) | about 20 at default options | 81 core-ms | busy 49.6 ms, **24.2 ms of it colour conversions** |

1. **The CPU is the limit in most views.** Three cores are saturated. The main game thread takes 29-33 ms per frame
   (about 90 % of its core) and cannot be split; the render thread and the ring thread take another 26-28 ms
   each. Probe t238 says that with the scene free on the GPU the game still stops at about 29 fps.
2. **The GPU is busy about 33-52 ms per frame**, and exceeds the budget in a few places: the colour conversions
   (4.5 full-screen conversions per frame at about 5.4 ms each in Citadel, because the engine stores its HDR scene as
   packed 7e3 words and re-reads it in other formats twice per frame) and scene pass 1 (about 2500 draws, about 20 ms
   in the worst grass interval). Of that scene time about 22 ms is pixel work and about 10 ms vertex work at 960x544.
3. **Cold start dips.** Pipelines compile on the ring thread (81 ms each after the per-stage cache). The last recorded
   cold run (t152) had a lowest in-game interval of 13.9 fps, and earlier cold runs reached 8.8-9.1 fps, well below the
   25 fps target for a cold start.
4. **Post-processing** (tone map, blur, grain, motion blur) is about 6-8 ms of GPU and runs at 1280x720 even in the
   960 mode, because the game upscales inside that pass; this cannot be reduced exactly.

## What remains open

| Open item | Why it matters | Next step |
|---|---|---|
| Main thread about 10 % cheaper | needed for a stable 30 | more guest-code work (hot functions, codegen), or the fourth core (declined) |
| Ring thread below 25 ms | second serial stage | the remaining ring ideas in [optimization-paths.md](optimization-paths.md) (hash cost, PM4 decode, record table) |
| Exact 2x to 1x depth | would allow keeping the prepass | not started |
| Colour conversions in heavy views | 24 ms in Citadel | estimator rejects one full-screen quad (G6), resolve straight from the 7e3 view (G7) |
| Cold-start pipeline compile | cold dips | shipped key list (E32c) needs a decision |
| Per-location correctness sweep | Eden Prime alone is not acceptance | needs saves per location |

## Where the sources disagree

The newest dated evidence is preferred in each case.

- **GPU clock.** The stage 3 notes say the console stayed at 307.2 MHz until the port asked for the 460.8 MHz
  handheld profile, and an early note says that profile "is already used". The later notes and the diagnostic runs
  (t210, t211) describe the stock GPU as 768 MHz, and run t212 was discarded because it ran at 460.8 MHz. This page
  does not decide between them; use [measuring.md](measuring.md) to read the clock from the profiler of your own run.
- **Backlog header.** The header of the backlog still says "Current: 6.7 fps"; that line was not updated. The runs log
  below it, newest runs included, is what this page follows.
- **5.7, 5.8 or 6.0.** Step e ends at 5.7, the next group starts at 5.8, and the NVK merge note says 6.0. They are
  within run noise.
- **Start-up time.** The backlog says 48 to 28 s from HOME to title; a later analysis dates the title at 15-16 s from
  the first log line of a warm run. They start the clock at different moments.
- **Package size.** 861 MB, about 930 MB, and 929 MB are quoted for the shader package in different notes (different
  package versions).
