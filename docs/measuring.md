# Measuring performance without fooling yourself

This page explains how to measure the port on the Switch in a way you can trust. Read it before you optimise
anything, and again whenever a result surprises you. Over 300 console runs were made; a large share of the early
conclusions had to be thrown away because a number did not mean what it seemed to mean. This page collects the methods
that held up and the mistakes to avoid. The results themselves are in [performance-history.md](performance-history.md);
words you do not know are in [glossary.md](glossary.md).

## In short

- Measure on the console, at stock clocks. The PC and an overclocked console both mislead.
- One change per run, behind a setting. Write down every result, also "no change".
- Never trust a single 10-second interval. Run-to-run noise is about 3 fps in the same build.
- Compare two builds back to back in the same session, and judge CPU changes by core-milliseconds per frame, not by fps.
- Acceptance is a cold start: no shader or pipeline caches.
- Judge a picture from many captures (one per route leg), never from two or three.
- Never read the `cntvct_el0` register on the Switch; use `armGetSystemTick`.

## Ground rules

- **Console only.** The PC build (macOS) is good for checking that something works. It is not a performance
  reference: it hid CPU costs of the mode-4 tile loops that the Switch's 1 GHz cores made visible, and its GPU is
  a different machine. From 1 October all runs and fixes are checked on the Switch.
- **Stock clocks.** CPU at 1020 MHz, three usable cores. Overclocking changes which part limits the game (a build that
  was GPU bound at stock became CPU bound with a fast CPU). The two overclocked runs (t210, t211) were diagnostics only,
  to learn which resource was the limit, and are never quoted as results. The official CPU boost mode throttles the GPU to
  76.8 MHz, so it cannot be used either (t243: 3.2-4.5 fps).
- **One variable per run.** Put the change behind a setting that a configuration file (toml) can switch. Record the result
  in the working list with the run name, even when nothing changed. Do not retry a rejected idea without new data.
- **Stop rules.** Revert below 1.0x. Keep 1.0-1.2x only if it simplifies the code or prepares a later step. Ship above 1.2x.
- **Image-changing variants** (lower shadow resolution, no decals, less post-processing) are also measured, because the
  user decides afterwards, but they are never on by default and are labelled "changes the image".
- **Report every number with its run and its log.** A number from one run multiplied by a counter from another means nothing.

## The test scene

All headline numbers use one save: the opening level, Eden Prime, played by an automatic route.

| Route | Use |
|---|---|
| out-and-back "legs" route | older runs (before t163) and A/B comparisons against them; each leg ends with a capture |
| forward route through the level | the default since t163; 45-90 s of walking and turning, a capture per leg |
| a map override through the game's configuration file (location sweep) | 45 s walk and turn per map, one contact sheet per map; it does not always select the level, so real per-location tests need saves |
| title mode | 20 frames 0.4 s apart of the title to main-menu transition (used for the planet bug) |

Two routes are not comparable: the series of GPU times quoted in the history belong to different routes (the same
build gave 163 ms on one and 132 ms on another). Only compare runs of the same route, and when it matters, run them back
to back.

## What to read in a run

A run produces a log of the game (`game.log`: GPU time per swap, EDRAM mode-4 reports, counters), a profiler log
(`profile.log`, one block every 10 seconds) and screenshots.

| Quantity | Where | Use |
|---|---|---|
| game fps | profiler block, "game ... fps" | headline number, per 10 s interval |
| CPU total and per thread | profiler block, thread lines | which thread is the limit |
| **core-ms per frame** | CPU % x 1000 / fps, averaged over the intervals (`tools/console-test/cpu_per_frame.py`) | the stable metric for CPU changes |
| GPU per swap, by category | game log. Raw NVK timestamps must be multiplied by 1.627 and divided by the swaps in the interval | the stable metric for GPU changes |
| "gap" | game log | time the GPU waited for the CPU. If it is large the CPU is the limit |
| EDRAM mode-4 counters | game log | transfers, redirected clears taken or declined and why, proofs |
| stack samples | profiler, if enabled | where the CPU time goes |

**core-ms per frame.** A change of 1 ms of main-thread time is 3 % of a frame at 30 fps, but fps in a single run moved by
more than that. CPU % divided by fps removes the noise of the game running faster or slower. Typical single-run noise
of this metric is +-0.3 to 0.7 ms per thread; the run-to-run noise of fps is about 3 fps (at the same build: 27.2, 24.5, 24.8).
Three numbers are tracked: total, main game thread, ring thread.

