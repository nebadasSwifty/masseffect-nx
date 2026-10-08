# The per-frame GPU wait on 0x1FC9B006 (frame handshake)

Offline analysis, 2026-10-07. Nothing here was run on the console. Function names are given as RU / EN (the RU
edition runs; D3D functions are EN = RU + 0x210, UE3 functions use editions/ru/address_map.json).

Measured before this analysis (Normandy walk, heavy windows, ~25-27 fps): the ring thread spends 13-27 ms per frame
(3-6.6 s per 10 s) in one `WAIT_REG_MEM` on memory `0x1FC9B006`, function 3 (equal), ref 0, mask FFFFFFFF.
`masseffect_vblank_adaptive` cut this only from 5.0 to 4.4-4.7 s per 10 s (25.8 -> 27.3 fps).

## 1. What 0x1FC9B000 is

It is the **D3D scratch-register write-back block**, not a ring control block.

- Allocated in D3D device init `sub_8222C8B8` / `sub_8222CAC8`: `sub_82811E90(32, 0x95800000)` (32 bytes of physical
  memory = 8 dwords), pointer stored at `device+10772` and zeroed.
- The same function programs the GPU: type-0 `SCRATCH_ADDR` (0x1DD) = physical address of the block, `SCRATCH_UMSK`
  (0x1DC) = `0x00020033` (write-back enabled for SCRATCH_REG0, 1, 4, 5; bits 16-17 = 32-bit swap).
- With write-back enabled, every PM4 write to `SCRATCH_REGn` (0x578 + n) is mirrored by the CP to
  `SCRATCH_ADDR + 4n`. Our `WriteRegister` does this (`me_native_system.cpp:902-908`).

Layout (physical 0x1FC9B000):

| offset | register | meaning |
|---|---|---|
| +0  | SCRATCH_REG0 | "interrupt pending" CPU mask (GPU sets 4 = CPU 2, the CPU interrupt clears its bit) |
| +4  | SCRATCH_REG1 | **"presentation pending" flag** (register table comment: "present interval"): GPU sets 1, CPU clears to 0 when the frame may be shown |
| +16 | SCRATCH_REG4 | GPU callback function pointer for the interrupt (0x0BADF00D = none) |
| +20 | SCRATCH_REG5 | callback argument |

**The "+2 / +6" is not an unaligned offset.** The two low bits of a WAIT_REG_MEM poll address are the Xenos endian
mode (2 = 8in32). `0x1FC9B006` = dword at `0x1FC9B004` read with 8in32 swap; `0x1FC9B002` = dword at `0x1FC9B000`.
`ReadMemory` (`me_native_system.cpp:980`) already handles it (`address & ~3`, `GpuSwap(v, address & 3)`). The fields
are plain 32-bit words.

## 2. Who emits the waits

### SynchronizeToPresentationInterval: `sub_822338C8` / `sub_82233AD8` (arg r4 = 0)

The UE3 present `sub_826E65D8` / `sub_826E5DE0` (called with `bLockToVsync` from RHIEndDrawingViewport
`sub_826DF8D8` / `sub_826DEF20`) does exactly the Xbox 360 present sequence:

1. `sub_822338C8(device, 0)` = `SynchronizeToPresentationInterval()`
2. `sub_82225FC0` = `Resolve(...)` of the scene into the front buffer (`[0x82EA0F1C]`)
3. `sub_82233DF0` / `sub_82234000` = `Swap(frontbuffer)` (VdSwap writes the XE_SWAP packet)
4. writes `D3DRS_PRESENTIMMEDIATETHRESHOLD` into `device+11572` bits 23..29: **10** when bLockToVsync, else the global
   `0x82E5DCF8` (RU) / `0x82E5DCD8` (EN), which a console command (`sub_826E5550` / `sub_826E4D58`) toggles 0/100 or
   sets to 0..100. It takes effect at the next frame.

Stream emitted by `sub_822338C8` (r28 = 0 path):

