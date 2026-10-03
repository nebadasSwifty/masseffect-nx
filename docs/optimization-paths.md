# Optimization paths

A catalogue of every optimization direction tried or considered for this port, organised by area. For each one:
the idea, its status, the evidence (run names and numbers from the console), and the setting that controls it.
The chronological story is in [performance-history.md](performance-history.md); how the numbers were taken is in
[measuring.md](measuring.md). Words you do not know are in [glossary.md](glossary.md).

## How to read this page

**Status words**

| Status | Meaning |
|---|---|
| shipped | on all the time (no setting, or a setting whose default is on) |
| shipped behind a setting | built and measured; on in the best-performing profile, or kept as a switch. The default in the code may be off |
| tried-no-gain | measured on the console, no gain, or worse. Usually kept as a switch, off |
| rejected | measured or analysed and dropped (wrong image, unsafe, or a user decision) |
| open | not done, or built but never run on the console |

**Setting names.** Settings are written in the form `masseffect_...` (the Spanish words in the names, such as `native`,
`ring`, `clear`, are the project's own vocabulary: native, ring, clear). Settings of the SDK layer have no prefix
(`thread_wait_blocking`). Code generator options are keys of the `perf_overrides*.toml` files. Build options are
CMake switches written in capitals (`MASSEFFECT_LTO`). "Backlog id" (such as C34 or E11) is the identifier the idea
had in the working list; it is kept so a result can be traced.

**Rule of the project.** Every idea, also the ones that changed nothing, is recorded, and an idea is not retried
without new data. Ideas that change the picture are also measured, but stay off by default.

## 1. Code generation (the recompiled game code)

The game's PowerPC code is turned into C++ ahead of time ([static recompilation](glossary.md#static-recompilation)).
The generated code keeps the PowerPC registers in a memory structure (the [ctx](glossary.md#ctx)), and about 20 % of
its instructions were loads and stores of it. Most options below make the compiler keep registers in real
registers. The main game thread takes 29-33 ms per frame, so this is the area that decides whether 30 fps is reachable.

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Direct calls: call `__imp__sub_X` straight from the caller for functions that are not hooked (C34). Rerun after every code generation | shipped | t119: 11.9-13.8 fps, steadier; GPU 85.5 ms; 157,617 calls in 190 files | post-generation tool `tools/direct_calls.py` |
| Guest loads and stores without `volatile` (C38) | shipped | t153: 16.4-22.3 fps against 14.5-18 (+15-20 %), image correct on 7 legs, no hang. Risk: a loop that only polls memory could hang | `tools/pch_no_volatile.py` |
| Guest atomics without the global lock (C44) | shipped behind a setting | t218: no hang, image correct, audio thread 29 to 24 %, fps within noise. Functions with atomics are 0.9-1.25 % of samples | `masseffect_lockfree_atomics`; `tools/pch_no_global_lock.py` |
| Non-volatile registers r14-r31 as C++ locals (C45) | shipped | t219: +9 % (21.3 avg), image correct; ctx.r14-r31 uses 1.09 M to 4 k; 35 k save/restore helper calls removed | `non_volatile_as_local = true` (and `cr_as_local`, `xer_as_local`, `ctr_as_local`) in `perf_overrides.toml` |
| Scratch registers r0/r2/r11/r12/f0 and vector registers v32-v63 as locals (C47, "nal") | shipped | static: ctx.r11 uses 1,089,918 to 4,058, ctx.r12 120,513 to 7,070, ctx.f0 154,485 to 193; hot-function code -4.8 % instructions, -15 % stores. t250: main thread 32.4 to 31.7 ms (-2 %) | `non_argument_as_local = true` in `perf_overrides.toml`; needs five `share_registers` marks (stack probe `82AC65E4` and four split-off fragments) |
| No `mfmsr`/`mtmsrd` fences, no `ctx.msr` stores, reservation in a local around guest atomics (C48) | shipped | t250: 31.3 ms (-3.4 % in all). All 96 functions with brackets contain exactly one atomic pair; memory barriers per site 5 to 2 | `skip_msr = true`, `reserved_as_local = true` in `perf_overrides.toml` |
| "Diet": no link-register stores before calls (except four listed return addresses that hooks compare), D-form load/store as base plus zero-extended register plus displacement, inline `fctiwz`, `fcmpu` builtins (backlog "diet") | shipped | t255: -13.3 % instructions on 38 hot functions; main thread 32.4 to 29.1 ms (-10 %), the best single main-thread result | `elide_lr_stores`, `lr_keep_returns`, `dform_disp_split`, `fctiwz_inline` in `perf_overrides.toml` |
| Flags `-fno-math-errno -fno-trapping-math` on the generated code | shipped | part of the diet run t255 | build option `MASSEFFECT_FAST_MATH` (default ON) |
| Arguments in registers (C47, [details](#arguments-in-registers)) | shipped behind a setting (default off) | static -9.9 % instructions; t263 main thread -2.0 ms (-6 %) on the old base; t264 on top of diet and locals: no gain (100.1 against 98.6 core-ms) | `args_in_registers` (default false); `ARGS_IN_REGISTERS=1` for one generation |
| Flush-to-zero (FZ) state handling of the vector unit in generated code | open | the codegen models the PowerPC vector flush mode; no measurement of an optimisation around it is recorded in the working notes. The one place it is mentioned (arguments in registers) lists the flush-mode checks as code that stays in `ctx` | none |
| Smaller generated code (`-Os`) | tried-no-gain (worse) | t234: 21.8 against 24.9 fps, main thread at 98-102 % of its core | `MASSEFFECT_GEN_OPT` (keep -O3) |
| Hot function ordering (hot functions first in the binary) | tried-no-gain (kept) | t158: 18.7 against 18.9 fps | `MASSEFFECT_FUNCTION_ORDER`, `app/function_order.ld`, `tools/function_order.py` |
| Native replacements of hot game functions behind guards (C9, C37) | shipped behind a setting | t261 best profile includes 11 verified hooks. 13 more (t281-t284): main thread 33.4 against 33.6 ms, no gain; individual savings listed in the backlog were 0.1-0.8 ms each and none showed up | `masseffect_hot_guest`, guard `masseffect_hot_guard_calls`, `masseffect_hot_guard_period`, one setting per function (`masseffect_hot_distribution`, `masseffect_hot_crt_memcpy`, `masseffect_hot_crt_wcscmp`, `masseffect_hot_hash_find`, ...) |
| Native `memcpy` and `wcsstr` of the C runtime (t245) | tried-no-gain | within noise of t232/t233 | `masseffect_hot_crt_memcpy` (off) |
| Native replacements of the Direct3D state setters | rejected | t246: the setter group costs about 0.9 ms per frame in total (0.1 µs per call); the real cost is the flush inside the draw bodies (about 6 ms per frame) | none |
| Only 9 Direct3D trace wrappers by default (the other 56 behind a build flag) | shipped | t249: within noise; removes 56 indirections (1-2 % of the render thread by estimate) | `-DMASSEFFECT_D3D_TRACE_ALL` |
| Remove disabled hooks that had a function-local static guard (a load-acquire on every call) | shipped | t254: no measurable change (expected about 0.4 ms) | none |
| `-ffp-contract=off -fno-strict-aliasing` on the game code | shipped | required for correctness: without it the compiler fuses multiply-add and results drift from the PowerPC (the executable grew from 77.6 to 78.8 MB when the flag was applied, proving fusion had been happening) | in `app/CMakeLists.txt` |
| Post-generation patches made regeneration-proof (find functions by search, `--dry-run`, `--check`, fail on missing pattern) | shipped | a new generation moves functions between files; the old fixed file numbers silently lost a patch once | `tools/verify_pch.sh` |

### Arguments in registers

The generated function `sub_X(ctx, base)` becomes a wrapper around `__fast_sub_X(ctx, base, r3, r5, f1, lr, ...)`.
Direct calls between recompiled functions pass r3-r10, f1-f13, r0/r11/r12/f0 and lr as C++ arguments and take r3 back
as the return value. A function nothing calls directly (56 % of them) keeps one body.

- Measured before building anything: 47 % of the `ctx` register accesses are arguments; 72 % of functions contain an
  indirect call, a hooked call or a kernel import, so their entry registers must stay in `ctx`.
- Static result on 1,413 functions of six hot files: -9.9 % instructions, -65 % `ctx` stores, -33 % `ctx` plus stack
  memory operations. On the whole tree: `ctx` stores 1,314,537 to 565,452, code +0.5 % (the wrappers), text +1.7 %.
- Dynamic mix (Mac build): 21.6 % of calls enter single-body functions, 53.5 % direct fast calls, 24.9 % through the
  wrapper (about 14 extra instructions).
- First console run (t260) froze after the first frame: GCC inferred that a read-only `__fast_` body is `pure`, so a
  polling loop around it was hoisted or deleted. Fix: every body starts with an empty `asm volatile("" ::: "memory")`.
  Version 2 (t263) works with 0 hangs.
- Validation on the PC: the first 6 M guest stores of the boot are identical to the old build apart from two timestamps
  and 47 junk spills; a differential fuzz of 20,905 functions found 14,925 identical, 85 excluded as junk-sensitive and
  13 different (10 of them only in a NaN sign or payload).
- Left as a switch because it overlaps with what the locals and the diet already removed (t264).

## 2. Build

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Link-time optimisation over generated code and app (C35) | tried-no-gain | t122: 226 core-ms per frame against 218-226 without; builds far longer; needs the build container raised to 16 GB | `MASSEFFECT_LTO` (OFF) |
| Profile-guided optimisation from a recorded run (C36) | tried-no-gain | t160: 19.6 avg against 18.7-18.9, but the same-session pair t161 (16.7) and t162 (16.6) shows no gain. The profile was applied (hot text section present) | `MASSEFFECT_PGO`, `MASSEFFECT_PGO_GENERATE`, `MASSEFFECT_PGO_DIR`, `MASSEFFECT_PGO_OPTIONS` |
| Smaller code (`-Os`) | tried-no-gain | see section 1 | `MASSEFFECT_GEN_OPT` |
| BOLT (post-link layout optimiser) after PGO | open | never tried; PGO gave nothing on its own | none |
| Pre-generated EDRAM shader includes built by a host tool | shipped | build detail: the compute and fragment utility shaders are compiled to headers by `glslang` before the Switch build | `MASSEFFECT_PREGENERATED_SHADER_DIR`, `MASSEFFECT_GLSLANG_VALIDATOR` |
| Function ordering | tried-no-gain | see section 1 | |

Lesson from LTO and PGO: when the generated code is 7 million lines and the hot functions are already called
directly, the compiler has little left to see across files. The measured gains came from changing what the generated
code does (sections 1 and 3), not from compiler switches.

## 3. Threads and the ring thread

The [ring thread](glossary.md#ring-thread) reads the game's command list and turns it into Vulkan. At the time of the
t172 profile it spent about 35 µs per draw and was near 100 % of a core.

### The ring thread's own work

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Reuse the identification of identical shader reloads (C1) and cache format queries that were made per draw (C2) | shipped | part of 5.8 to 6.5 fps | none |
| Draw records (game thread to ring) in an allocation-free table instead of a map with erase-begin under a shared lock (C33) | shipped | t118: ring 96 to 91 % CPU | none |
| Shader identification memo keyed by microcode content (C39) | shipped | t156: ring 46 to 44.5 ms per frame, 0 unproven vertex shaders | none |
| Lock-free draw-record queue (game thread to ring) (C13 in part) | shipped | t187: image correct, no gain by itself (ring then waits on the GPU) | none |
| Deferred recording: the ring queues every `vkCmd*` call and a worker records them (C20) | shipped behind a setting | t201: ring busy 62 to 40 %, worker 27 %; fps unchanged because the frame was then GPU or game bound. First builds hung: the worker was a standard-library thread created from an SDK thread, and the producer was identified with the standard thread id (SDK threads are not pthreads). Fixed with a libnx worker and a thread-local producer flag | `masseffect_native_deferred_recording`, `masseffect_native_deferred_recording_naps` (8), `masseffect_native_deferred_recording_parts` (diagnostic bisect) |
| Batched publication of the C20 queue (one store and wake test per 16 commands) | shipped behind a setting | ring agent, expected 0.5 ms | `masseffect_deferred_native_fast`, `masseffect_deferred_native_batch` |
| `vkUpdateDescriptorSets` queued instead of about 70 drains per frame | shipped behind a setting | ring waited 1.8-3.8 ms per frame for the worker; expected 1.1-1.7 core-ms | `masseffect_deferred_native_update` |
| Flat memo of vertex and pixel shader identity (direct-mapped tables keyed by entry and load generation) | shipped behind a setting | expected 0.7-0.9 ms | `masseffect_native_flat_identity` |
| Record table on dense arrays, lock-free object table, no draw-queue mutex for the 58 % of draws without a pair | shipped behind a setting | expected 0.5-0.7 ms; host tests against a verbatim copy of the original | `masseffect_native_fast_pair` |
| Compare raw guest words of a shader reload before byte-swapping | shipped behind a setting | expected 0.4-0.6 ms | `masseffect_native_raw_microcode` |
| Constant and fetch register runs swapped and compared four words at a time with NEON, PM4 prefetch | shipped behind a setting | expected 0.4-0.8 ms; 3 M random runs tested against the original loop | `masseffect_native_pm4_fast` |
| Notify ring progress without the mutex when nobody waits | shipped behind a setting | expected 0.2-0.4 ms | `masseffect_notify_no_lock` |
| About 15 `memcmp` calls per draw replaced by inline compares | shipped behind a setting | expected 0.5 ms | `masseffect_native_compare_fast` |
| Sampled fingerprint: unseeded XXH3 per 4 KB block chained with a bijective mix | shipped behind a setting | expected 0.3-0.6 ms (XXH3 was 2.7 ms in t242) | `masseffect_native_fingerprint_mode` = 1 |
| Hardware CRC32 pair as fingerprint | tried-no-gain (by analysis) | the A57 retires one CRC instruction per cycle, so 64-bit strength costs 4 instructions per 16 bytes, no faster than XXH3's NEON loop (about 3 GB/s already) | `masseffect_native_fingerprint_mode` = 2 (kept for an A/B) |
| Fast vertex copy, 64 bytes per iteration with prefetch 640 bytes ahead | shipped behind a setting | `CopyVertices` was the top function; ring 25.7 ms in t253 with it | `masseffect_native_fast_copy`, `masseffect_native_fast_copy_verify` |
| All ring switches together | shipped behind a setting | t252: ring 28.0 to 26.2 ms (-1.8); t253 with the fast copy: 25.7 ms; 0 differences against the self-checks | `masseffect_native_verify_n` (self-check uses per piece) |
| Vertex dedupe (two draws of one frame asking for the same vertex range upload it once) | shipped | turning it off: t155 12.7 fps, ring 72 ms per frame | `masseffect_native_dedupe_vertices` |
| Content check on large ranges skipped (static meshes matched by address, size and byte order) | shipped behind a setting | t232: XXH3 share of the ring 15 to 9 %, image and UI correct, fps in noise | `masseffect_native_dedupe_max_fingerprint` = 16384 |
| Same-frame index cache | shipped behind a setting | t233: 900 k hits, 2.9 GB saved in the run; fps in noise | `masseffect_native_indices_cache` |
| Cross-frame vertex cache | rejected | t230/t231: 27 % of copied vertex bytes equal an earlier frame, but verifying the content costs about as much as the write-combined copy (about 2 GB/s); at most 1 ms | none |
| Epoch invalidation instead of fingerprints for dedupe and index cache (C4) | rejected | the ring writes a read-pointer write-back after every segment (thousands per frame); invalidating at each empties the tables, not invalidating is not provably exact | none |
| Wake the ring with a libnx event instead of mutex plus condition variable (C46) | tried-no-gain (worse) | t224: 18.7 fps, ring at 100 % (the wait never slept; manual-reset event suspected). Reverted | none |
| Stop rebinding descriptor set 4 every draw (C18), submit and fence waits off the ring thread (C19), 16-bit index conversion straight into the upload buffer (C17), mode-4 diagnostic string maps behind a setting (C16) | open | agent estimates 1-2 % of the ring each | none |
| Split the translator into a parse thread and a record thread (C20 variant) | open | large; the deferred recording of C20 already moves part of it | none |
| Hook the game's Direct3D state calls to give dirty bits instead of re-parsing PM4 (C10), register shadow (C11) | open | large and game specific | none |
| Replace the PM4 decode word by word with bulk NEON byte-swap (C5) | shipped behind a setting | done by the `pm4_fast` switch above | `masseffect_native_pm4_fast` |

### Waiting and placement of threads

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Game thread waits for the GPU fence by sleeping until the fence word changes (C23, C31) | shipped behind a setting | t72: rejected, only 10 of 3375 sleeps woke on ring progress (wrong predicate: the game polls the fence word, not the read pointer). t103: fixed predicate, woken by the fence write, CPU 286 to 264 % | `masseffect_wait_blocking_ring`, `masseffect_wait_ring_max_us` |
| Output without waiting for the previous output's fence (C32) | shipped | t102: the ring spent 20 % in `vkWaitForFences` | `masseffect_native_output_no_wait` (default on) |
| Occlusion-query poll waits for ring progress (500 µs) | shipped behind a setting | part of t214; within noise | `masseffect_wait_occlusion_us` |
| Game-thread wait sleeps of 30-400 µs | tried-no-gain | t229: 23.5-23.7 avg against 24.0 | `masseffect_wait_game_us` |
| Sleep in the render-fence wait (t126) | tried-no-gain | no fps change | none |
| Main game thread alone on one core (t217), light guest threads allowed on its core (t221) | tried-no-gain | t217: main thread 85 to 90 %: it is bound by its own work (about 46 ms per frame at the time); t221 within noise | `masseffect_exclusive_core`: the value is the core, plus 10 to allow light threads, plus 20 to raise the render thread, plus 30 for both (tried 2, then 12) |
| UE3 render thread raised to priority 0x2B so it is above the ring and the C20 worker (0x2C) that preempted it | shipped behind a setting | t225: +1.8 fps (23.0 avg); main thread back to 90-99 % of its core | `masseffect_exclusive_core` = 22 (core 2 plus 20) |
| PhysX result poll yields with core migration (a code change), together with light threads on the main core | shipped behind a setting | t227: +1 fps (24.0 avg); main-thread cost of the PhysX join 19 % to 2 % | `masseffect_exclusive_core` = 32 (core 2 plus 30: light threads allowed and render thread raised; used in the best profile) |
| Fourth CPU core (core 3) by a custom homebrew loader descriptor | rejected | researched, never run: the user decided not to change the console's system configuration. Estimated 0 to +3 fps; risks: system services on core 3, scheduler stability (the loader authors removed core 3 in 2018 for "stability problems" with preemptive multithreading) | none |
| Official CPU boost mode (`appletSetCpuBoostMode`) | rejected | t243: 3.2-4.5 fps; the system throttles the GPU to 76.8 MHz while it is on | `masseffect_cpu_boost` (0) |
| Overclock (CPU 2397 MHz, GPU up to 1305.6 MHz) | rejected (diagnostic only) | t210: near 30 fps most of the time; t211: about 22 fps (GPU bound at stock). Not an accepted configuration | none |

## 4. Host runtime: SDK pollers, audio, physics

The SDK layer that emulates the Xbox 360 operating system also costs CPU on the same two cores as the ring and
render threads: its waits poll with 1 ms sleeps, and the audio mixer thread has a higher priority than the ring.

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Multi-object waits parked on a wake point instead of 1 ms sleep polls (Y1) | shipped behind a setting | the audio worker did about 1000 wake-ups per second; expected -1000 per second | `thread_wait_blocking` (default true), safety re-poll 20 ms |
| Alertable waits woken by an APC-style callback instead of 1 ms slices (Y2) | shipped behind a setting | about 8 idle guest threads; expected -3000 to -4000 wake-ups per second | same switch, 8 ms safety net |
| Event, semaphore, timer and mutant notify after unlocking (Y3) | shipped | a woken waiter ran into the mutex the signaller still held (4 % of the mixer thread blocked) | always on |
| Remove a 1 µs sleep in a volume-mask stub the mixer calls every frame (Y4) | shipped behind a setting | a Horizon sleep can give the core away for a whole slice (11.5 % of the mixer thread's wall time) | `audio_volumemask_sleep_us` (0) |
| Guest clock update never blocks (Y5) | shipped behind a setting | main thread 0.4 % blocked; about 0.15 ms per frame | `clock_nonblocking` (default true) |
| Bounded spin on the object-table critical region (Y6) | shipped behind a setting | main thread 0.5 + 0.4 + 0.3 % blocked; about 0.2-0.5 ms per frame | `global_lock_spin` (48) |
| Guest timestamp timer 1 ms to 4 ms (Y7) | open (changes guest-visible tick granularity) | expected -750 wake-ups per second | `kernel_timestamp_interval_ms` (1) |
| Critical-section spin before the full host waiter (Y8) | open | unknown; counter "critical-section host waits" | `kernel_critsec_spin` (0) |
| GPU vblank thread sleeps to the exact next vblank (Y9) | shipped | -940 wake-ups per second at priority 0x2C | `masseffect_vblank_sleep_exact` (default true) |
| NEON native audio kernels (ramped mix, smoother), bit-exact against the recompiled originals (Y10) | shipped behind a setting | 23,411 calls, 0 mismatches in validate mode; expected -3 to -4 core-ms per frame | `masseffect_audio_dsp_native` (0 off, 1 on, 2 validate), `masseffect_audio_dsp_mask` |
| NEON XMA frame conversion (Y11) | shipped | bit-exact against scalar in a host test; about 0.3 ms per frame | always on (aarch64) |
| Native resampler and reverb-like routine (Y12) | open | 1.1 % and 5 % of a core; reverb routine is 1468 assembly lines | none |
| Remove the XMA context zero-fill and sweep (Y13) | rejected | a 10 KB memset per frame is about 1 µs; the cost is the FFmpeg decode (383 µs per context, 165 per second) | none |
| Result of Y1-Y13 together (t251) | measured | "safe" 107.9, "validate" 110.7 (the check costs), "all" 105.8 against 108-110 core-ms: about -3 core-ms per frame (-3 %), main thread unchanged | (the experiment settings files were removed) |
| PhysX step: the game does step it, but the join polls because the "done" event is still signalled from the last frame (PX1) | shipped behind a setting | the poll cost 19 % of the main thread (t225), 12.7 % when the warning is not formatted (t226), 2 % after yield with migration (t227). The simulation itself is about 0.85 ms per frame | `masseffect_physx_no_warnings` (message not formatted), poll yield in code, placement `masseffect_exclusive_core` = 32 |
| "Fix A" for PhysX: block on a condition variable until the simulation has started | rejected | t244: `simulate()` is not called every frame (it runs only when enough time accumulated for a sub-step), so the join waited its 8 ms timeout on those frames; it would have to hook after the sub-step count | `masseffect_physx_wait` (removed) |
| Raise the PhysX thread's priority (Fix B) | open | needs an A/B | none |

## 5. The native renderer

How the renderer works is in [native-renderer.md](native-renderer.md). Here are the optimizations.

### 5.1 EDRAM mode 4: transfers

In [EDRAM mode 4](glossary.md#edram-mode-4) a draw whose target view does not own the tiles it needs triggers a
[transfer](glossary.md#transfer). The first four rows were the CPU cost of tracking tiles; the rest cut the GPU cost
of moving them.

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Mode 4 itself (EDRAM owned per tile, one Vulkan view per format) | shipped | the working mode since stage 3; mode 0 has a 7 fps ceiling and see-through characters (E25: rejected); modes 2 and 3 are experimental (mode 3 flashes) | `masseffect_native_edram_alias_mode` = 4 |
| Area-bounded tile loops (E1) | shipped | 1.5 to 2.6 fps; the old code scanned about 900 tiles twice per draw | none (always on) |
| O(1) fast path when a view already owns every tile (ownership epoch) (E2) | shipped | 99 % of tile visits gone, GPU bound afterwards | `masseffect_native_edram4_fast` |
| Do not transfer a tile again to a view that already received its current version (read-only rebinds) | shipped | part of the 2.6 to 3.0 step | `masseffect_native_edram4_cache_sync` |
| Proven rectangle bounds the area (D3D clear rectangles, full-window scissor) (E3) | shipped | part of 2.6 to 3.0 | `masseffect_native_edram4_area_rect` |
| Import render area and viewport equal to the tile rectangle (E10) | tried-no-gain | no change | `masseffect_native_edram4_import_area_tiles` (on) |
| Lazy stencil: depth-only imports, per-tile stencil source, late fetch, inheritance (E6) | shipped | 3.0 to 3.7 fps | `masseffect_native_edram4_stencil_lazy` |
| No stencil passes before a proven full stencil replace (E7); skip bit passes of bits never set (E8) | shipped | small | `masseffect_native_edram4_bits_stencil` |
| Stencil-only bit-pass shader (no depth write) (E9) | tried-no-gain | no change: the cost is the masked write, not the shader | `masseffect_native_edram4_stencil_light` (on) |
| Stencil through the copy engine: compute to buffer to `vkCmdCopyBufferToImage` (E11, also from colour sources E12) | shipped | **3.7 to 5.7 fps**. Maxwell has no shader stencil export. Costs the 1x depth view its ZCULL plane | `masseffect_native_edram4_stencil_copy` |
| Copy engine only for imports of 32 tiles or more; masked draws below (E15) | shipped | no measurable change | `masseffect_native_edram4_stencil_copy_min_tiles` (32) |
| Multi-rectangle overwrite proofs (E13) | shipped | 6.5 to 6.7 fps; the remaining ones are partial border strips (real data) | `masseffect_native_edram4_several_rect` |
| Narrow render pass start dependency (no MEMORY_WRITE: on NVK it flushes the L1 and shader caches at every pass begin) (E16) | shipped | no measurable change | `masseffect_native_pass_narrow_dependency` |
| Drop duplicate barriers around imports (E14) | tried-no-gain | no change (render pass dependencies already wait) | `masseffect_native_edram4_double_barriers` (removed) |
| Batch one sync's transfers: runs collected first, then compute transfers share one barrier pair and depth imports one pass (E17) | tried-no-gain | t75: about 60 operations per frame merged, GPU 132 ms unchanged. Per-operation waits are not the cost; volume is | `masseffect_native_edram4_batch` (on) |
| Exact bounds for pixel-aligned rectangles (E30) | tried-no-gain | t76: 0 pixel-exact rectangles in the stream | `masseffect_native_edram4_exact_edges` |
| Colour-to-colour conversions as fragment passes instead of compute dispatches (G3) | shipped behind a setting | depth imports cost 0.1 ms per operation against 0.4-0.5 ms for compute operations. t310/t311: about +1 fps average on Eden Prime (noise about 3 fps), +2.9 fps in Citadel; image unchanged by eye | `masseffect_native_conversion_frag` (code default off, on in the best profile) |
| Fewer GPU timestamp marks (only first and last per submission) (G4) | open | about 50 marks per frame (each a flush plus semaphore release); built, never run at 30 fps | `masseffect_gpu_marks_categories` (false = fewer) |
| Mode 0 (no aliasing) | rejected | 7 fps ceiling, see-through characters (E25) | `masseffect_native_edram_alias_mode` = 0 |

### 5.2 EDRAM mode 4: redirected clears

A depth or stencil clear is drawn through a view (the game clears through a 4x alias). If another view owns the tiles,
drawing the clear would move all the tiles there and back. The fix is to prove what the clear writes and apply it in
the owner's view.

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Constant-depth clear rectangles applied in the owner's view (shadow maps) (E4, E5) | shipped | part of 2.6 to 3.0; imports -40 % | `masseffect_native_edram4_redirected_clear`, `masseffect_native_edram4_clear_alias` |
| Full-screen stencil-only clear through the 4x alias applied to each tile's current depth owner (E33) | shipped | t76: half taken, -3 ms; t78: redirect 100 %, 4x to 1x depth round trip gone, 120.5 ms | `masseffect_native_edram4_clear_stencil` |
| Depth plus stencil clear into a colour owner: the EDRAM word decoded as the owner's format (E34) | shipped | t81: GPU 120.9 to 109.1 ms, image correct | `masseffect_native_edram4_clear_color` |
| Clear zero into the other depth encoding, partial edge tiles, straight into the view that consumed the tiles last time (E36) | shipped | t97: export 19.8 to 0.8 ms, GPU 125 to 109.5 ms | `masseffect_native_edram4_clear_zero`, `masseffect_native_edram4_partial_clear`, `masseffect_native_edram4_clear_consumer` |
| Clear siblings of a colour view with the zero word (G5) | rejected | 0 effect (Mac): that clear is not part of the per-frame conversion loop; removed | none |
| Color twin of the depth clear redirect (attenuation clear) (E31), non-tile-aligned shadow-slot clears (E35), shadow-slot clears through a 64-bit colour view (E39) | open | removes about 100 tiles per frame, about 150 clears per 10 s, about 20 small operations per frame; E39 needs proof the low bytes are never read | none |

### 5.3 EDRAM mode 4: proofs that a pass overwrites everything

If a full-screen pass is proven to overwrite every tile it touches, the transfer that would load the old content is
skipped. The proof reads the vertex shader on the CPU to see the rectangle.

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Rectangle estimator for the depth-restore pass: ALU with constants, register reuse, clip inside, OpenGL pixel centres (E37) | shipped | t114: depth 2x to 1x transfer gone | `masseffect_native_edram4_vs_alu`, `masseffect_native_edram4_clip_inside`, `masseffect_native_edram4_center_ogl`, `masseffect_native_edram4_overwrite` |
| Indexed six-vertex quads of two triangles, clip crop to the viewport box, pixel shaders without KILL (E38) | shipped | t112: colour 3 to 0 alias of the tone map gone; with E37 GPU 109 to 92.4 ms | `masseffect_native_edram4_quad_triangles`, `masseffect_native_edram4_ps_no_kill` |
| Post passes at the full 1280x720 in the 960 mode (E40) | rejected (not reducible exactly) | the game itself upscales 960 to 1280 inside post | none |
| Estimator rejects one full-screen replace quad whose fetch format is not admitted (G6) | open | about 2-2.7 ms in Citadel | `masseffect_diag_test_vs` (removed diagnostic) |
| Skip transfers whose destination is overwritten before it is read, by looking ahead in the PM4 stream (E26); whole-frame lookahead and reordering (E29) | open | estimate 10-30 % fewer transfers; risk of stale tiles | none |
| All transfers as 3D draws, no compute or copy engine (E27); exports and aliases as fragment passes (E18); dynamic rendering for barrier control (E19) | open | estimate 30-50 ms for E27; the fragment conversion switch above is the first piece of E18 | none |
| GPU-side scan of stencil bit usage (E20), CPU tracking of constant-stencil tiles (E21), plain image copy for same-format relocations (E22), not-equal depth test on round trips (E23), skip depth binding when neither test is on (E24) | open | small each | none |
| Resolve straight from the 7e3 view as UNORM10 words (G7) | open | only pays if the intermediate view stops being written | none |

### 5.4 Scene cost on the GPU

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Vertex shaders without the strict no-contraction flag except on the position path; input remap with `select()` (S1, S2) | shipped | scene 78 to 66 ms | in the shader package |
| A vertex shader writes only the outputs the pipeline's pixel shader reads (S6; package v30; zero for depth-only draws) | shipped | **scene 66 to 36.6 ms**; frame became CPU bound | `masseffect_native_vs_pruned_outputs` |
| Vertex input remap codes normalised so identity slots take the shader's fast path (S22) | shipped | t117: scene 41.9 to 36.9 ms, exact | `masseffect_native_normalized_remap` |
| Skip the scene depth prepass (fixes B1) | shipped | t148, t150: black shards gone; the material pass writes its own depth. Also removes the prepass's ~20 ms per presented frame of t127 | `masseffect_native_skip_prepass` (default on); `masseffect_native_prepass_keep_stencil` (removed) |
| Fixed-multiply-add (precise `mad`) in vertex shaders (S23) | tried-no-gain | t151: scene 28.2 against 28.4 ms (image was broken at t135, but that was the black-shard bug, not FMA) | none; package v31 not kept |
| Branch-free texture sign helper (S15) | tried-no-gain | scene 66.5 to 65.7 ms | in package v29 |
| Specialisation bit "no texture of the draw has signs" (S16) | tried-no-gain | t204: wrong image before a fix (gamma skipped); t205: exact, no gain: most textures carry gamma signs | `masseffect_native_no_signs` (removed) |
| Linearise gamma textures at upload (S26) | open | exact only if the Xbox 360 filters after linearisation; costs memory and bandwidth | none |
| Hardware texel offsets in `Sample()` (S17), one-sided clamp around log/rsq (S18), input remap as specialisation constants (S3), fewer skip guards (S4), single clamp for relative constants (S5) | open | 1-8 % of one shader's ALU | none |
| Early-Z for alpha-tested depth writers: depth-only prepass plus an equal-test colour pass (S25) | tried-no-gain | upper bound measured with a wrong-image probe: -2.3 ms of scene | none |
| Depth-test-only draws (smoke, particles, decals) declare early fragment tests | shipped | keeps the depth test before shading | `masseffect_native_z_early` |
| ZCULL for the scene depth (S24) | tried-no-gain | t173: scene 35.3 against 34.7 ms; the scene is vertex bound (M7). A driver patch design that keeps ZCULL with stencil-only copies exists in the development notes | `masseffect_native_zcull` (on) |
| Depth export of the float24 value (the Xbox 360's 24-bit depth) in pixel shaders | rejected (on by mistake, then fixed) | makes the shader write depth by hand: disables early-Z and ZCULL on this GPU; was the reason in-game rendering ran at 0.7-1.0 fps | `masseffect_native_float24_ps_mode` (must stay 0) |
| Fragment stage removed for draws that write no colour; alpha-only pixel shaders; skip draws that cannot change a pixel | shipped | the first console run had them off (1 Oct root cause 3) | `masseffect_native_no_ps_no_color`, `masseffect_native_ps_alpha_only`, `masseffect_native_skip_invisibles` |
| Defer the sky behind the opaque scene | shipped (default on) | did not change the image by definition | `masseffect_native_postponed_sky` |
| Shadow pass opened without loading its old content | tried-no-gain | measured t112: loadOp costs nothing on this GPU | `masseffect_native_pass_shadows_no_load` |
| Cheap per-draw state: viewport cache, state by differences, constants as dynamic UBOs, one vertex-buffer binding with vertex offsets | shipped | each measured as no visible change at the time | `masseffect_native_framing_cache`, `masseffect_native_set4_differences`, `masseffect_native_constants_ubo`, `masseffect_native_vertices_base_zero` |
| Merge consecutive guest draws that differ only in constants or vertex ranges (S20) | open (judged unlikely) | only about 100 distinct meshes of about 1100 scene draws, but per-light vertex constants differ, so reuse of output is not exact (M9); another racing port measured the same: 0 % identical state | none |
| Fewer descriptor-set binds and push descriptors (S19), draw offsets zero (S12), per-draw GPU state cost (S11) | open | front end per draw on this GPU | none |
| Drop extra light passes (setting removed) | rejected | t240-t242: 65-70 % of scene draws are repeats of a mesh, but 88 % of the repeats are opaque instances with identical state, only about 9 % are additive light passes | `masseffect_native_lights_max` (removed) |
| Shadow options of the game (filter radius, resolution), decals, motion blur, mesh LOD range, post-process | tried-no-gain | t175, t185, t186, t208, t236, t-nopost: no fps effect (some are not read; Eden Prime shadows are not the cost) | the game's own configuration file |
| Shadow flags of the earlier renderer (no vegetation in shadows, deferred sky, resolves without copies, shadow pass without load) (S9) | tried-no-gain | no change; shadow detection by pitch differs here (pitch below 1600) | `masseffect_shadows_no_vegetation` (off), `masseffect_native_resolver_no_copy` (off) |

### 5.5 Resolution

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Guest video mode 960x540: the game sizes its targets from it, EDRAM stays exact (R1) | shipped | t139: 15.0-17.3 against 10.3-13.9 fps; scene 37 to 27 ms; image softer | `video_mode_width`, `video_mode_height` (SDK settings) |
| Internal scene resolution 960x544 (viewport, targets, device, back and front buffer, UI viewport, HUD marker scale) | shipped | t216: avg 19.1 fps, GPU busy 41-53 to 28-36 ms; 544 is a multiple of the 16-pixel tile height | `masseffect_scene_width`, `masseffect_scene_height`, `masseffect_scene_ui` |
| 960x540 instead of 544 | tried-no-gain | t262: 27.4 against 27.7 fps, image identical | `masseffect_scene_height` |
| 800x450 / 800x448 | tried-no-gain (worse) | t182: 12.5 fps (scene 41 ms), t188: 12.7 fps; t235: no fps change; t259: the title menu did not render because the UI stage is not scaled | `video_mode_*`, `masseffect_scene_*` |
| 1120x624 | tried-no-gain (worse) | t303: 20.8 and 19.7 fps against 25.4 and 27.1; GPU +15-25 ms | `masseffect_scene_*` |
| Scale views by 0.75 inside mode 4 while keeping the HUD at 1280 (R2) | open | design only (2-3 weeks) | none |
| Half-resolution post-processing | rejected (not exact) | the game upscales inside the tone-map pass | none |

### 5.6 Textures, shaders and pipelines

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Shader package loaded by index only, each SPIR-V read on first use (U1) | shipped | library 13.3 to 1.0 s; about 800 MB less RAM (the process had been within about 4 MB of its 3189 MB limit) | `masseffect_shaders_index`, sidecar `.mesp.idx`, `tools/me_index_shaders.sh` |
| Per-stage NVK shader cache keys (E32a) | shipped behind a setting | cold compile of a new pipeline 137 to 81 ms (-41 %) | `masseffect_nvk_cache_per_stage` (code default off, on in the best profile; needs the patched driver build) |
| Pre-warm thread recreates the pipelines of earlier runs from a key list | shipped behind a setting | t311: 1765 pipelines, 16 really compiled (122 ms), the rest cache hits | `masseffect_native_pipelines_prewarm` |
| Asynchronous pipeline compile with workers (E32b) | tried-no-gain | t121: 4515 draws skipped (pop-in), kept off | `masseffect_native_pipelines_async` (removed) |
| Shipped list of pipeline keys so a thread compiles during the 8.6 s CPU-bound guest start-up (E32c) | open | needs a decision: does it count as a cold start? | none |
| Texture memory cap and caches between frames | shipped | 1 Oct root cause 4 (thrashing at 128 MB) | `masseffect_native_textures_mb_max`, `masseffect_native_cache_textures_between_frames`, `masseffect_native_invalidate_textures_each_copy` |
| Resolved images in a reuse pool; sampling at the logical size of the resolved texture | shipped | allocation churn 1445 down to 3 images in the diagnostic run; see [native-renderer.md](native-renderer.md) | `masseffect_native_reuse_alloc_resolved`, `masseffect_native_logical_resolved_size` |
| Texture recheck interval up to 32 frames | open | probable cause of texture popping; test with 4 | `masseffect_native_texture_interval_max` |

## 6. Driver (Mesa NVK for the Switch GPU)

The driver is a build of Mesa's Nouveau Vulkan driver for the Switch, linked into the executable. The patch is in
`mesa/mesa-switch-masseffect.patch`.

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Newer upstream driver build merged with the earlier patch (S8) | shipped | 5.7 to 6.0 fps | patch series |
| Reserve the tail of the push-buffer queue when queuing (GPFIFO filled exactly to the skid made the next submit fail: "device lost" after 4.5 minutes) | shipped | stage 2 run 10 to 11: no overflow in 7+ minutes | in the patch |
| Per-stage shader cache keys (the runtime links vertex and fragment shaders into one key while NVK compiles stages separately) | shipped behind a setting | 137 to 81 ms per new pipeline | `masseffect_nvk_cache_per_stage` |
| Fragment barrier (`PIXEL_SHADER_BARRIER`) instead of wait-for-idle when every barrier is attachment-write to fragment stage (E28) | tried-no-gain | t101: 109.0 ms, no change. Helps only 3D to 3D; compute and copy-engine switches stay waits | `masseffect_nvk_barrier_fragments` (removed) |
| ZCULL kept with stencil-only copies (design) | rejected | measured S24: no gain; design kept | environment switch in the design only |
| Per-draw NVK front-end savings: dirty groups, command emission in one write, cbuf flush skipping, cache hints | shipped | each measured as no visible change at the time | `masseffect_native_nvk_emission`, `masseffect_native_nvk_cbufs`, `masseffect_native_nvk_dynamic`, `masseffect_native_nvk_preload` |
| Push descriptors, non-push set binds, draw-parameter write (S12, S19) | open | pre-Turing GPUs fetch descriptor sets unprefetched at each non-push bind | none |
| Confirm the merged driver kept an earlier compiler tweak (texture latency, select peephole) (S14) and the upstream Switch PR changes (S21) | open (check) | | none |
| Exact 2x multisample images | rejected | not integrated; earlier experience says multisampling can hang this driver on the console | none |

## 7. Platform (Horizon, the Switch's operating system)

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Commit guest memory in 2 MB granules | shipped | stage 2: with 64 KB granules the kernel ran out of memory blocks (error 2001-0103) at 329 MB of backing | in the SDK |
| Official handheld GPU profile 460.8 MHz | shipped | 1 Oct: idle gap 207 to 2.5 ms together with three work slots | requested at start-up |
| Three work slots and asynchronous output | shipped | same run | `masseffect_native_slots_work` (3), `masseffect_native_output_no_wait` |
| Unwatch hot pages of GPU-watched guest memory | shipped (superseded) | stage 2: logo movies from 1.2-2.6 to about 30 fps; removed altogether by the native renderer, which never watches memory | in the SDK |
| NEON registers saved first in the exception entry | shipped | stage 2: corruption 3.6 % to 1.2 % under forced preemption; the rest is a kernel property | in the SDK |
| Close and reopen a host file around a rename (Horizon cannot rename an open file) | shipped | the profile save failed otherwise | in the SDK |
| Process address-space layout (the console's 39-bit address space for processes) and the 3189 MB process memory limit | open | the sources record the limit (the process ran within about 4 MB of it before U1) and a failed device creation on an emulator that reserved fixed allocations at the wrong address; no investigation of the address-space layout itself is recorded | none |
| Title takeover (hold R at launch) for full memory | shipped | needed for the 3189 MB | launch method |
| Run the start-up on an emulator (Ryujinx) as a test bed | rejected | found two start-up bugs the console hides, but NVK cannot create its device there and bindless handles are unsupported; a correct title screen estimated at 15-20 % after days of work | none |

## 8. Start-up

| Idea | Status | Evidence | Setting |
|---|---|---|---|
| Shader package by index (U1) | shipped | -12 s | see section 5.6 |
| Cached directory index of the game folder (4523 entries on the SD card) validated by re-listing 12 directories (U2) | shipped | mount 8.7 s to 206 ms | `vfs_index*` (SDK) |
| Larger read-ahead window so the game's 128 KB reads qualify (the window never hits: 0 hits against 605 direct reads, because the entry threshold is a quarter of the window and a floor of 256 KB keeps the reads out) | open | about 1.0-1.2 s in the first phase, 0.5 s later | `masseffect_io_window_kb`, `masseffect_io_ranges_min_kb` |
| Background prefetch of the recorded start-up read set (about 33 MB cap) | open | 3-7 s (upper bound: 7 s of blocking reads plus 1.2 s of opens before gameplay) | none |
| Read the 77 MB pipeline cache file on a thread started with the process | open | up to 1.5 s | none |
| Store microcode as hashes in the 48.5 MB shader index, or load it on a thread | open | 0.7-1.0 s | none |
| Hide pipeline compile in the 8.6 s CPU-bound window with a shipped key list | open | cold: up to about 10 s of stalls off the path | needs decision (E32c) |
| Replace the next top guest functions natively | open | 0.5-1 s of the 8.6 s (flat profile) | `masseffect_hot_*` |
| Cut the sequence-record log spam (4536 lines in 5 seconds of map load) | open | less than 0.2 s | none |
| Log writes | rejected | not a bottleneck: 298 lines in the first 5 s, already flushed once per second | `log_async` |
| Skip the loading movie `GLO_Relay_LOAD.bik` | open | 0.3 s | none |
| Defer XMA and PhysX initialisation; lazy 930 MB package | rejected | no stall visible in audio init; the package is already index-only | none |

Realistic result of the open items, an estimate: title about 16 s to about 12-13 s warm, map load 13 s to 9-10 s. The 8.6 s
of CPU-bound guest start-up stays as the floor until the guest code is faster.

## 9. What other ports do (checked for ideas)

Another static recompilation port, of an original-Xbox game that also runs on the Switch with the same driver
build, was read for ideas. The finding: it is not faster (it reports 19.5-22.7 fps at stock clocks, from 6-12), most of its
tricks are ones this port already has, and its results confirm several of our negative ones.

| Their result | Our result |
|---|---|
| LTO is noise; PGO not done | the same (C35, C36) |
| Registers as C locals: menu 29-32 to 38-40 fps | our nal and diet options: main thread -10 % |
| Native leaf functions with a compare mode | our hot natives |
| Draw merging "not feasible" (0 % identical state, 45 % differ only in vertex constants) | the same (M9, S20) |
| Core 3 not used: busy threads there starve each other, tested on an emulator | consistent with the four-core research |
| ZCULL through a clear load op, claimed to give about 2 ms | we measured none (S24) |
| Frame lag (wait for the previous frame's fence, not the current one): 9-18 to 21-24 fps | our output-without-waiting setting and three work slots |

Cheap leads not yet taken: per-frame "hitch" log lines with a compile/upload/transfer breakdown (for texture popping
and long frames on planets), generation counters instead of comparing constant blocks on the ring thread, and a cap
on the game's per-frame simulation time. The other project has no licence, so only its ideas were used, never its code.

## 10. Probes: bounds on what a change can give

A probe changes the image on purpose to find the upper limit of an idea. Never compare fps of a probe with a real run.

| Probe | Result | Setting |
|---|---|---|
| Scene draws with zero-area scissor (no fragments) | 66 to 45.6 ms (M6) | `masseffect_diag_scene_no_fragments_s` (removed) |
| One triangle per scene draw | 65 to 3.8 ms (M7); at 960: 34 to 2.4 ms | `masseffect_diag_scene_a_triangle_s` (removed) |
| Scene draws dropped on the Vulkan side | about 29 fps ceiling (t238) | `masseffect_diag_no_draws_s` (removed) |
| Force early fragment tests on alpha-tested Z writers | at most -2.3 ms (t174) | `masseffect_diag_z_early_forced` (removed) |
| Draw statistics (mesh repetition, vertex repeats) | about 100 distinct meshes of about 1100 scene draws, repeats x5 (UE3 per-light passes) | `masseffect_diag_geometry` (removed), `masseffect_native_diag_vertices_repeated` |
| Inclusive time of the Direct3D entry points | draw body 3.5-4.7 ms per frame, setters 0.9 ms | `masseffect_d3d_stopwatch` |
