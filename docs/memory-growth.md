# Host memory over long sessions: what grows, what fragments, what now fails gracefully

Long location tours died twice with `std::terminate: std::bad_alloc`:

1. about 70 min in, on the pipeline cache writer thread (fixed in 407bf65, [cold-start-hitches.md](cold-start-hitches.md) A2);
2. after 2.4 h and about 10 maps (`ru_integ4`, `masseffect_native_constants_dirty`, `masseffect_native_vertex_arena`
   and the audio natives on), on the GPU ring thread.

This document covers the second crash: where it comes from, what was changed, and how every long-lived host
table was audited.

## 1. The crash

`run/me1/tour_20261009_055101/hang_1/rex_crash.log`, last entry, symbolized against `ru_integ4.elf`:

```
operator new(unsigned long)
std::vector<unsigned char>::vector(n, value)       <- _M_fill_assign builds a temporary
std::vector<unsigned char>::assign(n, value)
PrepareTexture                masseffect_native_draws.cpp:10369   data.assign(bytes_data, 0)
DrawsVulkanImpl::Draw         masseffect_native_draws.cpp:3650
TargetsVulkan::Draw           masseffect_native_targets.cpp:2713
```

`data` is `temporal_`, the staging copy of a texture being updated: all its mips laid out for the host (up to
tens of MB). `assign` allocates whenever the vector has less capacity than the texture, and that happened often:
`temporal_` was handed to `Texture::data` on every texture update and only came back after the upload. So a
draw that updated two textures, or an update whose upload was postponed, allocated a fresh multi-MB block.

## 2. Why it fails: fragmentation, not exhaustion

On the Switch every host allocation comes from one newlib heap fixed at start-up (`me_packaged.cpp`
`__libnx_initheap`, about 3.1 GB). That includes **Mesa's GPU memory**: `nouveau_horizon_memory.c` backs each
`VkDeviceMemory` with `memalign` + `nvMapCreate`. The guest memory backing is also memalign'd there.

`rex_profile.log` of the same run prints the heap every 10 s (`HeapState`, `switch_perf.cpp`):

| time | malloc in use | free inside the arena (holes) | never taken (top) |
|---|---|---|---|
| 10 s | 1429 MB | 28 MB | 1658 MB |
| 410 s | 2230 MB | 289 MB | 649 MB |
| 2410 s | 2268 MB | 430 MB | 418 MB |
| 4410 s | 2295 MB | 650 MB | 171 MB |
| 7210 s | 2327 MB | 741 MB | 48 MB |
| 7980 s | 2301 MB | 814 MB | **0** |

What is in use grows only about 100 MB in two hours. The holes grow by 500 MB: freed blocks are too small for
the next large request, so the arena keeps taking fresh heap from the top until none is left. After that, any
request larger than the largest hole fails, even with 800 MB free. The texture staging copy was simply the first
large `operator new` to fail.

The main producers of variable-size large blocks:

- **Texture images.** `masseffect.toml` sets `masseffect_native_textures_pool = false`, so every texture gets its own
  dedicated `vkAllocateMemory` (a memalign of its exact size). The tour log shows the 128 MB texture cache constantly
  evicting ("cold textures dripping out", over 31,000 by the end). That is tens of thousands of variable-size
  memalign/free pairs.
- Texture staging copies (fixed here, section 3.1).
- Resolve images, render targets and read-back buffers: rare or bounded (section 4).
- Mesa's own BO cache recycles freed device memory by size, which limits but does not prevent the damage.

## 3. Changes

All of these keep the image the same. Only the out-of-memory paths behave differently: they used to call
`std::terminate`.

### 3.1 Texture staging pool (`masseffect_native_draws.cpp`)

- `StagingReserve(bytes)` runs before `data.assign`. It makes sure `temporal_` has the capacity, so `assign` only
  fills and never allocates.
  - Order of attempts: the current buffer, then the smallest spare buffer that fits, then a fresh one rounded up
    to 1 MB. The fresh one is allocated inside `try`.
  - On `bad_alloc` it frees every spare buffer and the check copy and tries once more.
  - If that also fails, the texture update is skipped. A prepared image keeps its old content; a new one stays
    unbound (slot 0). Its hashes are reset so the next frame reads it again. The ring logs
    `[native] MEMORY: texture staging of N KB could not be allocated`, counted and rate-limited.
