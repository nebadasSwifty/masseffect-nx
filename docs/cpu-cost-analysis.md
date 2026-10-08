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

## 2026-10-07: render fence waits measured (masseffect_fence_stats, RU, 960x544, Normandy cockpit, 1785/768)

The game thread's `FRenderCommandFence::Wait` calls (`sub_822FE760` EN / `sub_822FE560` RU), per 10 s at ~290 fps-frames:

| Fence | n | Calls / 10 s | Calls that slept | Time waiting / 10 s | Max |
|---|---|---|---|---|---|
| 0x82EB0D70 | 1 | ~290 (one per frame) | 92-98 % | 2.7-3.1 s (~10 ms per frame) | 53-96 ms |
| 0x82EB0F60 | 0 | ~580 (two per frame) | 1-2 % | 60-180 ms | 46-52 ms |
| heap objects | 0 | one-off (resource release) | 0 | 0 | 0 |

- The per-frame fence already lets the render thread lag one frame (n = 1), and the game thread still waits ~10 ms per frame on
  it. The game thread is therefore not the limit: the UE3 render thread (which itself waits for the ring thread at Swap) is.
  Raising n to 2 would only smooth spikes; the steady cost is in the render and ring threads (PM4 to Vulkan translation).
- The full flushes (n = 0) are cheap in total but explain single 50 ms hitches.
- The earlier `me_fence_stats.cpp` never ran: `me_ring_wait.cpp` already hooked the same function and the linker kept one
  definition silently. The statistics now live in `me_ring_wait.cpp`; `tools/edition.sh` fails on duplicate `REX_HOOK_RAW`.

## 2026-10-07: where the Normandy route loses frames (RU, 960x544, all GPU changes on)

- The cockpit itself runs at the 30 fps cap (295-300 Swaps per 10 s, 1-5 hitches per 10 s).
- The losses are in the walk windows with 700-850 draws per frame (vs ~140 in the cockpit): 22-23 fps with 40-55
  frames over 60 ms per 10 s, while the GPU is busy only 10-14 ms per frame there.
- Profile of those windows (heap1, sections with game fps <= 27.5): main thread 56 % CPU, UE3 render thread 21 %,
  ring thread 40 %. No thread is saturated: the frame is lost in the hand-offs. The render thread sleeps ~60 % of the
  time in the D3D ring wait (sub_8222F888 RU), ~3200 sleeps per 10 s of which only ~25 % are ended by ring progress,
  the rest by the 2 ms `masseffect_wait_ring_max_us` timeout.
- Ring thread hot spots in those windows: XXH3 hashing ~17 % (XXH_memcpy + hashLong), spdlog/fmt ~8 % (the
  synchronous file logger), malloc/free ~5 %.
- Experiments queued: `masseffect_wait_ring_max_us = 200` and `masseffect_wait_blocking_ring = false`.

## Feros firefight profile 2026-10-08

Source: `mass-effect-recomp/run/me1/manual2/rex_profile.log` (RU build `run/me1/ru_loc2.elf`, toml `run/me1/manual_best.toml`,
CPU 1785 / GPU 768, stack sampling on, ~30 samples per second per thread, 17 blocks of 10 s, 21.6-31.2 fps) and its
summary `profile_summary.txt`. Offline analysis; nothing here was run on the console. RU addresses unless marked EN
(EN = key of `editions/ru/address_map.json`, RU = value).

Tools used (reusable): the RU guest image can be rebuilt from the RU `default.xex` (not encrypted, "basic" compression:
concatenate the data blocks of optional header 0x3FF and append the zero runs; the result is the loaded image at
0x82000000, 16,515,072 bytes). With it, the `lis/addi` constants of a generated function resolve to strings and vtables.
Symbolize the 10-frame stacks with `nm -n -C` of the ELF (llvm `nm` on macOS reads the aarch64 ELF) and a bisect lookup.

### 1. Guest physical allocations on the render thread (BaseHeap::AllocRange)

Stack (render thread, all samples in this function):

```
BaseHeap::AllocRange (pc 0x7fbc: the top-down "base page not free" loop) <- PhysicalHeap::AllocRange
  <- MmAllocatePhysicalMemoryEx <- sub_82811C78 / sub_82811E90 (XPhysicalAlloc)
  <- sub_82224318 (D3D CreateTexture; EN 82224528) <- sub_82251628 (FGFxTexture resource InitDynamicRHI)
  <- sub_823D2030 (FRenderResource::InitResource) <- sub_82251280 (GFx texture: InitDynamicTexture(w, h, format))
  <- sub_82B4F998 (Scaleform GRenderer: CreateTexture + InitDynamicTexture) <- sub_82B4FCC8 (GImageInfo::GetTexture,
     creates the texture when the image has none cached)
```

- vtable 0x820C8C90 = { 82251628 InitDynamicRHI, 82251B98 ReleaseDynamicRHI, 82362280, 82362280, 823D2030 InitResource,
  823D20F0 ReleaseResource, 8223D480, 82251108 dtor, 82251280 InitDynamicTexture, ... }: the UE3-side texture of the
  Scaleform (GFx) renderer. Every GFx image without a cached texture (HUD icons, dynamic images, font cache pages) creates
  a new D3D texture: release the old one, `CreateTexture`, `XPhysicalAlloc` of the texels, upload.
- Cost: the SDK's first-fit search reads one 16-byte `PageEntry` per page of the 512 MB physical parent heap (131,072
  pages), top-down, and the top of the physical heap is the used part (`top_down` is forced for physical allocations).
  Each call walks the whole used top until it finds a hole: on a host model with 3/4 of the heap used, 16 us per call on
  an M-series core; several times that on the A57. Share: 2-3 % of the render thread's busy samples over the run, 7 % in
  the hitch frames (two 3.4 % leaves), 85 % of one 10 s block (a HUD/menu change). It also holds the heap mutex during the
  scan.
