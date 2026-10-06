# CPU cost analysis: where the CPU goes and the large changes left

Date: 2026-10-07. Scope: the CPU side of Mass Effect 1 on the Switch (3 usable Cortex-A57 cores, target clocks CPU 1785 MHz,
GPU 768 MHz). This is a read-only analysis of the docs, the console logs and the code. Nothing was run on the console. Words you
do not know are in [glossary.md](glossary.md); how numbers are taken is in [measuring.md](measuring.md).

Short version:

- **No ME1 stack profile exists at 1785 MHz.** Every per-function number below comes from stock-clock runs (1020 MHz,
  t223-t271, Oct 2-4). The newest per-thread numbers for this build are from `out/console-test/smoke2` (1020 MHz). Take a fresh
  profile before you start any item in section 3 (the procedure is in section 2).
- At 1020 MHz the frame costs about 100 core-ms (light views) to 142 core-ms (grass) over 3 cores. Scaled by 1020/1785, that is
  about 57-81 core-ms. On average this fits in the 100 core-ms a 30 fps frame allows, so at 1785 MHz the CPU limit is:
  (a) the serial chain main thread -> UE3 render thread -> ring thread, (b) cores 0-1 being overbooked while core 2 is partly idle,
  and (c) heavy views and hitches. Several views at 1785 are GPU bound (see 1.6), so judge CPU work in CPU-bound views.
- The thread-placement heuristic in `sdk/src/ui/switch_perf.cpp` uses CPU % thresholds tuned at 1020 MHz (60 % to pin the
  main thread, 35 % to raise the render thread). At 1785 MHz the threads may stay under those thresholds, so the pinning and the
  priority raise may never happen. Check this first (section 2, step 8). It is cheap and may explain part of the dips.

## 1. Where the CPU time goes, per thread

### 1.1 Thread picture of the current build (stock clock)

Source: `out/console-test/smoke2/profile.log` (2026-10-04, masseffect-nx build, production toml with `masseffect_exclusive_core = 32`,
`masseffect_hot_guest = true`, ring switches on, CPU 1020 MHz, Eden Prime, 15 "normal" blocks after the first 3, 23.1 fps on average).
Core-ms per frame = CPU % x 10 / fps, averaged over the blocks (the method of `tools/console-test/cpu_per_frame.py`, which gives
total 100.4, main 30.7, ring 25.7 for this run).

