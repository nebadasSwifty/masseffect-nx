# Known issues and limits

An honest list of what is not working, not finished, or not proven in this port. Each entry says what is known, the
evidence (run names and numbers from the console), and what a next step could be. Fixed bugs that shaped the design
are in [performance-history.md](performance-history.md) and [native-renderer.md](native-renderer.md). Words you do not
know are in [glossary.md](glossary.md).

Status words: **open** (not solved), **worked around** (hidden by a setting, root cause open), **not verified** (never
checked on the console), **fixed** (kept here because it is easy to hit again).

## Speed

### 1. Thirty fps is not reached: about 25 fps on average (open)

| Fact | Evidence |
|---|---|
| Average about 25 fps, intervals 19-31 at 960x544 and stock clocks | t261 best profile 27.7 fps (98.6 core-ms per frame); t271 with the saved in-game options 25.7 fps; t303 25.4 and 27.1 fps |
| The CPU is the limit in most views: three cores saturated (about 290 % of 3 cores), main game thread 29-33 ms per frame and ring thread 26 ms | t238 probe: with the Vulkan side of the scene free, still about 29 fps |
| Light views reach 30 or more (Normandy JUG00 32.5, LAV00 31.4, ICE00 29.3), heavy ones do not | t266-t269 location sweep; Eden Prime grass 20-24 fps at 142 core-ms per frame |
| A stable 30 needs the main thread about 10 % cheaper as well as the ring thread | t238 and the probes of t179/t180 |
| Citadel (STA00) is GPU bound: busy 49.6 ms, of which 24.2 ms are EDRAM colour conversions | G1; about 20 fps at default options |

The goal is a steady 30 fps with a correct image at stock clocks. It has not been proven unattainable, so work continues.
The remaining levers are code: guest code (codegen, hot functions), the ring thread, and GPU conversions. The levers
not taken: overclocking (excluded), the fourth CPU core (declined, see below), official CPU boost (throttles the GPU).
See [optimization-paths.md](optimization-paths.md) for every idea and its result.

### 2. Cold-start dips (open)

The acceptance rule is that a cold start (no shader and pipeline caches) never drops below 25 fps in any 10 s interval.
It is not met.

- Recorded cold runs: t74 6.5 5.4 6.7 7.3 8.8 7.1 (and 1.8 in a compile interval); t115 lowest 5.4; t116 lowest 9.1; t121
  lowest 8.8; t152 lowest 13.9 (17.3 16.3 14.6 17.1 17.2 13.9).
- Cause: every new pipeline is compiled on the ring thread, 81 ms each with the per-stage cache (it was 137 ms), and
  a cold run builds 194-211 pipelines (about 27 s of compile in total at 137-139 ms each, the original estimate; 16-27 s in later notes).
- No cold run is recorded after t152, so the present cold numbers are not known.
- Options: a shipped list of pipeline keys so a background thread compiles while the game starts (E32c; needs the user's
  decision whether that counts as cold), asynchronous compile (tried, t121: 4515 draws skipped, visible pop-in).

### 3. Slow start-up (open, improved)

About 28 s from launch to title (was 48 s); a later warm analysis puts the title at 15-16 s after the first log
line and the game in play at about 44 s with a bot-driven route. 8.6 s of that is one CPU-bound guest thread, the rest is
128 KB package reads. Ranked options in the start-up section of [optimization-paths.md](optimization-paths.md).

### 4. Memory headroom (not verified)

Before the shader package was loaded by index, the process ran within about 4 MB of its 3189 MB limit. About 800 MB were freed.
How much headroom the longest levels have is not measured.

## Picture

### 5. Black polygonal shards on terrain and rocks (worked around)

Present since the first Switch runs: black polygon-shaped unlit patches on Eden Prime terrain. Found by bisecting
(t138-t148): they are gone without the scene depth prepass, which is drawn into the 2x view and whose depth, brought
back to 1x, disagrees with the material pass on slopes. `masseffect_native_skip_prepass` (default on) skips the
prepass, and no shards were seen on any of 7 legs (t150). The root cause, an exact 2x to 1x depth transfer, is open,
and the prepass's early depth rejection (up to about 20 ms of GPU at 1280) is given up. Note: some older notes still call
the patches "a pre-existing bug"; it was fixed by the workaround above, not by a root-cause fix.

