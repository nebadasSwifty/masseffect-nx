# Zero-copy vertices: letting the GPU read guest memory directly

2026-10-09. Offline investigation (Mac, no console, no build). Question: can the ring thread stop copying vertex
(and index) data from guest memory into the upload buffer (`CopyVertices`, NEON `vld1q`/`vst1q`, up to ~13 % of the
ring in heavy scenes, 1.8-2.2 ms per frame in the t172/t232/t242 profiles) by binding guest memory itself as the
vertex buffer, with an exact image?

## Verdict

**Not feasible as an exact change in this architecture. Nothing was implemented for zero-copy; no cvar was added.**
The recommended alternative, the cross-frame vertex arena, is implemented behind two default-off switches: see
"Cross-frame vertex arena" below.

The driver part is the one that *does* work on paper: the Switch NVK in this tree advertises
`VK_EXT_external_memory_host` and has a real import path (nvmap over a user pointer). The blockers are elsewhere,
and each one alone is enough:

1. **Byte order.** Vertex data is big-endian; the pipelines fetch it with fixed-function Vulkan vertex formats
   (`R16G16_SNORM`, `R32G32B32_SFLOAT`, `B8G8R8A8_UNORM`, ...) that read little-endian. No Vulkan format reads a
   byte-swapped 16- or 32-bit component. Zero-copy needs every vertex shader to fetch raw words and swap/decode
   them itself: a XenosRecomp change and a retranslation of all 275 vertex shaders of the library.
2. **The GPU reads much later than the copy does, and guest fences retire at ring time.** There is one
   `vkQueueSubmit` per frame, from Present. A draw recorded early in a frame is executed by the GPU up to a whole
   frame later. The guest's GPU fences (`MEM_WRITE`, `EVENT_WRITE`, scratch write-back) are written by the ring
   thread when it parses them, not when our GPU finishes, so the game may legally rewrite a dynamic buffer as soon
   as the ring has passed its fence. `masseffect_native_uploads_thread`, which only moved the *CPU* copy a little
   later, already broke dynamic UI and movie vertices (missing letters, stray triangles; known-issues.md,
   cpu-cost-analysis.md). A GPU read is much later still.
3. **No exact way to prove a range static.** Horizon gives no cheap write tracking of guest memory (`RexGmProtect`
   works by unmapping one of five aliased views; writes through the other views, host I/O and resolves are not seen:
   ring-cpu-per-draw.md section 3.4). The C3 coherency events are declarations by D3D, not observed writes: CPU-written
   surfaces (Bink planes) change with no event at all (ring-cpu-per-draw.md F.5). Epoch invalidation was already
   rejected as not provably exact (C4).
4. **CPU cache maintenance.** The Tegra X1 GPU does not snoop the CPU caches. Guest memory is CPU-cached and must
   stay so (the whole game runs on it). A cached import needs a `DC CVAC` clean of every range before the GPU reads it
   (`vkFlushMappedMemoryRanges` / `armDCacheClean`), which is not free and would have to happen at submit time, after
   the last guest write, i.e. it brings back problem 2. A coherent import calls `svcSetMemoryAttribute(Uncached)` on
   the range, which would make guest RAM uncached for the CPU (and would likely be refused on the alias state anyway).

Even if 1-4 were solved, zero-copy could only serve ranges proven static, and those are exactly the ones a
**persistent cross-frame copy** (copy once into our own GPU buffer, already swapped) serves too, without the shader
rework, the nvmap import and the cache maintenance. Zero-copy is dominated by that alternative; see "What to do
instead".

## Evidence

### How the copy works today

`app/src/native/masseffect/masseffect_native_draws.cpp`:

- `WorkCopy` / `CopyVerticesBase` / `CopyVertices` (around line 1570): each binding range `[vmin..vmax]` is copied
  word by word with `xenos::GpuSwap(v, order)` (or the NEON `CopyBlocks64<kOrder>` with `vrev16q`/`vrev32q`). The copy
  is a transformation, not a plain memcpy: the destination is little-endian host order.
- `Draw` uploads stage (around line 3800): per binding, dedupe lookup (`SampleFingerprint` for bindings of 16 KB or
  less), `Reserve` in the upload buffer, `CopyVertices`, dedupe `Note`. The upload buffer is bound with
  `vkCmdBindVertexBuffers`.
- `AttributeFormat` (around line 2050): the Vulkan format of each attribute comes from the fetch constant's format
  (k_8_8_8_8, k_2_10_10_10, k_16_16, k_16_16_16_16, half floats, k_32*_FLOAT, ...). Only the `whole_entry` cases
  (integer fetches) read raw `R32*_UINT` words; everything else relies on the hardware format conversion.
