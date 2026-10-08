# Occlusion queries (EVENT_WRITE_ZPD) in the native renderer

Date: 2026-10-07. Status: implemented behind cvars, all off by default. Modes 0-2 measured on the Switch (results at
the end). Mode 3 (latency-1, section D) added 2026-10-08 and measured the same day (no gain, see the results).
Section E (occlusion depth, 2026-10-08): why the boxes almost never come out hidden, and the fix behind
`masseffect_native_query_occlusion_depth`: not compiled yet, not measured.
Background: [external-practices-2.md](external-practices-2.md) section 1.1 (steps A, B, C).

## How D3D9 on the Xbox 360 does it

- `Issue(D3DISSUE_BEGIN)` (English `sub_82228F58`) first stores the sentinel `0xFFFFFEED` (big endian `-275`) into
  words 0..3 of the end structure (+0x00) on the CPU, then points `RB_SAMPLE_COUNT_ADDR` at the query's begin
  structure (+0x20) and emits `EVENT_WRITE_ZPD`; `Issue(D3DISSUE_END)` points the register at the end structure, emits
  `EVENT_WRITE_ZPD` and stores the kick fence in `[query+20]`. (Corrected 2026-10-09: the sentinel is stored at the
  BEGIN. Consequence: a result the ring writes late can erase the sentinel of the next issue of the same object; the
  ring therefore pairs the END by address, `masseffect_native_query_pair_by_address`, image-defects-feros.md 3.8.2.)
- `GetData` (English edition `sub_82229158`, occlusion branch) answers "not finished" (`S_FALSE`) while end words 0..3
  (Total_A, Total_B, ZFail_A, ZFail_B) **all** still hold the sentinel. Otherwise it returns
  `(end.ZPass_A + end.ZPass_B) - (begin.ZPass_A + begin.ZPass_B)` (read with `lwbrx`: the words are little endian),
  summed over the predicated tiles.
- UE3's render thread spins on it: `sub_826E7C98` loops `while (GetData(query) == S_FALSE) Sleep(0)` (the
  `masseffect_wait_occlusion_us` hook in `me_ring_wait.cpp` shortens each sleep).
  It is GetData's only caller (8 call sites, all in `sub_82392F28`, light and primitive visibility); a read of 0
  samples culls. In mode 0, `masseffect_query_getdata_visible` (default true) answers GetData with 1000 samples at once
  (image-defects-feros.md 3.8.2).

Before this change the ring thread wrote begin = 0 and end = 1000 samples at parse time: every query said "visible".
The game therefore drew every occlusion-tested primitive, plus the bounding boxes it draws only to feed the queries.

## What is implemented

Code: `app/src/native/me_native_system.cpp` (`ZpdQuery`, `QueryDrawInert`, `ServiceQueries`, the report line),
`app/src/native/masseffect/masseffect_native_targets.{h,cpp}` (`WriteOcclusionCounts`, `QueryBegin/End/Forget/Service`,
`BeginOcclusionDraw`, `ReadOcclusion`), `masseffect_native_draws.{h,cpp}` (the per-draw bracket,
`SubmissionDraw::occlusion_query`).

All guest result writes go through `WriteOcclusionCounts`: ZPass_A = count, the other ZPass/StencilFail words 0,
a release fence, then Total_A = count and Total_B/ZFail_A/ZFail_B = 0. The sentinel words are cleared last, so a game
thread that sees the query finished never reads a half-written count (the old code cleared the sentinel first).

### A. `masseffect_native_query_skip_boxes` (bool, default false)