### 6. Title screen planet is black in the title-to-menu pan (open, closed by decision)

On the Switch at 960x544 (and 1040x576) the planet behind the title is black after "Press START" while the camera pans to
the main menu. It is correct at 1280x720, 1120x624, 960x720 and 1280x544, and **correct on the PC at every size**.

Evidence from many runs (t289-t313):

- Not caused by: 16-bit render targets (k_16_16), vertex shader identity, texture recheck, prepass settings, texture
  address overlap, the code generation options or arguments in registers (checked on the PC with the Switch code).
- In good frames the planet is lit in the HDR scene target and the front buffer. In black frames the draw sequence differs:
  the full-screen passes that read the resolved scene colour and the resolves into the shadow and light-attenuation textures
  are missing, and the lit planet passes write black. The PC renders the same sequence with the planet lit.
- Experiments with a memory barrier after every resolve, a GPU wait before output and no resolved-image reuse (all off by
  default) did not change it.
- The guest picks the same pixel shaders as in the bad Switch run when run on the PC, so the decision is not in the game logic
  alone. Lead if reopened: why the guest skips those passes on the Switch only (frame-time-driven quality scaling, a
  minimum desired frame rate of 35 in the game configuration; compare the guest's decisions between the two platforms).
- Later experiments (t314, t350), none of which changed it: on the PC a minimum Swap-to-Swap interval of 40 and 100 ms, a disk
  slowed to the speed of the SD card (`masseffect_io_us_per_kb`) and the whole best settings set all keep the planet lit through
  the pan, and the PC guest also drops the same passes in the pan, so a missing pass alone is not the cause; on the console,
  `masseffect_native_mipmaps = false` and `masseffect_native_diag_mips = true` leave the planet black. Remaining untested
  leads: the guest-visible memory and streaming budget, the thread and processor-count timing, the real pad against the
  scripted input of the PC test, and the `TEXTUREGROUP_*` and streaming keys of `Coalesced.ini`.
- The user decided to close the investigation: 960x544 stays and the planet is black only in this pan.

### 7. Eden Prime water is missing (open)

Water does not draw on the console (user check at t285/t286). The shader identity is not the cause (audit: 2.6 million
proven selections, 0 unproven; one depth-only vertex shader, 120 words, is unidentified). The water pixel shaders read the
resolved scene colour and depth, a cube map and two normal maps, so water needs the resolved scene depth (format 24_8 float)
and colour (FP16) as prepared textures plus the depth test against the scene depth. Diagnostics existed
(`masseffect_diag_water`, `_force`, `_ps`; removed); no fix.

### 8. Texture popping and flicker (not verified)

One suspected cause is the stable-texture recheck interval of up to 32 frames plus jitter. The shipped settings now
use `masseffect_native_texture_interval_max = 4`. A stationary Eden Prime console test stayed around 26 fps, but
that does not prove all temporal artifacts are fixed. 16-bit render target support made flicker worse (t285/t286)
and was rejected.

### 9. Mako wheels float apart from the body (open, not a renderer bug)

In the Ilos map (END00) the vehicle is drawn with its wheels apart from the body. It is identical with the native hooks
off, so it is not an optimisation bug. Suspected cause: the physics step is not reliably advanced (see issue 10).

### 10. PhysX step runs unreliably (open)

The game does step PhysX on a separate thread, but a race in a manual-reset event lets the join pass before the
simulation has started. On the console the worker thread is starved by the ring, render and recording threads. The
poll cost was cut (19 % to 2 % of the main thread) but `fetchResults` still reports that `simulate()` was not called. A
proper fix (hook after the sub-step count) was not made.

### 11. Resolution limits (open)

- Only 960x544 works with the fast paths and the UI; 800x448 and 800x450 are slower (off the 16-pixel tile grid) and the
  title menu did not draw at 800x448 (the Scaleform stage was not scaled; fixed only for 960x544).
- The image is softer than 1280x720 and the post chain runs at 1280x720 anyway.
- The HUD markers' world-to-screen Y is scaled for 960x544; other sizes are not tested.

### 12. Saved graphics options (not verified)

The in-game options (Intermediate display, motion blur, film grain) are saved on the SD card and change results; they cost
about 1.4 fps. Artifacts reported by the user with these settings have not yet been pinned down.

### 13. Other picture notes

- Duplicated glow and "ghost" copies of effects were seen on the Ilos level on the PC (remaining after the pixel shader retranslation).
  Not checked on the Switch.
- Depth above 1 is clamped; 24-bit depth quantisation is off; true multisampling is not integrated.
- Dark lit characters in some scenes (Profile Reconstruction) were seen on the first PC stage; the Switch path, with native Vulkan
  depth, may not show it. Not specifically checked.

## Coverage and correctness

### 14. Only partly checked across the game (open)

- Location sweep (t266-t269): NOR00, ICE00, JUG00, LAV00, PRO00, STA00, WAR00 loaded and rendered correctly; PRO10 and END00 had
  start problems in the sweep. The map override of the game's configuration does not always select the level (several maps start
  in the Normandy intro), so real per-location tests need saves.
