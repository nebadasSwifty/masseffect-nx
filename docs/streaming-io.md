# Streaming I/O: who reads the packages, what is reread, and the block cache

Mass Effect streams packages while you play: UE3 async loading of streamed levels and, above all, texture
streaming (mips go out of memory and come back in as the camera moves). On the Switch every one of those reads is
a `pread()` on the SD card. This page records what the console logs show about those reads (2026-10-07/08, Russian
edition, manual runs `run/me1/manual1..7`) and describes the block cache added for them (`masseffect_io_bcache_*`,
**off by default**).

Code: `sdk/src/filesystem/devices/block_cache.cpp` (cache), `sdk/src/filesystem/devices/host_path_file.cpp`
(`HostPathFile::ReadSync` calls it first), `sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp` (the `[io]` summary).
Offline simulator: `tools/io_cache_sim.py`.

## 1. Which thread reads, and who waits for it

Stack samples (`manual2/rex_profile.log` with `ru_loc2.elf`, `manual3/rex_profile.log`), symbolized with `nm` of the
same ELF:

- **Every package read during gameplay comes from one guest thread**, the UE3 async IO thread: `sub_82816B78`
  (thread entry) -> `sub_8239E260` -> `sub_8231C7B0` (request loop) -> `sub_826DC260` -> `sub_82812398` ->
  `NtReadFile`. It is `XThread652E8C40` in manual2 and `XThread90CC14A0` in manual3 (priority 0x3B, any core). In
  the console log it is one host thread id (`t13941159435809999279` in `masseffect_309.log`) for all package reads,
  `music_bank.isb` included; only the Bink logo movie is read by another thread.
- When it is idle it waits in `NtWaitForSingleObjectEx` (via `sub_8239D928`); between pieces it sometimes sleeps in
  `KeDelayExecutionThread` (`sub_8231BD38` -> `sub_82813320`). In the snapshots where it is listed it spends
  **13-25 % of its samples inside `pread`** and **60-81 % idle**: the SD is not saturated, requests come in bursts.
- `NtReadFile` is synchronous (the `true ||` in `NtReadFile_entry`; see the comment there), but that only blocks this
  IO thread, which is the thread UE3 made for that purpose.
- **The game thread waits for reads only while loading.** Its guest-side wait is a sleep loop
  `sub_8231D408` <- `sub_8239B100` <- `sub_8230BCF8` (called from package/linker code such as `sub_82399F58`). It is
  43-53 % of the game thread's samples inside hitches in the first (loading) snapshot, then 0 % in most gameplay
  snapshots and at most 5-9.5 % (manual2 at 40 s, manual3 at 110 s: streamed level loads such as
  `BIOA_PRO10_08_LAY.xxx`).
- **Inside gameplay hitches the game thread is almost always waiting for the render thread**:
  `FRenderCommandFence::Wait` (`sub_822FE560`, see cpu-cost-analysis.md) is 40-98 % of its hitch samples in every
  manual2/manual3 snapshot after loading.

Correlation of hitches with slow reads (every `[hitch] frame of N ms` >= 150 ms after the first 60 s, against the
`[io] SLOW: read` lines; "random" = chance that a random window of the same mean length contains a slow read):

| Log | Hitches | With a slow read inside | Random window |
|---|---|---|---|
| manual1/s303 | 10 | 60 % | 17 % |
| manual1/m301 | 27 | 26 % | 8 % |
| manual3/305 | 12 | 17 % | 11 % |
| manual6/308 | 5 | 60 % | 31 % |
| manual7/309 | 16 | 56 % | 33 % |

So hitches and streaming reads do happen together more often than chance, but the stacks say the game thread is not
waiting for the SD in those frames: streaming is a common cause (a new area brings both reads and a burst of texture
creation, e.g. `[hitch] ring: textures 66.0 ms (22 uploads, 9.3 MB; 22 created)` in the 1.04 s hitch of
`masseffect_309.log` at 08:31:03), not the blocking step. The worst hitch of that log (1.2 s at +114 s) has no slow read
at all. **Expect the cache to make streaming finish sooner (less time on low-res textures, music reads not queued
behind texture reads, shorter streamed-level loads), not to remove the big hitches by itself.**

