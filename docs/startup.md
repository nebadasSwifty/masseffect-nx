# Start-up: where the time goes, ranked options, and the read preload

Tags: **[V]** = verified by data in the console logs this analysis read, **[G]** = guess or estimate, needs a console run.
The analysis is log and source reading only. The logs are those of a warm run (pipeline and Mesa caches present) with
the logo-free `Coalesced.ini`, driven by the test bot's route (see [measuring.md](measuring.md)); the ids in the
[optimization catalogue](optimization-paths.md) (L2, E32, U1-U3) are the ones used below. Source files:
`sdk/src/filesystem/devices/host_path_{index,file}.cpp`, `sdk/src/core/logging.cpp`.

## 1. Timeline, t311 (warm pipeline/Mesa caches, seconds since the first log line)

| t (s) | Event | Source |
|---|---|---|
| 0.0-0.2 | NVK instance/device, swapchain (the NRO load itself before the first log line is not measured) | [V] |
| 0.4 | VFS mount 4523 entries: **204 ms** (index 8 ms read + 78 ms tree + 119 ms validating 13 dirs) | [V] |
| 0.5-1.6 | Shader library: index only, **1.1 s** (30129 shaders; `.mesp.idx` is 48.5 MB on disk) then microcode index 0.1 s. Serial, before the guest starts | [V] |
| 1.7-2.5 | Guest start, Coalesced opened (2.5), audio init (2.5, no stall visible) | [V] |
| 2.6-2.9 | `GLO_Relay_LOAD.bik` read, 3.3 MB, 0.3 s of slow reads (the loading movie, not a logo) | [V] |
| 3.0-5.7 | MEInit packages Core/Engine/GameFramework/UnrealScriptTest/BIOC_Base; Engine.xxx ~6 MB in 128 KB reads | [V] |
| ~4.0 | Pipeline cache file read + `vkCreatePipelineCache` (77 MB, 1765 pipelines); the ring thread logs nothing for 1.5 s before "cache active" | [V] gap, [G] it is mostly the 77 MB read |
| 4.2-7.2 | Prewarm thread (background, 1765 pipelines, 16 really compiled = 122 ms, rest Mesa cache hits); U1 shader prefetch 540 entries 13.6 MB in 2.7 s on its own thread | [V] |
| **5.7-14.3** | **No slow I/O at all, 8.6 s.** Game thread `XThreadD1BB0D60` at 90.7 % CPU (one core), rest of the process ~30 % | [V] |
| 14.3 | `EntryMenu.xxx` opened; the 11.7-21.7 window already has 13268 draws, so the title is visible at ~15-16 s | [V] |
| 16-22 | Layer1 character/weapon packages (UI world preload); several 20-60 ms opens | [V] |
| 30.8 | `state.sav` opened = bot pressed Resume (title -> menu -> resume is bot-paced; includes the bot's screen waits) | [V], latency share unknown |
| 31-40 | Map load: ~25 BIOA_PRO10_* files, 8-65 ms per open, then Layer1 textures | [V] |
| ~44 | Route playback starts (in game) | [V] |

Comparison: t203 (U1/U2 only): EntryMenu 18.6 s, save 37 s. t152 cold, before U1/U2: EntryMenu 40.6 s; cold vs warm
did not move the EntryMenu time, but the first title window had 80 swaps against ~151 warm [V] (compile stalls).

### I/O weight (the `[io] 15 s:` lines) [V]
| window ending | reads | MB | time in read() | opens |
|---|---|---|---|---|
| 17.6 s | 608 | 73.6 | 2.24 s | 27 / 150 ms |
| 32.6 s | 718 | 64.0 | 1.79 s | 58 / 417 ms |
| 48.2 s | 809 | 97.1 | 2.95 s | 62 / 673 ms |

About 7 s of reads + 1.2 s of opens are blocking the guest thread before gameplay (~19 % of 44 s), ~33 MB/s average.
The SDK's own fit (host_path_file.cpp) is 2.3 ms fixed per call + 69.5 MB/s, which reproduces phase 1 (608 calls x
2.3 ms = 1.4 s + 73.6 MB / 69.5 = 1.06 s). So over half of the read time is per-call overhead of 128 KB reads.
Ranges: 95 % of phase-1 reads are sequential (36 % in phase 2); 6-10 % are re-reads ("reread", 3.5-10 MB, ~0.3 s).
The read-ahead window never works for this game: `window: 0 hits / 1 filled / 605 direct`. Cause: the entry
threshold is window/4 = 64 KB (`kSubmissionMaxFraction`), but UE3 reads exactly 128 KB (compressed chunk size), and
`masseffect_io_ranges_min_kb` = 256 keeps all 605 reads out of the range cache ("605 below the floor of 256 KB").