- `StagingRelease` takes `Texture::data` back after the upload instead of freeing it. Up to 4 buffers of at most
  32 MB each are kept. Larger ones are freed, as before.
- In steady state a texture update no longer allocates. The cost is a few tens of MB that stay resident.
- The fast-order self-check copy (`check_tile_ = data`) is guarded too. If it cannot be allocated, the check is
  skipped and the draw goes on.

### 3.2 Rectangle-list expansion (`DrawImpl`, primitive type 8)

The expanded vertex streams used to be 16 fresh `std::vector<uint8_t>` per rectangle draw, each up to the 64 MB
upload size. They are now the persistent `rectangle_data_`. A buffer that grew past 4 MB is released at the end
of that draw. A failed resize rejects the draw with cause 60 ("rectangle expansion: out of host memory").

### 3.3 No allocation failure on the ring terminates the game

These handlers are layered, innermost first:

| Where | Catches | Then |
|---|---|---|
| `DrawsVulkanImpl::Draw` (wraps `DrawImpl`) | `std::bad_alloc` anywhere in the draw | Drops the draw, frees the staging and rectangle buffers, logs `[native] MEMORY: std::bad_alloc inside a draw (VS nX PS nY): the draw is DROPPED (N so far); heap used / free / never taken` (first 16, then 1 in 256). |
| `NativeGraphicsSystem::BackDraw` | the same, from the render target code around the draw (EDRAM preparation, publication) | Drops the draw and logs `[native] MEMORY: std::bad_alloc on the ring thread (draw)`. |
| `Packet`, PM4 type 3 | anything else a packet does (copies, resolves, indirect buffers) | The packet's words were already consumed, so the parse stays in step. The packet is skipped and logged the same way (`PM4 type 3 packet`). |

### 3.4 Emergency reserve for `operator new` (`me_native_system.cpp`)

`masseffect_mem_emergency_reserve_mb` (default 32) works like this:

- The first `[mem]` report allocates a block of that size and installs a `std::new_handler`.
- When any thread's `operator new` fails, the handler frees the block, and `operator new` retries. Freeing it
  also reopens a 32 MB hole in the middle of the heap.
- If the block is already gone, the handler throws `std::bad_alloc`, as `operator new` does without a handler.
- The next report logs `[mem] MEMORY: the 32 MB emergency reserve was released by a failed operator new`.
- The reserve is re-armed once the probe shows a free block of at least 4 times its size.

It does not help the C allocations inside Mesa. Those already trim the driver's BO cache and retry, and a failure
there comes back as a Vulkan error that the renderer handles (`Warn(32)` "could not create a texture").

### 3.5 The 60 s `[mem]` line

Every `masseffect_mem_report_s` seconds (default 60; 0 = off), the ring thread logs one line:

```
[mem] heap used 2301 MB, free 814 MB, largest free block 12 MB, never taken 0 MB; process 3185/3189 MB;
      emergency reserve 32 MB armed (0 releases); out-of-memory drops on the ring 0; top caches:
      | shader objects seen 9432, by address 5845, by code 1186, entry of object 9432; paired packets 2210, missing pairs 3
      | targets 41 + depths 17; resolved 64 (~23 MB GPU), clips 3 (2 MB), sleeping 10 (35 MB); read-backs 12 (96 KB, 0 released); restore sites 310
      | textures 950 (95 MB GPU; 0 uploads pending, 0 KB); views 1012, samplers 61, framebuffers 140 (+0 retired), passes 52;
        pipelines 1386 (list 651 + 735 new); modules 905, variants 2400 (0 KB source copies, 61000 KB shared),
        tracked codes 2100 (1900 distinct: 800 library, 150 owned 3600 KB, 950 dropped 36000 KB; 1100 drops 41000 KB so far,
        40 remade 12 ms max 900 us, 0 mismatches), driver module SPIR-V 3100 modules 75 MB,
        rectangle VS 12 (900 KB); library SPIR-V resident 1800 shaders (190 MB); vertex inputs 1300;
        staging 41000 KB (5300 reused, 9 allocated, 0 failed); diag tables 4000/3900/120 (0 resets); draws dropped (memory) 0
```