- Fix (done, SDK): `free_bits_`, a free-page bitmap in `BaseHeap`, kept in step with every page-table writer
  (Initialize, Reset, Restore, AllocFixed, AllocRange, Decommit, Release), and `FindFreeRangeBitmap`, the same first-fit
  candidate sequence as the page-table scan but skipping 64 used pages per load. The chosen address is identical (host
  test: 1.2 M random searches against the original loop, all page counts, strides 1-32 and odd strides, top-down and
  bottom-up, unaligned low bounds, 0 differences, ASan/UBSan clean). Host benchmark of the case above: 16.3 -> 1.95 us per
  call (x8.4).
  - cvar `heap_free_bitmap` (default false): use the bitmap search.
  - cvar `heap_free_bitmap_verify` (default false): run both searches, use the page-table result, log the first 32
    differences ("heap_free_bitmap mismatch"). Run once with both on, then keep only `heap_free_bitmap = true`.
  - Files: `sdk/include/rex/system/xmemory.h`, `sdk/src/system/xmemory.cpp` (`LinearFindFreeRange` is the old loop,
    unchanged).
  - Expected: removes the 2-3 % (7 % in hitch frames) of the render thread; the HUD-change blocks lose most of their
    stall. The bitmap costs 16 KB for the physical heap, the bit updates run inside the loops that already touch every
    page of the range.
- Not done (game level): a texture cache for the GFx path (reuse a released texture of the same size and format
  instead of `CreateTexture` + `XPhysicalAlloc`). It would also save the ring's new-texture work (`PrepareTexture`,
  hashing). Worth it only if the console run with the bitmap still shows `sub_82251280` in hitch frames.

### 2. Logging is a cross-thread stall (found in the same profile)

- Ring thread: `fsdev_write` 13.2 % of the hitch-frame samples (3 % overall), plus `fsdev_seek`, `fflush` and the sink
  mutex: ~260 of 5,087 ring samples (5 % of its wall time, counted as "waiting in the kernel", so it does not show in the
  busy %). The writers are `AcceptVertexShaderIdentity` / `IdentifyShader` / `PairDraw` (the "VS identity mismatch"
  warnings: 2,252,544 mismatches in the 14-minute manual1 session, ~2,700 per second, every 256th logged at warn level,
  which flushes the file at once), `TargetsVulkan` and `DrawsVulkanImpl` messages.