**Classifying the bottleneck first.** If the GPU busy time is below the frame time and the gap is large, the CPU is the
limit; if no hardware unit is near its peak and the gap is small, the GPU front end is starved by per-draw work (state
changes, barriers, waits for idle), not by shading. Our scene was front-end bound until the vertex outputs were pruned.

## Two regimes in one route

The route has a fast regime near 27 fps and a slow one near 24 (GPU 33 ms against 54-60 ms per swap), and the heavy
views (grass fields) are CPU bound while others (Citadel) are GPU bound. An average over a run mixes them. Look at the
intervals (28.9 25.8 40.6 30.9 22.9 ...), and at the share of intervals below 25, not only at the mean.
Menus and loading screens cost half of what gameplay costs; do not average them in (the first intervals of a run are
menus, and "menu" values of 26-28 fps appear in the log).

## Cold start versus warm start

A warm run has caches: Mesa's shader cache and the pipeline cache on the SD card. A cold run has none, and every pipeline
is compiled on the ring thread (81 ms each with the per-stage cache). The numbers differ a lot:

| Run | Warm | Cold |
|---|---|---|
| t73 (warm), t74 (cold) | 7.4-13.3 fps | 6.5 5.4 6.7 7.3 8.8 7.1, and 1.8 in a compile interval |
| t115/t116 | | 7.0-14.1 (lowest 5.4) then 9.1-14.2 |
| t150/t152 | 16.6-18.0 | 17.3 16.3 14.6 17.1 17.2 13.9 |

Rules:

1. **Acceptance runs are cold.** The user's rule: a cold run must never go below 25 fps in any interval.
   (Not met yet: see [known-issues.md](known-issues.md).) The cold mode sets `masseffect_cold_startup`: Mesa's cache is
   disabled and the pipeline cache file is deleted first.
2. **Warm runs are for quick A/B of one change.** They hide compile stalls.
3. Say which one a number is. Report cold numbers for milestones.
4. Do not mix start-up measures: "HOME to title" (the first marker is when the console launches the game) and "from the
   first log line" differ by many seconds.

## A/B tests that can be trusted

- Run A and B **back to back**, in the same session, same route, same console state (temperature, SD cache). The pair
  t161/t162 is the example: 16.7 against 16.6 fps, after a first comparison of 19.6 against 18.7-18.9 had suggested a gain.
  Order effects exist: repeat as B, A when a difference is small.
- Compare **the same leg** across runs. Check the baseline captures of that leg before blaming a change for an image
  defect. Black patches on terrain were wrongly blamed on two changes (a shader package and the 960 mode) because they had
  been there since the first Switch run and only a few captures were looked at.
- Use a setting that does nothing to measure the noise of your session.
- Never compare fps of a **probe** (a variant that changes the image on purpose) with a real run.

## Probes: finding the limit of an idea

A probe removes work on purpose to learn the most an optimisation could give:

| Probe | What it showed |
|---|---|
| zero-area scissor on scene draws (no fragments) | scene 66 to 45.6 ms: pixel work about 20 ms |
| one triangle per scene draw | 65 to 3.8 ms: vertex work about 40 ms, per-draw state about 4 ms |
| the same probes at 960 | game fps only 18.9 to 19.7-20.3: the frame was now CPU bound |
| scene draws dropped on the Vulkan side, parsing still done | about 29 fps: the ceiling of the GPU-side work |
| early fragment tests forced on alpha-tested depth writers | at most -2.3 ms |

A probe can also fail to fire: t237 used the wrong GPU category (at less than 1280 wide the scene pass is classed
differently), so the "probe" measured nothing. Check that the diagnostic really triggered (its own counter in the log)
before drawing conclusions.

## The stack profiler

The port has a sampling profiler that writes its report every 10 seconds. A flag file next to the logs
(`stacks_profile.flag`) turns on stack sampling; a second flag (`neon_probe.flag`) runs the register-corruption probe
used in stage 2.

- Resolve addresses with **the ELF of the same build**. Line tables (`-g1`) are in the Switch build, but a profile
  resolved with another build gives wrong functions, so the ELF is saved next to every profile.
- Kernel waits are recognised (the program counter sits on the system call itself) and grouped by caller, so a thread
  sleeping in a mutex is not counted as running.
- Share of samples is not time: the listed functions covered about half of the ring thread's busy time, so per-function
  figures are lower bounds of what they touch.