(The numbers above are illustrative; the real line is one line.)

- **heap:** `used`, `free` and `never taken` come from `mallinfo()` and `fake_heap_end - sbrk(0)`. `process` is
  `svcGetInfo` used/total. Code: `me_heap_report.cpp`, the only file that includes `<switch.h>` for this.
- **largest free block:** a binary search of malloc/free pairs in 1 MB steps, about 10 calls, once per line, capped at 512 MB (printed `>=512`).
  `masseffect_mem_report_probe = false` skips it and prints `?`.
- **What to watch:** `largest free block` going down while `free` goes up is fragmentation. A cache count that
  keeps rising from map to map is growth.

## 4. Audit of the long-lived host tables

Verdicts: **bounded** (with the bound), **evicted** (when), **capped now** (this change), or **kept** (why it is
left alone).

### 4.1 Draws (`masseffect_native_draws.cpp` and its `.inc` members)

| Table | Verdict |
|---|---|
| `textures_`, `views_`, `views_per_image_` | Evicted by the GPU budget (`masseffect_native_textures_mb_max`, 128 MB). About 260 B host per texture. `Texture::data` is freed or returned to the staging pool after the upload; pending ones are shown in `[mem]`. |
| `temporal_`, staging spares, `check_tile_` | Capped now: at most 4 spare buffers of at most 32 MB each (3.1). |
| `rectangle_data_` | Capped now: persistent, any buffer above 4 MB released after its draw (3.2). |
| `indices_`, `indices16_`, `converted_` | Bounded: grow-only, but the count is 16 bits, so at most 65,535 indices (≤ 384 KB). |
| `pipelines_` | Kept. 1,386 after the 10-map tour, about 170 B each in the map. The real cost is in the driver: NVK pipeline objects and the in-memory `VkPipelineCache`, whose serialized size was 56 MB (`pipelines.bin`). Evicting live pipelines would recompile them on the ring (hitches), and the count grows only with new shader/state pairs. Saving is capped (`masseffect_native_pipelines_save_cache_cap_mb`). |
| `modules_` and the transform variants (`modules_depth_half_`, `_depth_quantize_`, `_fragcoord_xy_`, `_texture_signs_`, `_ps_descriptors_`, `_vs_constants_`, `_7e3_`, `_restore_7e3_`, `_fixed16_`, `_alpha_only_`, `_z_early_`, `_rectangle_`), `codes_modules_depth_` | Kept (they follow the pipelines), but their CPU SPIR-V is no longer copied per entry and transformed code is freed when idle: see 4.1.1. `vs_constants` is limited per VS (`masseffect_native_fold_vs_constants_max_values`). |
| Library SPIR-V (`Shader::spirv_lazy`, `masseffect_shader_library.cpp`) | Kept, measured now (`library SPIR-V resident`). Each shader read on demand stays resident because `Spirv()` promises the vector never moves, and modules borrow it (`codes_modules_depth_`). It is bounded by the shaders the game uses, but that set grows with every new map. Eviction needs the borrowers converted first, so it is a follow-up. |
| `ShadersNative::Data::cache` (raw microcode hash → entry, `masseffect_native_shaders.cpp`) | Capped now: cleared at 65,536 entries (a miss only redoes the lookup). |
| `created_per_address_`, `key_per_shape_`, `words_per_shape_` (texture-creation diagnostics, not gated) | Capped now: all three cleared together at 16,384 entries. Each zone load adds new addresses. |
| `inputs_cache_` | Bounded at 4096. |
| `vertices_seen_`, `glyph_trace_pages_` | Bounded (200k / 128) and diagnostic only. |
| `samplers_`, `passes_` | Bounded by the sampler states and target layouts the game uses. |
| `framebuffers_` / `framebuffers_views_` | Evicted with the views they use (`ForgetView`, `masseffect_native_framebuffers_forget_views = true`). `fb_retired_` only grows after a MISMATCH error; a framebuffer is a tiny host object. Shown in `[mem]`. |
| `session_list_`, `index_list_`, `file_list_` | Bounded by the pipelines (one 456 B record each). The saved file is capped at 8192 records. |
| Vertex arena tables, draw cache arena, dedupe, sampler cache, PS descriptor plans | Fixed-size tables or library-bounded (see `me_vertex_arena.h`, `me_draw_cache.h`). |
| `occlusion_retired_` | Only with `masseffect_native_query_occlusion_depth` (off); grows once per view change. |