- `shaders/XenosRecomp/shader_recompiler.cpp` `recompile(const VertexFetchInstruction&)`: a vertex fetch becomes
  a read of a typed shader input (`i<usage><n>`, through `remapInput(..., g_InputRemap(location))`). There is no
  shader-side byte swap and no raw-word path for vertex inputs.
- The comment at line ~950 records the measurement that motivated the dedupe: "99.0-99.6 % of the bytes are
  byte-for-byte repeats" (2584 MB repeated within a frame, 3329 MB equal to an earlier frame, of 5955 MB). After the
  in-frame dedupe, what is still copied is mostly data equal to an earlier frame.

Why no format trick works on the unswapped bytes (order k8in32, the common case): an `8_8_8_8` word could be read
as `R8G8B8A8` with the components reversed (a swizzle), but a 16-bit or 32-bit component (positions, half-float
texcoords, `16_16` normals) arrives with its bytes reversed inside the component, which no `VkFormat` decodes.

### What a shader-side fetch would need

- XenosRecomp: declare every vertex input as `uint4` (`R32G32B32A32_UINT` etc. at the same offsets; Xenos fetches are
  dword aligned), then per input: byte swap per endian mode (Maxwell `PRMT`, one instruction per word) and decode the
  format (unorm/snorm 8/10/16, `2_10_10_10`, `10_11_11`, half, float; signed/normalized/"whole" flags, `exp_adjust`).
  The format and endian of each input location would come from a per-draw word table next to `g_InputRemap`
  (shared words 74-89), or be specialized into the pipeline.
- Retranslate the 275 vertex shaders (the library package), keep `spirv-val` clean, and recheck the DXC failures.
- GPU cost: tens of ALU instructions per vertex per attribute, on a GPU that the ZCULL work found vertex bound
  (mesa.md). Small in absolute terms (~0.1 ms per frame estimated), but not zero and not free of risk on the skinned
  shaders that already spill near 80 GPRs (vertex-shader-specialization.md).

This alone is a large rework, and it does not address problems 2-4.

### The driver: `VK_EXT_external_memory_host` exists on Switch NVK

Source: `out/mesa-switch-corrected` (the tree `mesa/mesa-switch-masseffect.patch` applies to).

- `src/nouveau/vulkan/nvkmd/switch/nvkmd_switch_pdev.c:473`: `.has_host_ptr_import = true`, so
  `nvk_physical_device.c:259` exposes `EXT_external_memory_host`; `minImportedHostPointerAlignment = 4096`.
- `nvkmd_switch_dev.c` `nvkmd_switch_dev_import_host_ptr` -> `nouveau_horizon_memory_create` with `import_host_ptr`
  (`src/nouveau/horizon/nouveau_horizon_memory.c` ~line 510): `nvMapCreate(map, host_ptr, size, 0x1000, Pitch,
  cached)`; pointer and size 4 KB aligned; size below 4 GB (guest physical memory is 512 MB, fine). For a coherent
  memory type it then calls `svcSetMemoryAttribute(addr, size, 8, 8)` (uncached). For a cached one it only does
  `armDCacheClean` once at import; later coherence is the application's job (flush ranges).
- `nvk_GetMemoryHostPointerPropertiesEXT`: on `__SWITCH__` both HOST_CACHED and HOST_COHERENT types are offered.

So a VkBuffer over guest memory is possible in principle, with these unverified points:

- Guest memory layout (`sdk/src/core/guest_memory_switch.{h,cpp}`): each chunk is `memalign`'d heap, turned into a
  code alias (`svcMapProcessCodeMemory`, the "shadow", made RW with `svcSetProcessMemoryPermission`), then mirrored
  into the 4.5 GB window views with `svcMapProcessMemory`. The original heap pointer is left locked with permission
  0 and must not be used. The import would have to use the shadow (AliasCodeData state). By the kernel's state
  flags that state should allow device mapping, but nvservices accepting it was never tested; the window views
  (process-memory state) are less likely to be accepted.
- One import per chunk (2 MB granularity, `kBlockSize`), so a binding that crosses a chunk boundary needs the copy.
- The import must be a cached memory type (see problem 4).

### Timing: when the GPU reads

- `masseffect_native_draws.cpp` ~line 937: "there is a single vkQueueSubmit per frame, at the end, from Present"
  (`masseffect_native_send_after_shadows` splits it in two at most). Recording happens on the ring while the guest
  render thread runs ahead; the GPU executes after the submit, and frames overlap.