### CPU weight [V]
Profile of the game thread in the 8.6 s window: flat. Top: `sub_82AC4AF0` 6.1 % (already hot-replaced), `sub_8239A8A0` 4.2 %,
`sub_8231D3B8` 3.7 %, `sub_824B3910` 3.6 %, `BaseHeap::AllocRange` 3.2 %, `sub_82211610` 2.5 %, `__aarch64_read_tp` 1.9 %.
No function above 6.5 %. A worker thread at 7 % CPU spends 92 % in `__fast_sub_827D2A00`; in the map-load window a
worker is at 30 % CPU with 60 % in three `82D8xxxx` functions (probably the package chunk decompression) [G for what
they are]. The prewarm thread spends its time in `_free_r` and `blake3_hash_many_neon` (Mesa cache keys).

### Logging [V]
Not a bottleneck: 298 lines / 40 KB in the first 5 s, ~100 KB per 5 s later; SDK already flushes once per second
(`flush_level` warn + `log_flush_interval` 1) and `log_async = true` is set in the toml (the SDK comment warns it
hurt fps earlier; not touched here). One burst: 4536 lines / 536 KB of `[seqrec]` in 40-45 s (map load).
`profile.log` is written every 10 s from the profiler, 1.7 MB per 170 s. The reference-port idea (RAM buffer, flush 0.5 s)
is already mostly done; expected gain < 0.2 s.

## 2. Ranked options

Seconds are for warm start unless said. "Hidden" means moved off the guest thread, not removed.

| # | Change | Expected | Effort | Risk | Status |
|---|---|---|---|---|---|
| 1 | **Cold start: hide pipeline compilation in the 8.6 s CPU-bound init window.** Prewarm thread only needs a key list; with a shipped list (E32c) the 2 free cores compile ~100+ pipelines (81 ms each, E32a) while the guest initialises. Without a list nothing can be done before the first draws. Open question: does a shipped list count as a cold start? | cold: up to ~10 s of compile stalls off the title/menu/map path; catalogue item E32 estimates 16-27 s total ring-thread compile | medium | low (image unchanged) | needs a decision; numbers from the catalogue (runs t115, t116, t120), [G] for the split |
| 2 | **Background prefetch of the startup read set**: record `(file, offset, len)` of the first ~48 s into a trace file, replay it on a low-priority thread into a read-once buffer (evict after use, ~32 MB cap) so the guest read is a memcpy. Also lets the range cache accept 128 KB reads | 3-7 s (the whole blocking I/O of 7 s + opens; upper bound, SD shared with U1 prefetch) | medium | medium (RAM near the 3189 MB process limit, wrong trace = wasted reads) | IMPLEMENTED (section 4), Switch result pending; the I/O share itself is [V] |
| 3 | Cheap first step of #2: window size `masseffect_io_window_kb` 256 -> 1024 so 128 KB reads pass the window/4 rule, with the 95 % sequential phase-1 reads this removes ~7/8 of the calls | ~1.0-1.2 s in phase 1, ~0.5 s later (from the 2.3 ms/call fit) | trivial (toml) | low | [G]; try first, measure the `window` hits |
| 4 | Read the 77 MB pipeline cache file on a thread started at process start (overlaps the 1.1 s library index and the guest init); create the Vulkan cache when ready | <= 1.5 s if the ring thread / first Swap is on the critical path | low-medium | low | [G] (gap is [V], dependency is not) |
| 5 | Shader index: 48.5 MB read in 1.1 s on the serial path. Store microcode as hashes only (fetch bytes lazily), or load the index on a thread while the guest starts | ~0.7-1.0 s | medium (format change, regenerate `.mesp.idx`) | low | [G] |
| 6 | Guest CPU: hot-replace the next top functions (`sub_8239A8A0`, `sub_8231D3B8`, `sub_824B3910`), cheaper `AllocRange`/`read_tp` | ~0.5-1 s of the 8.6 s (flat profile) | high per function | medium (guard needed) | [V] profile, [G] gain |
| 7 | Cut `[seqrec]` spam and other startup-time diagnostics from the default config | < 0.2 s | trivial | none | [V] volume |
| 8 | Stub the 0.3 s `GLO_Relay_LOAD.bik` loading movie, and for the original Coalesced serve a tiny valid `.bik` for the logo movies in the VFS instead of editing Coalesced | 0.3 s now; logo movies unmeasured (no log with movies in this set) | medium | medium (Bink player expectations) | [V] 0.3 s, rest [G] |
| 9 | Mesa disk cache on SD: already on for warm runs (16 real compiles of 1765). Cold acceptance disables it by design, so no gain there; keep it for the shipped product | 0 for cold | none | none | [V] |
| 10 | Multi-threaded file I/O by the guest (U3 "larger blocks") | covered by #2/#3; the guest itself issues one blocking read at a time | - | - | - |