- The rotating file sink writes to the SD card while holding the sink mutex. The render thread (`pthread_mutex_lock` 1.4 %
  of its hitch samples, from the Swap ring-wait log in `sub_8222F888`) and the main thread (the hot-guard "guard OK after
  N checks" lines) block on that mutex while the ring thread is inside an SD write.
- Proposals (not done here, the ring code belongs to the parallel ring work): rate-limit the identity mismatch log by
  time (one summary line per 10 s), log it at info so it does not trigger the flush, and stop the per-64-checks "guard OK"
  lines after the first one; longer term an asynchronous logger (a queue drained by a low-priority host thread on core 0).
  The mismatch path itself (`DumpLoadedVertexVariant`, `LogVertexWordDiff`, XXH3 of the candidate) runs for every
  mismatch, ~90 times per frame: check whether it can be skipped once a (loaded hash, candidate) pair has been seen.

### 3. What the hot guest functions are (RU addresses; names by structure, callers, vtable slots and strings)

Shares: "hitch" = samples inside frames over 45 ms, "all" = whole session; incl = inclusive. No class-name strings are
referenced by these functions (UE3 class names live in the package name tables, not in code), so the names below come
from what the code does, its callers and the already identified neighbours (`me_crowd_cpu.cpp`, `me_console_exec.cpp`,
`me_physx.cpp`, the hot natives).

Game main thread (`XThread650D3960`, core 2):

| RU | Identity | Leaf? | Share | Notes / action |
|---|---|---|---|---|
| 82216CE0 / 82216B48 / 82214F08 | guarded main loop / FEngineLoop::Tick (82214F08 waits on the render fence `sub_822FE560`) | no | 21-26 % incl | frame driver |
| 825E74B8 | UGameEngine::Tick (vtable slot 71; hooked by `me_console_exec.cpp`, hence `__imp__`) | no | 22-28 % incl | |
| 82486A80 | UWorld::Tick: tick groups (pre-async, physics `sub_823211F8`, during/post-async). Its own 2.3-2.7 % leaf is the per-component loop `for c in list: if (!c->vfunc14() && (c->+80 & 0x80000000)) c->Tick(dt)` (vfunc 77): the samples sit on the loads of the object's vtable, its flags at +80 and the indirect-call table entry | no | 20-25 % incl | memory latency, not code; see codegen item below |
| 82485D78 | UWorld::TickActors for one tick group (FActorIterator: six calls of the native iterator `sub_8225C688`) | no | 10 % incl, 1.7 % leaf | |
| 82485A40 | per-actor tick dispatch (actor->Tick, then its components) | no | 8-9 % incl | |
| 824C1F48 | USkeletalMeshComponent::Tick (vtable slot 77; hooked by `me_crowd_cpu.cpp`) | no | 8 % incl, 2.2-2.5 % leaf | `masseffect_skel_skip_unrendered` |
| 824C6A40 | USkeletalMeshComponent::UpdateSkelPose (bone blend + space bases; reaches PhysX `sub_82988B10` for physics-asset bones) | no | 3 % incl, 2.2 % leaf | 832 instructions, many calls; not a leaf |
| 824C1D58 | tick of the anim tree / skel controls (`sub_8269A768`) | no | 1 % leaf | |
| 823EFA00 | UPrimitiveComponent transform update (vtable slot 71 = byte 284 of ~20 component vtables; 7 indirect calls) | no | 1-2 % leaf | called from skeletal mesh (`sub_824C1508`) and `sub_824D8960` |
| 8299D160 | PhysX error-stream report; with `masseffect_physx_no_warnings` the samples are the yield of the `fetchResults` poll (`sub_823211F8` -> `sub_82BF8270` -> `sub_8299C5A0`) | - | 1.7-3.2 % leaf | waiting for the PhysX threads, not work |
| 8244C978 | builds the names of all properties of an object's class into a TArray<FString> (TFieldIterator over `Class->Children`, `"%s.%s"` for struct members, `"<uninitialized>"`); `sub_82460BD0` is TFieldIterator::operator++ | yes-ish | 2 % leaf (hitch) | called twice per tick from `sub_82952530` (vtable slot 220 of a Bio actor class, ticked through slot 89 `sub_82955B38`), which then string-compares every name (`sub_829766D0`) with a list of track names and writes a float property through `sub_8244D188`. That is O(properties x tracks) string building and wcscmp per actor per frame: 2 % (all) to 4 % (hitch) of the main thread. Fix belongs at the caller: cache the name -> property offset per (class, track list) |
| 82211498 | FMallocXenon::Malloc (pooled small-block allocator; large blocks go to `XPhysicalAlloc` / `NtAllocateVirtualMemory`) | yes | 1.2-2.1 % leaf | call count, not per-call cost. Its NtAllocateVirtualMemory path ends in the same `BaseHeap::AllocRange` scan (fixed by `heap_free_bitmap`) |
| 82302B68 | called once per world tick from `sub_82487908` (post tick); walks a list and enqueues render commands (`sub_822FE5B0`) | no | 1.6 % leaf (hitch) | not identified further |
| 825579A0 | dynamic light environment update (under UDynamicLightEnvironmentComponent::Tick `sub_825599C8`) | no | 1.4 % leaf (hitch) | `masseffect_dle_visible_every` |
| 82531C48 | particle emitter instance Tick (vtable slot 7; uses the distribution lookups `sub_824DE958` / `sub_824DB910`) | no | 1.2 % leaf | |
| 824D8AD8 | particle system component: update of the dynamic data sent to the render thread (from `sub_82580D98` in UWorld::Tick) | no | 1 % leaf | allocates per update (FMallocXenon) |
| 82B7F8D8 / 82BB9990 | Scaleform GFx display-list advance (mutual recursion over the sprite tree, HUD) | no | 1.2 % leaf, 2-3 % incl | |
| 8225C688, 8230D568 | already native (iterator advance, hash lookup) | | 1.5 %, 1.2 % | the cost is the call count from 82485D78 |

UE3 render thread (`XThread6511D840`):

| RU | Identity | Share | Notes |
|---|---|---|---|
| 822FE100 / 822FDEA0 / 8239E260 | RenderingThreadMain / ProcessRenderingCommands / FRenderCommand::Execute (virtual) | 12-21 % incl | |
| 824F8A20 / 824F6CF0 | RenderViewFamily_RenderThread / FSceneRenderer::Render (calls GSceneRenderTargets.Allocate `823D0CE8` and the RHI) | 10-19 % incl | |
| 824F5DB8 | FSceneRenderer::InitViews: per-primitive frustum and distance culling (plane reject `8262DBB0`, already native) | 2.5-3.3 % leaf | 973 instructions, loop over all primitives |
| 824F7D28 | base / depth pass drawing | 3-5 % incl | |
| 8264DFE8, 8264EBB0, 8264F038 | particle dynamic data (vtable 0x8217D2C0 slots 3, 21, 22): sprite and mesh emitter vertex fill, sort (`82654A68`, native) | 2-3.6 % leaf | `n_8264C7C0` already covers the sprite fill |
| 826E6AB0 | RHI shader-parameter setter (53 call sites, three D3D calls) | 2 % leaf | |
| 8266F9E8 | material shader parameters Set (uniform expressions -> `824FA968`, `82219150`, both native) | 2 % leaf | |
| 8262CF40 | drawing policy DrawMesh (D3D DrawIndexedPrimitive `82227550`) | 1.3 % leaf | |
| 824E1038 | particle system scene proxy update (copies the dynamic data, `8250C628`) | 2 % leaf | |
| 82BB9A28, 82BB61F8 | Scaleform GFx renderer (batches, 16 indirect calls) | 1.2 % leaf | |
| 82251280 -> 82224318 -> XPhysicalAlloc | GFx texture creation, see section 1 | 2-7 % | fixed by `heap_free_bitmap` |

### 4. Natives: why none were added for these functions

None of the listed functions is a pure leaf: each makes indirect (virtual) calls, allocates, or is a loop whose samples
sit on cache-missing loads of other objects (vtable, flags, next pointer). A native version of such code keeps the same
loads and the same calls, so it saves only the instruction overhead (byte swaps, which are single `rev`s on the A57),
not the misses. The earlier rounds measured exactly this (13 natives, 33.4 vs 33.6 ms, section 1.2). The only near-leaf
candidate, `sub_82460BD0` (TFieldIterator++), is under 1 % and only hot because of the per-tick name building above.
The work worth doing is at the caller level:

1. `sub_82952530`: cache the property lookups instead of building and comparing all property names twice per tick
   (2-4 % of the main thread). Needs the track list's lifetime rule (when it changes, rebuild); behind a cvar.
2. The physical/virtual allocator scan: done (`heap_free_bitmap`).
3. Logging on the ring thread: section 2.

### 5. Codegen-level observation: the indirect-call table

The host code of the hottest UWorld::Tick loop shows the virtual call as `ldr x2, [x27, (target - code_base) * 2]`:
the dispatch table has one 8-byte slot per 4-byte guest instruction (~23 MB for 11.5 MB of code), so every distinct
virtual target touches a different cache line and, often, a different TLB page; one of the four hottest sample
addresses of `sub_82486A80` is this load. Proposal (codegen + runtime, not done): a compact table indexed by function
number (47,648 functions x 8 bytes = 380 KB) with the generated code calling through a per-call-site monomorphic cache
(`if (target == site_last) fn = site_fn; else lookup`). Expected gain: a few % of the main thread in tick loops; it needs
the same checks as `REX_CALL_INDIRECT_FUNC` and a codegen change, so it is a separate task. Done (option, default off):
see "Indirect call dispatch" below.

## Indirect call dispatch (2026-10-08, option, default off, not yet measured on the console)

### Current mechanism (legacy, `MASSEFFECT_INDIRECT_DISPATCH=0`)

- Codegen (`sdk/src/codegen/builders/control_flow.cpp`): every `bctrl`, `bctr` without a jump table (tail call),
  `bnectr` and `blrl` becomes `REX_CALL_INDIRECT_FUNC(ctr.u32)`. EN: 39,448 sites (39,447 `ctr`, 1 `blrl`) in
  180 files.
- The macro lives in the generated pch (`masseffect_pch.h`, from `sdk/resources/templates/codegen/_indirect_call.inja`):
  range check `target - REX_CODE_BASE < REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE`, then
  `*(PPCFunc**)(base + IMAGE_BASE + IMAGE_SIZE + (target - CODE_BASE) * 2)`; a null slot or an out-of-range target
  stores `ctx.last_indirect_target` and calls `rex::runtime::ResolveIndirectFunction` (dispatcher hash map, else the
  fatal `InvalidFunctionTrap`). Natives use the same lookup (`app/src/native/me_hot_call.h` `CallIndirect`).
- The table is guest memory at 0x82FC0000 (EN), `(code_size + 64 KB thunk reserve) * 2` = 24.2 MB, one 8-byte slot per
  4-byte guest instruction, filled by `Memory::SetFunction` from `FunctionDispatcher::SetFunction`: at start from
  `PPCFuncMappings` (48,126 entries EN: functions, split fragments, funclets; a `REX_HOOK_RAW` hook is simply the strong
  definition of the same `sub_` symbol, so the mapping already points at the hook), later for import thunks
  (`AllocateThunk`, the 64 KB reserve after the code).
- Footprint of the 1,157 hot functions of `app/function_order.ld` (EN): 1,156 different cache lines, 758 different 4 KB
  pages, 197 different 64 KB pages of the table. Every target is its own line (8 slots = 32 bytes of guest code per line,
  median function distance 128 bytes), and the slot address depends on the target, so the load sits at the end of the
  object -> vtable -> target chain.

### New design (`MASSEFFECT_INDIRECT_DISPATCH=1` or `2`)

- Compact table (`sdk/include/rex/ppc/indirect_dispatch.h`, `sdk/src/system/indirect_dispatch.cpp`): host `.bss`,
  2^17 entries x 8 bytes = 1 MB, open addressing with linear probing, slot = top 17 bits of `target * 0x9E3779B1`.
  Entry = guest address (low 32 bits) | host function as a signed 32-bit byte offset from the anchor function
  `TargetZeroAnchor` (high 32 bits): one aligned 64-bit load gives key and value, so no torn reads between threads.
  An all-zero entry decodes to {target 0, `TargetZeroAnchor`}, and `TargetZeroAnchor` does exactly what the legacy
  macro does for target 0 (`ResolveIndirectFunction(0)`), so empty slots need no check in the hit path. EN numbers:
  48,126 entries, load 0.37, 82.5 % of the entries in their home slot (83.6 % is the maximum for this load), hot set =
  1,112 lines / 253 pages of 4 KB / 16 pages of 64 KB (legacy: 1,156 / 758 / 197).
- Mode 1: the home slot is checked inline (`ldr` + `cmp` + `add ..., asr #32` + `blr`); any other key goes to the
  out-of-line miss path (one static copy per file: probe the compact table, else the legacy lookup unchanged).
- Mode 2: one `static uint64_t` per call site (same packing; 8 bytes x 39,448 sites = 316 KB of `.bss`, only the
  executed ones are touched; the sites of one function are adjacent). The cache's address does not depend on the
  target, so its load issues in parallel with the vtable chain and the call address is ready as soon as the compare
  resolves. A miss calls the miss path, which probes the compact table and refills the site
  (cvar `indirect_dispatch_ic_refill`, default true = last target wins; false = keep the first target, no stores on
  polymorphic sites).
- Exact semantics: anything not in the compact table (mid-function targets, unregistered addresses, imports resolved by
  `ResolveIndirectFunction`, a host offset that does not fit 32 bits, a table over 75 % full) takes the legacy code path
  of mode 0, including `ctx.last_indirect_target`. Hooks (`REX_HOOK_RAW`) and natives are whatever the dispatcher map
  holds, as before.
- Maintenance: the table is built lazily on the first miss from the dispatcher map (under the dispatcher mutex; the
  build checks every entry against the guest-memory table and logs
  `indirect_dispatch: table built: N functions, ... % in their home slot, longest probe ...` and any
  `DIFFERENCE at build`). Later `SetFunction` calls (thunks) insert; a changed mapping updates the entry and clears every
  filled inline cache; a removal (`SetFunction(addr, nullptr)`, `UnregisterModule`) rebuilds the table and clears the
  caches. Mode 0 never builds it (the 1 MB `.bss` stays untouched).
- Natives: `me_hot_call.h` `CallIndirect` (EN and the RU overlay copy) uses the compact table first in modes 1/2.
- Code size (devkitA64 GCC -O3, `masseffect_recomp.169.cpp`, which holds UWorld::Tick EN `sub_82485D60` / RU
  `sub_82486A80`): mode 0 358,119 bytes of text, mode 1 359,611 (+0.4 %), mode 2 354,431 (-1.0 %: the slow path is out of
  line) + 2,288 bytes of `.bss`. With `__SWITCH__` the table and anchor are hidden symbols (adrp+add, no GOT load).

### Options

- CMake `MASSEFFECT_INDIRECT_DISPATCH` = 0 (default, the legacy macro, unchanged code) / 1 / 2, and
  `MASSEFFECT_INDIRECT_DISPATCH_VERIFY` = OFF/ON; `tools/build_nro.sh` passes the environment variables of the same
  names. They set `REX_INDIRECT_DISPATCH` / `REX_INDIRECT_DISPATCH_VERIFY` on `masseffect_recomp` and `masseffect`.
- The pch block is the SDK template; `tools/pch_indirect_dispatch.py` (run by `tools/codegen.sh`, checked by
  `tools/verify_pch.sh`) copies it into a pch made by an older generator, so no generator rebuild is needed.
- cvars: `indirect_dispatch_fast` (default true; false = every call takes the legacy lookup through the miss path, a
  run-time A/B inside a mode 1/2 build, set it in the toml), `indirect_dispatch_ic_refill` (above),
  `indirect_dispatch_verify_calls` (default 1,000,000; verify builds only).
- Verification: `MASSEFFECT_INDIRECT_DISPATCH_VERIFY=ON` computes the legacy result at every call as well, uses it for the
  first `indirect_dispatch_verify_calls` calls and logs `indirect_dispatch DIFFERENCE: guest ... fast ... legacy ...`
  (first 64) and `indirect_dispatch verify: N checks, M differences` every 2^20 checks and at the limit. Expected: 0.

### Host checks (scratch, not the console)

- Test harness (real `indirect_dispatch.cpp`, stubbed runtime; EN function list): every one of the 48,126 entries
  resolves to its function, 1,984,092 random and in-code non-entry addresses and every `entry + 4` miss, 16,384 thunk
  inserts after the build resolve, a remap clears the inline caches, a removal asks for a rebuild,
  `indirect_dispatch_fast = false` and the verify logic behave; 3 reader threads against a writer remapping 1,000 entries
  200 times: 1.6 M lookups, 0 wrong results.
- Micro-benchmark (M-series host; 4,000 sites, 75 % monomorphic, 25 % with 2-8 targets, 80 % of targets from the hot
  list, call stream of 20,000, real indirect calls): warm legacy 8.0-8.2 ns per call, mode 1 8.7-8.9, mode 2 7.8-7.9;
  caches flushed per frame 9.1-9.2 / 9.9-10.4 / 8.1-8.8; with 4 random loads from a 96 MB buffer per call (object
  misses) 10.2-10.6 / 10.9-11.8 / 9.9-10.2. Static inline-cache hit rate of that stream 81.6 %. The host's 16 MB L2 and
  big TLB hide most of the legacy table's cost, so these only show that mode 1 alone does not pay (its 18 % of
  non-home-slot targets take a call) and that mode 2 does not lose; the A57 (32 KB L1D, 2 MB shared L2, 32-entry L1 data
  TLB and 1,024-entry L2 TLB per core) is where the 24 MB table should hurt. Measure mode 2 first.

