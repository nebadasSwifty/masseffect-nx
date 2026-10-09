# Ring thread CPU per draw (Normandy walk windows)

The "GPU ring native" thread turns the game's PM4 command stream into Vulkan. In the Normandy walk windows it is the
bottleneck: 22-24 fps, GPU busy only 10-14 ms per frame, ring thread ~69 % of a core, about 30 ms per frame, about
40 us per draw (700-850 draws per frame, against ~140 in the cockpit). This page breaks down what that time is spent
on, lists the changes made for it (each behind its own cvar, default off) and says how to check them on the console.

Sources: `mass-effect-recomp/run/me1/heap1/console.log` (RU, 960x544, 1785/768 MHz) and its `profile.log`, plus
reading the code. The profile could not be symbolized reliably: the unstripped ELF was rebuilt (13:18) after the
run (12:46), and the function order changed (for example the ring thread's wait resolved to a guest function and
`image+0x28c` to spdlog). The pc-level names quoted in the task (XXH3 ~17 %, spdlog/fmt ~8 %, malloc/free ~6 %) were
used only as hints. Every number below comes from the run's log counters or from the code.

## 1. What one heavy 10 s window contains

The window with 164,011 ring draws, 234 Swaps (23.4 fps) and 3.1 M PM4 packets works out to these per-frame figures:

| Item | Per 10 s | Per frame | Per draw |
|---|---|---|---|
| Ring draws | 164k-171k | ~730 | 1 |
| PM4 packets | 3.1 M | ~13,300 | ~18 |
| Shader IM_LOADs: identical reload of the stage's program (`shader loads reused`) | 117k-124k | ~520 | 0.7 |
| Shader IM_LOADs: identity memo hits (a program loaded a few draws earlier) | 75k-79k | ~330 | 0.45 |
| VS identity mismatches (draw-time guard) | ~18k | ~78 | 0.1 |
| EDRAM mode-4 transfer spans (ops) | ~15k | ~65 | 0.09 |
| Texture rechecks (32 hitch frames shown) | 900 MB in 32 frames | 28 MB in a hitch frame | - |

With `masseffect_native_texture_interval_max = 4` (shipped), every stable texture in view has its whole guest
memory hashed (XXH3) every 4 frames. There is no jitter below the 32-frame cap, so textures that arrived together
are rechecked in the same frame. A hitch frame then shows `textures checked 25-38 MB` and `fingerprints 10-15 ms`,
at about 0.40 ms per MB (2.4 GB/s). The texture budget (`masseffect_native_fingerprints_kb_frame`) and the sampled
recheck (`masseffect_native_fingerprints_sampling`) never act at this cap, because both start at interval 8.

## 2. Per-draw cost breakdown (estimate, A57 at 1785 MHz)

The `C6 substages` report times 1 draw in 64 inside `DrawsVulkanImpl::Draw`. It gives 20.5 us per draw, of which
the textures stage is 15.2 us. That stage includes the amortized full texture rechecks above and the texture
creations (230 ms per 10 s). Indices take 1.5 us, vertices 0.75 us, PipelineFor 0.66 us and set 4 0.4 us. The other
~20 us per draw are spent before that, in the PM4 front end and the mode-4 bookkeeping.

| Part | us per draw | What runs |
|---|---|---|
| Texture rechecks (XXH3 of guest texture memory) | ~6-12 (amortized) | `PrepareTexture`: full XXH3 of base + mips every 4 frames, plus a sample hash that is never used at cap 4 (13-19 % extra bytes) |
| Rest of the textures stage | ~2-3 | fetch cache, key XXH3 (20 B), `textures_` map, `ResolvedTexture` lookup, `SlotView` |
| PM4 parse and register shadow | ~5-8 | ~18 packets per draw, type-0 runs (`pm4_fast` NEON on) |
| Shader loads | ~1-3 | identical reload: one memcmp of the raw words (0.2-0.6 us); memo hit: byte swap + XXH3 + map + 2 copies (~1.5-5 us for 300-966 words) |
| Identity checks at draw time | ~0.5-3 | after every memo hit the stage generation changes, so the next draw re-runs the word-by-word identity compares of its candidates (O(words)) |
| Vertex dedupe fingerprint + copy | ~1.5-3 | `SampleFingerprint` of each binding of 16 KB or less, before the dedupe lookup; NEON byte-swap copy on a miss |
| Index cache fingerprint + convert | ~1.5 | `RangeFingerprint` of the 16-bit indices, then the NEON convert on a miss |
| Mode-4 EDRAM bookkeeping | ~2-5 | `SynchronizeEDRAM4`/`PublishEDRAM4` tile walks; per transfer span 2-4 `fmt::format` strings plus `std::map<std::string>` lookups for the report statistics |
| Pairing, pipeline, descriptors, deferred-recording queue | ~3-4 | `PairDraw`, `PipelineFor`, set 4, about 10 `vkCmd*` queued |

What is formatted with fmt on the ring thread in normal operation (not counting log lines and the 10 s reports):
- `TargetsVulkan::SynchronizeEDRAM4`: for every transfer span, two image descriptions joined into a
  `std::map<std::string>` key, plus a draw key string with a nested format. About 65 spans per frame. This
  allocates too (string nodes), which fits the malloc/free and fmt hints in the profile.
  Changed: `masseffect_native_edram4_pair_keys`.
- `NativeGraphicsSystem::Draw`: the `extent_diagnostics_` key (`fmt::format` + `unordered_set<std::string>`) runs on
  every extent-eligible draw until 32 distinct keys have been seen. In heap1 it saturated (32 lines), so it costs
  nothing later in a long run. Not changed.
- `edram4_draw_desc_` (15-argument format per synchronized slot) stops after 60 transfer traces. heap1 reached 60.
  Not changed.
- `LogVertexWordDiff`, the identity mismatch details and `DumpLoadedVertexVariant` are bounded (sets, counters,
  environment variables). Not changed.

What is hashed per draw, per shader load and per texture, and why:

| Hash | Input | When | Why it exists |
|---|---|---|---|
| Identity memo hash (`IdentifyShader`) | XXH3 of the swapped microcode, 0.2-3.9 KB | every IM_LOAD that is not an identical reload (~330 per frame) | finds the library entry identified earlier for the same program at any address |
| Texture raw fingerprint | XXH3 of base + mips, KB to MB | every recheck (every 4 frames per stable texture), and on first use | detects the game writing new texels into an address already sampled |
| Texture sample | XXH3 of 1 in 8 4-KB blocks plus the last one | every recheck at interval 4 or more | sampled rechecks from interval 8 on (never at cap 4) |
| Texture key | XXH3 of 20 B of fetch words | every texture check | the `textures_` map key |
| Vertex dedupe | `SampleFingerprint` (block 0 + last block) of each binding of 16 KB or less | every binding | the dedupe table proves content, not only the address (the ME front end has no wait generation) |
| Index cache | XXH3 of the 16-bit indices | every 16-bit DMA-index draw | the same, for converted indices |
| Pass key, pipeline key, view key | XXH3 of 40-108 B | when they change | map keys |

## 3. Changes (all default off)

| cvar | File | Exact? | What it saves | Expected |
|---|---|---|---|---|
| `masseffect_native_fingerprints_skip_unused_sample` | masseffect_native_draws.cpp | yes | the sample hash on rechecks whose interval can never reach the interval that uses it (cap 4) | 13-19 % of the texture recheck bytes: about 1.5-2.5 ms in a hitch frame, ~0.5-1 us per draw averaged |
| `masseffect_native_load_memo` | me_native_system.cpp, me_shader_load_memo.h | yes | on an identity memo hit from the same address: the byte swap and the XXH3 of the microcode | ~1-3 us per memo hit, ~0.4-1 us per draw |
| `masseffect_native_load_memo_generation` (needs the one above) | me_native_system.cpp | yes | the re-run of the draw-time identity compares after A, B, A shader alternation | ~0.5-2 us per draw (depends on how many candidates a draw checks) |
| `masseffect_native_edram4_pair_keys` | masseffect_native_targets.cpp | yes (diagnostics only, the same report text) | 2-4 `fmt::format` and `std::map<std::string>` lookups per transfer span | ~4-8 us per span, ~0.3-0.6 us per draw |
| `masseffect_native_fingerprints_sample_min_interval = 4` | masseffect_native_draws.cpp | **no** | lets the 4-frame cap use sampled rechecks (1 in 8 still full) | ~70-80 % of the texture recheck bytes: ~4-9 us per draw, most of the 10-15 ms hitch frames |
| `masseffect_native_texture_spread_phase` | masseffect_native_draws.cpp | **no** (never staler, but frames differ) | spreads the rechecks of textures that reached the cap together over the cap's frames | same average; flattens the 25-38 MB recheck frames into ~7-10 MB per frame |