Rejected on the data: deferring XMA/PhysX init (audio init at 2.5 s shows no stall; PhysX only logs a dropped
warning), lazy 930 MB package (U1 already loads only the index; 0 MB resident).

Suggested order: #3 (one toml line, measure) -> #7 -> #4/#5 (independent threads) -> #2 -> decision on #1.
Realistic warm result: title ~16 s -> ~12-13 s, map load ~13 s -> ~9-10 s [G]. The 8.6 s CPU-bound guest init is
the floor until the guest code itself gets faster (#6).

## 3. Another game on the same SDK

The VFS index, the window and range caches, the pipeline cache and the shader package with its index are shared plumbing, so
options #3, #4 and #5 carry over unchanged. A first start without a VFS index pays a full scan of the game folder (the cost
grows with the number of files and their directory layout); the index fixes every later start. Record the read trace
of section 4 on the first good run of the other game: the trace path is per game folder.

## 4. Start-up read trace and background preload (item 2, implemented in `sdk/src/filesystem/devices/startup_trace.cpp`)

Status: it works on the console (the record thread needed the fix below); the preload reads about 224 MB in 4.7 s off the critical
path and shortens the start by about 0.5 s. Both modes are off by default.

### Idea
The guest reads the same files in the same order on every start. A RECORD run logs `(file, offset, length)` of every
guest read (and every open) of the read-only game root. A PRELOAD run parses that trace right after the VFS mount and a
low-priority thread replays it ahead of the guest: adjacent reads of one file are merged into one SD read (up to 1 MB,
so 8 x 128 KB become one call), sliced back into the exact ranges the guest will ask for and kept in a bounded RAM
store. When the guest asks for exactly one of them, the read is a memcpy. The thread also opens the files ahead of
the guest and parks the handles; `HostPathEntry::Open` takes a parked handle instead of paying the open.

### Cvars (all `Filesystem`)
| cvar | default | meaning |
|---|---|---|
| `masseffect_io_trace_record` | false | RECORD mode. When on, `masseffect_io_preload` is ignored |
| `masseffect_io_trace_record_s` | 50 | seconds after the mount at which recording stops and the final trace is written (a snapshot is also written every 15 s, so a killed run still leaves a trace) |
| `masseffect_io_trace_file` | "" | trace path; empty = `<parent of the game folder>/cache/startup_reads.bin`, i.e. `sdmc:/switch/masseffect-nx/cache/startup_reads.bin` |
| `masseffect_io_preload` | false | PRELOAD mode |
| `masseffect_io_preload_mb` | 64 | hard cap of the store (resident slices plus the read in flight) |
| `masseffect_io_preload_max_range_kb` | 4096 | trace reads larger than this are left to the guest |
| `masseffect_io_preload_coalesce_kb` | 1024 | max size of one merged SD read (0 = one call per trace read) |
| `masseffect_io_preload_opens` | 32 | max pre-opened read handles (0 = do not pre-open) |
| `masseffect_io_preload_check_mtime` | true | besides the size, require the recorded write time |
| `masseffect_io_preload_priority` / `_core` | 58 / -1 | Horizon priority (game threads run at 44 and below, 59 is the lowest allowed) and preferred core (-1 = kernel decides). The thread is not pinned |
| `masseffect_io_trace_markers` | `coalesced.ini,engine.xxx,bioc_base,entrymenu,bioa_pro10` | substrings (case-insensitive); the guest's first read of the first file matching each is logged as a `[io] timeline` line |
| `masseffect_io_preload_verify` | false | test aid: every hit is also read from the SD and compared (never for timing) |

