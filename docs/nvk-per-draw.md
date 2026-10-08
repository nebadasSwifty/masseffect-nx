# Per-draw CPU cost of Vulkan recording and NVK (2026-10-07)

The ring partition (`masseffect_native_ring_partition`) of the heavy Normandy walk windows shows the "Vulkan draw"
phase at 10-13 us per ring draw, the largest real work phase of the ring thread. This page breaks that time down,
says how much of it is NVK, lists the changes made for it (all default off) and how to check them on the console.

Source: `mass-effect-recomp/run/me1/ringpart_f/console.log` (RU, 1785 MHz, deferred recording and its batched
publication on, NVK "faster draws" on), windows 03:09:47-03:14:17 with 65k-196k ring draws per 10 s, plus reading
`masseffect_native_draws.cpp`, `masseffect_deferred_recording.cpp` and the patched NVK in
`out/mesa-switch-corrected` (pin d4a00ea0ab3 + `mesa/mesa-switch-masseffect.patch`, verified with
`git apply --reverse --check`).

## 1. The main finding: NVK does not run on the ring thread

With `masseffect_native_deferred_recording = true` (the profile) every `vkCmd*` call on the ring thread only
**queues** the call; a worker thread (libnx thread, priority 0x2C, core 0) replays the queue into NVK. So:

- the ring's "Vulkan draw" phase is the renderer's own code in `DrawsVulkanImpl::Draw` outside the sampler loop,
  plus ~8.6 queue insertions per draw (`deferred recording (10 s)`: 932,106 calls queued for ~108k draws);
- NVK's cost (section 3) lands on the worker, which shares cores 0-1 with the ring and the UE3 render thread
  (`docs/cpu-cost-analysis.md`, thread table). It matters for the overbooked cores, not for the ring's own time.

Removing NVK work therefore does not shorten the ring phase directly; it frees core time that the ring and the
render thread compete for.

## 2. Ring thread: what the "Vulkan draw" phase contains

Two denominators matter. The phase is divided by **ring draws** in the partition line; 35-45 % of the ring draws
in these windows never reach Vulkan (depth prepass skips, rejections), so per **recorded** draw the phase is
16-25 us (03:12:57: 2,109.5 ms for 98,642 textured draws = 21.4 us).

`C6 substages` (1 in 64 recorded draws, same windows) times part of it:

| Part (per timed recorded draw) | us | What runs |
|---|---|---|
| vkCmd* calls (BindPipeline, sets, set 4, dynamic state, stencil, VB, IB, Draw) | 1.4-2.7 | queue insertion only (wrapper + arena copy + 128-byte slot) |
| PipelineFor | 0.6-3.2 | pipeline key and lookup, rare creation |
| shared block | 0.5-0.7 | shared constants into the UBO |
| indices stage: targets and discards | 0.6-1.0 | |
| indices stage: indices | 1.6-2.6 | index cache fingerprint and conversion |
| indices stage: vertices and diagnostics | 0.6-1.0 | vertex dedupe fingerprint and copy |
| depth bias | 0.1 | |
| **timed total** | **6-10** | |

The remaining 8-12 us per recorded draw are not covered by any substage: upload space and pass change (stage 3),
uploads of textures, vertices and constants (stage 4), set 4 offsets and push constants (stage 5), and the time
of Draw calls that return without recording. The stage stopwatch (`stages_ns_`) already partitions every timed
draw but was never printed; `masseffect_native_report_stages` (below) prints it.

Per queued call, ~0.15-0.25 us is the queue itself (timed `Draw` 0.28-0.42 us per call, `set 4` 0.35-0.51,
`BindPipeline` 0.42-0.56; part of that is the two clock reads). Each slot is 128 bytes in a 16-byte aligned
array, so a call that carries 32-48 bytes of arguments writes 2-3 cache lines that the worker core just read.

## 3. NVK on the worker: per-draw breakdown

