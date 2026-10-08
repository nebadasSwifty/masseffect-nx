# Frame coherence of ring draws: measurement switch and cache design (2026-10-07)

Idea: most draws of a frame are identical to a draw of the previous frame in everything the ring thread's per-draw
work depends on (shaders, render state, fetch constants, constants, index/vertex ranges and their content, textures).
If so, much of that work could be reused instead of recomputed. This page adds a measurement-only switch that says
how many draws really are identical (part 1), and designs the cache that would use it (part 2, not implemented).

Baseline cost (RU, 960x544, 1785/768 MHz, heaviest Normandy walk window of `run/me1/ringpart_ab_g/console.log` and
`C6 stages` of `run/me1/nvk_o2c/console.log`):

| Ring phase (ring partition, 264,621 ring draws / 10 s, 219 Swaps) | us per ring draw |
|---|---|
| parse + registers | 1.23 + 1.38 |
| shader loads + pairing + draw front (identity, extent proofs) | 1.72 + 2.32 + 1.99 = 6.0 |
| EDRAM prepare / transfers / publish (mode 4) | 4.62 + 0.34 + 0.32 |
| Vulkan draw + textures (61 % of ring draws reach Vulkan) | 9.82 + 4.55 = 14.4 |
| copies, present | 0.81 + 1.04 |
| WAIT_REG_MEM (the game's VBlank handshake, a wait, not work) | 7.51 |
| busy total | 37.7 (30.2 without WAIT_REG_MEM) |

| C6 stage (heavy windows 04:55:33-04:55:53, us per recorded draw) | us |
|---|---|
| 0 state (entry, EntryFor) | 1.0-1.1 |
| 1 indices (index fingerprint, conversion, vertex sources) | 3.1-3.6 |
| 2 textures (per-fetch cache, PrepareTexture, rechecks, creation) | 6.5-6.8 |
| 3 upload space and pass | 0.7-0.9 |
| 4 uploads (texture uploads, vertex fingerprint + dedupe + copy, constants) | 7.1-8.5 |
| 5 pipeline and constants (PipelineFor, set 4) | 2.6-2.9 |
| 6 recording (queue of ~8.6 vkCmd* calls) | 3.1-3.2 |
| sum | 24.7-26.5 |

## 1. Measurement switch: `masseffect_native_coherence_stats` (default off)

Files: `app/src/native/me_frame_coherence.h` (all the logic, header only), hooks in
`app/src/native/me_native_system.cpp` (cvars, `Configure` at ring start, `BeginDraw` before `PairDraw`,
`CoherenceEndDraw` after `Draw()`, `CoherenceEndFrame` at `PM4_XE_SWAP`) and four one-line hooks in
`app/src/native/masseffect/masseffect_native_draws.cpp` (the include, `NoteContent` after the index cache fingerprint
and after the vertex dedupe fingerprint, `NoteStage` in `Stage()`). Test: `tests/cpu/test_native_frame_coherence.cpp`
(`tests/run_all.sh frame_coherence`).

```toml
masseffect_native_coherence_stats = true
# optional: frame pairs every N frames (default 8: frames f % 8 == 0 and 1 are keyed, the second is compared with
# the first). 1 = every frame compared with the previous one (more samples, ~4x the cost).
masseffect_native_coherence_every = 8
```

Both are init-only (read when the ring thread starts). Run it with `masseffect_native_report_stages = true` (the
stage split of matching draws needs the C6 stopwatch, which runs on 1 in 64 draws anyway) and, if wanted,
`masseffect_native_ring_partition = true`.

### What is keyed

Per ring draw (every draw that reaches `Draw()` in the PM4 loop, also the ones that are not identified or are
rejected later), on recording frames only:

| Key | Content | Notes |
|---|---|---|
| shaders | paired VS/PS library entries + XXH3 of the loaded microcode (`current_vs_hash_`, `current_ps_hash_`) | |
| state | 97 register words: RB surface/color/depth info, screen/window scissor, color mask, blend, stencil refs, alpha ref, viewport, SQ program/context, depth/blend/color control, clip, SC mode, VTE, mode control, point/line, AA config/mask, VTX cntl, guard bands, poly offset, bool and loop constants | |
| textures | the 6 fetch words of each sampler slot of the VS and PS (`ShaderEntry::samplers`) | exactly what the texture stage reads |
| constants | VS constants `0x4000 + constants_bytes/4` words and PS constants `0x4400 + ...` | hashed once per constant generation (memo) |
| geometry | draw initiator, `VGT_DMA_BASE/SIZE`, `VGT_MIN/MAX_VTX_INDX`, `VGT_INDX_OFFSET`, and for each index/vertex range the Vulkan draw fingerprints: address, size, byte order and the fingerprint | |
| fetch words | all 192 fetch constant words | conservative: stale unused slots count too |
| content | only the index/vertex fingerprints | address independent |
| full | shaders + state + textures + constants + geometry | what a cache entry would be keyed on |
| strict | full + fetch words | |

Index and vertex content is not hashed again: the hooks take the fingerprints the draw already computes (index
cache `RangeFingerprint`, vertex dedupe `SampleFingerprint`/XXH3). A range the draw did not fingerprint (over
`masseffect_native_dedupe_max_fingerprint` / 16 KB indices, dedupe off) is hashed by the diagnostic with a sampled
hash (first and last 1 KB and 8 blocks of 1 KB in between, at most 10 KB): counted as "ranges hashed by sampling";
changes outside the sampled blocks are missed, so that part errs on the high side. Index draws that take another
path (32-bit indices, restart, the cache off) have their geometry keyed by address only (counted as "DMA-index draws
without an index fingerprint").

Matching: "any position" = the key is in the hash set of the previous frame's keys; "same ordinal" = draw i equals
draw i of the previous frame. Copies and resolves are not draws and do not shift the ordinal.

### Cost

Off: one load and a branch per ring draw (twice), per Swap, per C6 stage of a timed draw and per fingerprinted
range. No allocation. On: ~1.2 MB of tables (9 keys x 2 frames x 8192 slots + 2 x 4096 draw records), allocated at
ring start. On a recording frame each draw gathers ~100 register words, hashes ~1 KB (state, all fetch words) plus
the constants when their generation changed, and does 18 set operations: estimated 1-2 us per draw on the A57,
about 0.25-0.5 us per ring draw averaged at `every = 8`. The report prints its own measured cost
("diagnostic cost ... us per recorded draw"). Frames with more than 4096 draws key only the first 4096
("over the per-frame cap").

### Log lines (game.log / console.log)

At start:

```
[native] frame coherence on (measurement): frame pairs every 8 frames, report every 10 s
```

Every 10 s (two lines, from the ring thread at a Swap):

```
[native] frame coherence (10.0 s, every 8 frames: 27 frame pairs compared, 32400 draws, 1200.0 draws per frame,
previous frames 1198.5): full key in the previous frame A % (same ordinal B %), strict C % (D %), repeats within the
frame E %, distinct full keys per frame K | shaders .. % (ordinal .. %), .. distinct per frame | state ... |
textures ... | constants ... | geometry ... | fetch words ... | content ... | vertex/index fingerprints in .. % of
draws, .. % with a range not fingerprinted, .. ranges per draw hashed by sampling here, N DMA-index draws of which M
without an index fingerprint (geometry by address only)
[native] frame coherence: draws differing from the same ordinal in one component only: shaders n, state n,
textures n, constants n, geometry n, in two or more n | ring time per draw (PairDraw to end of Draw): all X us,
full-key matches Y us, others Z us; matches are P % of the time | C6 stages of full-key matches (T timed draws, us
per timed draw): state .. indices .. textures .. upload space and pass .. uploads .. pipeline and constants ..
recording .., sum .. | C6 stages of the others (...): ... | estimate: stages state + indices + textures + pipeline
and constants of the full-key matches = E us per compared draw | diagnostic cost Q us per recorded draw (R recorded,
O over the per-frame cap; F of G frames recorded)
```

(each is one line in the log). How to read it:

- **full, any position** is the share a cross-frame cache could hit at best. **same ordinal** says whether a cheap
  "draw i of the last frame" lookup would do, instead of a hash table.
- **constants** is expected to be the weakest component (world matrices, skinning, time). The "one component only"
  counts say how many full misses are due to constants alone: those draws could still reuse everything except the
  constant upload (partial hit).
- **ring time ... full-key matches** includes pairing, identity, EDRAM prepare/publish and the whole Vulkan draw;
  "matches are P % of the time" times the per-draw busy time bounds what any reuse can save.
- **estimate** = share of matches x (stages 0, 1, 2 and 5 of matching timed draws): the part of the Vulkan draw a
  cache could skip without dirty tracking of guest memory (section 2.3). Stage 4 (uploads) and 6 (recording) are
  not in it.

Verification of the switch on the console: the start line, two lines every 10 s, identical images (the switch reads
only), the ring partition busy us per ring draw within ~1 us of a run without it (compare the `every = 8` leg).

## 2. Cache design (not implemented)

### 2.1 What the per-draw work depends on today, and what is already cached

| Work | Inputs | Existing cache (scope) |
|---|---|---|
| Pairing, identity (`PairDraw`, `Accept*Identity`, `MatchesVertexShaderIdentity`) | object table records, loaded microcode, identity cells | identity cells per stage generation; load memo (`masseffect_native_load_memo*`) |
| Extent proofs (rect clears) | registers, VS microcode, vertex memory | none (few draws) |
| EDRAM mode-4 prepare/publish | render target registers, ownership map, epoch | O(1) path per view while `edram4_epoch_` unchanged |
| EntryFor (stage 0) | VS entry, fetch words | per-shader table |
| Indices (stage 1) | DMA range, its content | 16-bit index cache, **same frame only** (`e.frame == frame_`), fingerprint check |
| Textures (stage 2) | fetch words of the samplers, texture content | per-fetch table by `generation_fetch` (crosses frames since `invalidate_textures_each_copy = false`); content rechecked every 4 frames (interval) |
| Vertices (stage 4) | fetch words, `[vmin, vmax]`, content | dedupe table, **same frame only** (upload buffer is per frame), fingerprint check |
| Constants (stage 5) | constant registers, shader's constant size | set 4 by differences (2.2 words per bind) |
| Pipeline (stage 5) | shaders, state words, pass | direct cache (82 % hits) + map |
| Recording (stage 6) | everything above | none (must be recorded every frame) |

So the coherence the cache would add over today's caches is mainly: (a) crossing the frame boundary for indices and
vertices (the upload buffer is per frame, so a copy cannot be reused next frame), (b) skipping the lookups
themselves (pairing, identity, EntryFor, per-fetch table, PipelineFor) with one key compare, and (c) partial hits
where only constants differ.

### 2.2 The cache

One entry per draw ordinal plus a hash table by full key (the measurement says which one matters), filled at the end
of a recorded draw:

- **key**: the same five components as the measurement (shaders, state, textures, constants, geometry), each kept
  separately so a partial hit can be told apart;
- **reusable results** (exact as long as the key matches and the referenced objects are alive):
  - draw pairing result (`draw_vs_`, `draw_ps_`) and the identity verdicts (pure functions of microcode words and
    candidate: valid while `current_vs_hash_`/`current_ps_hash_` and the candidate pointers are equal);
  - `VerticesEntry*` (stage 0), topology/primitive decode, framing (`VkViewport`, scissor, ndc) - the framing cache
    already does this by generation, the entry would do it by value across frames;
  - texture slot, view and sampler indices of each fetch (descriptor indices into the bindless set), only if the
    texture objects have not been retired or rebuilt since (a global "texture objects generation" bumped by retire,
    rebuild, `InvalidateImages`, resolved texture rewrite, format/swizzle change - the same events that already bump
    `generation_textures_`);
  - `VkPipeline` and the pipeline key (valid while the pass/render pass compatibility key is equal: part of state);
  - set 4 contents and the constant upload offsets **only on a full hit within the same upload slot** (offsets die
    with the per-frame upload buffer, so across frames only the packed bytes could be reused, not the offset);
  - index conversion result (min/max, converted indices) and vertex copies: across frames this needs a **persistent
    geometry arena** (a device-local or host-visible buffer outside the per-frame ring, LRU by key, 8-16 MB), where a
    range that matched in N consecutive frames is promoted; the draw then binds the arena offset instead of
    reserving and copying.
- **must still run every time**: recording (stage 6), EDRAM mode-4 ownership (`PrepareDrawEDRAM4` /
  `PublishEDRAM4`: ownership changes within a frame with every resolve, clear and draw; at most the sync walk could be
  skipped when the view's ownership epoch is unchanged, which the O(1) path already does), occlusion queries,
  and the upload of constants that differ.

### 2.3 What must be revalidated, and how to detect changes cheaply

| Input | Can change without any register write? | Detection |
|---|---|---|
| shaders, state, fetch words, constants | no: they are registers | compare (the key) - exact, ~0.3-1 us |
| index/vertex content | **yes**: the game writes guest memory with the CPU | fingerprint every use (today's cost: 1.5-3 us per draw) or dirty tracking |
| texture content | yes | interval rechecks (every 4 frames, already) - unchanged by the cache |
| render target / EDRAM ownership | yes, within a frame | not cached (see above) |
| texture objects (retire, rebuild, resolve) | yes, by the renderer | generation counter (exists) |

Dirty tracking of guest memory, options checked in the code:

- **RexGm page protection** (`sdk/src/core/guest_memory_switch.cpp`, `RexGmProtect`): every run's profile header says
  `watching: unmapping (every read faults)`, i.e. `svcSetProcessMemoryPermission` was refused for the views and a
  watched page is **unmapped**. Then every guest read of a watched page is an emulated fault (measured in that file:
  ~9 us per read, 108k reads/s ate a core). Vertex and index buffers are read by the game too (skinning, culling,
  UE3 dynamic buffers), so watching them is out. Not usable unless the read-only path is made to work (it would need
  the views mapped with a memory state that allows permission changes - an SDK change with its own risk).
- **`g_synchronizations_ring`** (guest waits for the GPU): bumped at every fence/WAIT_REG_MEM, i.e. several times
  per frame. It proves "no rewrite within a frame segment", not across frames. Useless for cross-frame reuse.
- **D3D lock hooks**: the game writes vertex/index buffers through `IDirect3DVertexBuffer9::Lock` / `IndexBuffer`
  Lock and UE3's dynamic ring buffers. Hooking the guest Lock/Unlock (as `me_d3d_trace.cpp` hooks other D3D entry
  points) would give a per-range "written" generation: a range never locked since it was keyed is unchanged. Exact
  only if the game never writes buffer memory without Lock (not known: 360 titles can place buffer headers over
  memory they write directly) - must be proven per buffer, e.g. by keeping the fingerprint check on 1 in N hits
  (the existing `verify_n` pattern) and turning the shortcut off at the first difference.
- **Keep the fingerprints** (no dirty tracking): the cache still skips the copies, conversions and lookups, and pays
  only the hashes it already pays today. This is the safe first step.

### 2.4 Estimate

Assume the measurement shows a full-key hit share h (any position) in the heavy windows. Per recorded draw
(25-26 us), a hit could skip, with fingerprints kept:

| Stage | Skippable on a hit | us |
|---|---|---|
| 0 state | entry and decode lookups | ~0.8 of 1.0-1.1 |
| 1 indices | conversion and min/max (persistent arena); fingerprint stays | ~1.5-2 of 3.1-3.6 |
| 2 textures | per-fetch lookups, SlotView/SlotSampler; rechecks and creation stay (amortized) | ~2-3 of 6.5-6.8 |
| 4 uploads | vertex copies (arena); fingerprints and texture uploads stay | ~2-4 of 7.1-8.5 |
| 5 pipeline and constants | PipelineFor and set 4 packing when constants are equal | ~1-2 of 2.6-2.9 |
| outside Vulkan: pairing, identity, draw front | one key compare instead of the identity checks | ~2-3 of 6.0 (per ring draw) |

That is ~7-12 us per recorded hit plus ~2-3 us per ring hit outside Vulkan. With 61 % of ring draws recorded:

- h = 0.8: ~0.8 x (0.61 x 7-12 + 2-3) = **5-8 us per ring draw**, 17-27 % of the 30 us busy ring time;
- h = 0.5 (constants differ often, partial hits only): ~3-4 us per ring draw, 10-13 %.

With dirty tracking from D3D lock hooks (fingerprints skipped for unlocked ranges), add ~1.5-3 us per recorded hit
(the vertex/index hashes): **6-10 us per ring draw at h = 0.8**.

The ring thread is not the only limit in these windows: it waits 7.5 us per ring draw on WAIT_REG_MEM (the game's
VBlank handshake), and docs/cpu-cost-analysis.md shows frames lost in hand-offs between the game, render and ring
threads. A ring saving turns into fps only to the extent the ring is on the critical path; the ring-CPU round 2
(+17 % draws/s for ~3-5 us per draw) suggests it largely is.

Effort: the key and the reuse of lookups (2.2 first bullets) ~3-5 days; the persistent geometry arena with promotion,
LRU and per-frame lifetime of the GPU reads ~1 week; D3D lock tracking with self-check ~1 week. Risk: stale geometry
or textures if any input is left out of the key (mitigation: every reused result rechecked against the normal path
for the first `masseffect_native_verify_n` hits and 1 in 4096 afterwards, `DIFFERENCE` turns it off).

### 2.5 Decision rule after the console run

- full any-position share >= 70 % in the heavy windows: build the cache (2.2) with fingerprints kept.
- full low but "constants only" large: build the partial hit (everything but the constant upload).
- same-ordinal share close to the any-position share: an ordinal array is enough (no hash table).
- full < 40 %: drop the idea; the remaining per-draw costs are better attacked one by one (ring-cpu-per-draw.md).

## 3. Build for the console

`run/me1/ru_coh.nro` / `run/me1/ru_coh.elf` (RU, built with `tools/edition.sh ru all`; the switch is off unless the
toml line above is added). Not run on the console by this task.


## Console result (2026-10-07, RU 960x544, Normandy route, coh.toml)

Full-key matches with the previous frame: 0.0 % (the shader constants change every frame: 0.9 % match). Per
component (any position / same ordinal): shaders 99.7 % / 78 %, state 98.9 % / 78 %, textures 100 % / 76 %, geometry
83 % / 54 %, fetch words 18.7 % (vertex fetch addresses move), constants 0.9 %. ~336 draws per frame in the sampled
frames, 13.7 % repeats within a frame. Ring time per draw 41-48 us with the diagnostic on.
Decision: no whole-draw cache; a per-component cache (texture bindings keyed by the texture fetch words, pipeline and
state) is worth building.

## 4. Per-component cache (2026-10-07, implemented, every part default off)

Built after the console result above: no whole-draw cache, but each component that repeats gets its own cache, each
exact, behind its own init-only switch, and each checking itself: the first `masseffect_native_draw_cache_verify_n`
hits (4096) and then 1 in `masseffect_native_draw_cache_verify_every` (4096) also run the path they replace and compare
bit for bit; a difference logs `DIFFERENCE: draw cache <component>` and switches that component off for the session.

Files: `app/src/native/masseffect/me_draw_cache.h` (the logic: tables, arena bookkeeping, check schedule; no Vulkan,
no logging), `me_draw_cache_cvars.inc` (settings), `me_draw_cache_members.inc` (renderer side, inside
`DrawsVulkanImpl`). Hooks in `masseffect_native_draws.cpp`: two calls around the sampler loop, one call instead of
`PipelineFor(SearchKey(key))` in stage 5, the arena lookup next to the 16-bit index cache (stage 1), the arena branch
in the index upload (stage 4), the index-buffer bind (tracks the buffer as well as the type), and one line each in
`CreateDescriptors` (DcInit), `UseSlot` (DcSlotStarted, DcReport), `BeforeSend` (DcFlush) and the destructor. Test:
`tests/cpu/test_native_draw_cache.cpp` (`tests/run_all.sh draw_cache`).

```toml
masseffect_native_draw_cache_textures = true
masseffect_native_draw_cache_pipelines = true
masseffect_native_draw_cache_indices = true      # needs masseffect_native_indices_cache = true
# optional
masseffect_native_draw_cache_indices_mb = 8      # persistent index buffer
masseffect_native_draw_cache_verify_n = 4096
masseffect_native_draw_cache_verify_every = 4096
```

### 4.1 Texture bindings (`masseffect_native_draw_cache_textures`)

Key: the pixel shader (library entry), its sampler registers below 16 in order, and the 6 fetch words of each
(at most 8 registers; longer lists take the old path). Value: the per-register sampler cache entry (`CacheSampler`)
each register had after the sampler loop: texture slot, heap, sampler index, host size, and the invalidation fields
of the cache it came from (frame, `valid_until`, `generation`, `generation_images`, address bucket and its
generation, resolved flag, copy count). 512 direct-mapped entries (~400 KB).

Validity reuses the existing rules, nothing new: an entry is taken only if every binding passes exactly the
per-register cache test (`cache_between_frames_ ? frame_ <= valid_until : frame == frame_`, `GenerationValid`, which
covers the global generation, `generation_images_`, the round-3 address buckets and the resolved-texture rule) and
none of the existing texture-cache checks (`VerifyDue`: copy, address, frames) wants that binding run through
PrepareTexture.

A hit does **not** write the shared constants: it primes `cache_samplers_` with the stored entries and the sampler loop
runs as always, each sampler taking the per-register cache hit. So the bindings are written by the same code as today
(compatible with the combined image+sampler heap rework: the cache stores slot/heap/sampler indices, never descriptor
writes or shared words). What a hit saves: the per-fetch table (XXH3 of 24 bytes plus a cold 100-byte line, and the
copy into the register entry) for every register whose own entry held another texture, and the PrepareTexture of
fetches whose per-fetch entry was evicted by a slot clash ("slot" misses of `C6 textures`). It cannot save a
PrepareTexture that is due (recheck, creation, resolved texture of a new frame): those are invalid by the same rules.

Check: on a due hit nothing is primed; after the loop the slot, heap, sampler and size of every register are compared
with the cached ones.

Expected: small. With `tex_ab.toml` (cross-frame and by-address on) most samplers already hit a per-register or
per-fetch entry; this removes the per-fetch part (~0.1-0.3 us each, ~1.3 per draw in the coh_r window) and the clash
misses (600-3,600 per 10 s at ~10-20 us): about 0.2-0.5 us per recorded draw.

### 4.2 Pipelines (`masseffect_native_draw_cache_pipelines`)

Key: the raw `PipelineKey` the draw built plus the lookup context (dynamic-state mode `eds_mode_`, canonical key on/off),
which is everything `SearchKey` reads. Value: the `VkPipeline`. Stored only when `PipelineFor` found the pipeline in the
pipeline map or its caches (`last_key_` equals the lookup key after the call): never the generic pipeline standing in
for a specialized one still compiling (`AsyncSpecializedPending` looks the generic key up, so `last_key_` is the
generic key), never a failed creation. Map entries are never replaced or destroyed during a session, so a stored pair
stays right. A one-entry shortcut (same raw key as the previous lookup) and 256 direct-mapped entries. A hit skips the
copies and compares of `SearchKey` (canonical memo, dynamic-state masking) and of `PipelineFor` (last key, XXH3 of the
key, direct-mapped cell, map).

Not done: skipping the key build itself. Its inputs (~25 register words, the texture signs of the bindings, the folded
VS constants, the pass formats, a dozen renderer flags) are as large as the key, so a memo keyed on them costs what it
saves; the key is the compact form. The derived dynamic state (blend constants, stencil, bias, viewport) is already
recorded only on change.

Check: a due hit also runs `PipelineFor(SearchKey(key))` and compares the handles. Expected: the `PipelineFor`
substage (0.78-0.87 us per timed draw) to roughly half; ~0.3-0.5 us per recorded draw.

### 4.3 Shader pairing / identity: not implemented

`PairDraw` must consume the game thread's draw record of every draw in order (`DrainRecords`, `FindDense`, `Consume`),
so the record lookup cannot be skipped. What follows it is already memoized: `LookupObject` is the lock-free object
table, and every identity verdict (`MatchesVertexShaderIdentity`, `MatchesPixelShaderIdentity`, `FetchCoherent`,
through `Accept*Identity` and `PreferVertexShaderCandidate`) is a hot direct-mapped cell per stage generation
(`masseffect_native_flat_identity`), with the load memo and its generations keeping those cells alive across A, B, A
shader alternation (`masseffect_native_load_memo_generation`). A memo of the whole verdict would replace 4-8 hot cell
lookups (~10-20 ns each) by one keyed lookup of similar cost, and would drop the mismatch counting and logging of the
identity guard. The pairing phase's 2.3 us per ring draw are mainly the cross-core record hand-off, which a cache
cannot remove.

### 4.4 Persistent index arena (`masseffect_native_draw_cache_indices`)

The 16-bit fast path of the index cache (no restart, no offset, no quads or fans, up to 16 KB so the range carries a
full `RangeFingerprint`, i.e. XXH3 of all its bytes): the converted indices go to a persistent host-visible index buffer
(8 MB, `CreateDedicatedAllocationBuffer` with the upload memory purpose) instead of the per-frame upload buffer, with
their min/max, keyed by (address, byte order, count, fingerprint). Only the second sighting of the same content is
stored (a direct-mapped tag table, `SeenBefore`), so ranges of dynamic buffers rewritten every frame do not fill the
arena; the first sighting takes the per-frame cache as before. A later draw in any frame with the same key binds
that buffer at the stored offset: no `IndicesFrom16`, no copy into the uncached upload buffer. The fingerprint is still
computed on every draw (the guest can rewrite indices at any time), so a changed range simply misses. Ranges without a
full fingerprint (over 16 KB) keep the per-frame cache.

GPU lifetime (why the per-frame buffer cannot be reused across frames and this one can): the arena is never written
where a submitted frame may read. It fills front to back; when a range does not fit, every entry is dropped and the
space is reused only after every frame started before that moment has completed. Completion comes from the work slots:
`BeginRecording` waits for a slot's fence before `UseSlot`, so at `UseSlot(slot)` the previous frame of that slot is
complete (`IndexArena::SlotStarted`). Until then draws use the upload buffer as before. Non-coherent memory is flushed
in `BeforeSend` (the range written since the last submission). The index bind now tracks the buffer as well as the
type. Off while the sky is postponed (`masseffect_native_postponed_sky`, whose replay binds the upload buffer).

Check: a due hit converts the guest indices as before and compares min, max and every index with the arena bytes.
Expected: the conversion and copy of every 16-bit range whose first use in a frame misses the per-frame cache (~half of
the index draws in the coh_r window: 28k per-frame hits of ~57k recorded draws per 10 s, ~4.3 KB each): the `indices`
part of `C6 uploads` (0.6-0.7 us per timed draw) and the conversion inside the `indices` substage, about 0.5-1.5 us per
recorded draw.

Considered, not done: the same for vertex bindings (`vertex copies` are the largest upload part, 4.6-6.7 us per timed
draw). Their dedupe fingerprint is sampled (`SampleFingerprint`, 1 block in 8), which is not exact across frames; a full
XXH3 of the source would cost a large part of the copy it saves (round 3, 3.4). A cross-frame vertex arena is a
separate decision.

### 4.5 Report and console runs

With any component on, one line every 10 s (from `UseSlot`):

```
[native] C6 draw cache (last 10.0 s) | textures on: H set hits (B bindings primed), misses: a no entry, b clash, c not
valid, d wanted by a texture check, e too many samplers; K checked, 0 differences; us per timed call: hit X (n), slow Y
(m) | pipelines on: H hits (L by the last-key shortcut), misses: a empty, b clash, c not stored (not final); K checked,
0 differences; us per timed call: hit X (n), slow Y (m) | index arena on: N lookups, H hits (M MB neither converted nor
copied), F seen for the first time (not stored), S stored (M MB), a without space, b while waiting for the GPU, R resets, U of 8 MB used; K checked, 0
differences
```

(one line in the log). Start: `[native] C6 draw cache: textures ON, pipelines ON, index arena ON (8 MB, memory type t,
coherent); the first 4096 hits of each and then 1 in 4096 are checked against the path they replace`. Once each:
`C6 draw cache: textures: the first 4096 hits checked against the sampler loop, all equal` (and the same for pipelines
and indices). Never `DIFFERENCE: draw cache`. The textures "us per timed call" are the whole sampler loop (substage
14) on hit and on slow draws; the pipelines ones are the `PipelineFor` substage (11), which now contains the cache.

Build: `run/me1/ru_dcache.nro`, unstripped ELF `run/me1/ru_dcache.elf`. Tomls (in `mass-effect-recomp/run/me1/`):

| toml | content |
|---|---|
| `dc_base.toml` | `tex_ab.toml` unchanged (round-3 exact set, report_stages, report_texture_uploads) |
| `dc_all.toml` | + the three switches |
| `dc_tex.toml`, `dc_pip.toml`, `dc_ind.toml` | + one switch each (to split the effect if dc_all differs) |

Same route as nvk_o1/o2 and tex_a/b (`tools/me1_anderson.sh`). Compare the heavy windows of dc_base and dc_all:
`C6 stages` (textures, uploads, pipeline and constants, indices), `C6 substages` (samplers, PipelineFor, indices),
`C6 uploads` (indices part), draws/s and Swaps; captures must be identical (all three are exact).