- Textures and shaders per location have not been fully checked (the closing milestone).
- 60 of 30,191 shader containers do not build (0.2 %), and one depth-only vertex shader (1383 draws seen) is unidentified.
- Shader identity proofs are strict, but "zero rejected draws" does not prove coverage: a skipped draw before the check is not counted.

### 15. Unexplained "Disc Read Error" dialog (open)

One run (t168) showed the game's "Disc Read Error" dialog. The session log has no dirty-disc call and no failed read, only the
usual failures to open the autosave content (present in every run). A rerun was clean.

### 16. Autosave open failures (open, benign so far)

Every run logs failures opening the autosave content folder. No effect has been seen.

## Platform and tools

### 17. Four-core use and overclock (declined)

The fourth CPU core (core 3) could be unlocked by a changed homebrew loader descriptor, with an estimated 0 to +3 fps and
stability risk. The user decided not to change the console's system configuration, and overclocking is excluded
(the project's rule is stock clocks). The research is in the development notes, never run.

### 18. GPU clock not pinned in the sources (not verified)

The stage 3 notes say the GPU is raised to the official 460.8 MHz handheld profile; later notes describe the stock GPU as 768 MHz. Check
the clock in your run's profile before comparing with the numbers here (see the end of [performance-history.md](performance-history.md)).

### 19. Stage 2 emulated path: guest crash from vector register loss (not used)

The stock GPU emulation crashed after minutes because the kernel does not preserve half of the vector registers of a thread
preempted inside its user exception handler. The native renderer never watches memory, so it does not take that path.

### 20. Host-side limits found on the PC (not relevant to the console)

- Exit can deadlock on the PC (a timer cancel spins while its callback waits for a heap mutex). Exit only.
- The graphics layer on the PC cannot disable primitive restart (a warning).
- C++ exception unwinding funclets found by the data scan are not declared; no evidence that the game throws.
- `setjmp` and `longjmp` are unsupported in the generated code: a recompiled `longjmp` returns to its caller instead of the
  `setjmp` site. This affects D3DX shader-compiler error paths only.

### 21. Risky code generator options (accepted, with switches)

- Arguments in registers rest on two unprovable assumptions (unknown callees do not read r0/r11/r12/f0; junk in unread
  clobbered registers is harmless). 13 of 20,905 fuzzed functions differ, 3 unexplained. It is off by default.
- Guest memory accessed without `volatile` can turn a polling loop into a hang or a deleted loop; this froze the first
  arguments-in-registers build. The `asm volatile` memory barrier prevents it.
- Native replacements of hot functions are guarded: the first 256 calls of each are checked against the recompiled
  original, and each function has its own setting.
- Atomics without the global lock and `skip_msr` rely on the 96 audited functions having exactly one atomic pair.

## Fixed, but easy to hit again

| Item | Note |
|---|---|
| `masseffect_native_float24_ps_mode` on | disables early-Z and ZCULL; in-game 0.7-1.0 fps. Must be 0 |
| Resolution settings of a different size | 800x448 and 800x450 hang the UI or lose the fast paths |
| `masseffect_native_uploads_thread` true | vertex worker reads transient buffers after the game rewrites them: nondeterministic corruption |
| Polling loops after the `volatile` removal | add the barrier to any new `pure`-looking function the compiler could delete |
| Another build's profile resolved with this build's ELF | wrong function names |
