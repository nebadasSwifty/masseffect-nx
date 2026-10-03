# Porting another Xbox 360 game to the Switch

## In short

This guide says what you can reuse from this project for another game, and in which order to work. It is written from the
experience of this port, which is **not finished**: it runs at about 25 fps instead of 30, so read the parts about speed as
"what was tried", not as a recipe that is known to reach 30. Most of the hard problems had little to do with Mass Effect:
they came from the Switch itself, from the recompiler, and from the Xbox 360's GPU memory, so a port of another game will
meet them too. Read it once from top to bottom before you start. If a word is new to you, it is in the
[glossary](glossary.md).

The order that worked:

1. Get the game running on a PC (here a Mac) with the recompiler.
2. Move it to the Switch system.
3. See the first frames with the SDK's own GPU backend, then write a faster renderer.
4. Translate the shaders before playing.
5. Measure on the console, every time.
6. Make it faster, one change at a time.

## What is reusable, and what is not

| Part | Reusable? | Notes |
|---|---|---|
| `sdk/` Horizon layer (memory, exceptions, threads, input, audio output, presentation, clocks, profiler) | Almost entirely | Written for any ReXGlue title. Check [platform-notes.md](platform-notes.md) first. A few settings are named after this game (`masseffect_*`) |
| `sdk/` code generator options (locals, arguments in registers, diet) | Yes | Selected from the game's toml; see [toolchain.md](toolchain.md) |
| `tools/` generation scripts and checks (gaps, `direct_calls`, `read_before_write`, `verify_pch`) | Mostly | Several contain this game's addresses (the `pch_*.py` patches); the gap tools and `codegen.sh` do not |
| `mesa/` driver patch and build scripts | Yes | Game independent. See [mesa.md](mesa.md) |
| `shaders/` package format, packer, index, validators, XenosRecomp | Yes | The scanner (`ue3_shader_scan`) is specific to Unreal Engine 3 packages; for another engine you need your own way to find the containers |
| `installer/` | Yes, with edits | Edition table, names and the disc exclusions are in `config.js` |
| `app/src/native/` renderer | Partly | The PM4 reader, EDRAM bookkeeping, texture cache and pipeline cache are general; the hook addresses, the render pass knowledge and the shader identification are specific to this game's Direct3D and engine |
| `tests/`, `tools/console-test/` | Yes | Standalone tests and the console measuring cycle |

## 1. Get the game running on a PC first

**Why:** every problem of the translation is much easier to find and fix on a computer than on the console.