### 4.1.1 Shader code of the transform variants (2026-10-09, branch `perf/variant-memory`)

The `[mem]` line of a long tour on the memory-fix build read: `modules 829, variants 4627 (107787 KB source copies),
tracked codes 2638 (63093 KB owned), library SPIR-V resident 2064 shaders (48 MB)`.

**What those were.**

- *Tracked codes* (`codes_modules_depth_`, VkShaderModule -> CPU SPIR-V): every pixel shader module that a later
  transform may start from. Library modules (`ModuleFor`) borrowed the library vector. Every module made by a
  transform owned a full copy of its output: no colour writes (`ModuleAlphaOnly`), 7e3 outputs (`Module7e3`), early Z
  (`ModuleZEarly`), texture signs, PS descriptor rewrites, restore into 7e3, k_16_16 encode, FragCoord XY, plus the
  prewarm's own copies of the first three. Owned code was not a duplicate of the library (it is transformed), but it was
  only ever read again when a *new* variant was built on that module (and once by `InputsMaskPS`).
- *Source copies*: every entry of `modules_depth_half_`, `_depth_quantize_`, `_fragcoord_xy_`, `_texture_signs_` and
  `_ps_descriptors_` copied the whole SPIR-V it was made from (`original`), as the exact collision guard of its hash
  bucket. That source is always a tracked code (library or owned), so all 107 MB were duplicates of code that was
  resident anyway: one copy per variant entry (4627 entries). Nothing reads the copy
  after `vkCreateShaderModule` except the equality test of later lookups.

**What changed** (`masseffect_native_draws.cpp`, `ShaderCode` and the functions next to `RegistrarModuleCode`;
`masseffect_ps_descriptors_members.inc`):

1. **Interned code.** Every tracked code is a `ShaderCode` held by `shared_ptr`. Registering code first looks for a
   live `ShaderCode` with the same XXH3, size and words (`codes_by_hash_`), and shares it if there is one. So two live
   `ShaderCode`s never hold the same words, and pointer equality is exactly the old full-content comparison. Library
   code is borrowed (one `ShaderCode` per library vector, `codes_library_`).
2. **Variant entries key on the `ShaderCode`** (`CodePtr source`) instead of a copy. The `shared_ptr` keeps the
   source alive as long as the entry exists, so a pointer can never be reused for other content (this matters for the
   prewarm, whose modules are forgotten and destroyed when it finishes). The hash of a lookup is the stored one, so
   the buckets are the same as before, and the per-lookup XXH3 of the whole source is gone.
3. **Idle transformed code is dropped and made again on demand.** Every owned `ShaderCode` records its parent
   `ShaderCode` and the transform that made it (a `std::function` with the transform's parameters: signs, bits and
   cleared words, slots, mode, 7e3 mask). Once per `[mem]` report the ring frees the words of owned code unused for
   `masseffect_native_shader_code_drop_idle_s` (120 s). When a new variant needs them, `CodeWordsLocked` runs the
   same transform on the parent's words (recursively, if the parent was dropped too). The transforms are pure
   functions of their input and parameters (their tables are keyed by SPIR-V ids, never by pointers). The result is
   still checked: size and XXH3 must equal the stored ones. A mismatch logs `[native] shader code ... could not be
   made again`, counts in `mismatches`, switches dropping off for the session, and the transform that asked is
   handled as "no tracked SPIR-V". That path already existed and rejects the draw or skips the optional fold. It
   never uses wrong code.