- docs/frame-handshake.md: the D3D scratch write-back and the fences are written by the ring's `WriteRegister` /
  `MEM_WRITE` handling, so the guest sees "GPU done" when the ring parsed the packet.
- docs/multithread-translation.md, rule R1: every guest-visible effect of the ring (fence and memory writes,
  scratch write-back, interrupts, occlusion results, read-pointer write-back) must happen after all guest-memory
  reads of the draws before it. A GPU-side read of guest memory breaks R1 by a whole frame; the
  `uploads_thread` corruption is the same rule broken by a much smaller delay.

The only exact fix of this would be to delay every guest-visible fence until our GPU has executed the work before
it, which serializes guest and GPU (no CPU/GPU overlap): far more expensive than the copy.

### Verify mode considered

"Hash the direct range at record time and again when the GPU has used it" can only *detect* a race after the frame
is already shown (the hash at completion time does not tell what the GPU read in between), and it costs a full hash
of the range, which is what the copy costs. It is a measurement tool, not a safety net.

## What to do instead (recommended next step)

The saving zero-copy aims at (the copies of ranges that are equal to an earlier frame) can be had with a
**persistent cross-frame vertex arena**: a device-local or uncached buffer holding the already swapped copy of a
range, reused across frames while the range is known unchanged. It needs no shader, driver or memory-map change;
the GPU reads our own memory, so problems 1, 2 (for the arena contents) and 4 disappear. What remains is the
validation, the same question C3 answered for textures:

1. **Measurement mode first (image identical):** for each copied binding, look up the previous frame's entry of the
   same `(address, bytes, order)`; classify by `me::native::texture_coherency::Clean(address, bytes, stamp)`
   (no event since that copy) x full XXH3 equal/changed. Report `clean+same` MB (what the arena would not copy) and
   `clean+CHANGED` (undeclared writes: must be 0 outside per-range exemptions, as with the Bink planes).
2. If `clean+CHANGED` is 0 for stable ranges: the C3 mode-1 pattern (confirmed-stable ranges only, per-range sticky
   exemption on any undeclared change, first N and 1 in M clean reuses still hashed, session guard). The previous
   rejection ("verifying content costs as much as the copy", cpu-cost-analysis.md) assumed a full hash per use; the
   event stamps make a check O(16 KB pages).
3. Keep the in-frame dedupe as it is; the arena would sit behind it (dedupe miss -> arena lookup -> copy).

Expected saving: the CopyVertices share of the ring (4.9-6.1 % of the busy samples in the F profile, 1.8-2.2 ms
per frame in heavy scenes), minus the arena lookups; the share that is truly dynamic (particles, UI, sprites,
movies) stays. Only the measurement mode can say how much is "clean+same".

Console checks for that follow-up (not for this document): the `clean+CHANGED` count over the location tour, the
ring CPU % in the Feros firefight and the Normandy cockpit, and captures of UI text and Bink movies (the cases that
broke `uploads_thread`).

## Cross-frame vertex arena (implemented 2026-10-09, both switches default off)

Branch `rework/vertex-arena`. Offline: compiled (`-fsyntax-only` with the Switch toolchain flags of `out/nx`), host
test `tests/cpu/test_native_vertex_arena.cpp` (`tests/run_all.sh vertex_arena`); **not run on the console, no NRO
built**. Code: `app/src/native/masseffect/me_vertex_arena.h` (logic, host testable), `me_vertex_arena_cvars.inc`,
`me_vertex_arena_members.inc` (renderer side), hooks in `masseffect_native_draws.cpp`; `me_texture_coherency.h` gained
per-consumer switches.

### Where it sits

`Draw`, vertex upload loop, per binding: in-frame dedupe `Search` (unchanged) -> on a miss, **measure** (switch 1) ->
**arena** (switch 2) -> otherwise `Reserve` + `CopyVertices` into the upload buffer as before. The rectangle draws
(`type == 8`) are never measured or served.

### Switch 1: `masseffect_native_vertex_arena_measure` (measurement only, image identical)

For every range the loop is about to copy: `stamp = texture_coherency::Current()` (taken before the bytes are read),
full XXH3 of the guest bytes, key `(guest address, bytes, byte order, stride)`. The previous copy of the same key
(hash, stamp) classifies this one: **clean** = `Clean(address, bytes, previous stamp)` (no coherency event and no
read-back on its 16 KB pages since that copy) or **dirty**, x content **same** or **changed**. Every 10 s:

