# Splitting the ring thread's PM4 -> Vulkan translation over two threads

Offline research and design, 2026-10-07. No code was changed and nothing was run on the console. Sources: the code of
`app/src/native/me_native_system.cpp`, `masseffect/masseffect_native_draws.cpp`, `masseffect_native_targets.cpp`,
`masseffect_deferred_recording.cpp`; the docs [ring-cpu-per-draw.md](ring-cpu-per-draw.md),
[nvk-per-draw.md](nvk-per-draw.md), [cpu-cost-analysis.md](cpu-cost-analysis.md),
[frame-handshake.md](frame-handshake.md), [best-config.md](best-config.md), [do-not-break.md](do-not-break.md); and the
ring partition lines of `mass-effect-recomp/run/me1/ringpart_ab_g` and `ringpart_f` (RU, 1785/768 MHz). Line numbers
are those of 2026-10-07; the files are being edited, so look functions up by name.

## Short version

- **Cut at the narrowest interface, not inside the draw code.** The front thread (today's ring thread) keeps the PM4
  parse, the register shadow, shader loads and identity, pairing and the draw-front proofs. A new back thread owns
  *everything behind* `TargetsNative`: EDRAM mode 4, `DrawsVulkanImpl::Draw` (textures, indices, vertex and constant
  uploads, pipeline, sets, vkCmd queueing), copies, present, occlusion queries. `TargetsVulkan` and
  `DrawsVulkanImpl` stay single-threaded and unchanged; only ~10 calls cross the boundary.
- **Split of today's work (heaviest measured window, 21.9 fps, 1208 ring draws / 737 Vulkan draws per frame):**
  front 10.4 ms per frame, back 26.0 ms, plus 9.1 ms in the present `WAIT_REG_MEM`. The back is the new bottleneck:
  the ring chain drops from ~36.5 to ~26.5-30 ms per frame (-18 to -27 %), depending on how many in-stream sync
  points force the front to wait (section 4).
- **The one real hazard is guest-memory timing**, and it has an exact rule: *every guest-visible effect the ring
  produces (fence and memory writes, scratch write-back, interrupts, occlusion results, the read pointer write-back)
  must happen after all guest-memory reads of the draws before it in the stream.* Today the ring satisfies it
  trivially. The split satisfies it either by a barrier (front waits for the back before the effect) or by making
  the effect an in-order token that the back executes.
- **The old `masseffect_native_uploads_thread` failure does not prove the opposite.** Its contract was "wait for the
  copies before returning the read pointer" (`WaitUploads`), but nothing in `me_native_system.cpp` calls
  `targets_->WaitUploads()`; and fences (`EVENT_WRITE_SHD`) are written at parse time and wake the render thread
  at once. A deferred copy could therefore run after the game had been told the GPU was past that draw. That is the
  missing letters and stray triangles in the UI and movies. With the rule above that cannot happen.
- **Gain on 3 A57 cores:** only if the extra thread gets CPU that is idle today. Cores 0-1 are nearly full at 30 fps;
  core 2 has ~13 ms per frame idle beside the main thread. Expected: +1-2 fps in the heaviest windows with barriers
  only and today's placement; +3-5 fps (up to the 30 fps cap in ~900-draw frames) with tokens and the NVK worker
  moved to core 2. **No extra frame of latency**: the present handshake in the stream stops any cross-frame
  pipelining, and every effect reaches the guest no later than today.
- **Safest first step:** the boundary and the register journal in *inline* mode (same thread, zero concurrency),
  verified bit-exact, then the back thread with a barrier before every guest-visible effect (section 6).
  Moving only "texture preparation + uploads" to a helper is not recommended: that code shares the upload buffer,
  submission splits, resolved images, descriptor slots and caches with the rest of `DrawsVulkanImpl`, and would
  need locks across the 13.5k-line file another task is editing.

## 1. Where the ring thread's time goes (input to the split)

`masseffect_native_ring_partition`, two heavy windows, converted to ms per frame (`busy` includes waits):

| Phase | Owner after split | W1: 219 Swaps, 264,621 ring draws / 10 s | W2: 248 Swaps, 215,079 ring draws / 10 s |
|---|---|---|---|
| parse | front | 1.48 | 1.24 |
| registers | front | 1.66 | 1.29 |
| shader loads | front | 2.08 | 1.70 |
| pairing | front | 2.81 | 2.45 |
| draw front (identity, extent and overwrite proofs) | front | 2.41 | 1.94 |
| **front total** | | **10.4** | **8.6** |
| EDRAM prepare + transfers + publish | back | 6.37 | 5.20 |
| Vulkan draw (indices, vertices, constants, pipeline, sets, vkCmd queue) | back | 11.86 | 10.53 |
| textures (sampler loop, PrepareTexture, fingerprints) | back | 5.49 | 4.69 |
| copies (resolves, clears) | back | 0.98 | 1.24 |
| present | back | 1.26 | 1.03 |
| **back total** | | **26.0** | **22.7** |
| WAIT_REG_MEM (present handshake, frame-handshake.md) | front | 9.1 | 8.7 |
| frame time | | 45.7 | 40.3 |

In W1 the ring has no idle time at all (wait phase 16 ms per 10 s): it is the bottleneck. The task's per-recorded-draw
figures (state 1.2-2.1 us, indices 3.4-4.5, textures 12-14, uploads 8.6-11.8, pipeline + constants 3-3.6, queueing 3.5)
all fall in the back column. NVK itself (1-2 us per draw) already runs on the deferred-recording worker.

Per-frame counts that matter for the boundary (best0_4609, ringpart_ab_g): ~1 ring segment per frame (`segment end`
224 entries / 10 s: the ring is always behind the write pointer), 5 memory/register `WAIT_REG_MEM` kinds of which only
the present handshake waits, 13-30 occlusion queries per Swap (each with a begin write and an end write to guest
memory), 18-33 copies per frame. The number of `EVENT_WRITE_SHD` fences, `MEM_WRITE`, `COND_WRITE` and `INTERRUPT`
packets per frame is not in the logs (the PM4 mix line prints only the top 8 opcodes); step 0 measures it.

## 2. Options considered

| Option | Front | Back | Verdict |
|---|---|---|---|
| A. Back does everything behind `TargetsNative` (recommended) | parse, registers, shader loads, pairing, draw front | EDRAM, textures, uploads, pipeline, recording, copies, present, queries | Front 10.4 / back 26.0 ms. Mechanical change at a ~10-call interface; draw code untouched. Guest reads move later: needs the ordering rule (section 3). |
| B. Front keeps every guest-memory read (indices, vertex copies, texture checks and uploads) | parse ... + guest reads | EDRAM, pipeline, recording | The safest memory semantics, but the reads are tangled with back state: upload-buffer `Reserve` and the `SendAndWait` when it is full, `sort_updates_texture` submission splits, resolved textures produced by copies on the back, descriptor slots, texture caches keyed by `generation_textures_`. It needs a rewrite of `DrawsVulkanImpl::Draw` and a front/back lock on its state. Balance is also worse (front ~23-26 us, back ~10 us per ring draw). |
| C. Only textures + uploads of the next N draws on a helper | ring as today | textures, uploads | Same entanglement as B, plus the helper needs draw N's state before the ring has computed it. Not exact without the same locks. |
| D. Look-ahead hashing helper (pre-hash guest ranges of upcoming draws, results used as validated hints) | ring as today | XXH3 of texture/vertex/index ranges ahead of the ring | Exact (section 3.4), but saves only the pure hashing (~2-4 us per recorded draw) and duplicates part of the PM4 parse on a scarce core. Keep as a later add-on to A. |
| E. Frame pipelining (thread B finishes frame N-1 while A parses N) | | | Not possible without faking the present handshake: the stream holds `WAIT_UNTIL` idle + `INTERRUPT` + `WAIT_REG_MEM [0x1FC9B006] == 0` before the Swap, and the release depends on the interrupt of frame N's end (frame-handshake.md). Skipping it is the rejected "write [blk+4] = 0 ourselves". A is pipelining *within* the frame, which is all the stream allows. |
| F. A pool (several workers per draw stage) | | | Not enough cores; ordering of EDRAM ownership, caches and the upload buffer makes per-draw parallelism inexact. |

## 3. Shared state and ordering hazards (option A)

### 3.1 Guest memory: when may the back read it?

Xbox 360 semantics: the GPU reads vertices, indices, textures and constant memory when it *executes* a draw. The CPU may
rewrite memory referenced by an already-submitted draw only after it learns that the GPU is past that draw. In this
emulator the only ways the guest can learn it are effects produced by the ring:

| Effect | Where today | Guest use |
|---|---|---|
| `EVENT_WRITE_SHD` fence write + `NotifyRingProgress` | `Type3`, ~line 1358 | D3D fences (`sub_8222C768` / `sub_8222FA98`), Swap's wait for the previous Swap, dynamic buffer reuse |
| `MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE` (memory), `EVENT_WRITE_EXT` | ~1328-1379 | generic GPU -> CPU writes |
| `SCRATCH_REGn` write-back (type-0 write with `SCRATCH_UMSK`) | `WriteRegister`, ~916 | present handshake flags (frame-handshake.md) |
| `INTERRUPT` (guest callback runs synchronously on the ring thread) | ~1248, `Interrupt` ~1006 | swap callback, decides the presentation slot |
| Read-pointer write-back at segment end | `RingLoop`, ~1134 | ring space reuse by D3D |
| Occlusion results: begin memset and end counts (`ZpdQuery` ~2534, `WriteOcclusionCounts`, `QueryService`) | ring | `GetData` polls |
| Resolve read-backs (`WriteReads`, at submission completion) | targets, `Complete` | CPU read-backs |
| VBlank interrupt (`VblankLoop`) | VBlank thread | presentation only; never "GPU done" |

**Rule R1 (exact):** a guest-visible effect at stream position p is performed only after every guest-memory read of
every draw before p is complete. Between two effects the guest has received no new permission, so the memory a draw
references cannot legally change between the front's parse and the back's read. The back then sees exactly the bytes
today's ring sees. Today the ring does both in one thread, so R1 holds trivially.

Corollaries:

- Effects become later than the parse, never later than today: the back reaches stream position p no later than
  today's ring did, because it no longer pays the front's share on the way there.
- The `masseffect_native_uploads_thread` corruption is R1 broken: the copy thread could run behind the next
  `EVENT_WRITE_SHD` (written at parse time) and behind the read-pointer write-back. `WaitUploads()` is declared for
  exactly this ("before returning the read pointer to the game", `masseffect_native_targets.h:122`), but
  `me_native_system.cpp` never calls it, and it would also have been needed before every fence. So do-not-break's
  "never move guest reads to a later thread" should become "never move a guest read past a guest-visible effect".
- Timing-based guest assumptions (polling a GPU status MMIO register) are not involved: `ReadMmio` returns the shadow,
  and the ring is already ~800 records behind the game today.

**Rule R2 (front reads):** the front still reads guest memory at parse time (`IM_LOAD` microcode, `LOAD_ALU_CONSTANT`,
the extent estimator's vertex reads in `Draw()`, the ZPD sentinel check, `WAIT_REG_MEM` and `COND_WRITE` polls). These
are the same reads at the same moment as today. Only when effects become back tokens (step 3) can a front read see
memory *before* an earlier effect in the stream has been written. Then the front must drain the pending write tokens
that overlap the range it reads, and all of them before a memory `WAIT_REG_MEM` or `COND_WRITE` poll. In ME1 the
overlaps are the scratch block (present handshake) and the query structures (ZPD sentinel).

**Rule R3 (GPU writes into memory the back reads):** `MEM_WRITE`, `EVENT_WRITE_EXT` and read-backs write guest memory
that a later draw could sample. With effects executed in stream order by the back (barrier or token), program order is
kept. A look-ahead helper (option D) must also invalidate its hints on these writes.

### 3.2 Register state and draw inputs

- `SubmissionDraw::registers` points at the live `registers_` (0x5003 words, 80 KB) and `vs_microcode` /
  `ps_microcode` are spans over the front's vectors. Both change under the back. A full 80 KB copy per draw is too
  expensive (~5-10 us). **Register journal:** every register store the front makes is appended to the queue in
  host order (`WriteRegister`; runs from `WriteConstantRun` / `WriteConstantRunRaw` as one record: index, count,
  words copied from `registers_ + index` after the apply; `REG_RMW`, `COND_WRITE` (register), the draw initiator and
  DMA base/size). The back applies the records to its own mirror; at a draw record its mirror equals the front's
  `registers_` at that draw. Measured volume: ~65-150 words per ring draw (17.2 M type-0 words + constants per 10 s),
  i.e. ~0.3-0.6 KB per draw and ~0.2-0.4 us of copying on each side.
- Generations (`gen_constants_vs_`, `gen_constants_ps_`, `gen_fetch_`, `gen_viewport_`, `gen_vs_`) are computed by the
  front and carried in the draw record, so the draw code's caches see the same numbers.
- Microcode: a record with the stage's host-order words whenever the stage's microcode changes (not on identical
  reloads, `ReloadsSameMicrocode`). Up to 966 words; ~0.5 per draw in heavy windows. Later optimization: pass the load
  memo slot + generation instead of the words.
- `ShaderEntry` objects are immutable after library load (no mutable fields, no `const_cast` found); the back may hold
  the pointers. Re-check if anyone adds a cache to `ShaderEntry`.
- The rest of `SubmissionDraw` (proof results, rectangles, `occlusion_query`) is computed by the front and copied by
  value (~0.4 KB; most optionals empty).
- `Copy()` builds `RegistersCopy` (21 words); `Present()` reads fetch constant 0 and the gamma ramp. Build both from the
  back's mirror, or capture them at parse; the gamma ramp goes with the present record when `gamma_version_` changed.

### 3.3 Back-only state (single owner, unchanged code)

`targets_` and everything in it: EDRAM ownership, tiles and epochs (`PrepareDrawEDRAM4`, `SynchronizeEDRAM4`,
`PublishDrawEDRAM4`, redirected clears), `resolved_` and crops, texture/sampler/view/per-fetch caches, vertex dedupe,
index cache, the upload buffer and its `Reserve` / `SendAndWait`, passes, pipelines, descriptor sets, work slots and
fences, read-backs, the occlusion query pool (`QueryBegin/End/Forget/Service`, `QueriesPending`), presenter calls
(`RampGamma` says "same thread as Present": both on the back), and `EnsureTargets()`. The FIFO keeps the order of
draws, copies (resolves and clears), queries and presents exactly. Resolves are host-only (no guest write), so a
draw's texture stage finds the resolved image of an earlier copy because the copy ran earlier on the same thread.

Thread-identity assumptions that move with it:

- Deferred recording: the producer is the thread that begins the first command buffer (`t_producer`,
  `g_pinned_producer`, `BeginCommands` in `masseffect_deferred_recording.cpp`) and `deferred::Flush()` (called today in
  `RingLoop`, ~1139) is producer-only. Both become the back. A fallback must therefore never move Vulkan work back to
  the front mid-session: fall back to **lockstep** (front waits after every record), not to inline execution.
- `copies_producer_` (`EnqueueCopy` / `WaitUploads` thread check), and the helper threads the draw code creates lazily
  (`copies_thread_`, `bindings_thread_`, `writer_cache_`, `prewarmed_thread_`, `preload_thread_`) are created from
  whichever thread runs `Draw`. Today that is an SDK `XHostThread` (the ring); create the back the same way
  (`rex::system::XHostThread`, next to `ring_thread_` in the system's start-up, ~line 770), never with `std::thread`
  and never lazily from the ring, so these keep being created from the same kind of thread. An `XHostThread` also has
  an `XThread` context, so the back can run guest interrupts (step 3).
- `Report()` (~2855) reads `targets_->StatsOfDraws`, `Stats`, `TimeGpuPerCategory`, `StatsPipeline`: the back prints
  those (or publishes a snapshot); the front prints only its own counters. The ring partition is per thread
  (`BindThisThread`) and reads CNTVCT; new timing code uses `steady_clock` only, sampled 1 in 64.
- Shutdown: stop the front, let the back drain, join the back before `targets_` and `presenter_` are destroyed.
  A ring re-initialization (`ring_generation_`) is a barrier.

### 3.4 Exactness of a look-ahead hash (option D, for later)

A helper may hash the guest ranges of a draw that is already published by the write pointer, before the ring reaches
it. The bytes cannot change in between (R1: the guest has had no permission since the draw was kicked), provided the
helper never scans past a memory `WAIT_REG_MEM` the ring has not yet passed (the "GPU waits for the CPU" pattern), and
any ring-side guest write (R3) invalidates the hints. Results keyed by (address, bytes, order) are hints: a wrong key is
just a miss. The shared 2 MB L2 of the A57 cluster also gives the ring warm lines. Cost: a second, light PM4 walk.

## 4. Handoff protocol

Single producer (front), single consumer (back), modeled on the compact deferred queue (`masseffect_deferred_ring.h`,
already stress-tested):

- **Records** in a 64-byte aligned byte ring (512 KB: ~500 draws of look-ahead with registers, enough because the back
  is the slower stage): `Regs{index,count,words}`, `Microcode{stage,words}`, `Draw{SubmissionDraw by value, gens}`,
  `Copy`, `Present{w,h,fetch0,optional ramp}`, `Query{begin|end|forget,address,draws,scale}`, `Barrier{id}`,
  `Verify{...}`; step 3 adds `GuestWrite{address,value,endian}`, `Interrupt{source,cpu}`, `Fence{address,value}` (+
  `NotifyRingProgress`), `ReadPointer{value}`.
- **Publishing:** the front publishes every 16 records and always before it waits (barrier, queue full, `WAIT_REG_MEM`,
  end of segment, idle). The back stores its read position every 16 records and at idle.
- **Waking**, as `WriteMmio` / `RingLoop` do today: the sleeper sets its flag and re-checks the predicate under the
  mutex (seq_cst); the other side notifies only when the flag is set. Condition variables with a 2-5 ms timeout as a
  safety net only. No `sleep_for` hand-offs (Horizon sleeps are coarse) and no `yield` spins between threads of
  different priority (on Horizon a yield only yields to the same priority on the same core). Short spins on the atomic
  are fine when the two threads are on different cores.
- **Barrier:** the front publishes, stores `barrier_target = written` (seq_cst), then waits on the condition variable
  until `back_done >= barrier_target`. The back compares `back_done` with `barrier_target` after each batch and notifies
  when it crosses it. Cost per barrier: one wake-up (~20-60 us on Horizon) plus the back idling while the front parses
  the next draw (~8 us).
- **Queue full:** the front waits until a quarter of the ring is free (hysteresis, so it does not wake per record).
- **Back idle loop:** `QueryService(idle, ...)` with the 250 us timeout while queries are pending (moved from
  `RingLoop` / `ServiceQueries`), `deferred::Flush()`, then sleep.
- No cycle: the back never waits on the front. It waits only on GPU fences (`SendAndWait`), the recording worker and
  the vertex copy thread, as the ring does today.

Barrier count decides how much overlap survives. Known per frame: 1 segment end, 1 present cluster (scratch writes,
interrupt, waits: one effective barrier because no draw sits between them), 26-60 occlusion writes (13-30 queries),
plus the unmeasured fences and memory writes. With ~60 barriers per frame: ~3-4 ms per frame lost. That is why step 3
turns the frequent ones (query writes, fences) into tokens.

## 5. Throughput and latency estimate

Per frame, W1 (worst window): today 36.5 ms of ring work. Split:

| | front | back | chain (ms per frame) |
|---|---|---|---|
| work moved | 10.4 | 26.0 | |
| journal + queue | +0.4-0.6 | +0.3 | |
| barriers, step 2 (~60 per frame) | | | +3-4 |
| barriers, step 3 (~2-5 per frame) | | | +0.2 |
| cross-core cache misses on registers and caches (estimate) | | +5-10 % | |
| **chain** | | | **step 2: ~29-31; step 3: ~27-28.5** |

W2 (867 draws per frame): back 22.7 ms, so the chain falls to ~24-27 ms, inside the 33.3 ms slot.

**CPU budget is the real limit.** At 30 fps two cores give 66.7 ms per frame. Cores 0-1 would carry front 10.9 + back
~27 + UE3 render thread 10-13 + recording worker 4-7 + audio/XMA/timers ~10 = 62-68 ms: saturated. Core 2 carries the
main thread (~18.6-20 ms at 1785 MHz) and has ~13 ms idle. So:

- Step 2 with today's placement: +1-2 fps in the heaviest windows (the two ring threads mostly compete for the same
  two cores; the gain is the overlap with the render thread's idle periods and with the back's GPU waits).
- Step 3 plus moving the deferred-recording worker (and/or the audio mixer) to core 2 below the main thread
  (cpu-cost-analysis item 1): +3-5 fps in ~1200-draw frames, up to the 30 fps cap in ~900-draw frames.
- Light views (cockpit, ~140 draws) are capped already: no change.

**Latency:** no extra frame. Frame N's last draw is done when the back reaches it, earlier than today by the front's
share; the present handshake then serializes frames exactly as now. Guest-visible effects happen at the back's
stream position, which it reaches no later than today's ring. The front may run ahead by up to the queue (~500 draws),
which costs only memory.

## 6. Recommended incremental plan

All behind one init-only cvar, default off: `masseffect_native_ring_split` (0 = today, 1 = inline journal,
2 = threaded with barriers, 3 = threaded with tokens), plus `masseffect_native_ring_split_verify` and
`masseffect_native_ring_split_queue_kb` (512).

### Step 0: measurement only (no behavior change)

In `Type3` count per 10 s, by kind, the guest-visible effects of section 3.1 and the ring draws between consecutive
effects (histogram 1, 2-7, 8-31, 32-127, 128+). One log line `[native] guest-visible effects (10 s): ...`. This gives
the barrier count of step 2 before writing it.

### Step 1: boundary + journal, inline (`ring_split = 1`)

Code locations (me_native_system.cpp):
- `WriteRegister` (~900), `WriteConstantRun` (~928), `WriteConstantRunRaw` (~960), `REG_RMW` / `COND_WRITE` register
  path, `DRAW_INDX` initiator/DMA writes (~1469): append `Regs` records.
- `IM_LOAD` (~1504) and `IM_LOAD_IMMEDIATE` (~1538): append `Microcode` when the stage's words changed.
- `Draw()` (~2564): build `SubmissionDraw` as now, but enqueue `Draw` instead of `targets_->Draw(request)` (~2707);
  `EnsureTargets()` and the attempt/failure counters move to the back.
- `Copy()` (~2713), `Present()` (~2744), `ZpdQuery` (~2534, only its `targets_->Query*` calls), `ServiceQueries` (~1147):
  enqueue / move to the back.
- New `app/src/native/me_ring_split.h` (record layout, `Place` / `RunOne` like `masseffect_deferred_ring.h`, host
  testable) and `app/src/native/me_ring_split_back.cpp` (the back: mirror, microcode vectors, dispatch to `targets_`).

In this mode the front executes every record right after enqueuing it, on the same thread. No concurrency, same
Vulkan producer thread, so it isolates journal completeness. Verification (`ring_split_verify` >= 1): at every draw
record, `memcmp` of the back mirror against `registers_` (80 KB, verification only) and of the microcode words; the
first difference logs `DIFFERENCE: ring split journal (register XXXX ...)` and switches the session to mode 0
(possible because nothing else changed thread). Unit test `tests/cpu/test_native_ring_split.cpp`: random record mixes
and sizes, wraps, mirror equality, and (for mode 2) one producer and one consumer with random barriers and
queue-full stalls, also under ThreadSanitizer on the Mac. Console check: identical captures, cost of the journal in the
partition (`registers` phase).

### Step 2: back thread, barrier before every guest-visible effect (`ring_split = 2`)

- Create the back as `rex::system::XHostThread` ("GPU ring back") next to `ring_thread_` (~770), priority 0x2C,
  cores 0-1 like the ring (placement is a later A/B). It runs the records and the idle loop of section 4.
- Front: before each effect of section 3.1 (`EVENT_WRITE_SHD`, `MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE` memory,
  `EVENT_WRITE_EXT`, scratch write-back in `WriteRegister`, `INTERRUPT`, ZPD guest writes, read-pointer write-back,
  ring re-init), publish and wait until the back has finished every record before it, then perform the effect itself
  exactly as today. `deferred::Flush()` moves from `RingLoop` to the back. Guest-visible order is then identical to
  today's.
- Verification (`ring_split_verify` = N):
  - **Read stamps (tests R1 directly):** for the first 4096 draws and 1 in N after, the front records an XXH3 of the
    first and last 4 KB of the index range (`VGT_DMA_BASE/SIZE`), of each vertex fetch range of the VS's bindings and of
    each texture base in the draw's fetch constants. The back recomputes them just before it reads those ranges. A
    difference logs `DIFFERENCE: guest memory changed between parse and back read` with the address, packet address
    and draw number, and switches to lockstep (barrier after every record).
  - **Register check:** every N-th draw the front puts an XXH3 of `registers_[0x2000..0x2FFF]` and `[0x4000..0x48BF]`
    in a `Verify` record; the back compares its mirror.
  - **Barrier report** every 10 s: barriers by kind, ms the front waited, ms the back idled.
  - **Watchdog:** a barrier or queue-full wait over 2 s logs both positions and switches to lockstep.
  - Captures of the same route pixel-compared with mode 0, warm then cold (`--cold`), as best-config.md requires.

### Step 3: tokens instead of barriers (`ring_split = 3`)

Effects become in-order back records: query begin/end writes and results, `EVENT_WRITE_SHD` (value captured at parse,
including the swap counter `counter_`) followed by `NotifyRingProgress`, `MEM_WRITE` / `REG_TO_MEM` / `COND_WRITE` /
`EVENT_WRITE_EXT` (values captured at parse), scratch write-back, the read-pointer write-back, and `INTERRUPT`
(executed by the back's `XThread` with `SetActiveCpu`, as `Interrupt` does now). This is closer to the hardware than
today (the CP's fence, interrupt and query writes land when the pipeline reaches them, not at parse). Apply R2: the
front keeps a small list of pending write ranges and drains before a memory `WAIT_REG_MEM`, a `COND_WRITE` poll or the
ZPD sentinel check of an overlapping address. The present sequence then needs no front barrier: the front publishes and
waits in `WAIT_REG_MEM` on the scratch block, the back drains the frame and runs the interrupt, and the guest's swap
callback judges lateness at the moment frame N is really finished. Watch in the A/B: the render thread's occlusion poll
(`sub_826E7C98`) and Swap fence wait, since results now arrive when the back gets there.

### Step 4: placement (A/B, each a separate run)

Back on cores 0-1 at 0x2C. Then, one at a time: deferred-recording worker on core 2 below the main thread (its
`threadCreate(..., -2)` core and priority); the front on core 2 above or below the main thread. Read per-thread CPU in
the profile and the barrier report.

## 7. What not to do

- Do not read guest memory on any thread after a guest-visible effect that follows the draw in the stream (R1). This
  covers vertex copies, index conversion, texture fingerprints and texture uploads alike.
- Do not run the present handshake early or skip it to gain cross-frame pipelining (frame-handshake.md).
- Do not create the back (or anything it creates) with `std::thread` from an SDK or guest thread; do not read
  `cntvct_el0` in the new code; do not use sleeps for hand-offs.
- Do not let the fallback move Vulkan recording back to the front (deferred recording pins its producer).

## 8. Implementation of steps 0 and 1 (2026-10-07, offline; not yet run on the console)

Build: `mass-effect-recomp/run/me1/ru_split1.nro` / `ru_split1.elf` (RU, SHA-256 prefix e50eff28d93ae9d9; contains the
whole tree as of 18:40, including other agents' uncommitted changes). No second thread. Both switches are init-only
and default off; with them off the only change on the ring path is one predictable branch per register store.

Code:
- `app/src/native/me_ring_split.h` (host testable): `EffectStats` (step 0), `Journal`, `Mirror`, `FirstDifference`,
  `VerifySchedule` (step 1). Test `tests/cpu/test_native_ring_split.cpp` (`tests/run_all.sh ring_split`; also clean
  under ASan/UBSan): 400k random front operations (single stores, constant runs, merged consecutive stores, rewrites,
  microcode changes of both stages), mirror compared bit for bit at ~20k syncs; malformed journals rejected.
- `me_native_system.cpp`:
  - Step 0 counters: `EVENT_WRITE_SHD` (fence), `MEM_WRITE`, `REG_TO_MEM`, `COND_WRITE` that wrote memory,
    `EVENT_WRITE_EXT`, scratch write-back (in `WriteRegister`), `INTERRUPT` packets, ZPD begin/end, read-pointer
    write-back (`RingLoop`), `WAIT_REG_MEM` memory/register. Gap histogram = ring draws between consecutive effects;
    "after back work" = effects preceded by a draw, copy or Swap since the previous effect (the barriers step 2
    would actually wait on; an effect right after another costs no wait).
  - Step 1 journal points: `WriteRegister` (plus `DC_LUT_RW_INDEX` advanced by the gamma path), the fast paths of
    `WriteConstantRun` / `WriteConstantRunRaw` (one record per run, values copied after the apply), the
    `COHER_STATUS_HOST` reset in `WAIT_REG_MEM`. The predicated-draw initiator swap is net zero with no back
    operation in between, so it is not journaled. Microcode: an epoch per stage bumped where `vs_microcode_` /
    `ps_microcode_` are assigned (`ApplyMemoHit`, `IdentifyShader`); the stage's words are journaled lazily at the
    next sync when the epoch changed.
  - MMIO writes from game threads (`WriteMmio`, all registers except `CP_RB_WPTR`) cannot be journaled by the ring:
    they set a per-register flag and an epoch; at the next sync the ring journals the live value of every flagged
    register. Step 2 must keep this (the front journals them).
  - The back: `SplitSync()` applies the journal; `BackDraw()` sets `SubmissionDraw::registers` and both microcode
    spans to the mirror; `Copy()` builds `RegistersCopy` and `Present()` reads fetch constant 0 from the mirror
    (`BackRegisters`). Syncs happen at each back operation and at every segment end (so the journal never outgrows a
    segment). Gamma ramp, `RampGamma` and the `Query*` calls are still called directly (same thread; they do not
    read registers). The front's own reads (proofs, estimator, coherence) still use the live `registers_`.
  - Verification: the first `masseffect_native_split_verify` back operations (default 4096), then 1 in
    `masseffect_native_split_verify_every` (default 1024): `memcmp` of all 0x5003 mirror words against `registers_`
    and of both microcode vectors. A difference in an MMIO-flagged register is a race with a game thread (counted as
    "races", mirror resynced); any other difference logs `DIFFERENCE` and turns the split off for the session (the
    back reads the live registers again: nothing else changed thread).

Overhead of the journal (host micro-benchmark in the test, M-series Mac, 1200 draws x (4 constant runs of 4-44 words
+ 20 single stores) with a sync per draw): 0.54 ns per register word plain vs 2.27 ns with journal + apply, i.e.
~0.2 us per draw, ~0.24 ms per 1200-draw frame. The A57 at 1785 MHz is typically 3-5x slower on such code: expected
~0.6-1 us per ring draw (~0.7-1.2 ms per heavy frame, 2-3 % of the ring), above the 0.2-0.4 us per side estimated in
section 4. A first version with `std::vector::resize` (zero fill before the copy) cost 3x more; the journal now
appends into an uninitialized growing buffer. The console figure comes from the `apply` field of the report and from
the partition A/B below (the append cost lands in the `registers` phase, the apply in `draw front`/`copies`/`present`).

toml (step 0 alone, measurement; combine with the partition for the barrier count of step 2):

```
masseffect_native_effects_stats = true
masseffect_native_ring_partition = true
```

toml (step 1, verify run; then a timing run with `masseffect_native_split_verify = 0`,
`masseffect_native_split_verify_every = 0` against the same route with `masseffect_native_split_lockstep = false`):

```
masseffect_native_split_lockstep = true
masseffect_native_split_verify = 4096
masseffect_native_split_verify_every = 1024
masseffect_native_ring_partition = true
```

Expected log lines:

```
[native] guest-visible effects statistics on (measurement): report every 10 s
[native] guest-visible effects (10.0 s, 248 Swaps): N total (x/Swap), B after back work = step-2 barriers (y/Swap);
    fence n (x/Swap), MEM_WRITE n (...), REG_TO_MEM ..., COND_WRITE mem ..., EVENT_WRITE_EXT ..., scratch ...,
    INTERRUPT ..., ZPD begin ..., ZPD end ..., read pointer ..., WAIT_REG_MEM mem ..., WAIT_REG_MEM reg ...;
    ring draws between consecutive effects: 0: a, 1: b, 2-7: c, 8-31: d, 32-127: e, 128+: f; max M
[native] ring split: lockstep (journal + back mirror on the ring thread); verification of the first 4096 back
    operations, then 1 in 1024
[native] ring split (lockstep, on) 10.0 s, 248 Swaps, 215079 ring draws: S syncs, R records, W register words
    (w per ring draw), U microcode words, J journal words; apply A ms (a us per ring draw, a' ms per Swap); verified
    V back operations (cumulative), T ms; MMIO registers K distinct, X re-journals, Y races
```

(one line each in the log; wrapped here). Pass criteria: no `DIFFERENCE: ring split journal` line, the status stays
`on`, captures identical to the run without the switch, and the `registers`/`draw front` partition phases up by no
more than ~1 us per ring draw in total. A `DIFFERENCE` names the register, both values, the draw and packet address:
it is a missing journal point (or an MMIO register missed by the flag), to fix before step 2. Watch `MMIO
registers K distinct`: if the game writes draw-state registers (>= 0x2000) through MMIO, step 2 needs the front to
journal them as done here, never the back reading `registers_`.

### 8.1 First console result and fix (ru_split2)

- `split_a1` (step 0): ~77 guest-visible effects per Swap, ~40 of them after back work (step-2 barriers); fences
  ~10/Swap, scratch 5, interrupts 1, ZPD begin and end ~16.7 each. Step 2 with barriers only would pay ~40 waits per
  frame (~2-3 ms at 20-60 us each): the ZPD and fence tokens of step 3 matter.
- `split_b1` (ru_split1, lockstep): `DIFFERENCE: ring split journal (draw: register 01C5 front 00000025 back 00000000
  ...; draw 1)`. 0x01C5 is `CP_RB_WPTR`, written by the game's MMIO kick; ru_split1 left it out of the MMIO flags on
  purpose (one flag + epoch bump per kick) and so out of the mirror. The other 0x5002 registers and the microcode
  were equal. The back never reads it, but the check is bit for bit.
- Fix (ru_split2): every sync journals `CP_RB_WPTR` when its live value differs from the mirror
  (`JournalIfChanged`); seeding is explicit (`SplitSeed`: full register file + both stages' microcode, MMIO epoch
  read first, log line `[native] ring split: back mirror seeded (20483 registers, VS n words, PS n words)`).
  Audit: every other register store is on the ring thread through the journaled paths (`WriteRegister`, the two
  run paths, `WAIT_REG_MEM` coherency reset); the VBlank thread's interrupts reach registers only through MMIO
  (flagged); ZPD and `INTERRUPT` write guest memory, not registers. Unit test `TestStartup` reproduces the console
  case (mirror seeded, then an unjournaled write-pointer store) and checks the fix.