4. **Driver copies are counted.** Mesa's `vk_shader_module_create` allocates `sizeof(module) + codeSize` and keeps the
   SPIR-V in every `VkShaderModule`. `CreateModuleCounted` adds up what the persistent tables handed to it
   (`driver module SPIR-V N modules M MB`, cumulative, the prewarm's temporary modules excluded).

**The `[mem]` fields:** `variants N (0 KB source copies, S KB shared)`, where S is what the copies would have been.
`tracked codes T (D distinct: L library, O owned K KB, X dropped Y KB; drops so far, remade R in ms / max us,
mismatches)`.

**Expected saving** on the tour above: the 107 MB of source copies go entirely. Of the 63 MB owned, what stays is
the code used in the last 2 minutes plus the prewarm-only sources until they idle out. Expected: a few MB, so
**about 150-165 MB** in total. The cost is about 150 B per distinct code. The ring spends well under 1 ms per
report on the drop pass. A pipeline miss whose source was dropped pays one transform pass per dropped level (the
same work its first creation did, typically below 1 ms per level), next to an NVK compile of tens of ms. Watch
`remade ... max us`.

**Image:** identical by construction. The modules, the pipelines and the variant lookups produce exactly what they did
before. The only new failure path is the checked one in point 3. `masseffect_native_shader_code_drop_idle_s = 0`
keeps every owned code, which is the old retention. Interning and sharing have no switch, because they change no
result.

**Not done: dropping `VkShaderModule`s after their pipelines exist (point 3 of the request).** Vulkan allows it, and it
would free the driver copies counted above. But module handles are the identity keys of the whole chain:
`codes_modules_depth_`, `masks_inputs_ps_`, `modules_restore_7e3_` / `modules_fixed16_` `{selected, slots}`, the
variant entries, and `modules_` / `_7e3_` / `_alpha_only_` / `_z_early_`. NVK reuses freed handle addresses, so a
destroyed module whose handle comes back as a different module would make those tables return a stale result,
which means wrong shaders. The safe route is a follow-up: give modules a non-Vulkan identity (the interned
`ShaderCode` + transform key already is one), and either recreate the `VkShaderModule` just for pipeline creation
from `CodeWordsLocked` and destroy it right after, or pass the code to `vkCreateGraphicsPipelines` through
`VkShaderModuleCreateInfo` in `VkPipelineShaderStageCreateInfo::pNext` (`VK_KHR_maintenance5`; upstream NVK
supports it, so check that the Switch build exposes it). The `driver module SPIR-V` counter gives its size: it should be about the library modules plus the
variant outputs, so tens of MB.

### 4.2 Render targets (`masseffect_native_targets.cpp`)

| Table | Verdict |
|---|---|
| `reads_` (read-back buffers per destination, rectangle and slot) | Evicted now. Each buffer is a dedicated allocation, and none was ever released. `WriteReads`' 10 s sweep now frees a buffer with no copy for 60 s when no submission in flight (`slots_[].reads`, `pending_reads_`, the list being written) still points to it. A later read-back creates it again. |
| `partial_recorded_` (log-once set of addresses) | Capped now at 4096. |
| `resolved_` (resolve destinations) | Kept, measured in `[mem]`. It grows with distinct destination addresses: 29 new ones in the last 1.5 h of the tour log, mostly 256x256. The images are reused through the bounded sleeping pool (`resolved_sleeping_`, 32 entries / 128 MB). Evicting live entries would change which content a later fetch at that address sees, so it was left alone. |
| `targets_`, `depths_` and their per-image side tables | Bounded by the EDRAM layouts; the side tables are erased in `Destroy`. |
| `resolved_clips_` | Bounded (32 entries / 128 MB). |
| `edram4_restore_sites_` | Kept: about 40 B per (VS, PS, base, slot). It holds the learned confidence that selects the restore path, so clearing it would change the path taken for a while. |
| Per-report maps (`edram4_*_reasons_`, `copies_per_target_`, `reads_per_target_`, pairs, operations) | Evicted every 10 s. |

### 4.3 Ring front end (`me_native_system.cpp`)

| Table | Verdict |
|---|---|
| `paired_packets_` (DRAW packet address → pairing; only the current frame's entries are used) | Capped now: past 16,384 entries the stale-frame entries are erased. |
| `missing_draw_pairs_` (cumulative diagnostic) | Capped now at 4096 distinct pairs. |
| `g_objects_seen`, `g_by_address`, `g_entry_of_object`, `g_code_mapped`, `g_by_code` | Kept, measured in `[mem]`: 9,432 objects / 5,845 addresses / 1,186 codes after the tour, under 1 MB. Clearing them on a map change would also need the lock-free object table and the per-thread `finished_cache` reset. That is a behaviour change for shader identification, which belongs in its own change (see 5). |
| `identity_memo_`, `vs_patched_hashes_`, record queues | Bounded (cleared at 8192 / capped at 4096 / fixed slots). |

### 4.4 Tour and profiler (only when enabled)

| Table | Verdict |
|---|---|
| `g_classes`, the death guard's `verdict` and `skipped` (`me_tour.cpp`) | Evicted now at every map finish, next to `g_props.clear()`, on the game thread like the hooks that fill them. They are keyed by class and UFunction addresses that can be reused after a map change. |
| `g_names` (FName index → string) | Bounded by GNames. |
| `g_pages`, per-map stops, grid and census | Already cleared. |
| `g_stats` (`me_script_prof.cpp`, `masseffect_script_prof`) | Bounded: cleared at 60,000 entries (about 9 MB peak). |

### 4.5 SDK (not changed here)

The SDK's GPU emulation caches (texture cache, shared-memory watches, pipeline caches) are never set up: the native
renderer replaces that graphics system. Three items found during the audit were not changed, because each needs a
console test of its own:

- **Freed guest memory is never returned to the host heap.** `memory_switch.cpp` `DeallocFixed` only re-protects the
  range. Backing stays at its high-water mark: 540 → 564 MB over the tour.
- **Host thread stacks** (16 MB each, `xthread.cpp:467`) are freed only by `pthread_join`/`detach`, which only happens
  after a host-side wait. The thread slots in `rex_profile.log` stay at about 73 over the tour, so no growth was
  observed.
- **`KeInitializeEvent` / `KeInitializeSemaphore`** create a new host object every time the guest re-initializes a
  structure, and the old handle is never released (`xboxkrnl_threading.cpp:454`). About 300–400 B each; the call
  rate is unknown.

## 5. Console tests to run

1. **The same long tour on this build.** Watch `[mem]`: `largest free block`, `free`, `never taken`, and whether any
   `MEMORY:` line appears. Survival is not the goal if `largest free block` still falls steadily: then
   fragmentation still wins, only later.
2. **A/B `masseffect_native_textures_pool = true`** (as an override; the code default is already `true`, but
   `masseffect.toml` turns it off). The pool sub-allocates textures from fixed 32 MB slabs
   (`masseffect_native_textures_pool.h`), which should remove the largest source of variable-size memalign churn.
   Measure `largest free block` after 1–2 h against the pool-off run, plus fps and image. Why the pool is off is not
   documented. Check the image (textures, black patches) before adopting it.
3. If `library SPIR-V resident` reaches hundreds of MB, implement its eviction (4.1 follow-up).
4. **`perf/variant-memory` (4.1.1):** the same tour, then compare `variants ... shared`, `owned`, `dropped`, `remade`
   (count and max us; no hitch should line up with a `remade` increase) and `mismatches` (must stay 0). Check the
   image on the usual capture points. A/B with `masseffect_native_shader_code_drop_idle_s = 0` for the hitch check.

## 6. New cvars

| Cvar | Default | Meaning |
|---|---|---|
| `masseffect_mem_report_s` | 60 | Seconds between `[mem]` lines; 0 = off. |
| `masseffect_mem_report_probe` | true | Measure the largest free block (about 12 malloc/free pairs per line). |
| `masseffect_mem_emergency_reserve_mb` | 32 | `operator new` emergency reserve; 0 = off. |
| `masseffect_native_shader_code_drop_idle_s` | 120 | Free the CPU SPIR-V of transformed pixel shader modules unused for this long (checked once per `[mem]` report, so only while `masseffect_mem_report_s` > 0) and make it again on demand; 0 = keep it (old retention). 4.1.1. |