`C6 NVK parts` (NVK's own counters, CNTPCT, 1 in 64 draws; heavy windows 03:10:47, 03:12:58, 03:13:38, 03:14:08):

| NVK entry point | us per draw | Notes |
|---|---|---|
| vkCmdDraw/DrawIndexed, whole | 1.8-2.8 | `nvk_cmd_flush_gfx_state` + 7 dwords |
| - dynamic state flush (`nvk_cmd_flush_gfx_dynamic_state`) | 0.1-1.2 | by dirty groups ("dynamic" improvement on) |
| - shader flush (`nvk_cmd_flush_gfx_shaders`) | 0.4-0.64 | only after a pipeline change; cbuf map compare + shader push words |
| - cbuf flush (`nvk_cmd_flush_gfx_cbufs`) | 0.66-0.85 | set 4 rebinds 3 dynamic UBOs almost every draw |
| - push descriptors, emit | 0.02-0.03, 0.06-0.07 | |
| vkCmdBindPipeline | 0.75-1.4 | 0.4-0.6 binds per draw, 1.3-2.9 us per call; `vk_dynamic_graphics_state_copy` is 0.64-1.15 of it |
| vkCmdBindDescriptorSets (set 4 by differences) | 0.95-1.5 | root 0.24-0.30, cbuf dirtying 0.37-0.47 |
| root table upload | ~0.1 | |
| new pushbuf chunk (64 KB) | ~0.04 | 1.5-1.7 us per chunk, 1 in ~40 draws |
| vkCmdBindVertexBuffers / IndexBuffer / vkCmdSet* | ~0.1-0.3 | runtime setters, cheap |
| **total** | **~4-5.5** | ~1.6-2.2 ms per frame at ~400 recorded draws |

The draw itself is an MME macro (`NVK_MME_DRAW_INDEXED`): the CPU writes 7 dwords and the GPU front end runs the
macro. The CPU cost is the state flush before it.

### Compiler flags of the driver

`out/mesa-switch-corrected/build-switch.sh` (and the build directory that produced `out/mesa-sdk`) configures
Mesa with **`-Doptimization=1`** (`-O1` for C, `opt-level=1` for the NAK Rust compiler), `-Db_lto=false`,
`-fPIC`. `docs/mesa.md` and `mesa/README.md` said "release, -Doptimization=2"; that was wrong. Mesa upstream is
normally built at -O2. -O1 affects the whole driver hot path above (struct copies, dirty-bit loops, inlining of
`nv_push` helpers) and NAK's shader compile time (cold pipeline compiles, ~81 ms each).

## 4. Changes (all default off)

| Switch | Where | Exact? | Saves | Expected |
|---|---|---|---|---|
| `masseffect_deferred_native_compact` (init-only cvar) | `masseffect_deferred_recording.cpp`, `masseffect_deferred_ring.h` | yes: same calls, same order | queue cache traffic: each call packed as 16-byte header + arguments rounded to 16 (32-128 bytes, typically 48-64) in a 64-byte aligned 128 KB byte ring, instead of a 128-byte slot | ~0.05-0.1 us per queued call, ~0.4-0.8 us per recorded draw on the ring, a little less reading on the worker |
| `masseffect_native_report_stages` | `masseffect_native_draws.cpp` | measurement only | - | prints `C6 stages`: us per timed recorded draw of stages 0-6 (sum = the whole recorded draw) plus the pass-change split, and Draw calls vs recorded draws in the window |
| Mesa built at `-Doptimization=2` | `out/mesa-sdk-o2` (no source change) | same source; rendering expected identical | compiler: whole NVK and NAK | NVK worker CPU -10-25 % (~0.5-1 us per draw, ~0.2-0.4 ms per frame on cores 0-1); faster cold shader compiles |

### 4.1 `masseffect_deferred_native_compact`

`masseffect_deferred_ring.h` holds the record layout shared by the app and the host test: `Header {run, bytes}`,
`RecordBytes`, `Needed` (adds a skip record when the call does not fit before the end of the ring), `Fits`,
`Place` (producer) and `RunOne` (worker; a skip record runs nothing). The protocol is the existing one with
positions in bytes: the producer waits for room exactly like `Reserve` (with the batched mode's own copy of the
read index, and publishing what it holds back before waiting), `PublishCompact` publishes every
`masseffect_deferred_native_batch` records in the batched mode, and the worker stores its read index every 16
records. Drain, flush and the arena are unchanged. The worker only ever sees whole records: a skip record is
written before the record that follows it and both are published together. The 10 s report adds
`deferred recording compact queue (10 s): N bytes per call (128 slot bytes before), M ring wraps`.

Test: `tests/cpu/test_native_deferred_queue_compact.cpp` (1 KB ring, record sizes 32-128, classic and batched
modes with batches 1/3/16/100, random drains and idle flushes; 1.2 M records per mode, also clean under
ThreadSanitizer). `tests/run_all.sh` picks it up.

### 4.2 Mesa `-O2` variant

Built from the same patched tree into a separate build directory, so `builddir-switch` and `out/mesa-sdk` stay as
they were:

```sh
# reuses out/mesa-switch-corrected/builddir-native (host tools) and the devkitpro-mesa-rust image
OPT=2 mesa/build_mesa_opt.sh   # meson setup builddir-switch-o2 ... -Doptimization=2, then ninja, then ar -M merge
# result: out/mesa-sdk-o2/opt/devkitpro/portlibs/switch/lib/libvulkan.a
```

The script runs the same container steps as `build-switch.sh` (wrappers, meson link, headers, clang alias), then
`meson setup builddir-switch-o2` with the options of `build-switch.sh` except `-Doptimization=2`, `ninja` of
`libnvk.a libvulkan.a libnak_rs.a libnak.a`, and the `libvulkan.a + libnak_rs.a` merge of
`mesa/build_mesa_docker.sh`. On the Mac (Apple silicon, Docker) it took **61 s** in total (6 s configure, 51 s for
1,234 build steps). The `bfd plugin: LLVM gold plugin has failed to create LTO module` lines of the merge are
harmless (they also appear in the shipped build).

**Result and default (2026-10-07).** On the Switch, -O2 gave +5 % draw throughput in the heavy scenes (15.7k -> 16.6k
draws/s, 25.8 -> 26.4 fps, frames over 60 ms 160 -> 108 per route). It is now the default: `mesa/build_mesa_docker.sh`
builds `MESA_OPT=2`, the local `out/mesa-sdk` is the -O2 archive (`out/mesa-sdk-o2` is a link to it) and the old one is
`out/mesa-sdk-o1` (`MESA_OPT=O1 tools/build_nro.sh` links it). See [mesa.md](mesa.md), "Optimization level".

Builds for the A/B, in `mass-effect-recomp/run/me1/` (both with the new cvars, all off):

- `ru_nvk.nro` / `ru_nvk.elf`: RU build linked against `out/mesa-sdk-o2`;
- `ru_nvk_mesa_o1.nro` / `ru_nvk_mesa_o1.elf`: the same objects linked against the shipped `out/mesa-sdk`.

The -O2 one was produced without touching the shared build folder's CMake cache: after
`tools/edition.sh ru all`, the link command of `out/edition-ru/out/nx` (`ninja -t commands masseffect | tail -1`)
was re-run in the `masseffect-nx-build` image with `out/mesa-sdk/` replaced by `out/mesa-sdk-o2/` and
`-o masseffect_mesa_o2`, followed by the same strip + `elf2nro` (nacp and icon of that folder) as the CMake
post-build step (6 s). Symbol sizes confirm the other driver (`nvk_CmdDrawIndexed` 0x2c0 bytes against 0x2fc).

## 5. Proposed, not done

- **Pipeline bind state copy (NVK, 0.64-1.15 us per draw).** `vk_dynamic_graphics_state_copy` walks every state
  group the pipeline sets, compares and copies it, even when the same pipeline was bound a few draws earlier and
  nothing in between touched those groups. A per-command-buffer "last pipeline copied + generation of
  `vkCmdSet*` calls" would make A-B-A alternation cheap, but needs a generation bump in every runtime setter
  (`vk_common_CmdSet*` in `src/vulkan/runtime/vk_graphics_state.c`). Exact, but a larger patch and not testable
  without the console; do it after the -O2 result is known.
- **Fewer queued calls per draw.** One "draw packet" (pipeline, set 4 offsets, dynamic state, buffers and draw in
  one record, replayed as the same Vulkan calls by the worker) would cut ~8.6 insertions per draw to one. Larger
  change to `Draw`'s recording block (lines around `MASSEFFECT_SUB(0..10)`); measure the compact queue first.
- **Worker placement.** The worker runs on core 0 at the ring's priority (0x2C). Moving it to core 2 below the main
  thread (`docs/cpu-cost-analysis.md` item 1) takes its 4-5.5 us per draw off cores 0-1 altogether.
- **LTO for Mesa**: not usable as is. `b_lto` was turned off upstream, the archive is linked by the NRO's gcc, and
  NAK is Rust (LLVM bitcode). -O3: try only after -O2 is measured (`OPT=3`, same script, ~1 min).
- **NVK measurement cost.** `masseffect_native_nvk_measure = 64` keeps 1-in-64 counters in every draw; set it to 0
  in acceptance runs once the parts are known (small).

## 6. How to verify on the console

One variable per run, warm then cold, the same Normandy walk route (`tools/switch_run.sh`, `--toml`):

1. **Stages first** (`masseffect_native_report_stages = true`, with `masseffect_native_ring_partition = true`):
   read `C6 stages` next to `C6 substages`. Stages 0-6 per timed draw should add up to the recorded-draw share of
   the "Vulkan draw" + "textures" phases; it names the untimed 8-12 us.
2. **Compact queue** (`masseffect_deferred_native_compact = true`): the log must show
   `deferred recording ready: compact queue of 128 KB` and, every 10 s, `compact queue (10 s): ~45-70 bytes per
   call`. Compare the ring partition "Vulkan draw" us per ring draw and the `C6 substages` vkCmd* per-call times
   against the same route with it off. Captures must be identical (it only changes how calls are carried).
3. **Mesa -O2** (`ru_nvk.nro` vs `ru_nvk_mesa_o1.nro`, same toml): compare `C6 NVK parts` (whole draw, BindPipeline
   state copy, sets), the worker's share in the profile (`perfil.log`, the unnamed libnx thread on core 0), fps of
   the heavy windows, and the cold-start pipeline compile times (`C6 prewarm ... ms each` on a cold run). The image
   must not change: same capture points, pixel compare.