Side finding: with the synchronous logger (`log_nonblocking = false`, the default) each `[io] SLOW` warning flushes the
log file from inside `NtReadFile` on the IO thread (`spdlog::logger::flush_` <- `NtReadFile_entry`, up to 12 % of that
thread's samples in a snapshot). The warnings stop after 300 per session (`g_io_warnings`), so later reads are not in
the log at all.

## 2. The read pattern

From all `[io] SLOW: read` lines of the manual logs (1,314 reads, the ones over 8 ms) and the recorded start-up trace
(`run/trace-test/startup_reads.bin`, every read, 1,058 reads):

- **Size**: 1,184 of the 1,314 logged slow reads are exactly 131,072 bytes; the rest are the tails of a request (120,063,
  77,962, ...). The trace: 1,056 of 1,058 reads are 128 KB. UE3's IO thread cuts every request into 128 KB pieces.
- **Alignment**: every offset is a multiple of 32 KB (1,313 of 1,314; the exception is a save file), only 32 % are
  multiples of 128 KB. 32 KB is the Xbox 360 DVD ECC block.
- **Sequential**: in the start-up trace 66 % of reads start exactly where the previous read of the same file ended,
  and 5 % overlap the previous read at a different 32 KB-aligned start (11 % of the 32 KB blocks touched were already
  touched). Overlaps cannot be served by an exact-range cache.
- **Rereads** (`[io] ranges:` line: same file, offset and size already read in the session):

| Log | Reads | MB | Time in read() | Reread (all) | After the first 60 s: MB, reread, unique |
|---|---|---|---|---|---|
| manual1/s303 (13 min) | 11,157 | 1,310 | 45.4 s | 58 % | 1,023 MB, **69 %**, 320 MB |
| manual1/m301 | 7,041 | 846 | 24.1 s | 47 % | 702 MB, 55 %, 316 MB |
| manual3/305 | 3,968 | 481 | 12.5 s | 24 % | 294 MB, 33 %, 196 MB |
| manual4/306 | 3,660 | 405 | 13.3 s | 29 % | 123 MB, 58 %, 52 MB |
| manual7/309 | 3,834 | 426 | 14.0 s | 32 % | 177 MB, **57 %**, 76 MB |

  In single 15 s windows of gameplay it reaches 81-84 % (e.g. `169 reads 20.0 MB in 706.0 ms ... 16.5 MB already read
  before`). Most-read files: `BIOA_PRO10_T.xxx`, `BIOG_V_Z_TEXTURES_A_Z.xxx`, `BIOG_HMM_HED_PROMorph.xxx`,
  `BIOG_HMM_HIR_PRO_R.xxx`, `BIOG_HMF_HED_PROMorph_R.xxx`, `BIOG_HMF_HIR_PRO.xxx` (texture packages: heads, hair, level
  textures).
- **SD speed**: 3.1-5.8 ms per 128 KB read on average (22-40 MB/s inside `read()`), median of the slow ones 11.2 ms,
  p90 59.7 ms, worst 78 ms. The model measured on large reads, 2.3 ms per call + 69.5 MB/s, gives 4.1 ms per 128 KB:
  more than half of each read is per-call cost, which is what coalescing removes.

### Why the existing caches get 0 hits

- **Window** (`masseffect_io_window_kb = 256`): only requests of at most window/4 = 64 KB go into it. Every streaming
  read is 128 KB, so all of them are `direct` (`window: 0 hits / 0 fills / 169 direct`).
- **Range cache** (`masseffect_io_ranges_mb = 64`, floor `masseffect_io_ranges_min_kb = 256`): it was sized for the
  previous game's 0.14-2 MB rereads. Every 128 KB read is below its floor (`not cached: 2006 below the floor of 256
  KB`), and it would also miss the overlapping reads.
- **Old block cache** (`masseffect_io_cache_mb`, off): 256 KB blocks against reads 1 % aligned to 256 KB read x3.3 the
  bytes asked for, and its single mutex was held across the `pread`.

## 3. The block cache

All settings are read once, when the first read-only file is opened; changing them needs a restart.

| Setting | Default | Meaning |
|---|---|---|
| `masseffect_io_bcache_mb` | 0 (off) | Cap of the cache in MB. RAM is taken in 4 MB slabs only as blocks are filled, from the host heap (~1.0-1.2 GB never taken in `rex_profile.log`). If an allocation fails the cache stops growing there (logged) and keeps working. |
| `masseffect_io_bcache_block_kb` | 32 | Block size. 32 KB = the game's own alignment, so a demand miss reads at most the request rounded up to 32 KB (only request tails round up). Larger blocks over-read. |
| `masseffect_io_bcache_max_request_kb` | 1024 | Larger reads bypass the cache (go to the window / range cache / SD as before). |
| `masseffect_io_bcache_readahead_kb` | 0 (off) | Read-ahead after a request that ends on a block boundary and is at least `_readahead_min_kb` (64 KB): a 128 KB piece of a longer request triggers it, the short tail of a request does not. Synchronous: the extra blocks are appended to the SD read of the miss (one call instead of several). |
| `masseffect_io_bcache_readahead_min_kb` | 64 | See above. |
| `masseffect_io_bcache_adaptive` | true | Halves the read-ahead when fewer than 30 % of the read-ahead blocks of the last window (512 issued) got hit; doubles it back (up to the setting) above 60 %. At 0 it is probed again (a quarter of the setting) after 2,048 missed blocks. Each change is logged. |
| `masseffect_io_bcache_async` | false | Switch only. The read-ahead is queued (max 8 jobs) to a dedicated prefetch thread (`io prefetch`, libnx `threadCreate`, cores 0-1, priority `_async_priority` = 44) and done after the guest's read returns. The thread opens its own handles (max 8): newlib's `pread` on Horizon is lseek+read+lseek, so a handle is never shared between threads. A guest read that needs a block of a job that has not started cancels the job and reads itself; a block of a running job is waited for (`_wait_ms`, 100 ms, then the guest reads it itself). |
| `masseffect_io_bcache_wait_ms` | 100 | See above (also used when two guest threads miss the same block). |
| `masseffect_io_bcache_skip` | `.bik` | Comma-separated, case-insensitive substrings of guest paths that never use the cache (movies are read once). |
| `masseffect_io_bcache_verify_n` | 512 | The first N reads the cache answers are also read straight from the SD and compared twice: the bytes the cache assembled, and the bytes found in the guest's buffer after the write. Any difference logs `[io] block cache: DIFFERENCE (...)` with file, offset, first differing byte and values, turns the cache off for the session and lets the usual path redo that read. -1 = every read, 0 = never. |
| `masseffect_io_bcache_checksum` | true | Every block is hashed when stored and checked on every hit; a block whose RAM changed after it was stored is a `DIFFERENCE (cache RAM)` (with the host address) and turns the cache off. |
| `masseffect_io_bcache_guest_write` | 1 | Switch: the answer is written into guest memory through the always-mapped shadow alias, page by page, so the copy never takes a fault (the direct path's bytes are written by the FS/kernel, not by a faulting user memcpy). `ReadInternal` still triggers the physical write callbacks afterwards. 0 = plain memcpy to the guest pointer. |
| `masseffect_io_bcache_arena` | true | The whole cap is taken in one allocation at the first file open, so cache blocks can never be heap memory that was guest backing earlier (see the incident below). Falls back to 4 MB slabs on demand if the allocation fails. |
| `masseffect_io_bcache_ghost` | false | Metadata-only LRU simulation of 32/64/128/256/512 MB caches of the same block size. Stores no data (a few MB of bookkeeping at most). Works with the cache off: one run tells which cap is worth its RAM. |

How a read goes through it (`block_cache::Read`):

1. Only read-only devices, files opened without write access, size known from the VFS, request fully inside that
   size and at most `_max_request_kb`. Anything else returns "not handled" and `ReadSync` continues exactly as
   before. Saves and profiles never get a cache id (same rule as the window and the range cache).
2. Under the lock: blocks another thread is reading are waited for (or their queued job cancelled); blocks in RAM are
   copied into the guest buffer (LRU touch); missing blocks are reserved as pending, plus the read-ahead blocks.
3. Lock released: one `pread` loop per contiguous run of missing blocks (a short read is not end of file; only 0
   bytes is), read-ahead appended to the last run.
4. Under the lock: blocks published, waiters woken. The guest's buffer gets its bytes from the temporary buffer.
5. If the SD returned end of file inside the request, the answer stops there, as the direct read would. On an SD
   error the cache answers "not handled" and the usual path reads (and reports the error) as before.

Exactness: only bytes the SD returned are stored; a block shorter than the block size exists only where the file
ended. `NtReadFile` still writes the status block, signals the event and queues the APC after `ReadSync` returns, so
the guest-visible completion semantics are unchanged. Tested on the Mac with a stress harness (4 threads, random
128 KB / unaligned / end-of-file reads, a file system that returns random short reads, caps of 1-64 MB to force
evictions, synchronous and asynchronous read-ahead with the libnx calls stubbed): 0 mismatches over 4 x 20,000 reads,
clean under AddressSanitizer, UBSan and ThreadSanitizer.

### Incident: wrong data on the console (2026-10-08, build ru_glob3, `masseffect_io_bcache_mb = 128`)

With the real cache on (the ghost-only run was fine), subtitles lost their most common Cyrillic letters and a
constant buzzing tone ran through gameplay audio: bytes the game received were wrong (ME1's TLK string tables are
Huffman coded, which would explain why the most frequent letters go first; the buzz is a looped garbage sample from a
sound bank). That first version wrote its answer with a plain memcpy, assembled it directly in the guest buffer, kept
blocks in 4 MB slabs taken on demand from the heap, and had no self-check.

What was ruled out on the Mac, with the real `block_cache.cpp` and stubbed platform calls: many files (40, from
1 byte to 3 MB, sizes on and off block boundaries), random 32 KB-aligned and unaligned reads, end-of-file reads,
handles opened and closed per read, 3-4 threads, a file system returning random short reads, caps from 1 MB (constant
eviction) to 128 MB, synchronous and asynchronous read-ahead: 0 differing bytes in several hundred thousand reads, clean under
AddressSanitizer, UBSan and ThreadSanitizer. Keys, partial last blocks, block alignment, eviction and the pending /
job logic return exactly what `pread` returns. So the difference comes from something the host cannot reproduce. The
two candidates, and what was changed for each:

1. **Writing into guest memory with a faulting memcpy.** The direct path never stores into the guest buffer from
   user code. The cache did, with NEON memcpy, into pages that may not be mapped in that view yet. Each such store is an
   exception round trip, and those round trips are measured to corrupt q registers (exception_handler_switch.cpp:
   1.2-3.6 % of emulated accesses). Now the answer is assembled in a private buffer and written through the shadow
   alias (`_guest_write = 1`), which never faults.
2. **Cache blocks in heap memory that the guest can still reach.** Guest backing chunks are heap allocations
   (`memalign` in `RexGmCommit`) that `RexGmDecommit` frees, and `UnmapWindowRange` ignores unmap failures. If a view
   kept a mapping to a freed backing (large-pages mode 1 was on in both runs and is new), the next big long-lived heap
   allocations, the cache's 4 MB slabs, would be read and written by the guest. Now the whole cap is one allocation
   taken at the first file open (`_arena`), before any guest memory is freed, and never released. Every block is also
   hashed when stored and checked on every hit (`_checksum`).

How to tell which one it was: with `_verify_n` (on by default) the next console run logs either nothing (fixed),
`DIFFERENCE (cache vs SD)` (the assembled bytes were already wrong: blocks or SD), `DIFFERENCE (cache RAM)` (a stored
block changed in RAM: candidate 2), or `DIFFERENCE (guest buffer vs SD)` (the write into guest memory went wrong:
candidate 1). A/B of the fixes: `masseffect_io_bcache_guest_write = 0` and `masseffect_io_bcache_arena = false`
restore the old behaviour one at a time. If candidate 2 is confirmed, it is a guest-memory bug independent of the
cache (any heap allocation could land there) and must be fixed in `guest_memory_switch.cpp`.

### The summary line

Every `masseffect_io_summary_s` (15 s), after the `[io] range-cache` line, only when the cache or the ghost is on:

```
[io] block-cache 15 s: 169 reads (120 all RAM, 9 partial, 40 all SD, 0 bypassed), 15.9 of 20.0 MB from RAM (80 %);
     SD 46 calls 5.5 MB (demand 4.1 + ahead 1.4) in 180 ms; saved ~560 ms | ahead: used 30 wasted 4, now 512 KB;
     waits 2 (5.0 ms, 0 timeouts); jobs 0 queued 0 dropped 0 cancelled, 0 opens (0 ms) | total: ... saved ~12.3 s |
     RAM 61.2 MB in blocks, 64 of 128 MB allocated, 0 evictions | checks: 512 reads verified against the SD,
     40211 block sums
[io] block-cache ghost (32 KB blocks), share of demand blocks already in RAM, last 15 s / whole session:
     32 MB 41/35 %, 64 MB 63/52 %, 128 MB 78/66 %, 256 MB 81/70 %, 512 MB 81/71 % (640 blocks in the interval)
```

(the example numbers are illustrative.) "saved" prices the bytes served from RAM at what this cache's own SD reads
cost per byte in the session, per-call overhead included (4.1 ms per 128 KB until 1 MB has been read): it is the IO
thread's time saved, an estimate, not frame time. The `[io] 15 s` line above it keeps timing every `NtReadFile`, so
its "N reads X MB in Y ms" drops directly when the cache works. Evictions growing while "wasted" grows means the cap
is too small for the read-ahead; "wasted" much larger than "used" means the read-ahead does not fit the pattern (the
adaptive control should already have reduced it).

## 4. What to run on the console

All cold starts (`--cold`), Eden Prime / the manual route, one setting at a time:

1. **Measurement only (no risk):** `masseffect_io_bcache_ghost = true`. Read the ghost line after a few minutes of
   gameplay: the smallest cap whose session hit rate is near the 512 MB one is the cap to use. Optionally, a separate
   run with `masseffect_io_trace_record = true` and `masseffect_io_trace_record_s = 900` records every read for
   15 minutes into `cache/startup_reads.bin`; then
   `python3 tools/io_cache_sim.py startup_reads.bin --from-s 60` compares caps and read-ahead offline (do not time that
   run: it writes the trace every 15 s).
2. `masseffect_io_bcache_mb = 128` (+ ghost). Check: the `[io] 15 s` read time and worst read; the block-cache hit %;
   `RAM ... allocated`; the `heap:` line in `rex_profile.log` (never taken must stay well above 0); fps and
   `[hitch]` lines; texture pop-in on heads/hair (BIOG_HM*_HED/HIR packages); music.
3. `+ masseffect_io_bcache_readahead_kb = 512`. Check: SD calls per MB go down; "ahead used" vs "wasted"; any
   `read-ahead A -> B KB` lines; level-load time (the game thread waits for reads while loading).
4. `+ masseffect_io_bcache_async = true`. Check: `jobs queued/dropped/cancelled`, `waits`, `opens` and their time;
   that the `[io] 15 s` worst read does not get worse (the prefetch thread competes for the SD).

The recorded start-up trace (Mac, 132 MB, 1,058 reads) simulated with `tools/io_cache_sim.py`: a cache alone gets
10 % of blocks there (start-up reads little twice), read-ahead 512 KB with a 64 MB cap cuts SD calls from 1,030 to
343 and the modelled SD time from 4.3 to 2.9 s (2,586 read-ahead blocks used, 62 wasted). Gameplay needs the
console measurement above.

Suggested toml block (commented out in `app/masseffect.toml`):

```toml
masseffect_io_bcache_ghost = true
masseffect_io_bcache_mb = 128
masseffect_io_bcache_readahead_kb = 512
# masseffect_io_bcache_async = true
```

Consider also `log_nonblocking = true` (see section 1): it takes the log flushes of the `[io] SLOW` warnings off the
IO thread.

## 5. Risks

- **RAM.** Up to the cap from the host heap, in 4 MB slabs, on top of the range cache (64 MB cap; it stays idle for
  128 KB reads). With 1.0-1.2 GB never taken there is room for 128-192 MB, but malloc in use grows over a session:
  watch the `heap:` line. An allocation failure stops the growth and is logged; it does not crash.
- **Bursty texture arrival.** Mips that come from RAM arrive faster and closer together; the render thread then
  creates more textures in the same frames (`[hitch] ring: textures ... created`). Compare those lines with and
  without the cache.
- **SD contention (async only).** The prefetch thread's reads compete with the IO thread's demand reads on the same
  card; a demand read can queue behind a 512 KB prefetch (~10 ms). Hence async is a separate step.
- **Stale VFS size.** The cache trusts the file size of the VFS index (the size the guest also sees). Requests beyond
  it are not handled by the cache, so a wrong index can only make the cache step aside.
- **Lock.** One mutex for the table; it is never held across a `pread`, only across lookups and block memcpy (at most
  `_max_request_kb` per read).