### Expected gain and risks

- Expected: mode 2 removes one dependent, often TLB-missing load per monomorphic virtual call from the critical chain;
  on the Feros profile that load is one of the four hottest addresses of UWorld::Tick. Estimate 1-3 % of the main thread
  in combat, similar on the render thread (it also dispatches through UE3 vtables); mode 1 likely nothing.
- Risks: (1) an inline-cache refill racing a remapping can leave one stale site (mappings never change after start in
  this game; the verify build would show it); (2) 1 MB `.bss` plus 316 KB of inline caches in modes 1/2; (3) switching
  the option recompiles all 180 generated files (the pch changes); (4) hot polymorphic sites with refill on store to
  the site's line on every miss (try `indirect_dispatch_ic_refill = false`).

## Guest cache hints: dcbt / dcbtst (2026-10-08, codegen option, default off, not yet measured)

- Until now the generator dropped `dcbt` / `dcbtst` (149 + 6 in the RU XEX; hot ones: UE3 GC object loop RU
  `sub_82302B68` prefetching 10 objects ahead, GC mark pass `sub_82302288`, CRT memcpy, PhysX narrowphase).
- Codegen option `dcbt_prefetch` (toml, `app/perf_overrides.toml` and the RU overlay copy, default false): emits
  `REX_DCBT(ea)` / `REX_DCBTST(ea)` with `ea = (ra|0) + rb` (32-bit wrap; RA/RB from the instruction fields, so a
  non-zero CT/TH field cannot shift the operands). The pch defines them as `__builtin_prefetch(REX_RAW_ADDR(ea), rw, 3)`
  (rw = 1 for dcbtst): AArch64 `PRFM` never faults, so any address is safe. `-DREX_DCBT_PREFETCH=0` turns them back into
  nothing without regenerating.