Total of the exact ones: roughly 2-5 us per draw, about 1.5-3.5 ms per frame at 730 draws, that is 5-12 % of the
ring thread. The texture variants are where the large savings are, and they need the user's judgment on popping.

### 3.1 `masseffect_native_fingerprints_skip_unused_sample` (exact)

`with_sample` (interval >= 4) computes `SampleFingerprint` of the base and the mips on every full recheck and stores
it on a match. The stored sample is only read when `sample_util` holds, which needs `interval >= 8` (or
`masseffect_native_fingerprints_sample_min_interval`). A texture's interval never exceeds `CapInterval(texture)`,
which is at most `masseffect_native_texture_interval_max`. So with the shipped cap of 4, or a texture whose adaptive
cap fell to 2 or 4 after late changes, the sample is computed and then never read.

With the switch, the sample is computed only if the current interval or the texture's cap reaches the threshold.
Otherwise the texture keeps no stored sample (`valid_sample = false`), so a later runtime change of the cap cannot
consume a sample that was not computed: it takes the full path, and its next full check stores a fresh one. The
bytes are still added to the frame budget as before, so postponements, the order of rechecks and every decision
stay the same.

### 3.2 `masseffect_native_load_memo` (exact)

`me_shader_load_memo.h`: a direct-mapped table of 1024 slots keyed by (stage, guest address, word count). Each slot
holds the raw guest words of the last load from there, the byte-swapped microcode and its XXH3 (the identity memo
hash). On an IM_LOAD that is not an identical reload of the stage's current program, `LoadFromMemo`:

1. finds the slot and `memcmp`s the guest words against the stored raw words;
2. if they are equal, runs IdentifyShader's own branches in the same order on the stored swapped words. First the
   repeated-identity test, then the identity memo lookup (`identity_memo_.find` with the stored hash);
3. on a memo hit, applies the same assignments as IdentifyShader through the shared `ApplyMemoHit`. On anything
   else it returns having changed nothing, and the usual swap + IdentifyShader path runs.

Why it is exact: the swap is a bijection, so equal raw words give equal swapped words and an equal XXH3. The stored
raw words are derived from the swapped words when they are stored, never read from guest memory a second time, so a
slot cannot describe two different programs. The first `masseffect_native_verify_n` hits also swap and hash the
guest words and compare. A difference logs `DIFFERENCE: shader load memo` and turns the memo off.

### 3.3 `masseffect_native_load_memo_generation` (exact, needs 3.2)

The identity cells (`MatchesVertexShaderIdentity`, `MatchesPixelShaderIdentity`, `FetchCoherent`) are memoized per
stage generation. Their results are pure functions of (candidate, stage microcode). By default every load that is
not an identical reload bumps the generation, so A, B, A recomputes A's word compares. With the switch, generations
come from one counter, so they are always fresh and never reused for other words. Each memo slot also remembers the
generation its words had. A load memo hit restores that generation: the stage holds exactly the words it held under
it (the memcmp proved it), so every cell computed under that generation is still right.

### 3.4 `masseffect_native_edram4_pair_keys` (exact, diagnostics only)

The per-transfer statistics behind the `EDRAM mode4 pair ... draws:` and `top transfer pairs` report lines are kept
in `std::map`s keyed by integer tuples (the same fields the strings print). `FlushPairKeysEDRAM4` builds the
identical strings into `edram4_transfer_pairs_` right before the report, so the report code, its sort and its text
do not change. The rendering never reads these statistics.

### 3.5 Variants (not exact; for the user to judge)

- `masseffect_native_fingerprints_sample_min_interval = 4`: at the shipped cap of 4, a stable texture is rechecked by
  its sample (the first and last 4 KB blocks and 1 in 8 others, for the base and the mips). Every 8th recheck of a
  texture is still full (`masseffect_native_fingerprints_sampling = 8`), and the first 3000 sampled rechecks compute
  both hashes (the existing guard switches sampling off at the first disagreement). A change that touches only
  unsampled blocks shows up to 32 frames late instead of 4.
- `masseffect_native_texture_spread_phase = true`: the first time a texture's interval reaches a cap below 32, its
  next recheck is brought forward by 0..cap-1 frames, from its key. A recheck is never later than before, so content
  is never staler than the cap, but which frame refreshes a texture can differ.

### 3.6 Considered, not done

- XXH3 with 8 NEON lanes instead of the default 6 NEON + 2 scalar on AArch64: results are identical by construction,
  but the A57 has 3-wide decode and 2 NEON pipes. Counting one 64-byte stripe: NEON-only is bound by the NEON pipes
  at ~14 cycles, the hybrid by decode at ~12. No gain expected.
- Hashing the vertex dedupe or index cache only after a key match: an entry that will be recorded needs the
  fingerprint of the content at copy time anyway. Reading the copy back from the upload buffer (uncached memory)
  would be slower.
- Sharing one texture recheck between texture keys over the same guest range in a frame: about 10 % of the bytes
  (66 repeated contents of 599). It is exact only if the game does not write that memory between the two draws, so
  it was not done.
- The 80 KB register copy per multi-rectangle rect list (`ProveRectangleList`): about 4 per frame, ~0.1 ms.

## 4. How to verify on the console

Turn them on together for an exact A/B (the image must be identical):

```toml
masseffect_native_fingerprints_skip_unused_sample = true
masseffect_native_load_memo = true
masseffect_native_load_memo_generation = true
masseffect_native_edram4_pair_keys = true
```

Log lines to check (game.log):
- Start: `[native] ring CPU switch: shader load memo by address on (1024 slots), identity generations restored on a
  hit true`.
- Every 10 s: `[native] shader load memo: N memo hits without swap and XXH3; table L lookups, E equal, C with changed
  words (cumulative)`. In the walk windows N should be close to the `identity memo hits` count on the line above
  (~70k per 10 s). If N stays near 0, the memo hits come from other addresses (not expected).
- Never: `DIFFERENCE: shader load memo`. If it appears, the memo has switched itself off; report it.
- `[hitch] ring: textures X ms (... fingerprints F ms)` and `[hitch] waits ... textures checked M MB`: with
  skip_unused_sample, F should drop by about 13-19 % for the same M. M itself does not change, because the skipped
  bytes are still counted against the budget.
- `[native] C3 fingerprint sampling: ... ({:.1f} MB read ...)`: the sample MB read drops to ~0 at cap 4.
- `EDRAM mode4 pair ... draws:` and `EDRAM mode4 top transfer pairs:` lines: same format as before.
- `C6 substages ... samplers (textures stage) X us/draw` should fall. The `GPU ring native` CPU % in profile.log for
  the low-fps sections should fall by 5-12 % of its value.
- For the variants, compare captures of the same route (texture pop-in, streaming textures) and the `ME stable
  texture changed ... after interval` lines (their count and `sample only` versus `full`).

Unit test: `tests/cpu/test_native_shader_load_memo.cpp` (random programs, in-place word rewrites, size and stage
changes, collisions. A hit must imply the same raw words, the same swapped words, the same XXH3 and the generation
stored with them).

## Round 2 (2026-10-07): where the rest of the 38 us goes, measurement switch, three more exact savings

Sources: `mass-effect-recomp/run/me1/ringx_r/console.log` (round-1 exact set plus `texture_spread_phase`) and
`ringxs_r/console.log` (the same plus `fingerprints_sample_min_interval = 4`), RU, 960x544, 1785/768 MHz, and the code.
No profile of the current ELF exists, so every number below is a log counter or an estimate from the code, marked as such.

### 2.1 The heavy windows of the two runs

| | ringx_r (line 4738) | ringxs_r (line 4823) |
|---|---|---|
| Swaps / 10 s | 206 | 214 |
| Ring draws / 10 s (per frame) | 223,677 (1,086) | 258,371 (1,207) |
| Draws that reach the Vulkan draw ("native: drawn", delta) | 136,556 | 157,414 |
| PM4 packets / 10 s (per ring draw) | 4.13 M (18.5) | 4.75 M (18.4) |
| Copies (resolves and clears) / 10 s | 4,316 | 4,735 |
| C6 substages: timed Vulkan draw, sum / textures stage (us per Vulkan draw) | 15.44 / 10.06 | 12.95 / 7.22 |
| Texture fingerprints in the hitch frames / 10 s | 444 ms (1.28 GB checked) | 98 ms (250 MB) |
| Texture creation on the ring / 10 s | 134.5 ms | 163.7 ms |
| Sampler table misses "by generation" / 10 s (of all misses) | 46,462 (99 %) | 39,264 (97 %) |
| EDRAM mode-4 sync: slow walks / tiles visited; publish tiles | 46,047 / 16.0 M; 2.06 M | 45,957 / 16.2 M; 2.01 M |
| EDRAM transfer runs / import passes / 9-pass imports | 14,478 / 9,554 / 2,155 | 14,993 / 9,944 / 2,193 |
| Deferred recording: calls queued (per ring draw); drains waiting for the worker | 929k (4.2); 43.8 ms | 1,071k (4.1); 40.9 ms |
| Render thread asleep in the D3D ring wait (sleeps ended by ring progress) | 5.9 s (25 %) | 6.1 s (19 %) |
| Ring-thread lines at warn level (each flushes the log to the SD) | ~110 | 110 (108 "VS identity mismatch") |

