# Cold-start hitches: where they come from and what removes them

Acceptance runs are cold (no caches) and must never drop below 25 fps. This document measures the long frames of
a first run, explains the pipeline machinery, and describes four changes (all behind cvars) with the log lines that
show them working and how to test them on the console.

## 1. The machinery today

| Piece | Where | What it does |
|---|---|---|
| `PipelineFor` | `masseffect_native_draws.cpp` | Ring thread. Looks the key up (last-key shortcut, direct-mapped cells, `pipelines_` map). On a miss it builds the shader modules (SPIR-V transforms: rectangle, VS constants fold, 7e3 clamp, alpha-only, early Z, texture signs fold, restore-into-7e3 epilogue, FragCoord, depth) and calls `CreatePipelineVulkan` **synchronously**: NAK compiles both stages unless the Vulkan cache has them. |
| Vulkan pipeline cache + prewarm list | one file, `<NRO folder>/cache/masseffect_native_pipelines.bin` (`NFPC` header, the `NFPL` list, then `vkGetPipelineCacheData`). With `masseffect_cold_startup = true` it is `cache/cold.bin` instead | Loaded at start-up. Saved after 64 new pipelines or 60 s with any new one: `vkGetPipelineCacheData` on the ring, then a writer thread writes the whole file (16 MB now). |
| List format version | `kVersionPipelinesList` = 4 (v3 added texture signs, v4 the folded VS constants) | A version change discards the list (not the Vulkan cache). |
| Prewarm thread `MASSEFFECT pipeline prewarm` | `TryPrewarm` / `PrewarmedLoop`, needs `masseffect_native_pipelines_prewarm = true` (default **false**) | Walks the list in creation order and creates + destroys each pipeline so the Vulkan cache holds it. Priority 0x3F..0x3B (the console only accepted 0x3B). Skips restore-into-7e3 records. |
| Shader preload thread `ME shader preload` | `TryPreloadShaders`, `masseffect_shaders_preload = true` | Reads from the SD the SPIR-V of every shader named in the prewarm list, so the ring does not read it on first use. With no list it preloads nothing. |
| Mesa disk cache | `sdmc:/.mesa/mesa_shader_cache_sf/` (single-file mode, `disk_cache_os.c` on `__SWITCH__`); disabled by `masseffect_cold_startup` (`MESA_SHADER_CACHE_DISABLE`) | Shader-level cache under the pipeline cache. |

## 2. Measurements (console logs, read-only)

Runs: `run/me1/vsfold_pd_b` (first run after the list went to v4: Vulkan cache present, 13 MB, but **list
discarded**, so nothing was prewarmed), `run/me1/vsfold2` and `run/me1/restore_r` (warm: list of 651, all prewarmed
in 1.5 s, 0 compiled).

| Run | frames > 60 ms | time in them | frames >= 500 ms | >= 1000 ms | worst |
|---|---|---|---|---|---|
| vsfold_pd_b (cold-ish) | 294 | 56.0 s | 17 | 9 | 4817, 4692, 2774, 1630, 1500 ms |
| vsfold2 (warm) | 246 | 23.6 s | 1 | 0 | 807 ms |
| restore_r (warm) | 251 | 25.6 s | 5 | 1 | 1817, 922 ms |

### 2.1 Pipeline compiles on the ring (cold only)

`C6 pipeline prewarm ... and N new (X ms average)` is cumulative. In vsfold_pd_b the ring created **356 new
pipelines, 40.6 ms on average = 14.5 s of ring time**, in bursts:

| window | new pipelines | ring time | hitch it produced |
|---|---|---|---|
| 22:57:08 - 22:57:19 (main menu -> load) | 24 | ~1.1 s | 2774 ms |
| 22:57:19 - 22:57:33 (first scene) | 46 | 3.6 s (78 ms each) | **4817 ms** |
| 22:59:55 - 23:00:26 (cutscene start) | 102 | 3.8 s | **4692 ms** + black frames |
| 23:00:26 - 23:00:36 | 52 | 2.1 s | 1351 ms, 600 ms |
| 23:00:36 - 23:00:46 | 35 | 0.3 s (cache hits) | - |
| 23:01:06 - 23:01:17 | 38 | ~1.5 s | 1630 ms, 1300 ms |

The `[hitch]` lines of these frames show `GPU 0.5 real, record 0.1-0.3` and only 50-70 ms of texture work: the
time is the synchronous `vkCreateGraphicsPipelines` in `PipelineFor` (it is not part of the `record` figure).
Per pipeline: 13 ms (cache hit for one stage) to 78 ms average in a burst; the code comments measured 68-159 ms for a
fully cold compile.