- Needs a generator built from this SDK (`tools/build_host.sh`; the macros come from `pch_h.inja`, the emission from
  `sdk/src/codegen/builders/system.cpp`, the key from `sdk/src/codegen/config.cpp`). Files:
  `sdk/include/rex/codegen/config.h` (`dcbtPrefetch`).
- Expected: helps the GC passes (pointer chasing over UObjects with the guest's own look-ahead), neutral elsewhere; risk:
  prefetches of lines the A57 would not use (the 360's 128-byte lines vs 64-byte host lines: one PRFM covers half of
  what the guest asked for).

## Eden Prime game-thread profile (prof_eden, 2026-10-08)

Source: `mass-effect-recomp/run/me1/prof_eden/rex_profile.log` with the ELF `run/me1/ru_glob19.elf` (RU, CPU 1785 / GPU 768,
stack sampling on, all 29 hot natives on, `MASSEFFECT_INDIRECT_DISPATCH` 2: the `rex_indirect_miss_` symbol is in the
ELF). Route: the Saren/Nihlus cutscene and the open area after it, 17 blocks of 10 s, 20.7-33.4 fps. "Slow blocks" =
the 4 blocks under 26 fps (24.6, 22.4, 20.7, 21.0 fps). RU addresses; EN = key of `editions/ru/address_map.json`.

How the numbers were taken (correcting an easy mistake): the `pc` lines of a thread are percentages of its *running*
samples (they add up to 100 % when the list is short), the `stack` lines are percentages of *all* its samples (kernel
waits included, `__syscall_nanosleep` shows up there). Self ms per frame = pc % x CPU % x 10 / fps. Stack lines begin at
the frame of the function that was running (the leaf's own caller frame is the first entry).

### Where the game thread's frame goes

| | All 17 blocks | 4 slow blocks |
|---|---|---|
| game thread busy (CPU % x 10 / fps) | 25.3 ms per frame | 31.4 ms per frame |
| sum of the 80 listed pcs per block | 13.1 ms | 15.4 ms |
| waiting in `FRenderCommandFence::Wait` `sub_822FE560` (stack share of all samples) | 17-60 % per block | 24 % (~11 ms per frame) |
| of that, the wait reached through `sub_825E2428 -> sub_823C69A8 -> sub_824B95F0 -> sub_822471B0` | 0-50 % per block | 11.5 % (~5 ms per frame) |

- `sub_825E2428` (called from UGameEngine::Tick): `GameViewport` (+760) gets an UnrealScript event through
  `FindFunction` + `ProcessEvent` (vtable byte 220), then `Viewport->Draw` (`sub_823C69A8`). `sub_822471B0` is the
  Scaleform (GFx) movie tick: `if (!sub_82485950(GWorld)) FRenderCommandFence(0x82EB0F60).Wait(0)` (a full flush of the
  render thread), then it advances the movies (timers at +84, ProcessEvent of the movie's tick event, `vfunc 316` of
  every visible movie). In the cutscene blocks this flush is the game thread's largest single cost, and it is a wait for
  the render thread: in those blocks the render and ring threads set the frame time, not the game thread's CPU work.
- Self time is flat: the largest guest function is 2.3 % of the busy time. Top self, all blocks (ms per frame):

| RU | EN | ms (all / slow) | Identity | Notes |
|---|---|---|---|---|
| 8225C688 | 8225CA80 | 0.59 / 0.63 | FActorIterator advance (native `n_8225CA80`) | call count: BioWare's time-sliced `UWorld::TickActors` `sub_82485D78` keeps 6 persistent iterators (tick buckets, budget in f30) and reads `mftb` twice per actor (`sub_82811BF0` -> `UpdateGuestClock`). Buckets of actors would change which actors tick in which frame: not exact, not done |
| host `rex::runtime::indirect::Find` + `rex_indirect_miss_` | | 0.58 / 0.70 (render thread 0.43) | inline-cache misses of polymorphic virtual call sites (UWorld::Tick component loop, TickActors) | 47 of ~100 samples sit on the `ldr` of the 1 MB compact table (a cache miss), 12 on the `ldarb` of `g_built`. Done: hot-miss cache below |
| 82270788 | 82270C78 | 0.29 / 0.45 | `Cast<T>` class-chain walk (native) | call count |
| 824C3208 | 824C2318 | 0.28 / - | bone composition of `USkeletalMeshComponent::UpdateSkelPose` `sub_824C6A40` (894 instructions, 5 calls of the 4x4 multiply `sub_82262D90`, memcpy) | only caller 824C6A40; FP-exact NEON rewrite possible but large; candidate for a later round |
| 824DD848 native | 824DD848 | 0.24 / 0.44 | particle distribution lookup (native) | |
| 826545D0 native | | 0.19 | bone matrices (native) | |
| 82211498 | 82211498 | 0.19 | FMallocXenon::Malloc | |
| 82531C48 | | 0.19 / 0.32 | particle emitter instance Tick | |
| 82485D78 / 82485A40 | 82485058 / | 0.19 / 0.18 | TickActors / per-actor tick dispatch | |
| 8230D568 | 8230D5F0 | 0.17 | FindFunction-style FName hash lookup over the class / state chain (native, 649 call sites) | a cache keyed by (first table, FName) would need invalidation on package load; saving <= 0.15 ms: not done |
| 82430B00 | 8242FD38 | 0.14 / 0.32 | 14-instruction leaf called only through a vtable: `child[i].weight = w * f1; child[i].node->+148 += child[i].weight` (anim-node blend weight propagation) | memory-bound, nothing for a native to save |
| 82460BD0 | 8245FF18 | 0.10 / 0.26 | TFieldIterator<UProperty>++ (native) under GetInterpPropertyNames `sub_8244C978` <- `sub_82952530` | `sub_82952530` builds the class's property FNames into a TArray, then for every (name, anim node) pair does FName::ToString (`sub_82389E90`) + stricmp (`sub_829766D0`) + free; visible only in the cutscene blocks here (~0.2-0.4 ms with its callees, upper bound from truncated stacks). A per-class cache of the names changes the TArray growth (heap state), so it was not done |
| 82383FB0 | | 0.11 / 0.23 | 22-instruction flag test + one virtual call | |
| `__aarch64_read_tp` | | 0.11 | TLS reads: the `thread_local` call counter of every hot hook and the pthread mutex of `UpdateGuestClock` | see the clock note below |

- Other threads, for scale (busy ms per frame, all blocks): UE3 render thread 13.5 (top self: InitViews `sub_824F5DB8`
  0.35, `indirect::Find` 0.33); two streaming threads `XThreadDFE959E0` / `XThreadDFE1ADE0` 1.1 each, of which the LZO1X
  decompressor `sub_827D3388` (EN `sub_827D2A00`) is the top function (0.22 + 0.25 ms per frame here; it dominates those
  threads during level streaming and loading).

### Done in this round (all default off)

1. **Indirect-call hot-miss cache** (SDK, exact; no hooked-set change). `sdk/src/system/indirect_dispatch.cpp`:
   a 4 KB direct-mapped cache (512 words in the compact table's own `{guest, host offset}` packing, hashed with the same
   multiplier) checked by `indirect::Find` before the 1 MB table; filled from the table on a miss, cleared whenever the
   table is rebuilt or a mapping changes (`BeginRebuild`, `EndRebuild`, `InvalidateInlineCaches`). With it on, the miss
   path also skips the `indirect_dispatch_fast` cvar accessor call and the `g_built` acquire load (the flags are latched
   when the table is published). Entries are self-validating 64-bit words, so a stale or empty slot only misses.
   - cvars: `indirect_dispatch_hot_cache` (default false), `indirect_dispatch_hot_cache_verify` (default false: probe the
     table on every hit, log `indirect_dispatch hot cache DIFFERENCE` (first 16) and a summary every 2^22 hits).
     Both are read when the table is built (first miss): set them in the toml. Log line when on:
     `indirect_dispatch: hot-miss cache on (512 entries, 4 KB)`.
   - Only for builds with `MASSEFFECT_INDIRECT_DISPATCH` 1 or 2 (ru_glob19 is one). Needs an SDK rebuild only.
   - Host check (scratch harness, real `indirect_dispatch.cpp`, stubbed runtime): 39,917 functions, 6 M lookups mixing a
     hot set, all registered entries, unregistered and mid-function addresses and target 0, with 1,500 remappings in
     between: 0 wrong results, 0 verify differences.
   - Expected: most of the table-line misses go (the hot polymorphic targets fit in 4 KB): about 0.2-0.4 ms per frame on the
     game thread and 0.15-0.3 ms on the render thread at 1785 MHz. Risk: the same remap race as the inline caches
     (mappings never change after start in this game).
2. **LZO1X decompressor native** `n_827D2A00` (EN `sub_827D2A00`, RU `sub_827D3388`, the same function instruction for
   instruction; the native uses no guest data constant, so one header serves both editions).
   `app/src/native/hot/n_827D2A00.h` (included by `hot_all.h`), hooks in `app/src/native/me_hot_guest.cpp` and
   `editions/ru/overlay/app/src/native/me_hot_guest.cpp`.
   - The original is a fully unrolled 5,373-instruction state machine (switch on state 0 / 0x100 / 0x200 + token, every
     copy byte by byte). Its variant details are reproduced: only token 17 with distance 0 ends the stream (other M4
     tokens with distance 0 copy from op - 0x4000), M3/M4 re-read the trailing-literal count from `in[ip - 2]` after the
     copy, the 64-bit return values (0, 0x00000000FFFFFFF8, 0xFFFFFFFFFFFFFFFC), `*out_len` cleared at entry and set at
     exit, the two zeroed stack words. Copies: memmove when the ranges allow it, 8-byte chunks for distances >= 8,
     bytes for short periods; all addresses 32-bit with the original's wrap.
   - cvar `masseffect_hot_lzo` (default false). It is NOT switched on by `masseffect_hot_guest` (new macro
     `ME_HOT_HOOK_OWN`; `InitHook` got an `umbrella` flag), because the production toml has `masseffect_hot_guest = true`.
   - Verification: the usual guard (`masseffect_hot_guard_calls` first calls, then 1 in `masseffect_hot_guard_period`;
     `[hot] DIFFERENCE sub_827D3388 ...` switches it off). `Writes()` runs a measuring decode to find the output range
     (declines to verify, and runs the original, for outputs over 1 MB, runaway streams and outputs that overlap their own
     input). For a full-coverage console check run once with `masseffect_hot_guest = false`, `masseffect_hot_lzo = true` and `masseffect_hot_guard_period = 1` (the guard settings apply to every enabled hook).
   - Host fuzz: `python3 tests/hot_fuzz/build.py 827D2A00` (`tests/hot_fuzz/cases/case_827D2A00.inc`: random LZO1X
     streams of the variant's grammar incl. extended lengths, short-period overlaps, matches reaching in front of the
     output, M4 distance-0 tokens, bytes after the end marker and truncated `in_len`): 22,000 iterations over 4 seeds,
     0 failures (r3 and every byte of the 512 KB window). `--bench` on the M-series host: 58 us -> 2.7 us per call
     (x21.7, literal-heavy random data). `liveness.py`: neither direct call site reads a volatile register.
   - Expected: the streaming threads' LZO time drops several-fold (~0.4 ms per frame on average in this route, much more
     while a level streams in and during loading: also helps the cold start). It is not game-thread time; it frees
     cores 0-1 (the streaming threads have mask 0x7 and run beside the render and ring threads).
   - **Hooked-set change**: yes. Run the codegen again (`tools/edition.sh ru all`, and `tools/codegen.sh` for EN) so
     the two direct call sites call `sub_827D3388` / `sub_827D2A00` instead of the inlined `__imp__` version. The EN
     header's name also marks RU `sub_827D2A00` as hooked in the RU tree (harmless, as with the other EN-named headers).

### Considered and not done (with the reason)

| Idea | Why not now |
|---|---|
| TickActors buckets (per-tick-group actor lists instead of six full FActorIterator walks) | BioWare's TickActors is time-sliced (persistent iterators, a time budget, `mftb` per actor): a bucketed walk changes which actors tick in which frame |
| GetInterpPropertyNames cache (`sub_8244C978` from `sub_82952530`) | 0.2-0.4 ms only in the cutscene blocks; a cached name list changes TArray growth and the allocator state; a native of `sub_82952530` that compares FName entries without building FStrings is the exact variant (next round if cutscenes stay CPU bound) |
| FindFunction cache (`sub_8230D568`) | 0.17 ms total; needs invalidation on every package load |
| PhysX spin-wait | not visible in this route (`sub_8299D160` 0.25 ms inclusive) |
| Removing the GFx full flush in `sub_822471B0` | the largest game-thread stall of the cutscene, but it orders the movie update against the render thread: a behaviour change |
| Guest clock lock (`UpdateGuestClock`: `std::mutex` try_lock + unlock, TLS reads, two shared counters per call) | ~0.15-0.25 ms per frame on the game thread; an atomic-flag try-lock is exact but the lock is also taken during `Runtime::Setup`, so the switch must be latched carefully. Next candidate |
| Native of the bone composition `sub_824C3208` | 0.28 ms, 894 instructions with FP: large FP-exact rewrite, expected gain ~0.15 ms |

## BioGameProperty timers and cooldowns (2026-10-08, analysis only, nothing implemented)

Why it was looked at: the UnrealScript profiler of the location tour (`masseffect_script_prof = 1`,
`run/me1/tour_20261008_205904/session2/summary.txt`) puts this subsystem at the top of the call counts on every map.
Eden Prime (BIOA_PRO00, 852 s): `BioGamePropertyTimer.Tick` (native) 1,535/s, `BioGamePropertyContainer.ProcessCooldown`
(event) 873/s, `BioGamePropertyManager.TickInternal` (event) 662/s, `BioGamePropertyContainer.Tick` (native),
`BioItemSophisticated.TickItem` and `BioItemXModdable.TickItem` 123/s each, `BioEquipment.TickEquipment` 87/s. Together
about 3,500 of the 7,550 counted script calls per second (~115 per frame at 30 fps).

### The functions (RU / EN)

| What | RU | EN | Notes |
|---|---|---|---|
| `UBioGamePropertyTimer::execTick` (native table `intUBioGamePropertyTimerexecTick`) | `sub_8291A658` | `sub_8291A128` | reads `float DeltaT` and a 12-byte struct parameter, then calls vtable byte 284 (slot 71) |
| `UBioGamePropertyTimer::Tick` (vtable `0x821B9008`, 75 slots, slot 71) | `sub_827B39B8` | `sub_827B3060` | below |
| `UBioGamePropertyContainer::execTick` = `UBioGamePropertyManager::execTick` (one folded thunk) | `sub_828CEFB0` | `sub_828CF020` | `float DeltaT`, then slot 71 |
| `UBioGamePropertyContainer::Tick` (vtable `0x821B9168`, slot 71) | `sub_827B3D50` | `sub_827B33F8` | `eventProcessCooldown(DeltaT)` (FName global `0x82E73DF0`) through FindFunction `sub_8230D568` + ProcessEvent (vtable byte 220), then `(+76)->vfunc 292` |
| `UBioGamePropertyManager::Tick` (vtable `0x821B9590`, slot 71) | `sub_827B57A8` | `sub_827B4D10` | only `eventTickInternal(DeltaT)` (FName global `0x82E73E00`) |
| Owner tick that drives both from C++ | `sub_826C4058` | `sub_826C32A8` | called from `TickActors` `sub_82485D78` directly and through the actor tick `sub_82285E48` -> `sub_827BCA20`; loops the owner's arrays at +128, +180, +192 and the container at +144 through vtable bytes 296-312 |

`UBioGamePropertyTimer::Tick` (RU `sub_827B39B8`): a re-entrancy bit (`+84` bit 31, set on entry, cleared on exit); a
delta list in the TArray at `+60/+64` (16-byte entries `{float time; UObject* gp; u8 active; u32 flags}`, processed from
the last entry): an entry whose time is above the remaining delta is decremented and the loop stops; otherwise its time
is set to 0, the delta is reduced, an event (FName global `0x82E73DE0`) is sent through ProcessEvent when the struct
parameter's first word is not null, a flagged entry (`flags` bit 31) is added to the array at `+72` (`sub_82460980`),
and the entry is removed (`sub_822F5E68`). Then every non-null object of the `+72/+76` array gets vtable byte 292 with
`DeltaT`. With both arrays empty the call only sets and clears the bit: a no-op in effect, but it is already a short
C++ function. The time is in the script VM around it (the TickInternal / ProcessCooldown bodies and the script call of
the native), not in the native.

### Cost (prof_eden, RU, ELF ru_glob19 = scratch `syms.txt`, 17 blocks, game thread busy 25.3 ms per frame)

| Measure | ms per frame |
|---|---|
| Stacks that contain `sub_827B57A8` (TickInternal) | 0.04 (lower bound: only the top 30 stacks per block, 10 frames deep) |
| Stacks that contain `sub_827B3D50` (ProcessCooldown) or `sub_8291A658` (Timer exec) | < 0.01 |
| Stacks that contain the owner tick `sub_826C4058` | 0.04 |
| Stacks that contain `ProcessInternal` `sub_823E93B0` at all (any script) | 0.09 |
| Self time of the whole script VM range (`sub_823D....` - `sub_823E....`, all scripts, the 80 listed pcs per block) | 0.63 |
| The same, scaled for the unlisted tail (listed pcs = 13.1 of 25.3 ms) | ~1.2 (upper estimate for all script on the game thread) |
| Self time of the subsystem's C++ (`sub_827B....`, `sub_8291A...`, `sub_828CE...`, `sub_826C4...`) | 0.01 |

The tour profile (`tour_20261008_205904/session2/rex_profile.sym`, ELF ru_glob25, script profiler on, so the VM is
slowed by its hook) gives the same picture per map: TickInternal stacks 0.03-0.07 ms, owner tick 0.03-0.09 ms
(BIOA_STA00 highest), all-script stacks 0.10-0.43 ms.

Estimate: the subsystem is 30-50 % of the visible script stacks, so **about 0.3-0.6 ms per frame including the script
VM**, and at most about half of the ~1.2 ms that all UnrealScript costs on the game thread. Below the 1 ms threshold set
for this work, so nothing was implemented (no cvar, no hook; the hooked set is unchanged).

If it is taken up later: the exact, cheap variant is a native of `UBioGamePropertyManager::Tick` / `Container::Tick`
that skips the ProcessEvent when the script bodies are known to do nothing (needs the decompiled TickInternal /
ProcessCooldown bytecode to prove it; their FName globals are above). A native Timer::Tick saves nothing (it is already
C++). For a real number first run one tour leg with `masseffect_script_prof = 2` (self and inclusive time per script
function).