```
type0 SCRATCH_REG1 = 1                        -> [blk+4] = 1
WAIT_REG_MEM mem 0x13, [blk+4]|2 == 1, wait 256     (1FC9B006 ref 1: met at once, it was just written)
sub_8222C630 / sub_8222C840 (flags 0 -> 6):
  type0 WAIT_UNTIL (0x5C8) = 0x20000
  type0 SCRATCH_REG4,5 = 0x82233730 (RU swap callback), param
  type0 SCRATCH_REG0 = 4                      -> [blk+0] = 4
  WAIT_REG_MEM (pred) [blk+0]|2 == 4          (1FC9B002 ref 4: met at once)
  INTERRUPT (pred) cpu mask 4                 -> guest interrupt callback, source 1, on CPU 2
  type0 CALLBACK_ACK (0x45E) = 4
  WAIT_REG_MEM (pred) [blk+0]|2 == 0          (1FC9B002 ref 0: the interrupt cleared it)
  type0 SCRATCH_REG4 = 0x0BADF00D
WAIT_REG_MEM mem 0x13, [blk+4]|2 == 0, wait 256     <== THE SLOW ONE (1FC9B006 ref 0)
```

`param` = `(interval << 8) | threshold`, where interval comes from `D3DRS_PRESENTINTERVAL` (`device+13220`: ONE -> 1,
TWO -> 2, THREE (4) -> 3, IMMEDIATE (0x80000000) -> 0) and threshold = `(device+11572 >> 23) & 0x7F`. The measured
waits up to 27 ms (> 16.7) prove interval = 2 (30 fps cap; cockpit runs at exactly 300 Swaps / 10 s).

Other callers of `sub_822338C8`: the D3D re-present thread `sub_82238538` (30 ms timeout, only with `device+10814`
bit 4), `sub_82758C20` (VdPersistDisplay path), `sub_82233C18` and `Swap` itself only when `device+21912` bit 4 is
set (it is created as 0x04000001 in `sub_826E6158`, so not in ME1). So the per-frame wait is the UE3 present.

### Who clears [blk+4] (CPU side)

The guest interrupt callback is `sub_8222C788` (RU; EN `sub_8222C998`), data = device:

- **source 1** (our `PM4_INTERRUPT`, `me_native_system.cpp:1229`): calls `[blk+16]([blk+20])` = the swap callback
  `sub_82233730` / `sub_82233940`, then clears this CPU's bit in `[blk+0]` under a spin lock.
- **source 0** (VBlank, `VblankLoop`, `me_native_system.cpp:1023`): if `D1MODE_VBLANK_VLINE_STATUS` (MMIO 0x7FC86544,
  we return 1) calls the VBlank handler `sub_82233630` / `sub_82233840`.

Swap callback `sub_82233730` (runs at the moment the GPU reaches the INTERRUPT):

```
swaps++ (device+16188)
pct  = (mftb - lastVBlankTime[16176]) * 100 / (freq / 60) + 1, clamped to 100
T    = lastTarget[16180] + interval
if (T <= vblankCount[16172]) {          // frame is late
    T = vblankCount
    if (interval && pct > threshold) T++  // late AND past threshold% of the current scan -> next VBlank
}
optional user callback [16164]
lastTarget = T
if (T != vblankCount && swaps != 1)  enqueue {T} into 16-entry queue [16196..], tail [16328]
else                                 flips++ [16192]; [blk+4] = 0     // release the GPU now
```

VBlank handler `sub_82233630`: `vblankCount++`, `lastVBlankTime = mftb`, pop every queue entry with
`T <= vblankCount`: `flips++`, `[blk+4] = 0`. So `[blk+4]` becomes 0 only:

- immediately in the INTERRUPT (source 1) when the frame is late and `pct <= threshold`, or
- on the VBlank thread at VBlank number `T` (on-time frames: `lastTarget + 2`; late frames past the threshold:
  the next VBlank).

Writers of `[blk+4]`: SCRATCH_REG1 write-back (GPU, sets 1), `sub_82233730` and `sub_82233630` (CPU, set 0). Writers
of `[blk+0]`: SCRATCH_REG0 (GPU, 4) and `sub_8222C788` (CPU, clears bit). Nothing else touches the block (other
readers: `sub_82231870`, `sub_829388C0`, `sub_82939498` are diagnostics / teardown).

## 3. The handshake, and how it maps onto our emulation

What the GPU waits for: **the guest's presentation-interval scheduler** (software VSync). After the frame's last draw
and before the resolve into the front buffer, the GPU stops until the D3D VBlank logic says "this frame's
presentation slot has come". It is not a back-buffer, ring or kick handshake. The CPU write that releases it is done
either by the INTERRUPT handler (ring thread, synchronous) or by the VBlank handler (`GPU VSync native` host thread).