Folded variants: the logs cap at 32 module messages, and both caps were reached (`VS constants folded ... 32 modules`,
`texture signs folded ... 31 modules`), so at least 32 VS and 32 PS variants were created in the first 3 minutes.
SPIR-V patching itself is cheap: 2-3 ms per folded VS module (`value set #1` -> `folded` timestamps). The restore
into 7e3 epilogue adds 1-2 pipelines per run (10-20 ms each: never prewarmed, but cache hits).

### 2.2 The cache file write (cold and warm)

Every `C6: pipelines saved (... in 1610-2443 ms, from its thread)` is immediately followed by a long frame that ends
right after the write ends:

| Run | write | following frame (record) |
|---|---|---|
| restore_r | 1642 ms | 922 ms (244 ms) |
| restore_r | 2443 ms | **1817 ms (555 ms)** |
| vsfold_pd_b | 2200 / 2014 / 2015 / 1636 / 1610 / 2417 / 2369 ms | 910 / 547 / 317 / **1500 (728)** / 667 / 654 / 369 ms |

The frame starts during the write, so it is not `vkGetPipelineCacheData` on the ring (that happens before the
writer starts): the 16 MB file goes out in one `ofstream::write` (one long SD request), and the game's own reads
(and anything on the ring that touches the SD) wait behind it. In the warm runs these are the **worst frames of the
run**. A warm run rewrites the full 16 MB for 1-2 new pipelines (the restore-into-7e3 ones).

### 2.3 What is not the renderer

The 807 ms (vsfold2) / 817 ms (restore_r) frame ~19 s after start has `record 0.2`, 22 textures (9.3 MB, 65 ms)
and no pipelines: game loading. In the cold run the same moment is 2774 ms (the extra ~2 s are its 24 pipelines and
the first save). Texture creation is at most ~70 ms per long frame (58 textures, 14 MB in the 4817 ms frame).

## 3. Changes (all in `masseffect_native_draws.cpp`; the new code is in `masseffect_pipelines_cold_members.inc`, the cvars in `masseffect_pipelines_cold_cvars.inc`)

### A. Chunked, off-ring cache write (default ON: only changes how the cache file is written)

* `masseffect_native_pipelines_save_chunk_kb = 256`: the file is written in 256 KB pieces, flushed, with
  `masseffect_native_pipelines_save_pause_ms = 8` between pieces (no pause while shutting down). 0 = one write as
  before.
* `masseffect_native_pipelines_save_serialize_off_ring = true` (init only): `vkGetPipelineCacheData` runs on the
  writer thread (Mesa's pipeline cache is internally locked; a ring pipeline creation can wait for it, but plain
  draws never do). If the cache did not change size and the list did not change, nothing is written.
* Log: `C6: pipelines saved (16141 KB of cache and 653 in the prewarm list, in N ms, from its thread; 65 pieces;
  cache serialized on this thread in X ms)`. The write takes longer (~+0.5 s); what matters is that no frame after it
  is long.

### B. Specialized pipelines compiled in the background (code default OFF; `app/masseffect.toml` sets 2 since 2026-10-08)

`masseffect_native_pipelines_async_specialized` = 0 / 1 / 2. For a key with `kSpecVsFolded` or `kSpecSignsFolded`
that misses the map:

1. **Cache probe** (`masseffect_native_pipelines_async_probe = true`): the pipeline is created with
   `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` (Mesa's runtime returns `VK_PIPELINE_COMPILE_REQUIRED`
   instead of compiling; NVK reports `pipelineCreationCacheControl`). A hit (warm run) is used at once, as before.
2. Otherwise the **generic key** (`GenericKey`: fold bits cleared, `vs_values`, `vs_fold`, `signs_*` zeroed - exactly
   the key the draw builds with both foldings off) is looked up/created and the draw uses it. Mode 1 only defers when
   the generic pipeline already exists (no extra compile ever); mode 2 always (the generic one may be compiled on the
   ring once; every later value set of the same shader and state is then free).