If the ring thread were busy the whole window, it would spend 10 s / 258k = 38.7 us per ring draw (ringxs). The C6 timed
region (the Vulkan draw) covers 12.95 us x 89k-157k calls = 1.2-2.0 s of it. The other 6-8 s per 10 s (25-30 us per
ring draw) are outside every existing timer: PM4 parse, shader loads, pairing and identity, the EDRAM sync walks and the
recording of the tile transfers, copies, present, logging and the waits. Section 2.3 adds the switch that splits them.

### 2.2 Per-draw cost by stage (us per ring draw, ringxs heavy window)

| Stage | us / ring draw | Basis |
|---|---|---|
| PM4 parse and register shadow (18 packets; ~150 register words, NEON run compare with `pm4_fast`) | 1-2 | code; PM4 mix counters |
| Shader loads (1.5 IM_LOAD per draw: 0.75 identical reloads by `memcmp`, 0.4 load-memo hits, the rest XXH3 + memo) | 0.5-1.5 | counters + code |
| Pairing and identity (`PairByAddress`, `Accept*Identity`; 0.11 VS identity mismatches per draw) | 0.5-1 | code |
| EDRAM prepare: sync walks (178 per frame miss the O(1) path, 350 tiles each, ~5-10 ns per tile) | 0.3-0.7 | counters x code estimate |
| EDRAM transfer recording (58 runs, 38 import passes, 8 9-pass imports per frame) | 2-4 | estimate (30-60 us per run with its barriers and passes) |
| Vulkan draw without textures (C6: 5.7 us per Vulkan draw x 0.61) | 3.5 | C6 substages |
| Textures stage (C6: 7.2 us per Vulkan draw x 0.61) | 4.4 | C6 substages |
| - of which PrepareTexture after a copy invalidated the caches (39k / 10 s at ~5-6 us) | ~0.9 | counters; texture stage minus creation and fingerprints |
| - of which texture creation / fingerprints | 0.6 / 0.4 | C3 and hitch lines |
| Copies (4.7k / 10 s: resolve, clear, the EDRAM transfers they trigger) | 1-3 | estimate |
| Logging on the ring thread (110 SD flushes at warn, ~400 info lines per 10 s) | 0.2-2 | estimate: an SD flush costs ~1-5 ms |
| Deferred recording queue (4.1 calls per ring draw) | 0.3-0.6 | estimate |
| Sum of the estimates | 15-24 | |

The estimates leave 15-20 us per ring draw unexplained, or the ring is not busy the whole window. Only a measurement
can tell; the profile of heap1 could not be symbolized (section 1). Section 2.6 says how to get both.

### 2.3 `masseffect_native_ring_partition` (measurement only, default off)

`app/src/native/me_ring_partition.h`. The ring thread's time is split into 16 phases; every scope entry and exit adds
the time since the previous switch to the running phase, read from CNTVCT_EL0 (19.2 MHz on the Switch, two reads per
scope, ~20-40 ns). The phases cover the thread without gaps, so their sum is the 10 s window, and nested scopes give
the time back to the enclosing phase: `wait` (condition variable), `parse` (the rest of the packet loop), `registers`
(type-0 runs, SET_CONSTANT*, LOAD_ALU_CONSTANT), `shader loads`, `pairing`, `draw front` (identity and extent proofs
in NativeGraphicsSystem::Draw), `EDRAM prepare` (redirected clears and PrepareDrawEDRAM4, i.e. the sync walks),
`EDRAM transfers` (phase 2 of SynchronizeEDRAM4, in draws and copies), `Vulkan draw`, `textures` (the sampler loop),
`EDRAM publish`, `copies`, `present`, `WAIT_REG_MEM`, `segment end` (read pointer write-back, wake-ups, deferred
flush) and `reports`. Every 10 s:

```
[native] ring partition (10.0 s, 214 Swaps, 258371 ring draws, 157414 Vulkan draws): busy N ms = P % of the interval,
U us per ring draw | parse X ms (Y us/draw, E entries) | wait ... | registers ... | ...
```

Logging is not a phase of its own (it happens inside the others); compare a leg with `masseffect_native_mismatch_log_info`.
Expected overhead while on: ~10 scopes per ring draw, 0.3-0.6 us per draw (1-2 %). Test:
`tests/cpu/test_native_ring_partition.cpp` (sum of the phases = elapsed time, nesting, kNone, other threads, reset).

### 2.4 Exact savings (each default off, each with a self-check)

| Setting | File | What it saves | Expected |
|---|---|---|---|
| `masseffect_native_invalidate_textures_each_copy = false` (existing cvar, code default false; the shipped toml says true) with the new check `masseffect_native_texture_copy_verify = 4096` (default) | masseffect_native_draws.cpp, masseffect_native_targets.cpp | the PrepareTexture re-run of every texture fetch after each of the ~22 copies per frame (97-99 % of the sampler table misses) | ~40k PrepareTexture calls per 10 s, ~0.5-1 us per ring draw, 2-4 % of the ring |
| `masseffect_native_mismatch_log_info = true` | me_native_system.cpp | the SD flush of the ~110 periodic identity-mismatch lines per 10 s (the logger flushes on warn) | 0.1-0.5 s per 10 s if a flush costs 1-5 ms; also fewer stalls on the sink mutex |
| `masseffect_native_edram4_sync_lean = true` | masseffect_native_targets.cpp | two integer divisions and the area compares per foreign-owned tile of the sync walk | 20-60 ms per 10 s (0.1-0.25 us per ring draw) |

**Texture caches across copies.** `CopyInternal` bumped `generation_textures_` on every copy, so the register cache
and the 4096-slot per-fetch table both forgot every texture ~22 times per frame, and the next use of each fetch ran
PrepareTexture again (XXH3 of the key, `textures_`, the region math, `SlotView` with its XXH3 and `views_`, `SlotSampler`,
`resolved_` / crop lookups). The code already invalidates what a copy can change, each with its own bump: a resolved
texture created or rebuilt at an address (GetResolved), a format or swizzle change, a crop rewritten (ResolvedWritten),
a resolved image prepared, a view retired, `InvalidateImages` for swapped images. Within a frame PrepareTexture of an
already checked texture returns the same slot without side effects (`texture.frame == frame_`), so skipping it does not
skip a content check. The check: `NoteCopyTextures` counts copies (always); each cache entry remembers the copy count
it was filled at. With the per-copy invalidation off, the first N hits whose entry predates the latest copy (the hits
the old behavior would have refused) are not taken: PrepareTexture runs exactly as before and its slot, heap, sampler
and size are compared with the cached ones. A difference logs `DIFFERENCE: texture cache kept across a copy` and turns
the per-copy invalidation back on for the session. With the toml's `true`, nothing changes (the check is off).

**Mismatch lines at info.** `AcceptVertexShaderIdentity` / `AcceptPixelShaderIdentity` log the first 32 mismatches and
then 1 in 256 at warn level. In the walk windows that is ~28k VS mismatches per 10 s (the draw-time guard rejecting a
paired candidate, normal), so ~110 warn lines, and `flush_on(warn)` writes the log file to the SD on the ring thread
each time. With the switch, the periodic lines after the first 32 are written at info (same text); the first 32 and
every DIFFERENCE stay warn/error.

**Lean sync walk.** `RangeTilesEDRAM4` already clips the walk to the tiles that intersect the area and the image, and
the walk visits only those (rows ty0..ty1, columns tx0..tx1, or one run when the range spans the pitch), so
`TileUsedEDRAM4(target, tile, area)` is always true there; its whole-tile flag is read only with a clear overwrite or
cutout. Without those, the switch skips the test for tiles owned by another view (the owned and empty ones are skipped
before it anyway). The first 4096 skips compute it and a false one logs `DIFFERENCE: EDRAM mode-4 lean sync walk` and
turns the switch off (the result of that tile is still the old one). Test: `tests/cpu/test_native_edram4_sync_lean.cpp`
(400k random images of every tile shape, any pitch, areas inside and outside, starts and limits: 20 M visited tiles,
the test is never false).

### 2.5 Considered, not done