On real hardware the GPU idles there too; the CPU keeps going. Here the coupling is tighter:

- The ring thread *is* the GPU, so frame N+1's commands queued behind the wait cannot be parsed.
- The render thread's `Swap` waits (reason 3, `sub_8222C558`) for the fence of the previous Swap, i.e. for the ring to
  have passed the previous frame's release. A frame deferred by one VBlank therefore also stalls the render thread
  (and through `FRenderCommandFence` the game thread).

Two kinds of wait time:

1. **On-time frames** (callback before VBlank `lastTarget + 2`): the wait is the 30 fps cap. Unavoidable without
   changing the presentation interval. Most of the 13-27 ms is this; with ring work ~15-20 ms per frame the ring is
   idle about half the time, which says the ring thread is not the limit in these windows.
2. **Late frames**: the D3D rule above uses threshold **10 %** (UE3 bLockToVsync): a frame that misses its VBlank by
   more than ~1.5 ms waits up to another 16.7 ms -> 33/50 ms quantization. This is the part `masseffect_vblank_adaptive`
   attacks, and its gain (+1.5 fps, ~0.4 s of waits per 10 s) is the size of this effect.

Our own added latency (all small):

| source | where | size |
|---|---|---|
| WAIT poll sleeps 50 us, Horizon sleeps ~0.35 ms | `me_native_system.cpp:1291-1295` | ~0.1-0.35 ms per release (spin test showed no fps change) |
| VBlank thread wakes late (sleep overshoot, priority) | `VblankLoop`, `:1054-1058` | ~0.1-0.5 ms per queued release |
| Interrupt delivery for source 1 | synchronous on the ring thread (`Interrupt`, `:992`) | 0 (better than hardware) |
| RPTR write-back / swap counter | not involved: the wait is on SCRATCH_REG1, not on RPTR or `counter_` | 0 |
| mftb vs KeQueryPerformanceFrequency | both 50 MHz guest ticks (`runtime.cpp:108`, `REX_QUERY_TIMEBASE`) | consistent, `pct` is right |
| Vulkan work of the frame tail not submitted until XE_SWAP | `Present()` -> `SendWork(false)`, `masseffect_native_targets.cpp:2371` | Switch GPU idle during the wait (see P3) |

So the flag is already released at the same logical moment as on hardware; the loss is policy (10 % threshold,
interval 2) plus the CPU/ring coupling, not delivery delay.

## 4. Proposals (all image-identical)

Our presenter shows whole frames at XE_SWAP, it never tears, so "present a late frame immediately" changes only timing,
never the picture.

### P1. Force PRESENTIMMEDIATETHRESHOLD = 100 for late frames (replaces `masseffect_vblank_adaptive`)

Hook the swap callback and raise the threshold byte of its argument; the guest's own lateness test (`T <= vblankCount`)
stays, on-time frames still wait for their VBlank (30 fps cap and the 30 fps grid are kept).

```cpp
// me_ring_wait.cpp (EN name; tools/edition.sh maps it to RU sub_82233730). New cvar, default off.
REX_EXTERN(__imp__sub_82233940);
REX_HOOK_RAW(sub_82233940) {
  static const int32_t t = REXCVAR_GET(masseffect_present_immediate_threshold);  // -1 = game's, else 0..100
  if (t >= 0) ctx.r3.u64 = (ctx.r3.u32 & ~0xFFu) | uint32_t(t);
  __imp__sub_82233940(ctx, base);
}
```