| Thread (name in the profile) | Priority, core mask | Core-ms per frame at 1020 MHz | Scaled to 1785 MHz (x 0.571, an estimate) | Role |
|---|---|---|---|---|
| `XThreadCE75C540` | 0x3B, mask 0x4 (pinned to core 2) | 32.5 | ~18.6 | UE3 game (main) thread |
| `GPU ring native` | 0x2C, mask 0x3 | 28.1 | ~16.0 | PM4 to Vulkan translation (ring thread) |
| `XThreadCE30BF60` | raised to 0x2B, mask 0x3 | 20.3 | ~11.6 | UE3 rendering thread (runs the game's D3D, writes PM4) |
| `XThreadCE1A1460` | 0x3B, mask 0x7 | 8.1 | ~4.6 | probably the audio mixer guest thread (the backlog's "audio thread", 24-29 % of a core at stock) |
| `?` (unnamed libnx thread) | 0x2C, mask 0x1 (core 0 only) | 7.4 | ~4.2 | probably the deferred-recording (C20) worker that records `vkCmd*` |
| `XMA Decoder` | 0x2B, mask 0x3 | 2.4 | ~1.4 | XMA decode |
| about 10 other guest and host threads | | ~8 | ~4.5 | streaming, PhysX, timers, audio worker, vblank |
| **total** | | **~100** | **~57** | |

The scaling column ignores memory-bound work (memory went from 1331 to 1600 MHz too) and spin-polling, so it is only a guide.

Things that matter in this table:

- **Cores 0-1 carry the ring thread, the render thread, the C20 worker, the XMA decoder, the audio worker, the timers and the vblank
  thread** (all host threads are created with mask 0x3 or 0x1). That is about 58 core-ms at stock on two cores. Core 2 holds only the
  main thread (32.5 ms, plus the light guest threads allowed by `exclusive_core = 32`). At 1785 MHz the main thread needs about 56 %
  of core 2; about 14 ms of core 2 per frame is idle while cores 0-1 still preempt each other.
- **CPU % includes polling.** The same block reports `svcSleepThread` up to 1 ms 6,647 calls/s (745 ms/s inside), 2,334 yields/s,
  123 fence waits/s (71 ms/s). The ME2 profile (S21) found the render thread 55 % in `SleepEx` idle-polling. Busy time is not the
  same as useful work, so a thread at 80 % may be mostly waiting.
- **Native audio DSP is off in production.** The system line says `native audio DSP calls 0`: `masseffect_audio_dsp_native` is not in
  `app/masseffect.toml` (default 0), although Y10 was validated on the console (t251: 23,411 calls, 0 mismatches).

### 1.2 Main (game) thread

| Fact | Source |
|---|---|
| 29-33 ms per frame at stock (t255 29.1 after the "diet"; t261 29.4; smoke2 30.7-32.5). About 90 % of its core | performance-history.md section 9, smoke2 |
| Probe t238 (scene draws dropped on the Vulkan side): the game still stops at about 29 fps, main thread about 31 ms per frame | backlog t238 |
| It waits for the render thread in `sub_822FE760` (`while (*counter > n) Sleep(0)`): 19 % of its samples in t223, about 44 % in t220 before the render thread was raised (t225) | backlog t220, t223; `app/src/native/me_ring_wait.cpp` |
| Flat profile. Top self shares in t265 (main thread): distribution lookup `824DD848` 3.56 %, CRT memcpy `82AC4AF0` 3.39 %, volume overlap `82B5F0E8` 2.09 %, wcscmp `82AC4520` 1.82 %, object hash lookup `8230F620` 1.38 %, wcsicmp `82AC3790` 1.19 %, hash find `82BAFF58`+`82BB0748` 1.92 %, object iterator `82210970` 1.11 % | backlog "Hot guest natives, round 2" |
| 24 native replacements exist (`app/src/native/hot/`, `me_hot_guest.cpp`). 11 are in the best profile (t261). The 13 more gave no measurable gain (t281-t284: 33.4 against 33.6 ms), although their estimated savings added up to a few ms | optimization-paths.md section 1 |
| PhysX: the join `sub_82320F80` polled `fetchResults` and cost 19 % of the main thread (t225). It was 12.7 % with the warning not formatted (t226) and 2 % after yield with migration (t227). The simulation itself is about 0.85 ms per frame | backlog PX1 |
| Codegen work already done: direct calls, no `volatile`, r14-r31 and scratch registers as locals, no MSR fences, no LR stores, D-form split, inline `fctiwz`. Together main 32.4 to 29.1 ms. Arguments in registers gave nothing more on top (t264) | optimization-paths.md section 1 |

Reading: the remaining main-thread time is spread over thousands of functions. Making single leaf functions faster no longer shows
up in the measurements. A main-thread gain now needs either less work (algorithmic changes at the caller level: section 3, item 3) or
a whole-program codegen change (item 5).

### 1.3 UE3 render thread

| Fact | Source |
|---|---|
| About 20 ms per frame at stock (smoke2). Raised to priority 0x2B because the ring thread and the C20 worker (0x2C) preempted it: +1.8 fps (t225) | smoke2, backlog t225 |
| D3D stopwatch (t246, inclusive time of the hooked entry points per frame): `82227C40` (indexed draw) 343-401 calls x 10-12 us = 3.5-4.7 ms; `82228568` (cheap draw) 1100-1400 x 1.3 us = 1.5-1.8 ms; `82227760` 0.5-1.9 ms; all state setters together about 0.9 ms. The Swap `82234000` takes 6-8 ms, and that time is a wait on the ring (`8222C768`) | backlog t246 |
| Waits at t223: `ring_mutex_` in `WriteMmio` about 7 % (fixed since then: the lock is taken only when the ring waits); idle sleep about 5 % | backlog t223; `me_native_system.cpp` `WriteMmio` |
| Occlusion poll `sub_826E7C98` (`while (GetData() == S_FALSE) Sleep(0)`) was about 9 % of the render thread; now it waits for ring progress (`masseffect_wait_occlusion_us = 500`) | `me_ring_wait.cpp` |
| Natives in the render thread: sprite vertices `8264C7C0` 2.39 %, skin rebind `8264ADA0` 1.67 %, plane reject `8262CFC0` 1.38 %, memcpy 1.60 % | backlog round 2 |

### 1.4 Ring thread

| Fact | Source |
|---|---|
| 25.7-28 ms per frame at stock after all ring switches (t253 25.7, t261 26.2, smoke2 25.7-28.1). Earlier about 35 us per draw at about 2500 draws and 42 k PM4 packets per frame | performance-history.md section 9, backlog "Ring thread CPU" |
| Per-draw stopwatch (t172): BindPipeline 2.4 us, Draw 2.1, samplers 2.0, indices 1.9, set 4 1.5 | backlog |
| Sampled profile (t172, t232, t242): XXH3 about 15 % (now about 9 % with mode 1 and the fingerprint limit), std::string ops about 8 %, std::sort about 6 %, NVK descriptor/cbuf/BeginRendering about 10 %, `CopyVertices` the top function (1.8-2.2 ms) | backlog, `mass-effect-recomp/docs/ring-cpu.md` |
| The ring runs about 800 records behind the game. The render thread waits on it at Swap (6-8 ms at stock) | ring-cpu.md, backlog t246 |
| Deferred recording (C20) moved `vkCmd*` recording to a worker: ring busy 62 to 40 %, worker 27 %, no fps change at the time (GPU or game bound) | backlog t201 |
| Fences are already retired when the ring processes the packet (`PM4_EVENT_WRITE_SHD` writes the fence and notifies at once), not after GPU completion. Idea C25 is therefore in place | `me_native_system.cpp` lines ~1159-1168 |
| Pipeline compile on the ring: 81 ms per new pipeline (cold), prewarm thread for warm starts. Texture creation and uploads happen on the ring as well (`[hitch] ring: textures ...`) | performance-history.md section 6, console logs |

### 1.5 Audio, XMA, PhysX and the host runtime

| Fact | Source |
|---|---|
| Audio mixer guest thread 29 to 24 % of a core at stock (t218, lock-free atomics). Hot kernels: `sub_82AA53C0` ramped mix 6.4 % of the mixer thread, `sub_82B4D580` smoother 17 % (Y10, native NEON, bit-exact, x3.3 / x5.4 on the Mac); `sub_82AAFB90` resampler 1.1 % of a core and `sub_82AA2A00` reverb-like routine 5 % of a core (Y12, not done) | backlog Y10-Y12 |
| ME2 comparison: Wwise thread about 52 % of a core in heavy Citadel; register-local clones of two leaf DSP functions were x2.58 (`sub_82E86220`) and x6.15 (`sub_82E864F0`), fuzzed with 0 failures | `mass-effect-2-recomp/docs/me2-backlog.md` S21, S23 |
| XMA at 1785 MHz (Anderson runs, 2026-10-07): worker 520 contexts in 109 ms per 10 s (about 1 % of a core); kick thread 4-7 ms per 10 s | `mass-effect-recomp/run/me1/and_fix1/console.log` `[xma]` lines |
| Host pollers (Y1-Y9) are mostly done: event-driven `WaitMultiple`, APC-woken alertable waits, vblank sleep, non-blocking guest clock. Together about -3 core-ms per frame at stock (t251) | optimization-paths.md section 4 |
| Still visible in smoke2: `UpdateGuestClock` 52,454 calls/s (4 contended), timestamp timer 1016 ticks/s (Y7 to 4 ms is open), critical-section host waits 228/s | smoke2 "system per second" |

### 1.6 What the 1785 MHz logs say (no profile, frame counts only)

| Run | Result | Source |
|---|---|---|
| Stationary Eden Prime, CPU 1785 / GPU 768 / memory 1600 | 26.0-26.2 fps per 10 s, very steady | `docs/renderer-console-check.md` |
| Normandy | about 29.6 Swaps/s, GPU per Swap about 35 ms | `mass-effect-recomp/docs/me1-30fps-no-cuts.md` |
| Anderson cutscene, bisect builds (2026-10-07) | 14 Swaps/s at 71 ms GPU per Swap with a 1.4 ms gap; `edram_alias` 45.7 ms of it. This view is GPU bound (and the builds were diagnostic) | `run/me1/and_fix1`, `and_g_p0` console logs |
| Cockpit A/B (`ab_c_good`) | 22-30 Swaps/s, GPU per Swap 33-46 ms, gap 1.4-35 ms | `run/me1/ab_c_good/console.log` |

So at 1785 MHz, views with a small gap are GPU bound and CPU work does not raise fps there. CPU work pays off in views with a large
gap (GPU waiting for the CPU) and in hitches. The new profile has to tell these apart: per 10 s block, compare GPU busy (GPU per Swap
minus gap) with the frame time, and look at the per-thread CPU.

## 2. How to capture a fresh per-thread stack profile (current build)

The profiler is compiled into every build (`sdk/src/ui/switch_perf.cpp`, a constructor starts it). It always writes the per-thread CPU
and counters. Stack sampling is switched on by a **flag file**. No toml line is needed.

1. **Build and keep the ELF.** `tools/build_nro.sh` writes `out/nx/masseffect-nx.nro` and the unstripped ELF `out/nx/masseffect`
   (about 91 MB, symbols plus `-g1` line tables for the app sources). Symbolize only with the ELF of the same build.
   `switch_cycle.sh` copies `${NRO%.nro}` (with a trailing `-nx` removed) to `OUT/masseffect.elf`. If you pass `--nro` with another
   path, put the ELF next to it under that name or copy it by hand.
2. **Create the flag** (empty file) next to the logs: `sdmc:/switch/masseffect-nx/logs/rex/stacks_profile.flag`
   (`RexSwitchLogDir()` = `<NRO folder>/logs/rex/`). From the masseffect-nx root, in bash:
   ```bash
   source tools/console-test/common.sh          # reads tools/console-test/credentials.env
   f=$(mktemp); : > "$f"
   ftp_put "$f" logs/rex/stacks_profile.flag     # --ftp-create-dirs creates logs/rex if needed
   ftp_list logs/rex                             # stacks_profile.flag must be listed
   ```
3. **toml.** Use the production `app/masseffect.toml` unchanged, so the profile matches the timing runs. Optional, in a separate run:
   `masseffect_d3d_stopwatch = true` (inclusive time of the D3D entry points on the render thread). Never put a key in twice: the
   toml parser rejects duplicates and the whole file is then ignored. `switch_cycle.sh` builds the run toml without the keys it manages.
4. **Clocks.** The user's sys-clk profile must give CPU 1785 / GPU 768. Each block prints a line like
   `clocks: CPU 1785.0 MHz, GPU 768.0 MHz, memory ...`. Do not use a run whose line says otherwise.
5. **Run.** For example
   `tools/console-test/switch_cycle.sh prof1785_eden 180 --no-movies` (Eden Prime resume, walk and turn), and for heavy CPU views
   `--route FILE` over the grass field, or `--map BIOA_STA00` (Citadel). The sampler starts 8 s after boot. It pauses one thread per
   millisecond, round-robin over all registered threads (about 30). So each thread gets roughly 30 samples per second. Play at least
   180 s and aggregate all blocks. Sampling costs a few percent: never quote fps from a profile run.
6. **Where the output lands.** On the SD card `/switch/masseffect-nx/logs/rex/rex_profile.log`. It is rewritten at every boot, so fetch
   it before the next launch. `switch_cycle.sh` downloads it to `out/console-test/NAME/profile.log`, the game log to `game.log` and
   the ELF to `masseffect.elf`. The older-tree scripts (`mass-effect-recomp/tools/me1_anderson*.sh`, `me1_newgame.sh`) download only
   the game log. With them, also run `ftp_get logs/rex/rex_profile.log NAME.profile.log`.
7. **Remove the flag** afterwards: `ftp_del logs/rex/stacks_profile.flag`.
8. **Read the header lines first.** In each block look for `-- exclusive core 2 for "XThread..."` and `-- priority 0x2B for "..."`.
   `ApplyExclusiveCore` pins the first XThread above 60 % of a core and raises the next one above 35 %, once, at a 10 s report.
   At 1785 MHz neither may trigger in gameplay, or the pin may land on a loading thread. If they are missing or wrong, that alone is
   a finding (see item 1 in section 3).
9. **Core-ms per frame:** `python3 tools/console-test/cpu_per_frame.py out/console-test/NAME` (total, main, ring).
10. **Symbolize.** The report prints addresses as `image+0x...`, which are ELF addresses (the ELF is linked at 0). Generated functions
    come out as `sub_XXXXXXXX` (the hook) or `__imp__sub_XXXXXXXX` (the recompiled original). The ME2 scripts
    (`mass-effect-2-recomp/tools/me2_symbolize_profile.py`, `me2_profile_deep.py`) parse the Spanish report of the older SDK
    (`modo:`, `-- hilo`, `imagen+0x`, `== durante`) and do not match this build's English report. Use this script instead (save it in
    the scratchpad; it was tested against `smoke2/masseffect.elf` with the local `devkitpro/devkita64:latest` image, no download):
    ```python
    # usage: python3 sym_profile.py profile.log masseffect.elf [TOP]
    import collections, pathlib, re, subprocess, sys, tempfile
    log, elf = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    blocks = [b for b in open(log, errors="replace").read().split("\n==== ")[1:] if "mode: normal" in b.split("\n")[0]]
    self_ = collections.defaultdict(collections.Counter); incl = collections.defaultdict(collections.Counter); cpu = collections.Counter()
    for blk in blocks:
        cur = None
        for l in blk.split("\n"):
            m = re.match(r'-- thread \d+ "([^"]*)": CPU ([0-9.]+)%', l)
            if m: cur = m[1]; cpu[cur] += float(m[2]); continue
            if l.startswith("== during the hitches"): cur = None
            if not cur: continue
            m = re.match(r"\s+([0-9.]+)%\s+pc image\+0x([0-9a-f]+)", l)
            if m: self_[cur][int(m[2], 16)] += float(m[1]); continue
            m = re.match(r"\s+([0-9.]+)%\s+stack (.*)", l)
            if m:
                for a in set(int(x, 16) for x in re.findall(r"image\+0x([0-9a-f]+)", m[2])): incl[cur][a] += float(m[1])
    addrs = sorted({a for d in (self_, incl) for c in d.values() for a in c})
    with tempfile.TemporaryDirectory() as d:
        pathlib.Path(d, "a.txt").write_text("\n".join(hex(a) for a in addrs) + "\n")
        pathlib.Path(d, "e.elf").write_bytes(pathlib.Path(elf).read_bytes())
        subprocess.run(["docker", "run", "--rm", "-v", f"{d}:/w", "devkitpro/devkita64:latest", "sh", "-c",
                        "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e /w/e.elf < /w/a.txt > /w/s.txt"], check=True)
        out = pathlib.Path(d, "s.txt").read_text().split("\n")
    name = {a: out[2 * i].replace("__imp__", "") for i, a in enumerate(addrs)}
    n = max(len(blocks), 1)
    print(f"{n} 'normal' blocks")
    for t in sorted(cpu, key=lambda t: -cpu[t]):
        if cpu[t] / n < 1.0: continue
        print(f"== {t}: average CPU {cpu[t] / n:.1f}%")
        for title, src in (("self (pc)", self_[t]), ("in stack (inclusive, waits included)", incl[t])):
            per = collections.Counter(); tot = sum(src.values()) or 1
            for a, p in src.items(): per[name[a]] += p
            print(f"   -- {title}")
            for f, p in per.most_common(top): print(f"   {100 * p / tot:5.1f}%  {f[:110]}")
    ```
    For a single address: `docker run --rm -v "$PWD:/w" devkitpro/devkita64:latest /opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line -f -C -e /w/masseffect.elf 0x16bda8`.
    Notes: only the top 30 stacks per thread and block are printed, so inclusive shares are lower bounds. Stacks come from the x29 frame
    chain and start at the caller frames: the pc is the leaf and `lr` its caller. Kernel waits are listed separately (`wait` lines,
    grouped by the first five frames), so a sleeping thread is not counted as running. Threads named `?` are merged by the script.
    Identify the render thread by its stack (the UE3 rendering-thread loop), not by its `XThread` name. Also read the
    "during the hitches" section: it shows what each thread was doing in frames over 45 ms.

What to extract from the first 1785 MHz profile, in order:
1. Per-thread core-ms per frame in light, heavy (grass, Citadel) and hitch windows, plus the share of samples waiting in the kernel.
2. Whether the main thread is ever pinned and the render thread raised (step 8).
3. Main thread inclusive top 30 (by caller frames), to find subsystems (item 3), not single leaves.
4. The share of the main and render threads spent in `sub_822FE760`, `sub_8222C768` / `sub_8222FA98`, `sub_826E7C98` and the PhysX
   join `sub_82320F80`: the chain waits.
5. Ring thread self top 20 and the C20 worker share.
6. `msr fpcr` samples (the FPCR writes from `setcsr`), visible as pc samples inside `enableFlushMode` / `disableFlushMode` inlines (item 5).

## 3. The five large CPU changes, ranked by expected gain per effort

Gains are estimates from the stock-clock data scaled to 1785 MHz. Each one is behind a setting, default off, and is measured A/B back
to back (warm), then cold, as [measuring.md](measuring.md) requires.

### 1. Thread placement and priorities by role, using the idle part of core 2 (threading)

**What.** Replace the CPU %-threshold heuristic with fixed roles, and move the helper work off cores 0-1:
- Identify threads by role, not by CPU %: the main thread (the guest thread that runs the game loop; check it by its entry or by its
  stack) and the render thread (its rendering-thread loop). `ApplyExclusiveCore` in `sdk/src/ui/switch_perf.cpp` uses thresholds
  (`s.cpu < 60.0`, `s.cpu < 35.0`) and a `static pinned` that is decided once. At 1785 MHz these thresholds may not be met, and a
  loading-time pin can hit the wrong thread.
- Main thread on core 2 at a raised priority (for example 0x2C). Put the helpers that are now on cores 0-1 on core 2 below it
  (0x2E-0x30): the C20 deferred-recording worker (created with `threadCreate(..., 0x2C, -2)`, which leaves it on one core: the profile shows mask 0x1), the XMA decoder, the audio worker, the audio mixer
  guest thread, the PhysX thread and the streaming threads. They then run only while the main thread waits. It waits about 20-40 %
  of the time (in `sub_822FE760` for the render thread, and in the PhysX join). Cores 0-1 are left to the ring and the render thread,
  the two stages of the critical chain.
- The main thread's waits must **sleep, not yield**, or the lower-priority helpers never get core 2: on Horizon a yield only gives
  the core to threads of the same priority. `sub_822FE760` already sleeps `masseffect_wait_game_us` (100 us) after a few yields. Check
  the PhysX join yield (`me_physx.cpp`) and the other `Sleep(0)` paths. Where the writer of the polled word is known (render-thread
  fence counter), wake the waiter directly as `masseffect_wait_blocking_ring` does for the ring fence.

**Files.** `sdk/src/ui/switch_perf.cpp` (`ApplyExclusiveCore`, `RexSwitchPerfExclusiveCore`), `app/src/native/me_resolution.cpp`
(`masseffect_exclusive_core`), `app/src/native/masseffect/masseffect_deferred_recording.cpp` (worker creation: core and priority),
the SDK thread creation for XMA and audio (`sdk/src/core/threading_switch.cpp`, the audio system), `app/src/native/me_ring_wait.cpp`,
`app/src/native/me_physx.cpp`. New cvar, for example `masseffect_thread_roles` (0 = today's heuristic).

**Expected gain.** At 1785 MHz about 10 core-ms per frame (C20 worker ~4.2, audio ~4.6, XMA ~1.4) leave cores 0-1. That removes the
preemption of the ring and the render thread, which set the frame time in the CPU-bound views. Similar moves gave +1.8 fps (t225,
render-thread priority) and +1 fps (t227, PhysX yield with migration) at stock. Estimate: +1-3 fps in CPU-bound views and fewer dips.
Effort 2-4 days. Risk: starvation of a helper (audio underruns: watch the profiler "audio:" line, "requests without data"), priority
inversion on locks shared with the main thread.

**Verification.** A/B with the new cvar on and off, back to back, same route, 2+ runs each: per-thread core-ms per frame and kernel-wait
share from the profile; the share of 10 s blocks under 25 fps; `[hitch]` counts per 10 s; audio underrun counters. Then a cold run.
Also confirm in the profile that the roles were assigned (new log line naming each thread and its core and priority).

### 2. Finish the native audio path (Y10 on, Y12 natives, register-local clones)

**What.** (a) Turn on the already validated `masseffect_audio_dsp_native = 1` in the production toml (it is absent today). (b) Write
natives for the remaining mixer kernels of Y12: resampler `sub_82AAFB90` (1.1 % of a core at stock), wrapper `sub_82AA54E8` (0.6 %),
the reverb-like routine `sub_82AA2A00` (5 % of a core, 1468 assembly lines). (c) For leaf functions of the mixer family
(`0x82AA13C0`-`0x82ABxxxx`) use mechanical register narrowing: `tests/hot_fuzz/autonative.py` derives a clone with every volatile
register as a local, exact by construction. The ME2 equivalent (`mass-effect-2-recomp/tools/me2_gen_leaf_native.py`) gave x2.58 and
x6.15 on Wwise DSP leaves with the same codegen options as ME1 (diet, nal). Pick the targets from the audio thread's self profile.

**Files.** `app/src/native/me_audio_hooks.cpp`, `app/src/native/me_audio_dsp.h`, `app/src/native/hot/` (new `n_<addr>.h`),
`app/src/native/me_hot_guest.cpp` (registration with `ME_HOT_HOOK`), `app/masseffect.toml`.

**Expected gain.** The audio thread is about 8 core-ms per frame at stock (smoke2), about 4.6 at 1785 MHz. Y10 alone was estimated at
-3 to -4 core-ms at stock. With Y12 and clones the thread could halve, saving about 2-3 core-ms per frame at 1785 MHz on the contended
cores (less if item 1 already moved it to core 2). Effort 1-3 days. Risk very low: guarded, bit-exact.

**Verification.** `tests/audio_dsp/run.sh` (bit-exact against the recompiled originals), `tests/hot_fuzz/build.py <case>` and
`liveness.py` for each new clone. On the console first `masseffect_audio_dsp_native = 2` (validate mode: the profiler's system line must
show the calls and 0 mismatches), and `[hot] guard OK` lines with no `[hot] DIFFERENCE` for the clones. Then mode 1 A/B: audio-thread
core-ms per frame, audio underruns, listening test.

### 3. Profile-driven native rewrite of main-thread subsystems (less work, not faster leaves)

**What.** The main thread's self profile is flat, and single-leaf natives stopped paying (t281-t284). The leaves that keep showing up
point at a few subsystems whose callers repeat work every frame:
- **Object iteration with a class filter** (`sub_82210970` object iterator advance, 1.11 %; Cast checks `82270C78`, `822E3158`,
  `822B9200`). If this is UE3's object iterator walking the whole object array to find the objects of one class, a native per-class
  index removes an O(all objects) scan per use: an algorithmic gain, not a constant factor.
- **String and hash lookups** (`wcscmp` 1.82 %, `wcsicmp` 1.19 %, hash finds 1.92 %, object hash lookup 1.38 %). String compares in a
  per-frame path usually mean a name lookup that could be cached at the caller (memoize by the caller's key, invalidate on the
  known writer).
- **Particles** (distribution lookup `824DD848` 3.56 % main, sprite sort `82654030`, sprite vertices `8264C7C0` 2.39 % render,
  plane reject `8262CFC0`). Replacing the per-emitter update or vertex fill with a NEON native of the whole emitter loop (not only its
  leaves) removes the call and the context traffic between them.

The first 1785 MHz profile (section 2, "inclusive top 30") decides which caller to rewrite. Take one subsystem at a time and only if
its inclusive share is at least 5 % of the main thread.

**Files.** New `app/src/native/hot/n_<caller>.h` plus registration in `me_hot_guest.cpp`. A data-structure change (per-class index)
needs hooks on the writers too (object creation and destruction). Layouts can be derived the way the ME2 ports derived them
(me2-backlog P5g, P5h).

**Expected gain.** Unknown until the inclusive profile exists. A subsystem at 10 % inclusive that becomes 3x cheaper is about 1.2 ms of
main thread per frame at 1785 MHz. If the per-class index removes whole scans, the gain is larger. Effort 1-2 weeks per subsystem.
Risk medium: callers have side effects (allocation, virtual calls). Keep those as guest calls, as the skin-rebind native does.

**Verification.** The hot-hook guard (`me_hot_guest.cpp` `Check`: the native runs on a copy, memory is rolled back, the original runs,
registers, written ranges and FPCR are compared; first `masseffect_hot_guard_calls` calls and then 1 in `masseffect_hot_guard_period`).
Host differential fuzz `tests/hot_fuzz` (random registers and memory, NaN canonicalisation; mutation checks as in round 2). For an
index or cache that replaces a scan: a self-check mode that runs both and compares the result sets for the first N uses. Console:
main-thread core-ms per frame A/B (noise about 0.3-0.7 ms per thread, so run 2+ pairs), captures per leg.

### 4. Ring thread as a two-stage pipeline, and creation work off the ring

**What.** The ring thread is one serial consumer. The render thread waits for it at Swap (6-8 ms at stock), and a frame's draws
become GPU work only after the ring has done PM4 parsing, register diffing, shader identity, guest-memory snapshots (vertex/index
copies, fingerprints, dedupe), EDRAM mode-4 ownership and Vulkan state, in that order, for every draw. Split it in two:
- **Front stage** (the current ring thread): PM4 parse, register shadow, shader identity, and every read of guest memory (vertex and
  index copies into the upload buffers, constant blocks, fingerprints). After it, the draw no longer depends on guest memory, so the
  RPTR write-back and fence writes stay where they are now: the game's view of the GPU does not change.
- **Back stage** (a new thread, or the C20 worker grown into it): EDRAM tile ownership and transfers, descriptor and pipeline
  selection, `vkCmd*` recording.
- Move pipeline creation (81 ms cold) and texture image creation and upload (`[hitch] ring: textures`) to the back stage or a worker
  with a wait only at first use. That removes those stalls from the stage the game waits on. Asynchronous compile with skipped draws
  (t121) is not this: the back stage still waits, but the front stage and the game keep going.

Do not move guest-memory reads to a later thread: that is what broke `masseffect_native_uploads_thread` (dynamic UI and movie vertex
buffers are rewritten before a deferred copy reads them).

**Files.** `app/src/native/me_native_system.cpp` (`RingLoop`, `Packet`), `app/src/native/masseffect/masseffect_native_draws.cpp`
(draw translation, copies), `masseffect_native_targets.cpp` (EDRAM mode 4), `masseffect_deferred_recording.cpp` (existing producer and
worker queue, already lock-free and stress-tested).

**Expected gain.** The front stage keeps roughly the guest-memory and parse part (the copy, fingerprint, PM4 and identity costs in
ring-cpu.md add up to about half of the ring time). The ring stage the game waits on drops from about 16 to about 8-10 ms per frame at
1785 MHz, and the Swap wait shrinks. Matters most in heavy views (about 2500 draws per frame) and hitches. Estimate +1-3 fps in
CPU-bound heavy views. Effort 2-3 weeks. Risk high: ordering between EDRAM state and draws, resolve and copy ordering, a third busy
thread on cores 0-1 (do item 1 first so it has room).

**Verification.** `masseffect_native_verify_n` self-checks for the moved pieces; `tests/cpu` deferred-recording queue stress test
extended to the new stage; Mac host image-identity run; console A/B with captures per leg and consecutive-frame recordings (temporal
correctness); ring and back-stage core-ms per frame, Swap wait time, `[hitch]` counts.

### 5. Whole-program codegen: FPCR mode switches and automatic leaf narrowing

**What.** Two codegen-level changes that touch every guest thread, measured before they are built:
- **FPCR flush-mode switches.** Generated code calls `ctx.fpscr.disableFlushMode()` before scalar FP (41,767 call sites) and
  `enableFlushMode()` before VMX128 float ops (377 sites). Each real mode change is an `msr fpcr`, a serializing write on the A57.
  Statically only 170 of 47,649 functions mix both modes (355 alternations inside functions), but the switches also happen across
  calls (a vector function, then a scalar one). The dynamic count per frame is unknown. Measure it first: a counter in
  `FPSCRRegister::setcsr` (`sdk/include/rex/ppc/context.h`) reported in the profiler's system line, or the `msr fpcr` pc samples in the
  profile. If it is high (tens of thousands per frame), options: (a) exact: track the mode across direct calls in the codegen so
  redundant switches disappear at function entry; (b) FZ always on (changes results only for denormals: an "image-changing" variant,
  to be measured and labelled as such).
- **Automatic leaf narrowing.** Run the `tests/hot_fuzz/autonative.py` transformation (volatile registers of a leaf function become
  locals, exact by construction) over every leaf function as a post-codegen pass (`tools/pch_*.py` style, with `--check` and
  `--dry-run`, run from `tools/codegen.sh` and checked by `tools/verify_pch.sh`). It targets what the diet and nal options left in
  `ctx`: arguments r3-r10, f0-f13, v0-v13. Caution: arguments in registers (t264) already removed most argument traffic across calls
  and gave nothing on top of diet and nal. So measure the static instruction delta on the hot leaves first (the t255 method) and build
  it only if it is above about 5 %.

**Files.** The codegen in the SDK (`sdk/` generator sources; the options live in `app/perf_overrides.toml`), `sdk/include/rex/ppc/context.h`,
a new `tools/pch_leaf_narrow.py`, `tools/codegen.sh`, `tools/verify_pch.sh`.

**Expected gain.** Unknown until measured. If FPCR writes are frequent, 2-5 % of all guest CPU (main, render, audio). Leaf narrowing:
the static -13.3 % on 38 hot functions from the diet gave -10 % main thread, so a further -5 % static on the leaves might give 1-3 %.
Effort: measurement 1 day, each pass 3-5 days. Risk: the denormal variant changes results; the exact variants are as safe as the diet.

**Verification.** Static: instruction and `ctx` access counts on the hot functions (as for C47/C48). Dynamic: Mac boot trace of guest
stores identical for the first 6 M stores (the args-in-registers method), differential fuzz of a sample of converted functions
(`tests/hot_fuzz`), then console main-thread core-ms A/B and captures per leg.

### Considered and not ranked

| Idea | Why not in the top 5 |
|---|---|
| Native D3D draw path (hook the game's draw calls and skip PM4 encode and decode, backlog C10) | The largest theoretical CPU cut (render thread draw bodies 5-7 ms plus most of the ring's parse and diff at stock), but a game-specific D3D reimplementation: 4-8 weeks, high risk. Revisit if item 4 is not enough |
| Batching the D3D wrapper calls / native state setters | Measured: all setters together about 0.9 ms per frame at stock (t246). The draw bodies and the flush hold the cost (item 4, C10) |
| Fourth CPU core (hbl NPDM core mask 0..3) | Researched (`mass-effect-recomp/docs/four-cores.md`), never run. The user declined changing the console's Atmosphere configuration |
| More single-leaf natives on the main thread | t281-t284: 13 more natives, no measurable gain. Only caller-level rewrites (item 3) |
| Removing guest polling waits in general | Mostly done (Y1-Y9, blocking ring and fence waits, occlusion wait, PhysX yield). The remaining part is folded into item 1 |
| Guest timestamp timer 1 ms to 4 ms (Y7), critical-section spin (Y8) | Small (about 750 wake-ups/s); Y7 changes guest-visible tick granularity. Cheap A/B, but not large |

## 4. Already tried: do not repeat without new data

| Idea | Result | Source |
|---|---|---|
| LTO (C35) | no gain (t122: 226 against 218-226 core-ms) | performance-history.md |
| PGO (C36) | no gain in a same-session pair (t161 16.7 / t162 16.6) | performance-history.md |
| `-Os` for the generated code | worse (t234: 21.8 against 24.9 fps) | performance-history.md |
| Hot function ordering (C40) | no gain (t158) | performance-history.md |
| Arguments in registers on top of diet and nal (C47) | no gain (t264: 100.1 against 98.6 core-ms); v1 froze because GCC treated read-only bodies as `pure` | optimization-paths.md |
| 13 more hot-function natives | no main-thread gain (t281-t284) | performance-history.md |
| Native `memcpy` and `wcsstr` of the CRT | within noise (t245) | optimization-paths.md |
| Native D3D state setters | about 0.9 ms total, not worth it (t246) | optimization-paths.md |
| Main thread alone on core 2 (t217); light threads allowed there (t221) | no gain at stock: the main thread was bound by its own work | optimization-paths.md |
| Game-thread wait sleeps of 30-400 us (t229); sleep in the render-fence wait (t126) | no change | optimization-paths.md |
| Blocking ring wait with the read pointer as predicate (C23, t72) | wrong predicate (10 of 3375 sleeps woke on progress); the fence-word version is in use | optimization-paths.md |
| Ring wake with a libnx event (C46, t224) | worse: the ring spun at 100 % | optimization-paths.md |
| PhysX "Fix A" (block until `simulate()` starts) | rejected (t244): the step does not run every frame, the join hit its timeout | optimization-paths.md |
| Vertex copies on a separate thread (`masseffect_native_uploads_thread`) | off: reading guest memory later races dynamic UI and movie buffers (missing letters, stray triangles) | `masseffect_native_draws.cpp` |
| Asynchronous pipeline compile with skipped draws (E32b, t121) | 4515 draws skipped (pop-in) | performance-history.md |
| Submitting every N draws (t206, t207) | no gain or worse; about 2 submissions per frame anyway | performance-history.md |
| Cross-frame vertex cache | rejected: verifying content costs as much as the copy | optimization-paths.md |
| Epoch invalidation instead of fingerprints (C4) | rejected: not provably exact | optimization-paths.md |
| Hardware CRC32 fingerprint | no faster than XXH3's NEON loop (analysis; mode 2 kept for an A/B) | ring-cpu.md |
| Vertex dedupe off (C41, t155) | 12.7 fps, ring 72 ms per frame: dedupe saves a lot | performance-history.md |
| Per-thread shader-object cache, negative cache (t184, t222) | no change | backlog |
| Constant-register run fast path (t125) | no change | backlog |
| Official CPU boost mode (`appletSetCpuBoostMode`) | unusable: the GPU drops to 76.8 MHz (t243) | platform-notes.md |
| XMA context zero-fill removal (Y13) | rejected: the cost is the FFmpeg decode | optimization-paths.md |
| Guest atomics without the global lock (C44) | kept but fps within noise (t218); audio thread 29 to 24 % | optimization-paths.md |
| Lower internal resolution on a CPU-bound frame (800x448) | no fps change (t235) | performance-history.md |
| ME2: 4th core, Wwise thread natives | 4th core declined by the user; Wwise clones x2.58 / x6.15 built, console A/B pending in ME2 | me2-backlog.md S23 |

## 5. Suggested order

1. Profile at 1785 MHz (section 2): one light route, one heavy route. Check the pin and raise lines. One day.
2. Item 2 (a), turning on validated audio DSP natives: a one-line toml change, A/B.
3. Item 1, thread roles: largest gain per effort if cores 0-1 are the contended ones in the profile.
4. Item 5 measurement (FPCR counter) in the same build as item 1: one counter, no behaviour change.
5. Item 3 on the subsystem the inclusive profile names; item 2 (b, c) in parallel (host-only work, fuzz first).
6. Item 4 only if, after 1-3, CPU-bound heavy views still dip below 30 fps and the ring or Swap wait is the reason.

Record every result, also "no change", in `mass-effect-recomp/docs/optimization-backlog.md` and in [optimization-paths.md](optimization-paths.md).