### Trace file format (version 1, little endian, packed)
```
char[8] "RXSTRC01" | u32 version=1 | u32 file_count | u32 event_count | u32 flags=0
file_count x { u16 path_len; char path[]; u64 size; u64 write_timestamp }   // guest path (Entry::path()), first-open order
event_count x 24 bytes { u8 kind (0 read, 1 open); u8 pad; u16 thread (hash); u32 file; u64 offset; u32 length; u32 t_us (since mount) }
u64 FNV-1a of everything above
```
About 27 bytes per event; the Mac ME1 trace is 1117 events, 59 files, 30 KB.

### Safety rules
* Only the first read-only device that mounts (the game root) and only files opened without write access: saves and
  profiles (writable devices) are never recorded, preloaded or pre-opened.
* A file is used only if its size and write time on the SD equal the recorded ones (checked by the preload thread,
  one stat per file); otherwise that file's ranges are skipped and a line says so. NOTE: a test cycle that uploads
  `Coalesced.ini` on every run changes its write time, so that one 100 KB file always shows as skipped; harmless.
* Missing, truncated or corrupt trace (checksum) = one log line `[io] preload: disabled, ...`, the game reads as before.
* A range is served only on an exact `(file, offset, length)` match with the bytes the SD returned. A range that is
  not ready when the guest asks is marked skipped (the guest reads the SD itself); one being read right now is
  waited for (max 250 ms). Slices are freed after their last recorded use; slices far behind the guest are dropped.
* SD error: the range is skipped; 4 errors in a row stop the thread. Out of memory: same.

### Log lines
```
[io] preload: trace ... loaded in 0.2 ms: 1117 events, 59 files, 987 distinct ranges (123.3 MB planned, ...), 441 SD steps, cap 64 MB
[io] preload progress|final: t=.. s, SD: N calls, X MB in S s; guest: M ranges matched (H hits = Y MB from RAM, late, waited, other),
      U reads not in the trace; store Z MB (peak, cap), dropped, handles pooled/taken, verified (mismatches)
[io] timeline: 2.49 s after the VFS mount, guest first reads 'Layer0\Maps\EntryMenu.xxx' (marker 'entrymenu'; 381 reads, 47.5 MB read so far; preload running)
[io] trace: snapshot|final: 1117 events, 59 files -> <path>
```
`late` = the guest asked before the thread had read it (the thread then skips it); `other` = skipped, dropped or already
used. `reads not in the trace` are guest reads with no recorded counterpart (different order, new files).
Compare the `timeline` seconds of a record run (no preload) against a preload run; they are measured from the VFS mount.

### Host proof (macOS, no-movies title run)
Record: 1117 events / 59 files in 40 s. Preload with the same trace: 987 distinct ranges (123 MB) read in 428 SD
calls instead of 987; guest at t=30 s: 998 of 1006 matched ranges served from RAM, 0 late; 50 of 50 handles
taken. With `masseffect_io_preload_verify=true`: 1045 hits compared with the SD, 0 mismatches. Missing trace, truncated trace
and a trace whose recorded size differs each fell back with the expected log line. (The Mac SD is the page cache, so
no time gain is visible there; the proof is functional.)

### How to refresh the trace
Run once with `masseffect_io_trace_record = true` in `masseffect.toml` through the same route as the measurement, then run with
`masseffect_io_preload = true`. Refresh the trace after the game files change (the stale check says so) and after a change that
alters the start-up reads (a different `Coalesced.ini`, another shader package). Record runs are slightly slower (a lock per
read): never use one for timing.

```
tools/console-test/switch_cycle.sh start-rec --toml record.toml      # record.toml = masseffect.toml + masseffect_io_trace_record = true
tools/console-test/switch_cycle.sh start-pre --toml preload.toml     # preload.toml = masseffect.toml + masseffect_io_preload = true
```
Read in the downloaded `game.log`: the `[io] timeline` lines against the record run, the `[io] preload final` line and the
`[io] 15 s:` lines (the reads and the time in read() should collapse in the first windows).

### Switch note: worker threads
The first console record run hung right after `[io] trace: recording ...`. Cause: the workers were started with
`std::thread(...).detach()`, which throws `std::system_error` on the Switch (same trap as in `audio/xma_context.cpp`).
Workers are now created once, never detached or joined (the `std::thread` object is leaked), and the log shows
`[io] trace: stage: creating thread` / `thread ... is running` / `recording, t=.. s, N events` (every 5 s) so a stall is visible.