- **Open addressing instead of `std::unordered_map`** for `textures_`, `views_`, `samplers_`, `resolved_`: they are only
  used on sampler table misses. With the per-copy invalidation off those fall from ~40k to a few thousand per 10 s.
  The maps in the per-draw path were already replaced in round 0-1 (object table, record table, identity cells,
  per-fetch table).
- **Per-register-group dirty tracking**: the Vulkan draw already skips by generation the framing (76 % cached), the
  constants (set 4 by differences, 2.2 words per bind), the pipeline (82 % direct cache) and the samplers (per register,
  per fetch). What is left of the 5.7 us is mostly NVK (`C6 NVK parts`: 1.6 us per draw).
- **Allocations**: no heap allocation was found on the steady per-draw path (the vectors are reused; `fmt::format`
  only in bounded diagnostics, `edram4_pair_keys` on). The per-check `std::vector` of WriteConstantRunRaw runs only for
  the first 2048 runs.
- **NEON byte swap**: the vertex copy, the index conversion and the PM4 register runs are NEON already.
- **Memo of the whole sync walk** (no transfer when the owner and version bytes of the range did not change): exact by
  comparison, but after the lean walk a visited tile costs ~2-3 ns, so the memo would save ~40 ms per 10 s, in the
  ownership code another task is changing.
- **Per-owner invalidation of `edram4_own`** instead of the global `edram4_epoch_` (any ownership change anywhere drops
  the O(1) path of every view; read-only depth binds never get it): potentially the larger EDRAM saving, but it
  changes the ownership bookkeeping. Wait for the partition numbers of `EDRAM prepare`.
- **Read-only depth binds and the keep-version publish without a fast path**: same.

### 2.6 Next console runs (for the main session)

Build: `run/me1/ru_ringcpu2.nro`, with its unstripped ELF next to it as `run/me1/ru_ringcpu2.elf` (copied from
`out/edition-ru/out/nx/masseffect` right after the build; symbolize only with that copy).

1. **Measurement run (PROFILE)**, the same route and toml as `ringxs` plus `masseffect_native_ring_partition = true`
   (prepared as `run/me1/ringpart.toml`):
   `PROFILE=1 tools/me1_anderson.sh ringpart_r run/me1/ringpart.toml` with `ru_ringcpu2.nro` installed. Then
   `cp run/me1/ru_ringcpu2.elf run/me1/ringpart_r/masseffect.elf` and
   `python3 masseffect-nx/tools/symbolize_profile.py run/me1/ringpart_r/profile.log run/me1/ringpart_r/masseffect.elf`
   (Docker with devkitpro/devkita64, as tools/function_order.py). Read the `GPU ring native` thread's "per function"
   block of `profile.log.sym` for the heavy windows, and the `[native] ring partition` lines of the same windows.
   Rule for every later run: save `cp out/edition-ru/out/nx/masseffect run/me1/<name>.elf` at the moment the NRO is
   copied to `run/me1/<name>.nro`; a profile is only symbolized with the ELF of its own NRO.