With query modes 0 and 1, a draw issued between a ZPD begin and end that cannot change any pixel is not recorded
at all (checked at the top of the ring's `Draw()`, before shader matching and everything else):
EDRAM mode color+depth or depth-only, no color writes (`RB_COLOR_MASK` = 0, or depth-only mode), no depth write
(`RB_DEPTHCONTROL` Z enable and Z write enable not both set), no stencil test (stencil enable off; a stencil test
could write stencil). Such a draw only fed the query, whose result is fake anyway. Counted as "boxes skipped".
Ignored in modes 2 and 3 (the boxes are what is measured). Pairing of Draw* records is not disturbed (`PairDraw` runs first).
Image change: none expected. Not covered: a vertex shader with memexport inside a query (none expected for boxes).

### B. `masseffect_native_query_mode` (int 0..3, default 0, init only)

- 0: as before, every finished query reports 1000 samples.
- 1: diagnostic, every finished query reports 0 samples: UE3 hides everything it occlusion-tests. The picture is
  wrong (missing meshes); the fps bounds what real queries could gain at most.
- 2: real queries (below).
- 3: latency-1 real queries (section D): answered at once from the history of the same query, measured for the next.

### C. Mode 2: real Vulkan occlusion queries

- **Per-draw queries.** Each draw issued inside a guest query gets its own `VK_QUERY_TYPE_OCCLUSION` query
  (`VK_QUERY_CONTROL_PRECISE_BIT` when the device has `occlusionQueryPrecise`), begun and ended right around its
  `vkCmdDraw*` inside its render pass. A guest query therefore never has to be open across a render pass change or
  a target switch; the guest count is the sum of its draws' counts. Pool: 4096 queries per work slot; at each slot's
  start only the queries it used last time are reset (non-timestamp resets are expensive in NVK).
- **Sample units.** Each draw's count is multiplied by guest MSAA samples per pixel / host pixels per guest pixel
  (the X raster grid of the 2x views, and the scaled shadow map's scale squared). The guest total is then scaled by
  1280x720 / (`masseffect_scene_width` x `masseffect_scene_height`) when the internal resolution's viewport part is
  on (960x544: x1.765), rounded up so that any visible sample stays visible.
- **Writing the result.** The end structure keeps D3D's sentinel; the begin structure is zeroed at parse time as
  before. When the work slot's fence has signaled, the ring thread reads the slot's queries
  (`vkGetQueryPoolResults`, no wait) in `Complete` and writes each guest result. The ring thread also polls the fences
  of finished submissions (oldest first, without waiting) while results are pending: in its wait loop it wakes every
  250 us instead of 5 ms only while something is pending.
- **No deadlock.** If the ring has parsed everything the game wrote (the game is probably spinning in GetData) and a
  finished query has waited `masseffect_native_query_flush_us` (default 500, -1 = never) in the work still being
  recorded, that work is submitted early instead of at the Swap. A result not delivered within
  `masseffect_native_query_timeout_us` (default 50000 = 50 ms, 0 = none) is written as "visible" (1000). The game can
  therefore wait at most about the timeout on any query.
- **Fallbacks to "visible" (1000, written at the end packet):** a draw of the query was not measured (it was
  rejected, had no identified shaders, was dropped by a filter, or there was no room in the pool); the query spans a
  submission (e.g. a mid-frame submission for a full upload buffer); the pool could not be created. A query with no
  draws at all gets 0, as on the console.
- **Reissue.** If the game issues the same query object again (ZPD at its begin = end + 0x20, or at its end) while
  an older result is pending, the old result is dropped ("superseded") so it can never overwrite the new sentinel.
- **Filters.** `masseffect_native_skip_prepass` (default on) drops depth-only draws at the screen pitch; a draw
  inside a measured query is exempt (otherwise every query would fall back to visible).

### D. Mode 3: latency-1 real queries

Mode 2 is correct but slow: the UE3 render thread spins in GetData until the GPU result arrives (Port Hanshan
2026-10-08: 194 Swaps per 10 s against 270 in mode 0 and 300 in mode 1). Mode 3 never makes the game wait.

- **Answer at the end packet.** At the end ZPD the ring writes the answer through `WriteOcclusionCounts` at once (the
  sentinel never stays). The answer comes from a history table keyed by the query's identity (below):
  - no identity, or no entry: "visible" (1000), counted as *unknown*;
  - entry whose last folded issue ended more than `masseffect_native_query_max_age` Swaps ago (default 4):
    "visible", counted as *stale*;
  - entry with at least `masseffect_native_query_hidden_after` consecutive zero results (default 2): 0, counted as
    *hidden*;
  - otherwise the last non-zero real count (scaled as in mode 2; lens flares read the magnitude), or 1000 if the
    entry never had one.
  - A query with no draws gets 0, as in mode 2.
- **Measurement.** The draws of the issue are measured with the mode 2 machinery (per-draw Vulkan queries,
  `BeginOcclusionDraw`, same sample multipliers and resolution scale, rounded up). Only issues that mode 2 could
  measure are recorded (one submission, every draw measured); the others still get an answer from the history.
- **Folding.** `ReadOcclusion` (in `Complete`, when the slot's fence has signaled) folds each measured issue into the
  table: a non-zero count sets `last_nonzero` and clears the zero streak, a zero increments it. Nothing is written to
  guest memory afterwards: the end structure may already belong to another primitive. The ring polls the fences of
  finished submissions without waiting (`QueryService` step 1) at most once per millisecond while measurements are
  outstanding; no early submission, no timeout, no 250 us ring wake-ups. `masseffect_native_query_flush_us` and
  `_timeout_us` are unused in this mode.
- **Identity (`masseffect_native_query_history_key`, default 0).** UE3 (2007-era source) allocates its occlusion
  query objects from a LIFO pool (`FOcclusionQueryPool::AllocateQuery/ReleaseQuery`) every frame, and the grouped
  batcher puts several small primitives into one query. So the end structure address is **not** a stable primitive
  identity: when the set of tested primitives changes, the pool hands the same object to another primitive (not
  verified in the ME1 binary; the report line measures it). The same source draws the boxes with
  `DrawIndexedPrimitiveUP` from **world-space** corners, so the vertex data is stable per static primitive whatever
  the camera. The ring therefore hashes, for every draw inside the query, the position stream's data (the vertex
  fetch constant of the position element's `vfetch`, up to 4 KB), the draw initiator, the vertex shader fingerprint
  and `RB_DEPTH_INFO`/`RB_SURFACE_INFO` (one view vs another). A draw whose fetch cannot be read (mini fetch, not a
  vertex constant, out of range, unidentified shaders) leaves the query without identity: "visible".
  - 0: address + content. Safe with pooling (a reshuffled object is a new identity: "visible" for one or two
    frames); fewer history hits if the pool reshuffles every frame.
  - 1: content only. Survives pooling; wrong only if two primitives draw byte-identical boxes into the same target
    (a shared static unit-cube vertex buffer with per-primitive constants would do that: the first logged box
    draws show whether the box data lives in a static buffer).
  - 2: address only (what Xenia's "fast" mode does). Wrong if objects are pooled; for comparison only.
  - Moving primitives get a new content hash each frame: always "visible" (never hidden, never wrong).
  - The element's `instruction` index is the container's; D3D may reorder fetches in the patched microcode. For the
    box shader (position only) that cannot matter; elsewhere the hash still covers a deterministic vertex stream.
- **Hysteresis.** Hidden only after N consecutive zero results limits flicker from a single stale-depth zero and
  pop-out; it does not delay pop-in (one non-zero result answers visible). Pop-in latency: the GPU result of an issue
  is folded 1-3 frames later (the "avg age" in the report), and UE3 adds its own frame.
- **Memory.** Table pruned every 64 Swaps (entries older than max(64, max age) Swaps), cleared above 65536 entries;
  the pooling diagnostic map is cleared above 16384. A few hundred KB at most.


### E. Occlusion depth: the boxes test a depth buffer without the prepass

#### What the Port Hanshan mode 3 run shows (run/me1/ab/q3_hanshan, 2026-10-08)

| Window | Swaps per 10 s | Queries per Swap | Folded results with 0 samples | Avg samples |
|---|---|---|---|---|
| 00:44:33-00:47:03 (after arrival, GPU bound) | 254-260 | 131 | 983-1003 of ~33,600 (3 %) | ~37,000 |
| 00:47:33-00:48:23 (last view, capped) | 300-301 | 35 | 0 of ~10,500 | ~102,000-104,000 |

The same windows in mode 0 (occ_qb_hanshan): 253-258 and 300; mode 1 (all hidden): 292-301 throughout. Mode 3
answers ~255 hidden per 10 s out of ~33,600 queries: no measurable gain. (The "270 vs 300" in the table below mixed
the two windows.) The last window is already at the 30 fps cap, so it says nothing about speed; it only shows that no
box there is ever hidden.

#### Verdict: confirmed (inference; there is no per-draw sequence trace)

- **What the boxes are** (the first 24 logged query draws): EDRAM mode 5 (depth only), color mask 0, depth control
  `00700762` = Z test on, Z write off, stencil off, function 6 = GEQUAL (the game uses reversed Z: the base pass is
  `00700766`, GEQUAL with write; the scene clear is depth 0 = far), surface info `0F0003C0` = pitch 960, MSAA 1x,
  depth info `00010000` = base 0, D24FS8. So they test the scene depth `D000/f1` (960x960 1x view), the same view
  the base pass writes.
- **No MSAA at 960x544.** The guest runs in its SD path at `video_mode_width = 960` (no MSAA, no predicated
  tiling): the log never prints `targets: MSAA target`, `tile replays reusing a pairing 0, draws dropped by predicate
  0`, scene color is `C2D0/f0:960x960:mx0my0` (1x). The only other view of `D000` is `D000/f1:480x720:mx1my1`, the
  4x MSAA half-width rectangle D3D uses to clear a 1x surface (`aliased depth/color clear proof: slots 1 mode 5 dc
  00008777 ... depthinfo 00010000 ... rect 0,0-480,272`), and it is transferred to the 1x view twice in the whole run.
  So at 960 the prepass, the boxes and the base pass all use the one 1x view; the "2x view" of B1 exists only with the
  game's MSAA (1280x720).
- **The prepass is dropped** at 960: `masseffect_native_skip_prepass` drops EDRAM mode 5 draws at the screen pitch
  (`video_mode_width = 960` = the boxes' pitch) except rectangles and query draws.
- **Order.** UE3 renders, per depth priority group, the depth prepass, then the occlusion queries (`BeginOcclusionTests`,
  boxes with color and depth writes off), then the base pass. With the prepass dropped, the depth at box time is the
  clear (0, far) plus whatever few depth writers were not dropped, and GEQUAL against 0 passes everywhere. That
  matches the counts: 97 % of boxes visible in a dense port, 100 % in the last view. If the boxes came after the base
  pass instead, a dense outdoor/indoor scene would hide far more than 3 %.
- **Sample scaling is not the cause.** The per-draw multiplier is `(1 << surface MSAA) / (raster grid X x shadow
  scale^2)` = 1 for the boxes (1x, `masseffect_native_depth_samples_x = false` at 960, not a shadow map), then x1.765
  (1280x720 / 960x544), rounded up: it scales counts but turns no zero into non-zero and no non-zero into zero.
- **The 3 % zeros** are boxes rejected by something other than the prepass (outside the depth range, or behind the
  few depth writers drawn before the boxes); not investigated.

#### Simpler alternative first: is the 2x mismatch the only reason the prepass is skipped?

At 960 there is no 2x view: the prepass would be drawn into the same 1x view the boxes test, so recording it "with
the same 2x view as the boxes" is trivially consistent (same view, same viewport, same rasterization, same depth
conversion). Two options follow:

1. **`masseffect_native_skip_prepass = false` at 960** (no code). The boxes see the real prepass and the base pass
   gets early-Z back. Risks: the B1 bisection (t138-t148) was at 1280 with MSAA; whether the prepass and material
   vertex shaders produce bit-identical depth at 960 (GEQUAL with write in the base pass drops a pixel whose material
   depth comes out farther than the prepass's) was never tested: B1-like shards are possible. Costs the prepass on
   GPU and ring CPU (below). Listed as an image-risk console variant in best-config.md.
2. **A private occlusion depth** (implemented, below). Same geometry, but the scene depth never sees it: no shard
   risk, same costs as 1 minus the early-Z benefit.

At 1280 with the game's MSAA the prepass is in the 2x view (`D000 mx0my1`); whether the boxes are 1x or 2x there was
never logged. The implementation keys the twin by the full view (MSAA included), so if the boxes are in another view
than the prepass the twin stays empty and the boxes keep testing the scene depth (report: boxes "without prepass
draws"); that case would need a 2x-to-1x conversion of the twin (conservative is enough for queries), not done.

#### `masseffect_native_query_occlusion_depth` (bool, default false, init only)

Active only with `masseffect_native_query_mode` 2 or 3 and `masseffect_native_skip_prepass` on.

- **Twin image.** Per scene depth view (keys[4]: base, format, pitch, MSAA) one private image with the view's size,
  format (D32S8 half range for D24FS8), raster grid and depth half-range flag, so the render pass formats and every
  pipeline are the same ones. Usage depth-stencil attachment only; moved to GENERAL once at creation; never sampled,
  resolved, aliased, imported, exported or published. Code: `OcclusionDepth`, `OcclusionDepthFor`,
  `ApplyOcclusionDepthClear` in `masseffect_native_draws.cpp`.
- **Prepass.** A draw the skip filter drops (mode 5, screen pitch, not a rectangle, not in a query) that writes depth
  is recorded into the twin instead, when the guest cleared that surface in the current frame. Its draw key carries
  `kKeyOcclusionDepth` (keys[4] bit 61), so it never shares a render pass with the scene depth; `BeginPass` binds the
  twin. It is not counted in `Drawn()`, so the EDRAM mode 4 code does not publish the scene depth tiles as written by
  it (its `PrepareDrawEDRAM4` sync runs as it did when the draw was dropped).
- **Boxes.** A draw inside a measured query (mode 2/3) that is depth only (mode 5, no color) binds the twin of its
  view when that surface was cleared in the current frame; otherwise it tests the scene depth as before. Depth test and
  write state are the guest's (boxes do not write). The Vulkan occlusion query, its multiplier and the mode 3 history
  are unchanged.
- **Clear.** The render target code reports every whole-surface depth clear to the draws (`NoteDepthClear`): a D3D
  Clear draw proven to write one constant depth from 0,0 across the full pitch with depth ALWAYS + write and no color
  (`NoteDepthClearDraw`, before a redirect can return; the 4x half-width rectangle is matched in samples along X), and
  a resolve that clears depth over the full pitch (`ClearDepth`). The value is the guest depth (and stencil when the
  clear replaces it). The twin gets it lazily, with `vkCmdClearAttachments` over the pass's render area the next time
  one of its passes records a draw (also when the pass stayed open across the clear). Clears are matched by base,
  format and pitch in samples, and only clears since the last Swap (`NoteSwap` from `Present`) count: in a frame
  without one, the prepass is dropped and the boxes use the scene depth, exactly as without the option.
- **GPU time.** Passes on a twin are category 14, `occlusion_depth` in the `GPU per Swap` line and the per-pass
  lists: it holds the re-drawn prepass **and** the boxes (which leave the `640` category, where the 960 scene is
  counted).
- **Report** every 10 s: `[native] occlusion depth (N Swaps): P prepass draws per Swap recorded into it, D dropped
  (surface not cleared this frame, or no depth write); box draws per Swap: B on it after prepass draws, E on it
  without prepass draws, S left on the scene depth; clears C noted, A applied; T twins, M MB`. At start:
  `[native] occlusion depth: on (...)` and, at the first use, `[native] occlusion depth: twin of depth view ...
  created, WxH format F (M MB ...)`.
  - Healthy: P in the hundreds, D near 0, B close to the box count, E and S near 0, A about one per Swap.
  - S large: the guest's clear of the scene depth is not recognized (not proven constant, or another path): read the
    `aliased depth/color clear proof` lines.
  - E large with P near 0: the boxes are in another view than the prepass (MSAA case above).

#### Costs and risks

- **GPU.** The prepass geometry is drawn again (vertex work for every occluder; depth-only fill at 960x544). At 1280
  the prepass cost about 20 ms per presented frame in t127 (early build, MSAA); at 960 it is unmeasured: the
  `occlusion_depth` category gives it directly. It pays off only if the culled draws cost more: mode 1 bounds the gain
  at roughly 255 -> 300 Swaps per 10 s in the Port Hanshan window.
- **Ring CPU.** Every prepass draw now goes through the whole draw path (indices, vertices, constants, pipeline,
  record) instead of being dropped at the skip filter: about 8.5 us per recorded draw on the console; with the
  prepass at roughly a third of the ring draws in heavy windows (draw-count-options.md #1), several ms per frame. On a
  ring-bound scene this can cost more than it saves.
- **Memory.** One twin per depth view used by boxes: the scene depth view at 960 is a 960x960 host image (EDRAM rows),
  D32_SFLOAT_S8_UINT: ~4.6 MB (5 bytes per texel); at 1280, 1280x1280: ~8.2 MB. A twin replaced after a view change
  is kept until shutdown (in flight).
- **Image.** None by construction: the twin is read only by the query boxes. Wrong answers can come only from the
  query side: a twin cleared too late or not at all in a frame would make boxes test stale depth (hidden by mistake);
  the per-frame clear rule (clear since the last Swap, else scene depth) prevents that unless the guest skips its
  depth clear while still issuing the prepass.
- **Render area.** The twin is cleared over the pass's render area (`masseffect_native_pass_area_util` may limit the
  height to the rows the game uses); rows outside it are never drawn by the boxes' passes either.

## Edition support

Address-free: no guest addresses in the new code paths (the GetData address is only cited in comments). No overlay
file was touched: `me_native_system.cpp` and the `masseffect_native_*` files have no Russian overlay.
`tools/edition_drift.py ru` reports the same two files as before (`me_shader_dump.cpp`, `me_hot_guest.cpp`), nothing
new.

## A/B on the Switch

Baseline (what runs today): no lines, or explicitly

```toml
masseffect_native_query_mode = 0
masseffect_native_query_skip_boxes = false
```

A (no image change expected):

```toml
masseffect_native_query_mode = 0
masseffect_native_query_skip_boxes = true
```

B (upper bound; image wrong):

```toml
masseffect_native_query_mode = 1
masseffect_native_query_skip_boxes = true
```

C (real queries):

```toml
masseffect_native_query_mode = 2
masseffect_native_query_flush_us = 500
masseffect_native_query_timeout_us = 50000
```

D (latency-1 real queries):

```toml
masseffect_native_query_mode = 3
masseffect_native_query_max_age = 4
masseffect_native_query_hidden_after = 2
masseffect_native_query_history_key = 0
```

E (latency-1 queries against the occlusion depth, section E):

```toml
masseffect_native_query_mode = 3
masseffect_native_query_max_age = 4
masseffect_native_query_hidden_after = 2
masseffect_native_query_history_key = 0
masseffect_native_query_occlusion_depth = true
```

D variants: `masseffect_native_query_history_key = 1` if the report shows many unknowns and a high "box content at
another address" count (pooling); `masseffect_native_query_hidden_after = 1` (more culling, more pop risk).

C variants worth one run each: `masseffect_native_query_flush_us = -1` (never submit early: fewer submissions, more
timeouts) and `= 0`; `masseffect_native_skip_prepass = false` together with mode 2 (see risks: the boxes may need the
prepass depth to be useful).

## What to look for in the log

- At start: `[native] occlusion queries: mode N (...), skip boxes ..., flush ... us, timeout ... us, count scale ...`
  and, in mode 2, `[native] targets: real occlusion queries ready (4096 per slot, precise yes|no)`.
- First 30 ZPD packets: `[native] ZPD begin|end at XXXXXXXX (mode N): S samples` (end + `(pending)` in mode 2).
- First 24 draws inside queries: `[native] draw inside an occlusion query: mode control M color mask MMMM depth
  control DDDDDDDD surface ... depth info ... inert 0|1`. This says what the boxes look like (depth-only or color
  masked, which depth buffer they test against).
- Every 10 s: `[native] occlusion queries (mode N): B begun, E ended (Q per Swap), D draws inside, K boxes skipped;
  unpaired: ...` and in mode 2 `; real: R resolved (Z with 0 samples, avg S samples), latency avg F frames / M ms;
  fallbacks to visible: U unmeasured, P split, T timeout, X read, N no targets; ...empty, ...superseded, V Vulkan
  queries, W early submissions`.
  - A healthy mode 2: R close to E, Z a sizeable share of R in interiors (that is the gain), T near 0.
  - U large: some filter or rejection still drops query draws; the draw lines above show which state.
  - Z near 0 everywhere: the boxes test against a depth buffer that does not hold the scene yet (see risks).
  - T large or W very large: the game waits for results within the same frame; compare fps with flush -1/0.
- Mode 3: at start `[native] occlusion queries, latency-1: max age A Swaps, hidden after H zero results, identity
  ...`; the first 16 box draws: `[native] query box draw (mode 3): position fetch slot S at XXXXXXXX, B bytes hashed,
  initiator ..., VS nN, content ...` (an address in the ring buffer and different content per line = UP world-space
  boxes, as assumed; the same content on every line = a shared static box buffer: do not use key 1). Every 10 s
  `; latency-1: H answered from history (Z hidden), U unknown, S stale; M measured, N not measured; F folded (... with
  0 samples, avg ... samples, avg age ... frames / ... ms), R read failures; table T entries; box content at another
  address P; ... empty, ... Vulkan queries`.
  - Healthy: H close to the ended count, U small after the first seconds, S near 0 (raise max age if not), Z a share
    of H in interiors, avg age 1-3 frames.
  - P a sizeable share of the ended count: UE3 does reshuffle its pooled query objects; with key 0 that shows up as
    U; try key 1.
- Compare `draws` in the `[native] 10.0 s: ...` line and the fps between runs.

## Risks

- **Pop-in.** UE3 reads a query a frame or more after issuing it; an object that becomes visible is drawn once its
  query says so. Real results add the GPU latency (average shown as "latency avg"). Timeouts and fallbacks answer
  "visible", which never hides anything; only a real 0 hides a primitive.
- **Depth at box time.** If the boxes are drawn before the scene depth exists in the target they test against (for
  example because `masseffect_native_skip_prepass` drops the prepass, or EDRAM mode 4 keeps the depth in another
  view), they pass everywhere and the gain is nil but the image stays right. If they test against stale depth from an
  earlier pass, a visible primitive could be reported hidden for a frame (flicker). Check the draw lines and
  screenshots in mode 2.
- **CPU/GPU serialization.** If the game waits for results of the frame it is still building, every wait costs up to
  one GPU round trip; early submissions add submissions per frame (more chances to block in `Record` on a slot's
  fence with 3 slots). Measure fps, not only the draw count.
- **Extra cost.** Two Vulkan commands per measured draw, a pool reset per slot of the queries used, and the 250 us ring
  wakeups only while results are pending.
- **Mode 3 specific.** A hidden answer is based on a result 1-3 frames old (plus max age): a primitive that just
  came out from behind an occluder stays hidden until a non-zero result is folded (pop-in of 2-4 frames on fast
  turns). Hysteresis 2 avoids hiding on a single zero. Wrong identities (key 1 with shared box data, key 2 with
  pooled objects) can hide a visible primitive for up to max age frames. The extra ring CPU is one XXH3 over the box
  data per query draw (~1 KB) and a few map operations per query.
- **Concurrency.** The game reads the result on another core while the ring thread writes it; the write order
  (counts first, sentinel last, release fences) makes a torn read impossible on the writer's side.

## Switch results, 2026-10-07 (RU, 960x544, 1785/768, Normandy cockpit route, LONG=1)

| Variant | Swaps per 10 s (last 10 reports) | Queries | Game thread waits on fence 0x82EB0D70 |
|---|---|---|---|
| Shipped (mode 0, fence960c) | 204-299 | not logged | ~3.0 s / 10 s |
| Mode 0 + skip boxes (occ_skip) | 141-298 (mostly 285-298) | 17 per Swap, all boxes skipped | ~3.1 s / 10 s |
| Mode 2, flush 500 us, timeout 50 ms (occ_real) | 183-297 (mostly 200-250) | 15-21 per Swap; 2-4 % resolve to 0 samples; latency 1 frame / ~36 ms; 15-37 % time out | ~0.01 s / 10 s |

- Only ~17 queries per frame in this scene, almost all visible (boxes average ~360 k samples): culling has nearly nothing
  to remove here. Skipping the boxes gives no measurable gain.
- Real queries cost frames: the UE3 render thread now spins on results that arrive one frame (~36 ms) later, and many
  hit the 50 ms timeout. The game thread stops waiting only because the render thread is slower still.
- Decision: keep mode 0 (shipped). Revisit only in a location with many queries (open outdoor maps) if the location
  sweep shows the render thread there drawing many occluded objects. A latent mode (answer with the previous result of
  the same end structure) is unsafe unless the query objects are proven to be per-primitive, not pooled: mode 3
  (section D) therefore keys by box content (+ address by default) and reports pooling.

## Switch results, 2026-10-08 (Port Hanshan, GPU-bound, 35-66 queries per Swap, ~1231 draws per frame)

| Variant | Swaps per 10 s |
|---|---|
| mode 0 | 270 |
| mode 1 (all hidden, upper bound) | 300 |
| mode 2 | 194 |
| mode 3 (q3_hanshan) | 254-260 in the GPU-bound window (mode 0: 253-258 there), 300 in the last view; ~3 % of results 0 (section E) |