3. The specialized pipeline goes to the thread `MASSEFFECT pipeline async`
   (`masseffect_native_pipelines_async_priority` = -1 -> 0x3F..0x3B, `masseffect_native_pipelines_async_core` = -1;
   -2 = a core other than the ring's), compiled with the same `CreatePipelineVulkan` and the same modules and render
   pass as the ring would use.
4. Once per frame the ring installs finished pipelines in the map (and in the prewarm list); from the next draw the
   specialized one is used. A background failure leaves the key on the generic pipeline for the session.

**Same image**: the folded module computes what the generic module computes from the values the draw uploads anyway
(the fold code's own contract: "the value folded is the one the shader would have read"); only the GPU saving is
postponed for the frames the compile takes. Not deferred: restore-into-7e3 (its epilogue is required, not an
optimization), and any key without fold bits.

Log every 10 s: `C6 async pipelines (mode 2): D specialized deferred, P pending, I installed, F failed, U duplicates;
background compile X ms average | N draws with the generic pipeline | cache probe: A asked, H hits, E errors (Y ms
average) | generic pipelines asked on the ring Z ms in total; G not deferred for lack of a generic one (mode 1)`, and
once `C6 async pipelines: thread at priority 0x3b; preferred core ...`. In the prewarm report the installed ones count
as `new` with 0 ms. `E errors` > 0 means the driver ignored the flag (then nothing is deferred: as before).

Limits: in a fully cold run the generic pipeline usually does not exist yet, so mode 1 rarely fires and mode 2 still
pays one compile per distinct generic key; it removes the extra compiles that folding added (value sets 2-4 of a
skinned VS, each sign set of a PS), not the baseline ones. The baseline is handled by C and D.

### C. Shipped prewarm list (default ON since 2026-10-08, shipped with every install)

`masseffect_native_pipelines_shipped_list = "masseffect_prewarm_list.bin"` (relative to the NRO folder, init only;
set in `app/masseffect.toml` together with `masseffect_native_pipelines_prewarm_threads = 2` and
`masseffect_native_pipelines_async_specialized = 2`). A missing file is one warning (`C6 prewarm: shipped list ... not
found or empty`); a list of another `kVersionPipelinesList` is ignored with a warning.
At start-up its records that the normal list lacks are appended (`C6 prewarm: shipped list ...: N records, A added,
P already in the list, R rejected`). It is the `NFPL` part of `cache/masseffect_native_pipelines.bin`: pipeline keys,
vertex formats, shader fingerprints - our data, no game data and no compiled code. It also feeds the shader preload
thread, which otherwise has nothing to read in a cold run. Needs `masseffect_native_pipelines_prewarm = true`.

Make it after a long warm console run that went through every location:

```
curl ... -o /tmp/p.bin ftp://SWITCH/switch/masseffect-nx/cache/masseffect_native_pipelines.bin
python3 tools/extract_prewarm_list.py /tmp/p.bin masseffect_prewarm_list.bin [more_runs.bin ...]
# upload masseffect_prewarm_list.bin next to masseffect-nx.nro (NOT into cache/)
```

It has to be regenerated whenever `kVersionPipelinesList` changes (a stale one is ignored with a warning). It no longer
has to be regenerated when the shader package is rebuilt, only when shaders are removed from it:

**Package-independent at load (2026-10-09).** `key.vs` / `key.ps` in each record are shader package entry numbers + 1,
which shift on every package rebuild; the `vs` / `ps` fingerprints (XXH3 of the guest container) do not. Before this
fix the ring matched its keys against the list by hashing the whole key, so with stale numbers nothing matched: every
ring pipeline counted as new and was appended again, the list filled up to the record cap and stopped recording, and
the `MISMATCH` guard and the "from the list" counters were meaningless (of the 3023-record list only 99 records had
current numbers, `tools/prewarm_list_coverage.py`). Now `RenumberPipelinesList` runs once on the ring as soon as the
shader library is loaded - before the shader preload and prewarm threads start (`TryPrewarm` calls it first) and
before the ring's first `NotePipelineCreated` (which also calls it) - and rewrites every record's numbers from its
fingerprints (`PerFingerprint`, the same lookup and numbering as `PipelineFor`), drops records whose shaders are not in
the package and records that become identical to an earlier one, and rebuilds the key index. One log line:
`C6 prewarm: list renumbered: N records, R renumbered, M without shaders, D duplicates dropped (K kept)`. The
renumbered list is what the next cache save writes to `cache/masseffect_native_pipelines.bin`. The record cap
(`kMaxRegistersList`) went from 4096 to 8192 (448 bytes each: 3.6 MB per copy).

#### Shipping

* **One list per edition.** The records hold fingerprints of the edition's own shader package, so the RU and EN lists
  differ. Sources: `app/prewarm/masseffect_prewarm_list-ru.bin`, `app/prewarm/masseffect_prewarm_list-en.bin` (the
  RU list is the 1956-record list of build ru_glob12, best-config.md; the EN one still has to be made on an EN console
  run). Regenerate the edition's file with `tools/extract_prewarm_list.py` after a long all-locations run whenever the
  shader package, the pipeline key or `kVersionPipelinesList` changes, and replace it in `app/prewarm/`.
* **Release.** `.github/workflows/build.yml` (and `tools/publish_release.sh`) attach `app/prewarm/*.bin` to the
  release under those names; `release.yml` lists them in `SHA256SUMS`; the starter zip carries the EN list as
  `switch/masseffect-nx/masseffect_prewarm_list.bin`.
* **Installer** (`installer/config.js` `editions[].prewarmList`): downloads the edition's list from the site next to
  the NRO and the toml, checks it against `releases/manifest.json` like the NRO, and writes it next to the NRO as
  `masseffect_prewarm_list.bin` in the full zip, the update zip and the NSP RomFS. A release without it installs
  without it (warning in the log only).
* **Installed NSP.** The list is a RomFS file (`tools/build_full_nsp.py` and `installer/js/nsp.js` take it from the
  input folder when present). Packaged mode resolves a relative `masseffect_native_pipelines_shipped_list` to
  `<data_dir>/<name>` when that file exists, else `romfs:/<name>` (`me::packaged::ShippedFile`, `app/src/me_packaged.cpp`).
  A normal update NSP carries the new release's list; a program-only update keeps the base's (copy a new list into the
  data dir to override it). See full-nsp.md sections 2 and 8.

### D. More prewarm threads (code default 1; `app/masseffect.toml` sets 2 since 2026-10-08)

`masseffect_native_pipelines_prewarm_threads` = 1-3 (init only). Every thread claims the next record (`prewarm_next_`),
writes its state, and the records done are published in order (`PrewarmPublish`), so the ring's guard and the list
saving read the same states as before. Extra threads prefer a core other than the ring's.
`masseffect_native_pipelines_prewarm_priority` = -1 (0x3F..0x3B as before) or an explicit value. With a 650-record
list and ~60 ms per cold compile, one thread needs ~40 s; two ~20 s.

## 4. Testing on the console

Cold = delete, before the run:

* `/switch/masseffect-nx/cache/masseffect_native_pipelines.bin` (Vulkan cache + list), and `.tmp` if present;
* `/switch/masseffect-nx/cache/vfs_index_game_root.bin` (what `tools/me1_ab.sh` with `COLD=1` already deletes);
* **`/.mesa/` (the Mesa shader disk cache, `sdmc:/.mesa/mesa_shader_cache_sf/`)** - `me1_ab.sh` does not delete it
  today, so a "cold" run there can still hit Mesa's shader cache. Alternatively set `masseffect_cold_startup = true`,
  which disables the Mesa cache and uses `cache/cold.bin` (delete that file).
* Keep `masseffect_prewarm_list.bin` next to the NRO: it is part of the build, not a cache (the installer puts it
  there). To measure a truly list-less cold start, delete it or set `masseffect_native_pipelines_shipped_list = ""`.

Suggested runs (one variable at a time, same route; note the worst frame, frames >= 500 ms and the sum of `[hitch]`
frames):

1. Baseline cold, new NRO, defaults (only A is new): the frames after `pipelines saved` should be gone.
2. Cold + `masseffect_native_pipelines_async_specialized = 2`: expect `async pipelines` lines, fewer/lower hitches in
   the cutscene burst, identical screenshots.
3. Cold + shipped list + `masseffect_native_pipelines_prewarm = true` (+ `masseffect_native_pipelines_prewarm_threads
   = 2`): expect `shipped list ... added`, `U1 shader preload ... N entries`, and in the prewarm report most ring
   creations as `from the list already prewarmed`.
4. All of them together, cold, then the same NRO warm (to check that the warm worst frame no longer comes from saves).

Lines to grep: `[hitch] frame of`, `C6 pipeline prewarm:`, `C6 prewarm:`, `C6 async pipelines`, `C6: pipelines saved`,
`U1 shader preload`.

## 5. Ideas not implemented

* Prewarm the restore-into-7e3 records (1-2 per run, 10-20 ms each).
* Save the cache only at long frames already lost (loading screens) or on HOME/exit.
* Per-pipeline creation time in the `[hitch]` line (`masseffect_native_targets.cpp`, edited by another task).
* A NAK binary cache shipped with the build would remove compiles entirely, but it is derived from game shaders.