1. Extract the disc (`tools/extract_iso.py`) and translate the game's `default.xex` to C++ with
   [ReXGlue](glossary.md#rexglue-the-sdk) on your computer (`tools/build_host.sh`, `tools/codegen.sh`).
2. Fix what the translation gets wrong. It usually misses some functions, some jump tables and some function chunks.
   Declare the missing code in a hand-written toml like `app/overrides.toml`, and find it with `tools/find_gaps.py`,
   `tools/gap_fixpoint.py`, `tools/pointer_scan.py` (code pointers in data) and `tools/runtime_gap_resolver.py` (from the
   log of a game that stopped on an unregistered function). Every entry needs its evidence.
3. Get the game to start and play with ReXGlue's own graphics, which imitate the Xbox 360 GPU. Here that was done on
   macOS first (stage 1); the first frames on the Switch came later.

Two options of the code generator are worth knowing from the start: `share_registers` (the pieces of a split function
share their register locals; marking ordinary functions is dangerous, see [toolchain.md](toolchain.md)) and the register
options that make the code faster. Turn them on after the game runs, not before.

## 2. Move it to the Switch system

The Horizon layer should work for another game as it is. It gives the game its memory, catches its crashes, runs its
threads with the right priorities, and provides clocks, audio output and the showing of frames on screen. The problems
that cost the most time are in [platform-notes.md](platform-notes.md). The main ones, in short:

- **There is a limit on memory mappings.** Horizon limits how many pieces of memory a program can map (error `2001-0103`),
  and the Xbox 360's mirror views use up many of them. Commit in 2 MB granules and commit physical memory when touched.
- **The process needs the whole application memory.** In album applet mode the process gets about 400 MB. Use a game
  override or a forwarder, and set the loader's address space as the README says.
- **Two crashes at the same time break each other.** libnx has one exception stack, so the SDK hands out a set per thread.
- **The kernel loses vector registers in the exception handler** when it preempts a thread there. The only real cure is to
  avoid emulated reads, which is why the memory-watching GPU emulation has to go.
- **Only priority `0x3B` is time-sliced.** The game's busy waits must run at `0x3B` and the host's threads above them.
- **Never read `cntvct_el0`.** It faults on Horizon.
- **The game gets three cores, not four.**

## 3. First frames, then a faster renderer

ReXGlue's graphics imitate the Xbox 360 GPU: they translate its commands, imitate its [EDRAM](glossary.md#edram), convert
shaders while playing and watch pages of guest memory. That works on the Switch, and it is the right way to see the first
frames. For Mass Effect it was far too slow (menus at 1.5-2.5 fps) and it crashed in game, for the reasons in
[native-renderer.md](native-renderer.md) and [performance-history.md](performance-history.md).

So the port has its own renderer. The game keeps using its own [Direct3D](glossary.md#direct3d), which writes its list of
GPU commands (the [PM4 ring](glossary.md#pm4-ring)) as always. The [ring thread](glossary.md#ring-thread) reads that list
and draws the same thing directly with Vulkan. What you can reuse from `app/src/native/`: the reader of the command list,
the tracking of the GPU state, the recording of each draw, the EDRAM bookkeeping (one Vulkan image per format and
[transfers](glossary.md#transfer) between them: "mode 4"), the texture cache, the pipeline cache and its pre-warm, and the
pattern of replacing the busiest game functions with native code, each with a [guard](glossary.md#guard).

What is specific: the addresses of the [hooks](glossary.md#hook), and knowing the engine's render passes. Start with a
tracing phase: wrap the game's Direct3D functions and write every call to the log (`app/src/me_d3d_trace.cpp`). That
confirms each address before you replace anything. Build the renderer on the computer first, where correctness is the
difficulty; speed has to be checked on the console.

Things that cost days here, and will cost you: matching the Xbox 360's depth, MSAA and EDRAM formats exactly (an
engine that stores its HDR scene as packed 7e3 floats and re-reads it in other formats forces many conversions);
a setting that made shaders write depth by hand and turned off early-Z (under 1 fps); and a request for the GPU's official
460.8 MHz handheld profile that had been forgotten.

## 4. Translate the shaders before playing

**Why:** translating [shaders](glossary.md#shader-vertex-shader-pixel-shader-fragment-shader) while playing causes stutters and costs CPU.

1. Find the shader containers in the game's data (in Mass Effect, the Unreal packages) and translate them before the game
   runs: [XenosRecomp](glossary.md#xenosrecomp) turns the microcode into [HLSL](glossary.md#vulkan-spir-v-hlsl-dxc), and
   [DXC](glossary.md#vulkan-spir-v-hlsl-dxc) turns the HLSL into [SPIR-V](glossary.md#vulkan-spir-v-hlsl-dxc).
2. Put everything in a [shader library](glossary.md#shader-library-mesp), where each shader is found by a fingerprint of its
   container. The tools are in `shaders/`, and [shaders.md](shaders.md) explains them.

XenosRecomp needed several fixes for this game, and some translation choices changed the GPU time a lot: a vertex shader that
writes only the outputs its pixel shader reads (the scene went from 66 to 36.6 ms), and constants in one dynamic buffer.
The installer page builds the library on the user's own computer so no game data is ever shared (its in-browser DXC is not
finished yet).

## 5. Measure on the console

Read [measuring.md](measuring.md) before you optimize anything. The key points:

- The PC tells you where the work is, but never how much it costs on the Switch. It hid costs that the Switch's slow cores
  made visible.
- One change per run, behind a setting. Compare two builds back to back in the same session. Run-to-run noise is about 3 fps.
- Judge CPU changes in core-milliseconds per frame, not in fps.
- Acceptance is a cold start (no caches). Judge a picture from many captures, never two or three.
- `tools/console-test/` has the scripts: deploy, an unattended cycle with a recorded route, a location sweep and the
  core-ms computation.

## 6. Optimizations you can reuse

In the order they paid off here ([performance-history.md](performance-history.md) has the numbers):

- **The GPU profile and the pipeline between CPU and GPU:** the official 460.8 MHz handheld profile, three work slots, and
  asynchronous output removed an idle gap of 207 ms.
- **EDRAM transfers:** make them rare and cheap. Redirected clears and proofs that a full-screen pass overwrites everything
  took the frame from 6 to 14 fps. This is the biggest game-specific effort and the most reusable idea.
- **The generated code:** direct calls between translated functions, guest accesses without `volatile`, registers as locals
  and the "diet" lowerings. The main thread went from 32.4 to about 29 ms.
- **The game's own busy waits.** The Xbox 360 Direct3D waits for the GPU and for other threads by spinning. On three slow
  cores that steals time from the threads that do real work.
- **CPU cost of each draw in the renderer and the driver:** caches instead of repeating work, no allocations or log lines on
  the ring thread, no duplicate uploads (see [mesa.md](mesa.md)).
- **Native replacements** of the busiest game functions, each checked against the original by a differential fuzzer
  (`tests/hot_fuzz`). Each saved 0.1-0.8 ms or less, and thirteen more gave nothing measurable.

## 7. What did not work

- **Link-time optimization, profile-guided optimization, smaller code and function ordering** gave no measurable gain on
  millions of lines of generated code.
- **Arguments in registers:** about 10 % fewer instructions, but no gain on top of the locals and the diet.
- **Lower internal resolution on a CPU-bound frame, or off the tile grid:** no gain, sometimes worse.
- **Asynchronous pipeline compile:** the pop-in was not acceptable.
- **The official CPU boost:** it throttles the GPU to 76.8 MHz.
- **Overclocking** hides problems instead of solving them: with a fast CPU the stock GPU became the limit, and the other way
  round. This port was measured and tuned at the console's normal clocks.
- **A fourth CPU core:** researched, never run.

A rule that held throughout: write down every idea and its result, also the ones that changed nothing, and do not retry one
without new data ([optimization-paths.md](optimization-paths.md) is that record).