Expected: at least the adaptive VBlank gain (25.8 -> 27.3 fps), probably a bit more: the decision is made inside the
INTERRUPT with zero delay (adaptive needs >= 200 us of waiting plus a VBlank-thread wake-up, fires only when
`now - last XE_SWAP >= 2*interval - 0.5 ms`, which lags the guest's view), and the VBlank counter stays exactly 60 Hz
(adaptive injects extra VBlanks and restarts the phase). After a late frame the next target is still the regular grid
(`T + 2`), so the schedule catches up instead of shifting. Turn `masseffect_vblank_adaptive` off with it.

Risks: frame pacing of late frames becomes irregular (35-45 ms instead of 50 ms) - judder instead of hitches, no
tearing on our output; on a real 360 the same setting would tear. Do a diagnostic run first: log per 10 s the param
(interval, threshold), how many callbacks were late, deferred and immediate, and the `pct` histogram - this measures
exactly the share of P1.

### P2. Variant to measure: PRESENTINTERVAL ONE (60 Hz grid) with P1

Same hook, also set bits 8-11 of r3 to 1. On-time frames then release at `lastTarget + 1` (16.7 ms grid): heavy windows
run at the render thread's own rate (no wait for the 33 ms slot), light scenes (cockpit) exceed 30 fps. Image-identical,
changes pacing and power; UE3 uses delta time. Only as an A/B variant (user decides). Not the 0 = IMMEDIATE value: it
skips the queue logic differently (`interval == 0` disables the threshold test) - fine too, but 1 keeps the flip
counters meaningful.

### P3. Submit the frame's Vulkan work when a memory WAIT_REG_MEM starts to block

At this wait the frame's post-process and UI draws are recorded but unsubmitted until `Present()` after the release, so
the Switch GPU sits idle for 13-27 ms and then has the tail plus the copy plus the next frame queued behind it. In the
`PM4_WAIT_REG_MEM` case (`me_native_system.cpp:1265`), on the first not-met iteration with `info & 0x10`, call a new
`TargetsNative::SubmitPending()` (`masseffect_native_targets.h:~110`) that does `SendWork(false)` when `recording_ &&
!occlusion_open_` (the same early submit `QueryService` already does, `masseffect_native_targets.cpp:3284-3291`).
Expected: GPU-side frame work overlaps the wait; shrinks "GPU fence"/"output" waits (check `[hitch] waits` and
`ns_waits_gpu_` first - gain only if they are non-zero). Risks: one extra submission per frame (slot pressure: the next
`Record()` may wait for that slot's previous submission; render pass closed/reopened - the next packet is the resolve
copy anyway); vertex dedupe already clears on every WAIT_REG_MEM, so it stays exact.

### P4. Wake the ring as soon as the flag is written (small)

Replace the 50 us `rex::thread::Sleep` for memory polls (`:1291-1295`) with a condition-variable wait (1 ms timeout)
notified by `VblankLoop` after each `Interrupt(0, 2)` (`:1040`, `:1046`). And let `VblankLoop` wake ~0.5 ms before
`next` and yield to the deadline. Expected ~0.2-0.5 ms per frame earlier release (1-1.5 %), which also unblocks the
render thread's Swap earlier. Risk: none for correctness; a little CPU on core of the VBlank thread.

### Not recommended

- Skipping the wait or writing `[blk+4] = 0` ourselves: the 16-entry flip queue (`device+16196`, no bound check, the
  only throttle `16184 - 16192 >= 15` is in the `21912 & 4` path, inactive) would overflow, and the cap disappears.
- `masseffect_native_waitregmem_spin_us`: already measured, no change (the time is policy, not wake-up).

## Code map

| what | RU | EN |
|---|---|---|
| SynchronizeToPresentationInterval | sub_822338C8 | sub_82233AD8 |
| scratch/interrupt packet emitter | sub_8222C630 | sub_8222C840 |
| guest interrupt callback | sub_8222C788 | sub_8222C998 |
| swap callback (decides release) | sub_82233730 | sub_82233940 |
| VBlank handler (queued release) | sub_82233630 | sub_82233840 |
| Swap | sub_82233DF0 | sub_82234000 |
| UE3 present (Sync, Resolve, Swap, threshold) | sub_826E65D8 | sub_826E5DE0 |
| UE3 RHIEndDrawingViewport | sub_826DF8D8 | sub_826DEF20 |
| threshold console command / global | sub_826E5550, 0x82E5DCF8 | sub_826E4D58, 0x82E5DCD8 |
| scratch block allocation, SCRATCH_ADDR/UMSK | sub_8222C8B8 | sub_8222CAC8 |

Device fields: 10772 scratch block ptr, 11572 threshold (bits 23-29), 13220 present interval, 16172 VBlank count,
16176 last VBlank mftb, 16180 last target VBlank, 16184 Swaps issued, 16188 swap callbacks, 16192 flips,
16196/16200 queue entries, 16324/16328 queue head/tail, 16336 spin lock, 21176 refresh rate (0 -> 60).