2. **Exact A/B**: `run/me1/ringpart.toml` against `run/me1/ringpart_ab.toml` (both prepared: ringxs.toml plus the
   partition switch; the second also has these three settings, the first line replacing the toml's `true`):
   ```toml
   masseffect_native_invalidate_textures_each_copy = false
   masseffect_native_mismatch_log_info = true
   masseffect_native_edram4_sync_lean = true
   ```
   Check: `C6: texture caches kept across copies; the first 4096 hits ...` at start, then `C6: texture caches across
   copies: 4096 hits checked against PrepareTexture, all equal (check finished)`; never `DIFFERENCE`. The
   `C6 per-fetch sampler cache` line: "by generation" should fall from ~40k to a few thousand per 10 s, and the
   `textures` phase of the partition by ~0.5-1 us per ring draw. Draws/s of the heavy windows against ringxs_r
   (25.8k draws/s, 214 Swaps). Images must be identical (same captures).

## Round 3 (2026-10-07): the textures and uploads stages

Sources: `mass-effect-recomp/run/me1/nvk_o2/console.log` and `nvk_o1/console.log` (`run/me1/nvk_base.toml`: the
round 1 and 2 sets, `texture_spread_phase`, `fingerprints_sample_min_interval = 4`, `invalidate_textures_each_copy =
false`, `report_stages`), RU, 960x544, and the code. Build: `run/me1/ru_tex.nro`, unstripped ELF `run/me1/ru_tex.elf`.

### 3.1 What the two stages contain (heavy Normandy window, nvk_o2: 212,032 Draw calls, 124,282 recorded / 10 s)

`C6 stages`: textures 12.33 us and uploads 8.56 us per timed recorded draw, i.e. ~1.53 s and ~1.06 s per 10 s. Note
the divisor: draws rejected after a stage add their time but not to the divisor (40 % of the Draw calls are not
recorded), so these per-draw figures are upper bounds of the cost of a recorded draw.

Textures stage (the sampler loop; `C6 substages` samplers = 12.09 of the 12.33 us):

| Item / 10 s | Count | Note |
|---|---|---|
| Register-cache hits | 293,003 | ~50 ns each |
| Per-fetch table hits | 128,316 | XXH3 of 24 B + compare, ~0.1 us |
| Per-fetch misses -> PrepareTexture | 51,071 | **48,433 "by generation"**, 508 expired, 2,130 slot |
| Texture creation on the ring (C3 line) | 192-289 ms | 208-257 textures |
| Rechecks (C3 sampling, per 30 s) | ~23k sample + ~3k full | ~0.15-0.3 s / 10 s of XXH3 |

So ~0.4-0.5 s of the 1.53 s is creation and recheck hashing; the rest (~1 s, ~15-20 us per PrepareTexture call: key
XXH3, `textures_` map, region math, `ResolvedTexture` map, `SlotView` XXH3 + `views_` map, `SlotSampler` map) is
calls that find nothing to do: the texture was already checked this frame or its recheck is not due. They are misses
because the generation of the sampler caches is still bumped ~200 times per frame with the per-copy invalidation off:
every resolved texture created or rebuilt (`GetResolved`), prepared (`Prepare`), re-swizzled, and every copy into a
resolved texture that has logical crops (`ResolvedWritten`; the 256x138 crop at 1F574000 has ~8k copies per 10 s)
drops every entry, although each of these only concerns the fetches of one address. On top of that the shipped toml
has `masseffect_native_cache_textures_between_frames = false`, so every distinct fetch misses once per frame anyway
(it was measured "not worth it" while the per-copy invalidation was on: the entries never survived a frame then).

Uploads stage: texture uploads (rare), per vertex binding the dedupe fingerprint (`SampleFingerprint` of cold guest
memory, up to 8 KB hashed for a 16 KB binding) + dedupe lookup + NEON swap copy into the uncached upload buffer, the
index copy, and the VS / PS constants: 101,906 VS and 96,722 PS re-uploads per 10 s "by generation" (any register
write of the bank changes the generation, even with the same value), each a memcpy of up to 4 KB into uncached memory
plus a new set 4 offset. No split existed, so 3.2 adds one.

### 3.2 `masseffect_native_report_texture_uploads` (measurement only)

Every 10 s two lines (only with the switch on; all PrepareTexture calls are timed, the uploads parts on the 1 in 64
timed draws):

```
[native] C6 textures (last 10.0 s): samplers R register hits, F per-fetch hits, P PrepareTexture (G by generation, E
expired, S slot); PrepareTexture X ms in P calls | resolved n (ms, us each) | checked this frame ... | not due ... |
postponed ... | same sample ... | same fingerprint ... | same data ... | changed ... | created ... | invalidations:
format n, created n, crop n, prepared n, prepared (not resolved) n, each copy n, untargeted n | by address on/off
(checked N), cross-frame on/off (checked N)
[native] C6 uploads (us per timed draw; T timed; sum S) | texture uploads | vertex fingerprints | dedupe | vertex copies
| indices | VS constants | PS constants || vertex bindings B: H dedupe hits, C copied (MB, B each; A fingerprinted
after the copy) | VS constants U uploads (MB), Q with the same bytes as the last upload (Z reused) | PS ...
```

"with the same bytes as the last upload" is counted with the reuse switch off too: it is exactly what 3.5 saves.

### 3.3 Exact savings (each default off, each with a self-check)

| Setting | What it saves | Expected |
|---|---|---|
| `masseffect_native_texture_inval_by_address = true` (+ `masseffect_native_texture_inval_verify = 4096`) | the PrepareTexture of ordinary textures after a resolved-texture change at another address | most of the ~48k "by generation" misses |
| `masseffect_native_cache_textures_between_frames = true` (existing cvar, code default true, toml false) with the new check `masseffect_native_texture_frames_verify = 4096` | the once-per-frame PrepareTexture of every distinct fetch whose recheck is not due | with the line above: PrepareTexture only for due rechecks, creations and resolved fetches; textures stage ~12 -> ~4-6 us per recorded draw (the creation and the recheck hashing stay) |
| `masseffect_native_constants_same_content = true` (+ `masseffect_native_constants_same_verify = 2048`) | the VS / PS constant copy (up to 4 KB into uncached memory) and the new set 4 offset when the bytes the shader reads did not change | depends on the "same bytes" count of 3.2; up to ~1-3 us per draw |
| `masseffect_native_dedupe_hash_after_copy = true` | the cold-memory read of the dedupe fingerprint for bindings that cannot hit (no live entry with that address and size): copy first, then hash the same bytes from the cache | ~0.5-2 us per copied binding |

**Invalidation by address.** `InvalidateTextures()` at the five targets call sites became
`InvalidateTexturesAt(address, reason)`: the old global generation is bumped exactly as before (so with the switch
off nothing changes), and also a per-bucket generation (1024 buckets of 4 KB pages of the fetch base). Each cache
entry stores both, its bucket and whether PrepareTexture answered it with a resolved texture. With the switch an entry
stays valid while (a) no invalidation of everything happened (an image retired: `ForgetImage`, `DropImages`; the
per-copy invalidation; a failed check), (b) its bucket was not invalidated and (c) it is not a resolved-texture entry,
which still needs the old rule. Why it is exact: for an ordinary texture (no prepared resolved texture at its base),
PrepareTexture reads only `ResolvedTexture(base)` (an exact map lookup by address: it changes only when a resolved
texture at that base is created, prepared, rebuilt or swapped, each of which now bumps that base's bucket), the
texture's own image and view (changed only by retirement, which bumps everything) and its recheck schedule (governed by
`valid_until`, unchanged). `Prepare` of an image no resolved texture holds (a render target) changes nothing a fetch
can see; `Prepare` scans `resolved_` to tell which. The image swaps (`SwapWithResolved`, the lazy-front rotation)
never invalidated ordinary entries; with the switch they bump the bucket too (`NoteResolvedAt`), so the new rule is
stricter there than the old one. Check: the first 4096 hits the old rule would refuse run PrepareTexture and compare
slot, heap, sampler and size; a difference logs `DIFFERENCE: texture cache kept across an invalidation of another
address` and switches it off.

**Cross-frame check.** `masseffect_native_cache_textures_between_frames` already ties every entry to `texture.next - 1`
(resolved textures to the current frame), and a PrepareTexture whose recheck is not due has no side effect
(`texture.frame`, the eviction age, is only written by a recheck). The new check takes the first 4096 hits from an
earlier frame through PrepareTexture and compares; a difference logs `DIFFERENCE: texture cache kept across frames`
and turns the cross-frame cache off.

**Constants by content.** A CPU-cached shadow keeps the bytes of the last VS and PS constant upload (written with the
copy). When the generation changed but the upload buffer epoch is the same, the shader reads no more than was uploaded
and the PS tonemap override is the same, the shadow is compared with the registers (NEON); if equal the old offset is
reused and the bank is marked as holding the new generation **for those bytes only** (`constants_*_bytes_` shrinks to
them, so a later shader of the same generation that reads more uploads again; the unit test found that case). The
upload buffer only moves forward until `UseSlot` (new epoch), so the bytes at the offset are still the uploaded ones.
Check: the first 2048 reuses read the upload buffer back and compare it with the registers (override applied);
`DIFFERENCE: VS constants reused by content` switches it off.

**Fingerprint after the copy.** The dedupe fingerprint of a binding is only compared in `Search` when an entry with the
same address, byte order and size is live (`DedupeVertices::Candidate`); otherwise it only goes into `Note`. Then the
copy runs first (only the synchronous copy, not the copy thread) and the fingerprint is taken afterwards from the same
guest bytes, now in L1/L2. Both orders read the same memory on the same thread; they differ only if the guest writes
the range between the two reads, which the old order does not protect against either. Check: the first 4096 uses also
take the fingerprint before the copy and compare; `DIFFERENCE: vertex fingerprint after the copy` switches it off.

Tests: `tests/cpu/test_native_texture_upload_round3.cpp` (model of PrepareTexture's answer under random targets events
with the real call mapping: the new rule never returns a stale answer and takes every old-rule hit except after a
swap, where the old rule itself returned stale answers; constants reuse against the old logic on separate forward
upload buffers, the shader's bytes always equal the registers; `Candidate` false implies `Search` false).

Latent issue found (not changed): `InvalidateImages` compares the entry's `slot` with the view slot, but entries keep
`RemappedSigns(fetch) << 24` in the slot's top byte, so an entry of a signed texture view is never invalidated there.
Only swapped target / resolved images go through it and signed fetches of those are unlikely; worth a look if a swapped
image ever shows a stale view.

### 3.4 Considered, not done

- **Guest page write tracking** to skip rechecks and vertex hashing of unwritten pages: `RexGmProtect` exists, but on
  Horizon it works by unmapping (every read then faults and is emulated) or, if the kernel accepts it, read-only
  permissions on one of the five views of the same physical memory: writes through the other views, host writes (file
  I/O, audio) and the GPU resolves would not be seen. Not exact without trapping all of them; large and risky.
- **Rechecks on a worker thread**: the hash would be of guest memory at another moment than the draw that uses the
  texture, so a change could show one frame later or earlier: image-changing, opt-in only. It would remove the
  ~0.15-0.3 s per 10 s of recheck hashing from the ring. Not done in this round (needs a libnx thread and a
  hand-over of results); the sampled recheck (`fingerprints_sample_min_interval = 4`) already covers most of it.
- **Vertex cache across frames** (keep static meshes in a persistent GPU buffer): proving the content unchanged needs a
  full hash of the source, which costs about as much as the NEON copy it would save.
- **Reusing the image of a live texture with the same content** (C3 reuse line: 145 of 257 new textures, 16 MB per 10 s):
  saves creation and upload, but the two keys then share one image and a later change of either needs copy-on-write.
- **Untiling straight into the upload buffer**: saves one copy only for changed or new textures (a few MB per 10 s).

### 3.5 Console runs (for the main session)

Install `run/me1/ru_tex.nro` (ELF `run/me1/ru_tex.elf`). Two tomls are prepared:

1. `run/me1/tex_base.toml` = `nvk_base.toml` + measurement:
   ```toml
   masseffect_native_report_texture_uploads = true
   ```
2. `run/me1/tex_ab.toml` = `tex_base.toml` + the exact set (the first line replaces the toml's `false`):
   ```toml
   masseffect_native_cache_textures_between_frames = true
   masseffect_native_texture_inval_by_address = true
   masseffect_native_constants_same_content = true
   masseffect_native_dedupe_hash_after_copy = true
   ```

Same route as nvk_o1/o2 (`tools/me1_anderson.sh` with the toml). Check in the log:
- Start: `[native] C6 round 3: report textures/uploads on; invalidation by address ON (first 4096 hits checked);
  cross-frame hits checked 4096; constants reused by content ON (first 2048 checked); vertex fingerprint after the
  copy ON (first 4096 checked)`.
- Then, once each: `C6: texture caches by address: 4096 hits checked against PrepareTexture, all equal`, `C6: texture
  caches across frames: 4096 hits checked ..., all equal`, `C6: constants reused by content: 2048 reuses checked ...,
  all equal`, `C6: vertex fingerprints after the copy: ... all equal`. Never `DIFFERENCE`.
- `C6 stages` of the heavy windows: textures and uploads against nvk_o2 (12.3 / 8.6 us). `C6 textures`: PrepareTexture
  calls should fall from ~50k to the due rechecks + creations + resolved (~10-15k), "by generation" to a few
  thousand. `C6 uploads`: "reused" close to "same bytes"; the VS / PS constants parts should fall accordingly.
- Draws/s and Swaps of the heavy windows against nvk_o2; captures must be identical (exact set).
- If tex_base shows a large "same bytes" count but tex_ab saves little, or if the textures stage stays above 5 us,
  the next step is the creation (C3 creation ms) and recheck hashing, see 3.4.

## Feros firefight profile 2026-10-08

Sources: `mass-effect-recomp/run/me1/manual2/rex_profile.log` (stack sampling, 1 ms) and its `profile_summary.txt`,
symbolized with `run/me1/ru_loc2.elf` (`-g1`: line tables, no inline info; `.text` at VMA 0, so `image+X` = ELF
address X), toml `run/me1/manual_best.toml` (1785/768 MHz). Tools: `xcrun llvm-objdump`, `xcrun llvm-dwarfdump
--lookup` (Xcode has no `llvm-addr2line`/`llvm-symbolizer`), `xcrun llvm-nm -S`.

### F.1 The "TargetsVulkan::Draw leaf 17 %" is a misattribution

`switch_perf.cpp` records two separate histograms per thread: `pc` (the real leaf) and `stack` (the return
addresses read from the x29 frame chain). The first element of a `stack` chain is **not** the pc: it is the return
address saved by the innermost function that has a frame record, i.e. a call site in its *caller* (and, for frameless
leaves such as memcpy, memcmp and XXH3, the caller of the caller). The summary's "leaf" column was built from
`stack[0]`, so every name in it is shifted one frame up. Checked by disassembling each top address (it always
follows a `bl`/`blr`):

| `stack[0]` | share of ring stack samples | Source line | What it really means |
|---|---|---|---|
| `0x596a0` in TargetsVulkan::Draw | 14.1 % | `me_ring_partition.h:75` (the `blr x2` right before is the virtual call to `DrawsVulkanImpl::Draw`) | pc inside **DrawsVulkanImpl::Draw** itself or its frameless callees |
| `0x9e4a0` in PrepareTexture | 9.7 % | draws.cpp:9101 (build copy) `XXH3_64bits(raw_base, extension)` | pc in XXH3 hashLong: texture base-level recheck hash |
| `0x9e4b8` in PrepareTexture | 3.4 % | draws.cpp:9103 `XXH3_64bits_withSeed(raw_mips, ...)` | XXH3 of the mips |
| `0x2d9fc` in DrawsVulkanImpl::Draw | 3.5 % | draws.cpp:3526 | pc in CopyVertices |
| `0x341dc` / `0x34300` in Packet | 3.9 / 2.9 % | me_native_system.cpp:1227 / 1239 | pc in Type3 / WriteConstantRunRaw |
| `0x59e64` in TargetsVulkan::Draw | 2.2 % | targets.cpp:2471 | pc in PrepareDrawEDRAM4 |
| `0x4d6f8` in AcceptVertexShaderIdentity | 0.9 % | me_native_system.cpp:1755 | pc in MatchesVertexShaderIdentity |
| `0xbb82c`, `0x6538c`, fs_dev | 25 %, 4 %, 8 % | nanosleep, cond wait, SD writes | waits and logging (handled elsewhere) |

TargetsVulkan::Draw's own code (EDRAM ownership/tile bookkeeping inlined into it) is only **0.7 %** of the pc
samples (targets.cpp:5583-5595 `SyncDrawEDRAM4`-inlined lines, `me_edram_ownership.h:70-130`). There is no hidden
EDRAM hotspot there.

Real leaf (pc) shares of the ring thread (the pc lists are the top-80 per 10 s block; 4,108 of 5,087 busy samples
attributed; hitch-only column = samples inside frames over 45 ms, 447 attributed):

| Function | all | hitch |
|---|---|---|
| XXH3_hashLong_64b_default (the copy at 0x20ef0) | 21.4 % | 23.6 % |
| XXH3_hashLong_64b_withSeed (0x45150) | 3.6 % | 3.8 % |
| memcpy | 7.9 % | 5.1 % |
| NativeGraphicsSystem::WriteConstantRunRaw | 5.6 % | 8.4 % |
| CopyVertices | 4.9 % | 6.1 % |
| DrawsVulkanImpl::Draw (self; spread over ~60 lines, top: xenos.h:1032, vector, dedupe.h:121, hashtable) | 4.1 % | 4.7 % |
| memcmp | 2.8 % | 3.3 % |
| MatchesVertexShaderIdentity | 2.7 % | 3.1 % |
| PairDraw | 2.6 % | 3.3 % |
| _free_r | 2.6 % | 1.6 % |
| PrepareDrawEDRAM4 | 2.1 % | 3.6 % |
| PrepareTexture (self) | 1.8 % | 1.8 % |
| TargetsVulkan::Draw (self) | 0.7 % | 0.9 % |

Who calls the hot hashLong copy: `XXH_INLINE_XXH3_64bits` (tail call) from PrepareTexture's raw fingerprint (the
9.7 % + 3.4 % above, about **64 % of all XXH3 time**), the vertex dedupe block hash (draws.cpp:1596, fingerprint
mode 1, ~1.9 %) and the index cache `RangeFingerprint` (draws.cpp:2979, ~1 %). The shader identity path does not
hash: `AcceptVertexShaderIdentity` hashes the candidate only for the logged mismatches (the first 32, then 1 in 256,
~10 per second), its result is already memoized per (candidate, VS load generation) with
`masseffect_native_flat_identity` (on in manual_best), and the `XXH3_hashLong` copy it calls (0x20440) has no sample.

Settings that make the recheck this expensive in manual_best.toml: `masseffect_native_texture_interval_max = 4`
(every stable texture in view is fully re-hashed every 4 frames) with `fingerprints_sample_min_interval` at its
default 8 (sampling never acts at cap 4). Off in that toml (defaults): the round-3 set
`texture_inval_by_address`, `constants_same_content`, `dedupe_hash_after_copy`, and
`cache_textures_between_frames = false` explicitly. The constant copies (memcpy 7.9 %, WriteConstantRunRaw is the
register side) are what `constants_same_content` targets; it was off in this run.

### F.2 What is hashed per texture recheck

PrepareTexture, for a 2D texture or cubemap whose recheck is due (`frame_ >= texture.next`): XXH3 of the whole
guest footprint of all layers (`extension`: tiled upper bound of the base level, `(layers-1)*stride_face` more for
cubemaps) plus XXH3-with-seed of the whole mip region (`extension_mips`). Interval 1, 2, 4 then capped at 4 by the
toml (2 after three late changes with `adaptive_texture`). The hitch lines of earlier runs measured 25-38 MB per
recheck frame at ~0.40 ms per MB on the A57 (memory-bound XXH3, ~2.5 GB/s). Nothing else reads those bytes unless
the hash changed.

### F.3 `masseffect_native_texture_coherency` (new; default 0)

Files: `app/src/native/me_texture_coherency.h` (new), `me_native_system.cpp` (event hooks),
`masseffect/masseffect_native_draws.cpp` (PrepareTexture, report), `masseffect/masseffect_native_targets.cpp`
(read-back marks). Test: `tests/cpu/test_native_texture_coherency.cpp` (`clang++ -std=c++20 -O2 -I app/src/native
...`, passes on the host). No copy of these files under `editions/ru/overlay`.

**Idea.** The 360 GPU caches texture and vertex data, so whenever memory the GPU may have read is changed by the CPU
or by a DMA, D3D must tell the GPU to drop that range: in the ring, a type-0 run `COHER_SIZE_HOST` (0x0A2F, bytes),
`COHER_BASE_HOST` (0x0A30, physical), `COHER_STATUS_HOST` (0x0A31, TC/VC action bits) and a WAIT_REG_MEM on the
status. The recompiled D3D does exactly this (sub_82227210, 13 callers: base rounded down to 4 KB, size rounded up;
sub_822261D0 with status 0x03000100; sub_8222C170; sub_8222E140 with size 0). The native ring used to only clear the
status. Now (only when the switch is on) every such range stamps the 16 KB pages it touches (one global counter,
widened by 4 KB on both sides in case an address is virtual), and so do the resolved texels the targets read back
into guest memory (`WriteReads`). Each texture keeps the stamp taken right before its last full hash and the ranges
it covered. At a due recheck, if no page of its base or mip range has a newer stamp, the bytes were not declared
changed.

**Mode 1 (skip).** A clean recheck is answered without XXH3, with exactly the state changes of a full recheck that
found the same hash (interval doubling to the cap, `next` with the same jitter, `SpreadPhase`, `valid_until`,
`kPrepareSameFull`), and costs no fingerprint budget. Guards (the existing self-check pattern):
- the first `masseffect_native_verify_n` (2048) clean rechecks still hash and must find the same bytes;
- after that, 1 in `masseffect_native_texture_coherency_full_every` (8) clean rechecks of each texture still hash
  (a standing guard: an undeclared change is caught at most 8 rechecks, i.e. 32 frames at cap 4, later);
- any clean recheck whose hash changed logs `DIFFERENCE: texture ... changed in guest memory with no coherency
  event` and switches the skip off for the session (the texture itself is then uploaded by the normal path);
- a texture whose range changed (format, size, address) or that was never hashed with the switch on is not eligible.
Dirty rechecks (an event touched the pages) take the usual path unchanged, including sampling and budget.

**Mode 2 (measurement).** Every recheck still hashes; each one is classified clean/dirty x same/changed. Image
identical to off. Run this first if the risk of mode 1 is a concern: `clean+CHANGED` must stay 0 for mode 1 to be
safe, and `clean+same` MB is what mode 1 would not hash.

**`masseffect_native_texture_coherency_early` (default false, NOT frame-identical).** A texture whose recheck is not
due is rechecked at once when an event touched its pages after its last hash. Fresher textures (a declared change
shows on the next use instead of up to `interval` frames later), so it attacks the popping that made the toml use
`texture_interval_max = 4`. With it, a higher cap (8-32) costs no extra staleness for declared changes. It acts when
PrepareTexture is reached (every frame per fetch with `cache_textures_between_frames = false`, as in manual_best).

Exactness: with the hypothesis true (every change of texture memory is declared before the draws that use it), mode 1
gives the same answer at every recheck as today, so the image is the same. The only scheduling difference is the
fingerprint budget (`fingerprints_kb_frame`, 6 MB): skipped rechecks no longer use it, so fewer other rechecks are
postponed (never staler); the budget only acts from interval 8, so with `texture_interval_max = 4` there is no
difference at all. Where the hypothesis could fail: a game write with no invalidation (memory the GPU had not read
since a full invalidation, or a pool slot refilled without D3D), or GPU memexport (not used by the native path). The
guards and mode 2 measure exactly that.

Cost: marking is O(pages) per event (a 1 MB range = 64 CAS); a check is O(pages) loads (a 1 MB texture = 64 loads,
~50 ns) against ~400 us of XXH3 for that 1 MB.

**Expected gain.** PrepareTexture's raw hashing is ~13 % of all ring stack samples (~16 % of the busy ones; 25-27 %
of the busy pc samples are XXH3 in total, ~64 % of it from this site). If most stable textures see no event between
rechecks, mode 1 removes ~7/8 of it: ~11-14 % of the ring's busy time, ~3-5 ms per frame in the Feros firefight and
most of the 10-15 ms recheck bursts in hitch frames. Unknown until mode 2 runs: how many events the game sends and
how wide they are (a full-memory invalidation every frame would make everything dirty: no gain, no harm).

**Log lines.** Start: `[native] C3: texture recheck by coherency events (masseffect_native_texture_coherency) = 1;
clean rechecks skip XXH3, the first 2048 and 1 in 8 per texture still hash; early recheck on an event off`. Then
once: `... 2048 clean rechecks hashed, all equal (verification finished ...)`. Every 10 s: `[native] C3 coherency
(last 10 s, mode M): events E (BASE writes, waits, MMIO, read-backs; empty), MB declared, pages stamped | rechecks
skipped S (MB not hashed), guarded, early | hashed: clean+same, clean+CHANGED, dirty+same, dirty+changed`. Never:
`DIFFERENCE: texture ...`. Mode 2 logs the first 16 `clean ... changed with no coherency event` cases at info.

**Console runs.** Same route as manual2, manual_best.toml plus:
1. `masseffect_native_texture_coherency = 2` (measurement; image identical). Read the `C3 coherency` lines: events
   per 10 s, `clean+CHANGED` (must be 0), `clean+same` MB vs `[hitch] ... textures checked` MB.
2. If 1. is clean: `masseffect_native_texture_coherency = 1`. Compare the ring CPU % / fps of the firefight windows,
   the hitch lines' fingerprint ms, and captures (must be identical except for budget postponements).
3. Optional: add `masseffect_native_texture_coherency_early = true` (image change: fewer late textures), then try
   `masseffect_native_texture_interval_max = 8` or `32` with it.

### F.4 Other findings, not changed

- The profiler's `stack` chains omit the pc. Any tool that wants a leaf must use the `pc` histogram; a joint
  histogram (pc + frames) would need `switch_perf.cpp` to key the stacks with the pc first (more distinct chains).
- The vertex dedupe block hash (~1.9 %) and the index cache fingerprint (~1 %) could use the same table with the
  VC-action events (vertex/index buffers), but dynamic buffers are rewritten every frame and `g_synchronizations_ring`
  already scopes the index cache; left for a later round after mode 2 shows the event coverage.
- memcpy 7.9 % + WriteConstantRunRaw 5.6 %: the constant uploads; `masseffect_native_constants_same_content` (round 3)
  was off in this run and is the existing switch for it.

### F.5 First console runs (2026-10-08) and the per-texture exemption

- Mode 2 (`run/me1/ab/g1_a`): 35 report windows. 30 windows had `clean+CHANGED 0`, but five had 183-888 (2,514 cases
  in total, in the intro movies and the `GLO_Relay_LOAD.bik` loading movie). Every case was one of 12 textures, all
  **format 2 (k_8), linear, 1280x720 (900 KB) or 640x360 (pitch 768, 269 KB)**. These are the Y/U/V planes of the
  Bink movies, double-buffered (`ME video plane ...` lines at the same addresses: 1F37A000/1F336000/1F2F2000 and
  1F4E3000/1F49F000/1F45B000, later 14A4E000/148F3000/14937000 and 1F4CF000/1F447000/1F322000). Bink writes them with
  the CPU every frame and D3D sends no coherency event for them. Apart from those, about 10k clean+same rechecks
  (~3 GB) per 10 s matched the events, and the few dirty+changed rechecks were all declared.
- Mode 1 (`run/me1/ab/g2_b`): the first recheck of 1F37A000 (the logo movie's Y plane, 40 ms after creation) was a
  guarded hash that changed. The rule then was "one DIFFERENCE turns it off", so it switched itself off at startup.

Change (same cvar, still default 0):
- **Only stable textures may skip.** A texture must first be hashed once and found unchanged while the events
  called it clean (`coherency_confirmed`). A range change (address, size, mips) resets that. A texture rewritten
  every frame is never confirmed, so it is never skipped.
- **Linear textures are always hashed** unless `masseffect_native_texture_coherency_linear = true`. Video planes and
  other CPU-written surfaces are linear, while game textures are tiled.
- **Per-texture exemption.** Any texture that changes with no event is hashed on every recheck from then on (sticky
  for that cache entry). Those changes are logged at info for the first 32 textures.
- **Session guard.** Only an undeclared change on a texture that was *eligible* for the skip (confirmed, tiled, not
  exempt) logs `DIFFERENCE: ... after it had been stable (n of N allowed ...)`. After more than
  `masseffect_native_texture_coherency_max_undeclared` (default 4; 0 = the old first-one-off rule) the skip turns off
  for the session. The first `verify_n` eligible clean rechecks and 1 in `full_every` per texture are still hashed.
- Report: `clean+CHANGED X (of stable eligible textures Y; textures exempted Z)`. Y is what could have been shown
  late and must stay 0. On g1_a's data Y would have been 0: all 2,514 cases were on linear planes that are never
  confirmed.

## Heap allocations on the ring thread (2026-10-09)

Profile finding: the GPU ring thread spent ~1.3-3.3 % in `_free_r`, 1-1.9 % in `_malloc_r` and ~1 % in
`std::string::_M_create`, about 1.25-2.4 ms per frame. The churn also fragments the newlib heap, which is what ended
earlier runs in out-of-memory crashes (docs/memory-growth.md). Branch `perf/ring-no-alloc`. Everything below is
exact: same commands, same image, same log text (the keys and lines of the 10 s reports are byte-identical).

### H.1 Changed unconditionally (pure refactors, no cvar)

| Where | What allocated | Now | How often it ran |
|---|---|---|---|
| `NativeDrawExtentEstimator::Diagnostics::reason` (me_native_draw_extent_estimator.h) | a `std::string`; `"axis-aligned-sdk-estimate"` (25 chars) is past the 15-char SSO, so every accepted estimate allocated and the `Diagnostics` destructor freed it (me_native_system.cpp, end of the extent block) | `const char*` to the string literal (every assignment was a literal) | every extent-eligible draw and every rectangle of `ProveRectangleList` |
| `ProveRectangleList` (me_native_system.cpp) | `std::vector<uint32_t> regs(registers_)`: the whole register file, ~0x5003 words = ~80 KB malloc + memcpy + free per call | the two per-rectangle registers (`VGT_DRAW_INITIATOR`, `VGT_INDX_OFFSET`) are patched in `registers_` and restored by a scope guard on every return, as the predicated-draw path already does for `VGT_DRAW_INITIATOR`. Nothing else in the loop reads those two registers (`Register()` calls and `ProveFullOverwrite` read others), and the ring thread is the only writer of draw registers | every multi-rectangle (RectangleList, >3 vertices) draw with a PS |
| `RedirectClearDepthEDRAM4` / `RedirectClearStencilEDRAM4` (masseffect_native_targets.cpp) | a local `std::vector<Region>` per call | member vectors (`clear_depth_regions_edram4_`, `clear_stencil_regions_edram4_`) borrowed through `ScratchVectorEDRAM4` (moved out, cleared, moved back on every exit; a nested call would just get an empty vector of its own) | every proven clear candidate in mode 4 |
| `edram4_depth_no_reasons_`, `edram4_stencil_no_reasons_`, `edram4_restore_no_`, `edram4_operations_`, `edram4_fetch_pairs_`, `edram4_import9_pairs_` (masseffect_native_targets.cpp) | `fmt::format` into a temporary `std::string` key (or a `std::string` built from a literal) for `map::operator[]` on every increment | `std::map<..., std::less<>>` + `DiagnosticAt` / `DiagnosticAtFormat`: the key is formatted into a 256-byte stack buffer and found as a `string_view`; a `std::string` is made only when a new key is inserted. `NoteFetchEDRAM4` takes the kind and its detail separately instead of a pre-formatted `"{kind}, {detail}"` string (same key text) | every declined redirected clear, every `RestoreTargetEDRAM4` decline, every EDRAM conversion (`EnsureCapacityConversionEDRAM4`), every late stencil fetch, every 9-pass import |
| `extent_diagnostics_` (me_native_system.cpp) | `fmt::format` key + `unordered_set<std::string>::insert` on every extent-eligible draw until 32 distinct keys were seen (a location with fewer keys never saturates) | stack-buffer key and a transparent-hash `find` first; a `std::string` only for a new key (logged exactly as before) | every extent-eligible draw while the set has fewer than 32 keys |

Second pass (audit of the Draw / PrepareDrawEDRAM4 / SynchronizeEDRAM4 paths), also unconditional and exact:

| Where | What allocated | Now |
|---|---|---|
| `PrepareDrawEDRAM4`: `edram4_draw_desc_` (targets) | a new ~200-char `fmt::format` string per mode-4 draw while `edram4_traces_ < 60`; that counter only rises on 64+ tile transfers, so in practice on every draw | formatted into the member string's own buffer (`clear()` + `format_to`), capacity kept; same text |
| `DrawImpl`: `rectangle_entry` (draws) | a local `VerticesEntry` copied from the entry (two vectors) on every rectangle-list (type 8) draw | reused member `rectangle_entry_`; copy-assignment keeps the capacity |
| `EntryFor`: `entry_ = VerticesEntry{}` (draws) | dropped the vectors' capacity on every input-cache miss (every VS change once the cache holds 4096) | fields reset in place (same state) |
| `SynchronizeEDRAM4`: `spans` | local vector per sync that records a transfer | member `edram4_sync_spans_` through `ScratchVectorEDRAM4` |
| `CopyTilesEDRAM4`: `regions` | local `VkImageCopy` vector per copy | member scratch |
| `CopyStencilEDRAM4`: `PlanStencilCopyRegions` + `regions` | a returned vector plus a local `VkBufferImageCopy` vector per stencil copy | out-parameter overload of `PlanStencilCopyRegions` (the returning one now wraps it; the CPU test passes unchanged) and member scratch |
| `ImportColorDepthEDRAM4`: `rects` | local `VkClearRect` vector per stencil-only import | member scratch |
| `CloseImportBatchEDRAM4`: `copies.swap(...)` | the swap with a fresh local freed the batch list's buffer each batch | swapped with a spare member that keeps its capacity |
| `FailureEDRAM4`: key | `std::string` + 2 `std::to_string` + concatenations per failure | same `reason/target/source` text on the stack, transparent-hash lookup, `std::string` only for a new key |
| `ForgetSynchronizedEDRAM4` (restore / create) | erased the view's 2048-entry version table (8 KB) and stencil-source table (32 KB); the next use allocated them again | zeroed in place; only `Destroy` erases. A zero version never matches (`SynchronizedEDRAM4` requires version != 0) and an empty source record is skipped by every reader, so a zeroed table answers exactly as a missing one |

`ScratchVectorEDRAM4` moves the member out, clears it and moves it back on every exit, so a reentrant call gets an
empty vector of its own instead of aliasing. The deferred-recording queue deep-copies the region arrays of the
`vkCmd*` calls (the locals were already freed right after each call), so reusing the buffers is safe there too.

### H.2 `masseffect_native_edram_reuse_descriptor_sets` (new, default false, exact)

The EDRAM conversions allocate one descriptor set per dispatch/pass from the work slot's pools
(`pool_conversion_edram`, `pool_conversion_depth_edram`, `pool_import_depth_edram`, `pool_conv_color_frag`,
`pool_stencil_copy`; 11 call sites), and `TargetsVulkan::BeginRecording` resets all five pools each time the slot is
reused. In NVK every `vkAllocateDescriptorSets` is a `vk_object_zalloc` plus a pool heap allocation, and the reset
frees them all: two to three malloc/free pairs per conversion (~65 transfer spans per Normandy frame, more with
imports).

With the switch, each pool keeps the list of the sets allocated from it. `BeginRecording` (after `Complete(slot)`,
i.e. after the slot's fence, which is when the reset was legal too) only rewinds the list, and `AllocateSetEDRAM`
hands out the next listed set before allocating a new one. Why it is exact:
- Each pool serves exactly one set layout, so a reused set always has the requested layout.
- Every call site rewrites the bindings it uses with `vkUpdateDescriptorSets` right after the allocation, as before.
  Two sites write fewer bindings than the layout has (stencil import with a color source: 1 of 2; the guestspace
  depth resolve: 2 of 3); after a fresh allocation those bindings were undefined, now they hold an older valid
  descriptor. The shaders of those paths never declare them: `me_edram_color_to_depth.frag` and
  `me_edram_raw64_to_depth.frag` use binding 0 only, `me_depth_resolve_guestspace(_msaa2).comp` bindings 0-1.
- In reuse mode every set allocated from a pool is in its list (a pool whose list is empty, i.e. its first use, is
  still reset), so the k-th allocation of a recording fails exactly when it failed with resets (k > maxSets = 256),
  and `EnsureCapacityConversionEDRAM4` caps the conversions per slot below that anyway.
- With `masseffect_native_deferred_recording`, `vkResetDescriptorPool` was a drain point; `vkResetCommandPool` in the
  same `BeginRecording` drains anyway, so the queue behaves the same. `vkUpdateDescriptorSets` (queued or not) runs
  after the slot's fence in both modes.
- The switch is read at every `BeginRecording`; off resets the pools and empties the lists (as before).

Expected: the per-conversion `vk_object_zalloc`/pool allocation and the per-recording reset frees go away, about
2-4 us per conversion on the A57, ~0.15-0.3 ms per heavy frame. The draws side (`DrawsVulkanImpl`) already allocates
its sets once at start-up.

### H.3 Not changed

- `edram4_transfer_pairs_` (2-4 strings per transfer span): `masseffect_native_edram4_pair_keys = true` in
  docs/best-config.md already removes it.
- Gated by non-default cvars: the query maps (`masseffect_native_query_mode` 2/3), GPU labels
  (`masseffect_native_gpu_labels`), the `WrittenStencilEDRAM4` erase (`masseffect_native_edram4_stencil_known`).
- Bounded diagnostics (`LogVertexWordDiff`, the VS identity detail, missing-shader dumps, unpaired-draw lines): they
  run a fixed number of times per session.

### H.4 Expected savings and console check

| Change | Saves |
|---|---|
| `Diagnostics::reason` | one malloc/free pair per extent estimate (each extent-eligible draw, each proven rectangle) |
| `ProveRectangleList` | ~80 KB malloc + memcpy + free per multi-rectangle draw: ~10-20 us each on the A57 (the memcpy dominates), and the largest single source of heap fragmentation on this thread |
| Region vectors | one malloc/free per redirected clear candidate |
| Diagnostic maps | one malloc/free per counted decline / conversion / fetch / import whose key is longer than 15 chars |
| `edram4_draw_desc_` | one ~200-byte malloc/free per mode-4 draw (the most frequent string allocation left on the thread) |
| Rectangle entry, `entry_` reset | 2-4 allocations per rectangle-list draw / per input-cache miss |
| Sync spans, copy/clear region lists, stencil copy plan, batch list | 1-3 malloc/free per EDRAM transfer span / copy / import |
| `ForgetSynchronizedEDRAM4` | 8 KB + 32 KB free and re-allocation per restored target (about once per frame per swapped target) |
| Descriptor reuse (cvar) | 2-3 malloc/free per EDRAM conversion |

Together the profile's ~1.25-2.4 ms per frame of `_malloc_r` / `_free_r` / `_M_create` on the ring thread should
mostly go (the rest is in NVK command recording and texture creation). Console check: the same tour with and
without the branch (and then with `masseffect_native_edram_reuse_descriptor_sets = true`): the ring thread's
`_malloc_r` / `_free_r` / `_M_create` share in profile.log, ring CPU % and fps in the heavy Normandy and Feros
windows, `heap` largest-free-block lines (fragmentation), and captures, which must be identical. The 10 s report
lines (`declined by`, `late stencil fetches`, `9-pass`, `operations`, `bounded draw extent`) must have the same
text.