```
[native] vertex arena measure (last 10.0 s): ranges N (X MB copied) | first seen n (MB) | clean+same n (MB, what the
arena would not copy) | clean+CHANGED n (MB, must be 0 outside exempt ranges) | dirty+same n (MB) | dirty+changed n (MB)
| coherency events E, ranges tracked T, table resets R
[native] vertex arena measure: top clean+CHANGED ranges (D distinct): 1C2A4000+3072 (stride 24, order 2) x41; ...
```

(one line each in the log). The second line (top 5 by count, only when there were any) names the ranges written
without an event: compare their addresses and sizes with the UI / particle / Bink buffers to decide exemptions. The
table holds at most 131072 keys and starts over when full (counted as table resets). Cost: one full XXH3 per copied
range (comparable to the old full-hash dedupe, ~18 % of the ring): use it for measurement runs only. With the arena on
as well, a clean+CHANGED range found by the measurement makes its pages exempt at once.

### Switch 2: `masseffect_native_vertex_arena` (the saving)

A persistent host-visible vertex buffer (`masseffect_native_vertex_arena_mb`, default 32 MB, 8 segments) holding the
byte-swapped copy of ranges. Per binding after a dedupe miss (`VaBinding`):

1. Ranges below `_min_bytes` (256) or above a segment (4 MB at 32 MB), and ranges touching an **exempt page**, are copied
   as before.
2. **Promotion.** First sighting of a key: remember its fingerprint (the dedupe's `SampleFingerprint`, usually already
   computed), copy as before. Seen again with the same fingerprint: copy it **into the arena** instead of the upload
   buffer (one copy, same cost as today) and bind the arena. Ranges whose content changes every frame never get in.
3. **Store.** `stamp = Current()` is taken right before `CopyVertices` reads the guest bytes; with verification on, a
   full XXH3 is taken between the stamp and the copy.
4. **Hit** (any later draw or frame): the entry's segment is not recycled or draining, **no page of the range has a
   stamp newer than the store's**, and the content check agrees (`_check`: 0 = none, 1 = sampled fingerprint
   (default; whole range for ranges up to 8 KB), 2 = full XXH3). The draw binds the arena buffer at the entry's offset;
   nothing is copied. Not noted in the in-frame dedupe (whose offsets are upload buffer offsets).
5. **Dirty** (an event touched the pages): same fingerprint -> copied into the arena again with a fresh stamp;
   different -> back to candidate.
6. **Clean but changed** (the check failed although no event was seen: an undeclared write): copied as before, its
   pages become **exempt for the session** (logged, first 32 and every 256th). If it was a reused range larger than
   8 KB with check 1 (where earlier hits could have missed a change outside the sampled blocks), it counts against
   `_max_undeclared` (4); above that the arena turns **off** for the session.
7. **Verification**: the first `_verify_n` (4096) hits and then 1 in `_verify_every` (64) also hash the guest range in
   full and compare with the hash taken at the store. A difference logs `DIFFERENCE: vertex arena` and turns the arena
   off for the session.

**GPU lifetime.** The arena is never written where a frame still on the GPU may read. Segments are filled in turn;
every store and every hit records, per work slot, the newest frame using that segment. A segment is recycled (its
entries die by an epoch bump) only when, for every slot, that frame has completed (the slot was reused, i.e. its fence
waited; same rule as the index arena of frame-coherence.md 4.4). Hot ranges would keep a segment busy forever, so once
the current segment is half full the next one **drains**: it serves no more hits, its last readers complete within
~2-3 frames, and its hot ranges are copied again into the current segment on their next use (one copy per trip round
the arena: an approximate LRU without moving anything the GPU reads). When the next segment is still busy, stores wait
(copies go to the upload buffer). Non-coherent memory is flushed in `BeforeSend` (`VaFlush`).

**Binding.** Arena bindings never take the base-zero path (`CheckBaseZero` checks upload-buffer limits): a draw with an
arena binding binds explicitly (redundant binds of the same buffers and offsets are still skipped; the comparison now
includes which bindings come from the arena). The postponed sky saves the arena mask and rebinds the same buffers.

**Coherency stamps.** `me_texture_coherency.h` now records marks while *any* consumer is on (`kConsumerTextures`,
`kConsumerVertices`). Before, the texture recheck's own guard (`Enable(false)` after a DIFFERENCE) would have stopped the
marks under the arena, which would then have trusted stale stamps. The vertex consumer is switched on in `Initialize`,
before any copy; it is switched off only when the arena turns itself off and the measurement is off.

### Exactness argument and its limits