- Read the code that increments a counter before you believe the counter.

Typical findings that came from the profiler: the game thread spinning in its GPU wait (30 %), a fingerprint hash at 15
% of the ring thread, an audio worker waking 1000 times a second at a priority above the ring thread, and a static-guard
check (a load-acquire on every call) at 2.6 % of the main thread.

## GPU timing

- NVK timestamps are in different units: multiply by 1.627.
- Each timestamp mark costs a flush and a semaphore release. A whole-frame A/B with marks off (or only first and last
  per submission, `masseffect_gpu_marks_categories = false`) must be compared by fps, because the per-category columns
  disappear. A whole-frame test with cheaper marks gave the same fps at 6.5 fps (marks do not inflate the frame).
- A "GPU per swap" with a column called "reflection" at 960x544 is the scene pass (the category depends on target width).
- One frame's marks can be dumped (`masseffect_diag_dump_marks_s`) to see each pass's size, formats and draws.

## Things that must never be done

- **Never read `cntvct_el0` on Horizon.** It faults: the game starts to a black screen and ends in `std::terminate`.
  Use `armGetSystemTick` (which reads `cntpct_el0`).
- **Do not use the on-screen frame-rate overlay** (the SaltyNX one). It froze at 25 from run t203 on, after the
  deferred-recording, start-up and logging changes. Use the profile.
- **Duplicate command-line options.** The launcher once appended settings to its defaults; the command-line parser rejects a
  duplicate scalar option and the program continued with defaults, so those A/B runs had not changed what they claimed.
  Print the resolved arguments (a dry-run mode) and look for the setting in the log's configuration echo.
- **Do not judge temporal correctness from a still image.** Still captures of a flickering game looked clean more
  than once. Record consecutive frames and inspect them.
- **Do not compare runs of different routes**, or runs whose interval contains a load.
- **Do not trust zero rejected draws** as proof of coverage: a draw skipped before the check is not counted.

## The automation

Console runs are automated so that a whole cycle needs no one at the console. In generic terms the tools do this:

| Tool | What it does |
|---|---|
| test cycle script | builds the executable (optionally), uploads it with its settings file over the console's FTP service, launches it from the HOME menu's first tile, drives the title, loads the save, plays the route for N seconds, takes a capture per leg, closes the game, downloads the logs, and prints a summary. Options: skip the build, choose the settings file, choose another executable or shader package, cold start, map override, title mode, hold in game |
| frozen copy of the cycle script (a frozen copy of `tools/console-test/switch_cycle.sh`) | the same, kept stable for a series of A/B runs while the main one is edited |
| input bot (`switch_bot.py`) | sends controller input through a system module on the console and waits for a recognised screen using reference pictures before the next input. Inputs are **held**, never clicked: at a few fps the game polls the pad too rarely to see a click |
| manual variant | runs without the input bot, for starting a run by hand |
| map sweep script | boots each map through a changed game configuration and builds a contact sheet |
| settings generators | write the configuration files of a set of variants (for example the GPU test series) |
| core-ms script | computes fps, total, main and ring core-ms per frame from a run's profile |
| restore script | uploads the original game configuration (logo movies) and the play settings after a test session. Run it after every session |

Things learned:

- The FTP service on the console stalls after about 4 files per connection and then answers with an error; 3 files per
  connection with a 15 s pause (60 s after an empty batch) works. Large shader packages (about 0.9 GB) take minutes to upload.
- The test cycle uploads a configuration that removes the logo movies (they cost start-up time and are not what is being
  measured); the original is restored by the restore script.
- The saved in-game graphics options persist on the SD card and change results (Intermediate display, motion blur and film
  grain cost about 1.4 fps). Record which profile a run used.
- Always shoot **many captures per run** (one per leg, 8-10), and look at them side by side with the baseline's.

## A checklist for a new idea

1. Is the bottleneck the one your idea attacks? Read the gap and the thread percentages first.
2. Is there a probe that bounds the gain before you build it?
3. Is the idea behind a setting, with a self-check that compares the old and the new path for the first N uses?
4. Run A and B back to back, warm, same route; then cold for a milestone.
5. Check fps, core-ms (total, main, ring) and GPU per swap, and the captures of every leg.
6. Write the result, including "no change", in the list. If the gain is below 1.0x, revert; between 1.0 and 1.2x keep
   only if it simplifies or prepares.
