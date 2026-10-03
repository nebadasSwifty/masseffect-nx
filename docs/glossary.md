# Glossary

Every technical word used in the performance, measuring, renderer and known-issues documents, explained in plain words.
If a document uses a term you do not know, look for it here. The terms are grouped: the Xbox 360 and the game, the
recompilation, the Switch, graphics, building, measuring, and this port.

## The Xbox 360 and the game

### Guest

The original game and everything that belongs to its world: its code, its memory, its threads. "Guest memory" is memory as
the game sees it. The opposite is the [host](#host).

### Host

The machine that really runs the port: the Switch, or a PC (macOS) while developing. "Host code" or "native code" is code
written for the port itself.

### PowerPC

The kind of processor the Xbox 360 has. The game's code is PowerPC code, which the Switch's ARM processor cannot run;
that is why it is [recompiled](#static-recompilation).

### Xenos

The Xbox 360's graphics chip (its GPU). It has no direct equivalent on the Switch.

### EDRAM

10 MB of very fast memory inside the [Xenos](#xenos) where the Xbox 360 draws. It is so small that the game draws large images
in pieces ([tiles](#tile)) and then copies the result to normal memory ([resolve](#resolve)). The same words can be read in
several formats at different times, which a PC or Switch GPU cannot do. See [EDRAM mode 4](#edram-mode-4).

### Tile

A block of EDRAM: 80x16 pixels at 32 bits per pixel and no multisampling. A 960x544 image is 408 tiles; 1280x720 is 720.
The EDRAM bookkeeping of the port works tile by tile. The 16-pixel tile height is why internal heights that are a multiple
of 16 (544) are faster than ones that are not (540, 450).

### Resolve

The copy of a finished surface from EDRAM to a texture in guest memory, usually with a conversion. The port keeps the
result as a Vulkan image, not as bytes.

### Exponent bias

A setting of a resolve that multiplies every colour by a power of two. Unreal Engine 3 asks for -3 (divide by 8) on its
HDR resolves; ignoring it made the picture overexposed.

### 7e3

A packed 32-bit colour format: three 10-bit floats (7 bits of mantissa, 3 of exponent) and no alpha. The engine keeps its HDR
scene colour in it. Reading the same words as an ordinary 8-bit or 10-bit colour needs a [conversion](#transfer).

### UNORM10, RGBA8, raw64, FP16

Colour formats of the same EDRAM words: RGBA8 is four 8-bit channels; UNORM10 is three 10-bit channels plus 2-bit alpha;
FP16 is half-float channels, 64 bits per pixel, and "raw64" treats those 64 bits as bits, preserving NaN and sign bits.
The port calls the views f0 (RGBA8), f2 (UNORM10), f3 (7e3) and f7 (raw64).

### Float24 (20e4)

The Xbox 360's 24-bit floating-point depth value (20e4: 20 bits of mantissa, 4 of exponent), stored together with 8 bits
of [stencil](#stencil). The host depth buffer is a 32-bit float.

### Half-range depth

The Xbox 360 depth can cover [0, 2); the port halves the value so it fits the host's [0, 1].

### Stencil

An extra 8-bit value per pixel used for masking. The Switch's GPU cannot write it from a shader, which is why transfers use
the [copy engine](#copy-engine) for stencil.

### MSAA (multisampling)

Drawing several samples per pixel to smooth edges. The scene is drawn by the game at 2x. The port imitates its sample
layout in single-sample images; true multisampling is not integrated.

### Direct3D

Microsoft's graphics interface. On the Xbox 360 the game links the development kit's Direct3D into itself; it keeps
a mirror of the GPU registers and writes the [PM4](#pm4-ring) packets. The port keeps it as it is.

### Unreal Engine 3 (UE3)

The game engine of Mass Effect. It builds shader objects from its own cache, keeps shaders inside compressed
packages, and runs its own render thread.

### Scaleform

The user-interface system used by the game (menus, HUD). Its stage was laid out at 1280x720 and had to be told the
internal resolution.

### XMA

The Xbox 360's audio compression. The port decodes it on the CPU with FFmpeg.

### PhysX

The physics engine used by the game. Its step runs on its own thread and the main thread joins it.

### Coalesced.ini

The game's configuration file inside its data folder (graphics options such as shadow filter, motion blur, decals). A
version without the logo movies is used for tests; the original is restored afterwards.

### Eden Prime, Citadel, Normandy

Levels of the game. Eden Prime (the opening level) with its save is the test scene for every run; Citadel (STA00) is
the GPU-heavy one. Map names such as PRO00, STA00, NOR00, LAV00, ICE00, WAR00, JUG00, END00 are the game's own file names.

## The recompilation

### Static recompilation

Translating a program's machine code into C++ ahead of time, once, instead of running it in an interpreter. The Mass
Effect program is turned into about 48,000 C++ functions (`sub_XXXXXXXX`, named by their Xbox 360 address) that are built for
the target.

### ReXGlue (the SDK)

The recompilation toolkit this port is built on: the code generator ("codegen"), the runtime that imitates the Xbox 360's
operating system (threads, files, audio) and a GPU backend. The port includes its own modified copy.

### ctx

The structure in memory that holds the PowerPC registers of a recompiled function. About 20 % of the generated
instructions were loads and stores of it. Many options keep registers in real ones instead.

### Register

A fast storage cell of a processor. PowerPC has r0-r31 (integers), f0-f31 (floating point), vector registers, and special
ones (lr, the link register that holds a return address; ctr; cr; xer; msr).

### Hook

A replacement or wrapper for a game function, written in C++. Hooks add what the ring does not say (which shader a draw uses),
replace slow functions, or fix behaviour.

### Guard

A self-check of a hook or a fast path: for the first N uses both the old and the new path run and are compared; any
difference is logged ("DIFFERENCE") and the new path is switched off.

### Native replacement, hot function

A hot function is a game function where much CPU time is spent. A native replacement is a hand-written C++ (often NEON)
version of it, checked against the recompiled original by fuzzing and by a guard.

### Direct call

The generated code normally calls other functions through a table. A direct call goes straight to the function, saving the lookup.

### `volatile`

A C++ keyword that forces every memory access to really happen. The generated code used it for every guest load and store;
removing it let the compiler optimise, but it can delete a loop that only waits for memory to change.

### `share_registers`

A code generator mark for functions that share registers with their callers (split-off pieces of a function, exception
funclets). Marking an ordinary function by mistake once corrupted a register and crashed the start-up.

### Registers as locals (locals options)

Code generator options (`non_volatile_as_local`, `non_argument_as_local`, `cr_as_local`, `xer_as_local`, `ctr_as_local`,
`reserved_as_local`) that keep a register class in C++ local variables, where the compiler can use real machine registers, instead
of in [ctx](#ctx).

### Arguments in registers

A code generator pass: functions receive the PowerPC argument registers as C++ arguments and return r3 as the C++ return
value, instead of passing them through [ctx](#ctx). A wrapper keeps the old calling convention for indirect calls.

### Diet

The name of a set of bit-exact code generator savings: no link-register stores before calls, simpler addressing, inline
conversions, builtins for compares.

### `skip_msr`

An option that removes the machine-state-register reads and writes the PowerPC code uses around atomic operations.

### Atomic sequence (lwarx/stwcx)

PowerPC's pair of instructions that load a value with a reservation and store it only if nothing else changed it; the
recompiled code needs it for locks and lists.

### Flush-to-zero (FZ)

A floating-point mode in which tiny numbers (denormals) become zero. The PowerPC vector unit has such a mode; scalar
floating-point keeps denormals. The generated code switches between the two modes.

### Fused multiply-add (FMA), contraction

A single instruction computing a*b+c with one rounding. A compiler may fuse separate multiply and add; the result then
differs slightly from the PowerPC, so fusing is turned off (`-ffp-contract=off`).

### NaN

"Not a number", a floating-point value with a sign and payload bits. Native replacements and tests must reproduce them
exactly, so the tests canonicalise them.

### Fuzzing, differential test

Running two versions of a function on many random inputs and comparing the results.

### setjmp / longjmp

C functions that save and jump back to a place in the program. The recompiled version cannot return to the saved place.

### toml

A simple text format for settings files. Code generator options and run settings are toml files.

### Setting (cvar)

A named option of the program, set in a toml file or on the command line. Names starting `masseffect_` belong to this port;
SDK settings have no prefix.

## The Nintendo Switch

### Horizon

The Switch's operating system.

### Tegra X1, Cortex-A57, GM20B (Maxwell)

The Switch's chip. Its CPU has four ARM Cortex-A57 cores at 1020 MHz (stock); the game gets three (the fourth belongs to the system). Its
GPU is NVIDIA's GM20B, of the Maxwell family.

### Stock clocks, overclock

Stock clocks are the speeds the console uses by itself. An overclock raises them with a homebrew tool. This project does not use
one (diagnostic runs only, t210 and t211).

### Official CPU boost

A system request (`appletSetCpuBoostMode`) that raises the CPU but lowers the GPU to its minimum; unusable.

### Atmosphère, homebrew, NRO, title takeover

Atmosphère is the custom firmware that allows homebrew (unofficial programs). An NRO is a homebrew executable. Title
takeover (holding R when launching a game) starts the homebrew with the memory of that game, which this port needs
(a process limit of 3189 MB).

### sys-ftpd, sys-botbase

System modules on the console: an FTP service for uploading files and downloading logs, and a service that receives controller
input.

### SaltyNX overlay

An on-screen frame-rate display. It froze in later runs; use the profile instead.

### libnx, `armGetSystemTick`

The Switch homebrew library, and its function that reads the system counter. Reading the `cntvct_el0` register directly faults
on Horizon.

### NEON

The ARM vector instruction set. Used for fast copies, hashing and some native replacements.

### Core, thread priority

A thread runs on one core at a time; a lower priority number means a higher priority. The main game thread, the UE3 render
thread, the ring thread and the audio threads share cores 0-2.

### Preemption

The kernel stopping a thread to run a more important one. While a thread is inside its exception handler, the kernel loses half
of its vector registers when preempting it; this crashed the first, emulated GPU path.

### APM (performance mode)

The system's performance configuration. The port asks for the official handheld GPU profile (460.8 MHz).

### GPFIFO, push buffer

The queue of commands a Switch GPU reads. If a submission fills it exactly, the next submit failed and the GPU was lost until the
driver reserved a tail.

### SD card layout

The game runs from the SD card: the NRO, the shader library, the game files (`game_root`), caches.

## Graphics

### Vulkan, SPIR-V, HLSL, DXC

Vulkan is the graphics interface the renderer uses. SPIR-V is Vulkan's shader binary format. HLSL is a shader language;
DXC compiles HLSL to SPIR-V.

### XenosRecomp

The translator from Xbox 360 shader microcode to HLSL.

### Shader, vertex shader, pixel shader (fragment shader)

A small program on the GPU. A vertex shader runs per vertex and positions it; a pixel shader runs per pixel (per fragment) and colours it.

### Microcode, container

Microcode is the Xbox 360 shader's machine code. A container is the package of microcode with its constant tables, as stored by the engine.

### Shader library (`.mesp`)

The file of translated shaders built from the disc, with an index file (`.mesp.idx`). Entries are looked up by a hash of the microcode.

### Vertex variant

Direct3D rewrites a vertex shader in place to match the vertex declaration; the rewritten one is a variant of the library's entry.

### Pipeline, pipeline cache

A pipeline is the compiled state for one set of shaders and render state; creating one costs tens of milliseconds. The pipeline cache stores
compiled pipelines on the SD card so later runs skip the compile; Mesa has its own shader cache too.

### Specialisation constant

A value fixed when a pipeline is created, used to choose a shader variant without recompiling the shader source (for example,
which outputs the vertex shader writes).

### Varying (interpolator, output)

A value a vertex shader passes to the pixel shader. NVK links none, so outputs the pixel shader does not read can simply not be written.

### Draw, draw call

One command to the GPU to draw a batch of triangles. A frame has thousands (about 2500 in the scene pass).

### Swap, frame, fps

A swap presents a finished frame. fps is frames per second; 30 fps is a 33.3 ms frame.

### Prepass

An early pass that draws only depth, so later passes can skip hidden pixels. The scene's prepass is skipped by the port.

### Early-Z

Testing a pixel's depth before running its pixel shader, so hidden pixels cost nothing. A shader that writes depth by hand turns it off.

### ZCULL

The GPU's coarse hierarchical depth test, which rejects blocks of hidden pixels. Needs the depth image to be set up for it.

### Barrier, wait for idle (WFI)

A barrier orders GPU work. On this GPU many barriers, and every switch between engines, are a wait until the GPU is idle.

### Render pass, subpass, load/store op

A Vulkan unit of drawing into attachments. Load and store ops say whether old contents are kept; they cost nothing on this GPU.

### Compute shader, fragment pass

Two ways to move or convert data on the GPU: a compute shader (a general program run over a range) and a fragment pass (a draw
into the destination, so the pixel shader does the work). Switching between engines costs a wait for idle.

### Copy engine

The GPU's fixed-function copier, a separate engine from drawing. The port uses it to write stencil, because a shader cannot. Switching to it costs a wait for idle.

### Descriptor, push descriptor

The record that tells a shader which texture or buffer to use. On pre-Turing GPUs binding a normal descriptor set splits the command buffer.

### Uniform buffer (UBO), constants

A block of constant values for a shader. The port gives shaders their constants as dynamic uniform buffers.

### Fence, queue, submission, work slot

A fence tells the CPU a piece of GPU work has finished. A queue receives submissions. A work slot is a set of resources for one frame in
flight; three slots let the CPU and GPU overlap.

### Alpha test, KILL, blend, scissor, viewport

Alpha test discards pixels by transparency; KILL is the shader instruction that discards; blend mixes colour with the existing one; the
scissor and the viewport limit and map the drawing area.

### HDR, tone map, post-processing, gamma ramp

HDR is a colour range above 1. The tone map converts it to display range. Post-processing (blur, film grain, motion blur) works on the finished scene. The gamma ramp is the
output colour curve.

### Texture untiling, mip levels, gamma sign

Xbox 360 textures are stored in tiled order and must be reordered. Mip levels are smaller copies for distant surfaces. The "sign" of a texture fetch tells the shader to apply a gamma curve
or sign conversion.

### Occlusion query

A GPU counter of how many pixels passed the depth test; the game polls it.

### MoltenVK

The Vulkan layer over Apple's Metal used by the PC (macOS) build.

### Mesa, NVK, NAK

Mesa is the open graphics driver collection. NVK is its Vulkan driver for NVIDIA GPUs; NAK is its shader compiler. A build of them for the Switch is linked into the program.

## Building

### LTO, PGO, BOLT

Link-time optimisation lets the compiler see all files at once. Profile-guided optimisation uses a recording of a run to place and
inline code. BOLT rearranges a finished binary from a profile. LTO and PGO gave no gain here; BOLT was never tried.

### Function ordering

Placing the hottest functions together in the binary.

### `-O3`, `-Os`

Compiler flags: `-O3` optimises for speed, `-Os` for size. `-Os` was slower.

### devkitA64, Docker

The compiler toolchain for the Switch, run inside a container.

## Measuring

### Core-ms per frame

CPU percentage of a thread (or all threads) times 1000 divided by fps: how many milliseconds of one core a frame costs. The 30 fps budget with three cores is about
100 core-ms per frame. It is more stable than fps for judging CPU changes.

### Interval, run (tNNN), route, leg

An interval is a 10 second block of the profiler report. A run is one automated test, named t1, t2, ... t313. A route is the
fixed walk of the test scene and a leg is one stretch of it, ending in a capture.

### Warm start, cold start

A warm start has the shader and pipeline caches; a cold start has none and must compile every pipeline.

### A/B test, noise

Comparing two builds under the same conditions. Noise is the run-to-run spread of the same build (about 3 fps).

### Probe

A change that alters the image on purpose to find the most an optimisation could give.

### Idle gap ("gap")

GPU time per frame when the GPU waits for the CPU. A large gap means the CPU is the limit.

### Busy, regime

GPU busy time is the frame time minus the gap. A regime is a stable pattern of the run: a fast one near 27 fps and a slow one near 24.

### Stack profiler, ELF, line tables

A sampling profiler that records call stacks. The ELF is the build's executable with symbols; line tables map addresses to source lines.
A profile must be read with the ELF of the same build.

### Category (GPU report)

The grouping of GPU time by kind of pass: scene, shadows, copies, EDRAM conversions, post.

## This port

### PM4 ring

The list of GPU commands (PM4 packets: register writes, constants, shader loads, draws, resolves, swap) that the game's Direct3D
writes into a ring buffer in guest memory. The renderer reads it instead of letting a GPU execute it.

### Ring thread

The thread that reads the [PM4 ring](#pm4-ring), tracks GPU state and records Vulkan commands. About 35 µs per draw at the t172 profile; one of the
serial stages that limit the frame.

### Game thread, main thread, render thread

The **game thread** (also called the main thread) runs the game's own logic and takes 29-33 ms per frame. The **render thread** is Unreal Engine 3's
render-submission thread. Together with the ring thread they are the serial stages that limit the frame.

### Deferred recording (C20)

The ring thread queues its Vulkan calls and a worker records them, so one core is not the whole wall.

### Vertex dedupe, fingerprint

Draws in one frame that ask for the same vertex data upload it once. A fingerprint is a hash used to check that the data is the same
(XXH3, a fast hash; CRC32 is another).

### EDRAM mode 4

The renderer's complete mode for imitating EDRAM: tiles are owned by views of different formats, with exact transfers between them. Other modes (0, 2, 3) are incomplete.

### Ownership, sync, publish

Which view holds the current data of a tile (ownership); bringing a draw's tiles into its view before drawing (sync); recording that the view now owns what it wrote (publish).

### Transfer

Moving tile data between views: an **import** into a depth or colour view, an **export** out of one, an **alias conversion** between colour
formats of the same words. Transfers run on the compute or copy engine, so each costs a wait for idle, and the data volume is the real cost.

### Redirected clear

Applying a proven clear value in the view that owns the tiles instead of drawing the clear through another view.

### Overwrite proof, estimator

A proof from the vertex shader (read by the CPU) that a pass covers a whole rectangle; the estimator computes that rectangle. If a pass overwrites everything, the transfer
that would load old data is skipped.

### Canonical stencil clear

A clear that writes only the stencil of tiles whose depth is kept.

### Resolved texture, logical size

The Vulkan image that holds a resolve. Its logical size is the picture's real width, smaller than the padded pitch of its memory.

### Work slot

See [Fence, queue, submission, work slot](#fence-queue-submission-work-slot).

### Black shards (B1)

Black polygon-shaped patches on terrain, caused by the prepass depth disagreeing with the material pass; fixed by skipping the prepass.

### "diag" settings

Settings named `diag` were meant as diagnostics; several are now required (renaming is pending).

### Best profile

The settings file of the best-performing configuration (for example "best"); it overrides some code defaults.

### Backlog

The development list of every idea, source, status and result. Ids such as E11, S6, C34, U1 and Y3 come from it: E for EDRAM, S for scene, C for CPU, U for start-up, Y for host overhead, G for GPU
heavy views, M for measurement, R for resolution, B for bugs.

### Stages

Stage 1: the game on a PC. Stage 2: the first Switch boot with the SDK's GPU emulation. Stage 3: the native renderer. Stages 4 and 5:
stabilisation and the Switch build. Stage 6: the first Switch optimisations.

## General words

### Thread, mutex, lock-free queue

A thread is a line of execution. A mutex is a lock that lets one thread at a time into a piece of code. A lock-free queue passes
items between threads without a lock.

### Poll, spin, wake-up

To poll is to check a value again and again; to spin is to poll without sleeping (it burns a core). A wake-up is the
system waking a sleeping thread; thousands per second cost CPU.

### Memo, cache, hash

A memo or cache stores a result to avoid recomputing it. A hash is a short number computed from data, used to compare large
data cheaply.

### Byte-swap, big-endian

The Xbox 360 stores numbers with the most significant byte first (big-endian); the Switch does the reverse, so guest data must
be byte-swapped.

### Inline, spill, compiler barrier

Inlining puts a function's body into its caller. A spill is a value the compiler has to keep in memory because it ran out of registers.
A compiler barrier (`asm volatile("" ::: "memory")`) stops the compiler moving or deleting memory accesses across it.

### Bit-exact, bisect

Bit-exact: identical to the last bit. To bisect is to find which change causes a problem by halving the set of changes.

### Smoke test, capture, contact sheet

A smoke test boots a map and checks it runs without errors. A capture is a screenshot, and a contact sheet is many captures on one image.

### FTP, HOME menu

The console's FTP service is how files are uploaded and logs downloaded. The HOME menu is the console's start screen; the test cycle starts
from it.