- The arena never serves bytes that differ from the guest's **at the time of the store** (it is a copy made then) and
  is never overwritten while a GPU frame may read it (host test: random frames over three slots, stores, hits, guest
  rewrites; every overwritten copy's readers have completed; every hit's bytes are the stored ones).
- Whether the guest bytes are still the same **at hit time** rests on the same hypothesis as C3 for textures: the game
  declares every rewrite of memory the GPU may have read with a coherency event (COHER_BASE/SIZE_HOST) in stream order.
  The stamp is taken before the copy reads, so an event issued before the copy is covered by the copy and an event
  issued after it makes the range dirty. Undeclared writes are what switch 1 measures. The guards: sampled fingerprint
  (exact for ranges up to 8 KB, which covers most UI and sprite buffers), sticky page exemption, the session limit,
  and the full-hash verification schedule. With `_check = 2` a hit is exact up to a 64-bit hash collision (but saves
  only the copy's write).
- In-frame repeats keep their existing argument (dedupe) and do not depend on the arena.

### Expected saving

`CopyVertices` is 1.8-2.2 ms per heavy frame (4.9-6.1 % of the ring's busy samples, F profile). After the in-frame
dedupe, the PC measurement says ~98 % of what is still copied is equal to an earlier frame (3329 of ~3371 MB); the
console's clean+same share (switch 1) is the number that matters. If it is ~80-90 %: **~1.2-1.8 ms per heavy frame**
less on the ring (3-5 %), minus the per-hit costs: one table slot and 1-20 page stamps per binding, explicit vertex binds
on arena draws (~0.3 us each, the base-zero saving is lost for those), 1 in 64 hits hashed in full. The fingerprint of
check 1 is usually already paid by the dedupe. The first frames after a level load copy as before (promotion needs a
repeat).

### Console checklist

1. **Measure** (image identical; build with this branch, same route as the C3 runs, cold start):
   ```toml
   masseffect_native_vertex_arena_measure = true
   ```
   Look for `[native] vertex arena: measurement ON` at start and the two report lines every 10 s. Record clean+same MB
   (the saving ceiling), clean+CHANGED count and the top ranges per location (Normandy cockpit, Eden Prime, Feros
   firefight, Citadel UI/menus, a Bink movie). clean+CHANGED should be 0 or limited to a few small ranges (UI); a
   large range listed every report is a dynamic buffer to exempt (or a sign the hypothesis fails for vertices).
2. **Arena, guarded** (same toml + the switch; keep the measurement for this run so its exemptions feed the arena):
   ```toml
   masseffect_native_vertex_arena = true
   masseffect_native_vertex_arena_measure = true
   ```
   Expected at start: `[native] vertex arena: measurement ON ...; arena ON (32 MB in 8 segments of 4096 KB, ...)`, after
   ~4096 hits `vertex arena: the first 4096 hits verified with a full hash, all equal`. Never `DIFFERENCE: vertex arena`
   or `vertex arena: OFF`. Report: hits and MB not copied, waits for the GPU (should be small), exempt pages.
3. **Arena timing run** (measurement off, it costs a full hash per copy):
   ```toml
   masseffect_native_vertex_arena = true
   ```
   A/B against the same toml without it: `C6 stages` uploads stage and `C6 uploads` vertex copies, the ring partition
   `Vulkan draw` phase, fps in the Feros firefight and the Normandy cockpit. Optionally `_verify_every = 0` after a
   clean run, and `_mb = 64` if "waits for the GPU" or "no space" stay high.
4. **Captures**: UI text (menus, codex, dialogue wheel), a Bink movie, particles/explosions, skinned characters,
   loading into a new area (memory reuse). Compare with the baseline captures: any stuck or stretched geometry or wrong
   letters means a missed write; then set `masseffect_native_vertex_arena = false` and report the log lines around it.

## Files read

- `app/src/native/masseffect/masseffect_native_draws.cpp` (copy, uploads stage, attribute formats, submit comment)
- `shaders/XenosRecomp/shader_recompiler.cpp` (vertex fetch translation)
- `sdk/src/core/guest_memory_switch.{h,cpp}` (guest memory commit, aliases, large pages)
- `app/src/native/me_texture_coherency.h` (page stamps)
- `out/mesa-switch-corrected/src/nouveau/vulkan/{nvk_physical_device.c,nvk_device_memory.c,nvkmd/switch/*}`,
  `src/nouveau/horizon/nouveau_horizon_memory.c` (host pointer import)
- docs: ring-cpu-per-draw.md, cpu-cost-analysis.md, frame-handshake.md, multithread-translation.md, mesa.md,
  vertex-shader-specialization.md, known-issues.md
