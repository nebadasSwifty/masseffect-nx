# Horizon and NVK: platform notes

Things about the Switch's operating system ([Horizon](glossary.md#horizon)) and its graphics driver that cost a lot of
time to find out. The comments in the SDK's Switch layer (`sdk/src/**/*_switch.*`, `sdk/src/ui/switch_*.cpp`) point here
for their reasons; this page keeps the same claims in one place. Each fact says how it is known: **measured** on the
console (or read from the kernel sources), or **inherited** from the other ports this Switch layer comes from and not
rechecked for Mass Effect. Words you do not know are in the [glossary](glossary.md).

## In short

- A process cannot map memory freely: there is no sparse memory, no file `mmap`, and the pieces of mapped memory are
  limited in number. The guest's memory is committed in 2 MB granules and the physical memory only when it is touched.
- Only the thread priority `0x3B` is time-sliced. Every other priority is cooperative: a thread that never blocks keeps
  its core. The recompiled game spins constantly, so the game's threads run at `0x3B` and the host's service threads
  run above them.
- Faults arrive through one handler on one stack, cannot make system calls, and cannot be thrown as C++ exceptions. While
  a thread is in that handler the kernel loses half of its vector registers if it preempts it.
- The official CPU boost throttles the GPU to its minimum. Never read `cntvct_el0`: it faults.
- The process gets about 3189 MB. In album applet mode it gets about 400 MB and the game cannot start.

## Hardware at stock clocks

| Part | What the port runs on |
|---|---|
| CPU | 4 ARM Cortex-A57 cores at 1020 MHz. The port uses three (cores 0-2) |
| GPU | NVIDIA GM20B (Maxwell). Handheld default 307.2 MHz; the port asks for the official 460.8 MHz handheld profile; docked it is 768 MHz |
| Memory | 1331.2 MHz handheld, 1600 MHz docked. The port never raises it |
| RAM | 4 GB shared by CPU and GPU. The process memory limit is 3189 MB with title takeover (**measured**) |

On the cores: the glossary and every measurement here use three cores. A comment in the SDK says core 3 is reserved by
the system except in title takeover; the project never ran a build that uses it (the user declined to change the console's
system configuration, see [optimization-paths.md](optimization-paths.md)), so this is **not verified**.

## Guest memory

All of this is in `sdk/src/core/guest_memory_switch.cpp` and `memory_switch.cpp`, and was **measured** on the console.

- **A 4.5 GB window, but no sparse memory.** The Xbox 360 memory map needs a contiguous reservation of `0x120000000`
  bytes of address space, with several windows that alias the same physical memory (`0xA0000000`, `0xC0000000` and
  `0xE0000000` are mirrors). Horizon allows reserving address space, but not committing it page by page. The window is
  reserved empty and memory is committed in chunks, each mapped into every view that covers it, a view on its first
  access.
- **Commit in 2 MB granules.** The first versions committed 64 KB at a time. The kernel ran out of memory blocks (error
  `2001-0103`) at 329 MB of backing, long before the mapping limit. Now every commit is rounded up to a 2 MB granule.
- **The detour through code memory.** `svcMapProcessMemory` rejects heap memory as its source (error `0xD401`). The
  backing is allocated from the heap, turned into a code alias with `svcMapProcessCodeMemory`, and then mirrored. After
  that call the original heap pointer has permission 0 and must not be touched.
- **Permissions cannot be changed on the window.** The mapped memory lacks the permission-change flag, so "protecting" a
  page means unmapping it, and its permissions are kept in a table of the SDK's own. A read of a watched page then faults
  and is emulated. The code tries once to lower the permission to read-only instead (so reads are free) and falls back
  to unmapping if the kernel refuses; the outcome is reported in the profiler log. The native renderer never watches
  memory, so in play these faults are rare. Under the SDK's own GPU emulation they were 47,000 to 390,000 per second
  (see [performance-history.md](performance-history.md)).
- **Physical memory is committed when touched.** Committing the 512 MB of physical memory at start-up is not possible: each
  chunk is mapped into the Xbox 360's five views of it, so the process was accounted for more than 2 GB and the kernel
  refused the next allocation. The first access to an uncommitted page lands in the exception handler, which commits a
  block and retries the instruction.
- **The heap is capped.** `__nx_heap_size` is 1024 MB (`runtime_switch.cpp`). With libnx's default (all available memory)
  the game died at about 1.8 GB mapped, because the mappings need kernel memory of their own for their page tables. The
  1024 MB hold the guest backing (506 MB measured), the host thread stacks and what Mesa asks for. What the GPU uses
  (textures, buffers, compiled shaders) does not come from this heap.
- **Applet mode is not enough.** The program declares itself a full application (`__nx_applet_type =
  AppletType_Application`), which goes with title takeover. In applet mode (for example, started from the album) the
  process gets about 400 MB, and the guest's physical memory alone is 512 MB. This is why the game is started with a
  game override (hold R) or a forwarder.
- **The 3189 MB limit is close.** Before the shader package was loaded by index, the process ran within about 4 MB of it;
  the index freed about 800 MB. How much headroom the longest levels have is not measured
  ([known-issues.md](known-issues.md)).
- **39-bit address space.** The other ports of this family tell forwarder makers to choose a 39-bit address space, and
  this port reserves a 4.5 GB window, which needs plenty of address space. No test of this port with a smaller address
  space was recorded, so the advice in the README is inherited and **not verified**.
- **No file `mmap`, no `dlopen`.** Files are read into RAM when a pointer is needed (`mapped_memory_switch.cpp`), so a
  7 GB disc image cannot be used: the game's files must be extracted. Everything is linked statically; the GPU plugin is
  registered statically.
- **`pread` is emulated** with `lseek` plus `read`, with one lock per descriptor spread over 64 stripes, so two different
  files do not wait for each other (the game reads its data from several threads).

## Exceptions

`sdk/src/core/exception_handler_switch.cpp`, all **measured**:

- There are no signals. A fault reaches `__libnx_exception_handler`, which libnx calls as a normal function on a stack of
  its own. Returning from it kills the process. To continue, the SDK raises a trap exception and returns a corrected
  context with `svcReturnFromException`.
- The fault syndrome says whether the access was a read or a write but arrives without the register or the size
  (`ISV = 0`), so emulating an access means decoding the instruction.
- libnx accepts one handler. What are separate signal handlers on other systems (memory-mapped I/O, write watching,
  structured exceptions) all go through it, in a fixed order: commit uncommitted physical memory, emulate reads of
  watched pages through the shadow alias, the SDK's own handlers, structured exceptions if used, and otherwise a fatal
  stop with the registers, the address and the stack in `logs/rex/rex_crash.log` (error 2345-0102).
- **Two faults at once broke the game.** libnx has one exception stack and one dump, both global. With twenty-odd threads
  faulting, two coincided: the second overwrote the first's registers, and the first resumed with the wrong ones (silent
  corruption). The fix is eight stack and dump sets taken with `ldaxr`/`stlxr`; a thread waits if none is free.
- **Half of the vector registers are lost.** When a thread inside a user exception handler is preempted outside a system
  call, the kernel saves only `q8`-`q15`; `q0`-`q7` and `q16`-`q31` come back from a stale save area (read in the
  Horizon kernel sources). Mass Effect crashed on a guest pointer held in `v29` across three emulated reads. The handler
  now stores the vector registers as its first instructions and reloads them just before returning, which narrows the
  window to a few instructions: the corruption in a forced-preemption probe went from 3.6 % to 1.2 % of emulated reads.
  The rest is a property of the kernel that user code cannot close, so the only lever is the number of emulated reads,
  and that is why the native renderer replaced the SDK's GPU emulation.
- **No structured exception handling.** A fault cannot be turned into a C++ exception here: the handler runs on its own
  stack without a frame to unwind through. The generator option `generate_exception_handlers` is off, so the macros are
  not emitted; a fault inside a guest exception scope is a fatal stop. Games that depend on it are not supported by this
  layer.
- Silent deaths are made loud: `abort()`, `exit()`, an uncaught C++ exception (error 2345-0104) and writes to standard
  error are logged under `logs/rex/` (`switch_crash_hooks.c`, `runtime_switch.cpp`). Returning from `main` is not a clean
  exit on Horizon (libnx never runs static destructors and the threads would outlive the NRO), so the port asks the system
  to close it (`windowed_app_main_switch.cpp`).

## Threads

`sdk/src/core/threading_switch.*`, `sdk/src/system/xthread.cpp`:

- **Only `0x3B` is time-sliced.** Horizon time-slices threads at the preemption priority and treats every other priority
  as cooperative. Recompiled code busy-waits constantly, so the game's own threads run at `0x3B` and host service threads
  run above them (a lower number) so they can preempt the spinners:

  | Priority | Threads |
  |---|---|
  | `0x2B` (audio) | the audio worker and the XMA decoder (the name is compared by prefix: with an exact comparison they stayed at `0x2C`) |
  | `0x2C` (presentation) | the GPU command and vblank threads, the kernel dispatch thread |
  | `0x3B` (guest) | every thread created by the game |

  The render thread of the game can be raised to `0x2B` with `masseffect_exclusive_core` (see below). Some comments in the
  SDK name other priorities for the ring thread (`0x2C` or `0x2D`); the table above is what the code assigns.
  Without this split, audio dropouts and stutter are guaranteed.
- **A neighbour at the same priority starves.** A presentation thread at the ring thread's priority only ran when the ring
  blocked, and it starved. A spdlog worker thread inherits the priority of the thread that creates it, and with the cores
  busy it almost never ran, so a full queue blocked every log call (`log_async`, see below).
- **Priorities are Horizon numbers.** `pthread_setschedparam` passes the number straight through (lower is more
  urgent), without inverting the scale. A thread has no signals, so pausing a thread uses `svcSetThreadActivity`.
- **A preferred core is not a pin.** Horizon accepts a preferred core and moves the thread anyway (0.24 migrations per
  loop, measured). `masseffect_exclusive_core` removes the other cores from the thread's mask. Its values: `core` pins
  the hottest guest thread (the game's main thread) to that core alone and moves every other thread off it; `10 + core`
  also lets light guest threads run on it; `20 + core` raises the render thread to `0x2B`; `30 + core` does both. The
  shipped configuration uses `32`: the main thread on core 2, light threads allowed, render thread raised. It is
  dangerous if that core saturates, which is why it is a setting.
- **Lost wake-ups.** Deciding to sleep on a condition variable and signalling without the lock held hung a worker. Every
  signal in the presenter is sent with the lock held.
- **`log_async`.** The SDK keeps the asynchronous logger off by default and says that, on the earlier title it was
  measured on, turning it on made stutter far worse (the queue blocked the thread that feeds the GPU, and the logger
  thread rarely ran). The shipped `masseffect.toml` turns it on and says it removes the stutter of the periodic flush.
  These two statements have not been reconciled for Mass Effect: if you see long frames in the log, try `log_async =
  false`.
- **Fibers.** devkitA64 has no `ucontext`, so fibers switch contexts in assembly (`fiber_switch.cpp`).
- **Timers.** Never read `cntvct_el0` directly: it faults on Horizon (the game started to a black screen and ended in
  `std::terminate`). Use libnx's `armGetSystemTick`.

## Clocks

`sdk/src/ui/switch_apm.cpp`, `switch_sysclk.cpp`, `switch_saltynx.cpp`:

- **The GPU profile.** In handheld mode the GPU runs at 307.2 MHz by default. The port asks Horizon's performance API
  (`apm`, the one every commercial game can use) for the configuration `0x92220008`: GPU 460.8 MHz with memory left at
  1331.2 MHz. That is not an overclock and touches no sysmodule. Docked, the port asks for nothing.
- **Memory must not rise.** The first version picked `0x92220007` (GPU 460.8 with memory at 1600 MHz) by accident,
  because `apm` takes a configuration number, not MHz. The system applies the change later, so a read right after the
  request sees the old value (it took 0.25 to about 1 s), and lowering the memory with `clkrst` afterwards is accepted
  and then reverted by `pcv`, which reimposes the clock of the active configuration. The cure was the right
  configuration number, a 1.5 s poll instead of a single read, and a watchdog kept as a safety net.
- **The official CPU boost is unusable.** `appletSetCpuBoostMode` raises the CPU to 1785 MHz, and the system throttles
  the GPU to 76.8 MHz while it is on: 3.2-4.5 fps (run t243). The port has a setting for it (`masseffect_cpu_boost`, off)
  only to measure it again.
- **Reverse-NX and sys-clk.** With a docked console and Reverse-NX on "Fake Handheld", the window had been left at 1080p
  while the scene went to 720p; the SDK now follows the effective mode. Sysmodules that set clocks ask the hardware for
  the mode, not the game, so the SDK can send the docked clocks configured in Horizon OC through its manual override.
  The override does not expire if the game closes abnormally. **Do not call a service that is not registered**:
  `smGetService` then waits forever; the SDK asks Atmosphere first (`AtmosphereHasService`).
- **FPS in console overlays.** Status Monitor and its forks read the frame rate from memory that SaltyNX publishes after
  hooking the game's present call. This port goes through none of the hooked calls (NVK is linked inside the NRO), so
  the SDK writes the block itself, in the format those overlays read. The glossary notes that this has been seen to
  freeze in some runs; the profiler log (`logs/rex/rex_profile.log`) is the reliable source.

## Input

- Pads come through libnx's HID (`sdk/src/input/switch/`): up to four pads, handheld, Joy-Con pairs and the Pro
  Controller. A face button acts as the Xbox 360 button with the same letter unless `input_xbox_layout` is set. ZL and ZR
  are digital: the game sees the trigger at 0 or 255.
- The first `padUpdate` can abort with error 2345-0008 if HID was not brought up; the driver initializes it itself.
- Rumble goes through an IPC call that blocks its thread, and the game calls `XInputSetState` very often (5.8 % of the
  thread that prepares each frame, in a profile of the title the driver was first written for), so the driver only
  sends a vibration value when it changes.
- The touch screen is read by the overlays only (the game itself has no touch input). The L+R menu shortcuts are listed in the
  [README](../README.md).

## NVK on Horizon

The graphics driver is NVK from Mesa, linked into the NRO; what the patch changes is in [mesa.md](mesa.md). Facts that
bite:

- **Every fence query is an `ioctl`,** even when the GPU has already finished. The profiler counts the libnx calls that
  cost IPC or walk memory (fences, submissions, new NvMaps, GPU mappings, cache cleans, window queue, sleeps), with the
  time the calling thread spends inside, because their count per second decides what is worth changing.
- **The command queue is finite.** The GPFIFO holds 0x800 entries and a submit needs up to three more than the skid the
  driver reserved. A batch that filled the queue made the next submit fail with `no queue space`, which NVK treats as a
  lost device ("Graphics device lost" after minutes of play). The patch reserves the tail.
- **Maxwell has no stencil export from a shader,** so stencil moves through the copy engine (see
  [native-renderer.md](native-renderer.md)), and pre-Turing GPUs fetch descriptor sets unprefetched at each non-push bind.
- **Early-Z and ZCULL are easy to lose.** A shader that writes depth by hand turns both off on this GPU. A setting that
  did that by mistake made the scene run at under 1 fps (see [performance-history.md](performance-history.md)).
- **Timestamps are in odd units:** NVK GPU timestamps must be multiplied by 1.627 ([measuring.md](measuring.md)).
- Mesa links against `libelf` for a NVIDIA binary-format parser that never runs here; `switch_elf_stubs.c` satisfies the
  linker. NVK reads its environment (`NVK_SWITCH_*`, `MESA_SHADER_CACHE_DISABLE`) when the Vulkan device is created, so
  the app sets it early.

## Presentation

Measured by the SDK layer on the console, on the title it was first written for, and kept as design rules:

- **The surface is FIFO with implicit vsync.** With that, the SDK's paint mode used to fall back to painting from the UI
  thread, and its mailbox discarded the previous image: half of the rendered frames never reached the screen (three
  independent measurements). The SDK now presents from the thread that produces the image
  (`host_present_ignore_implicit_vsync`).
- **The Horizon WSI clamps the swapchain to three images.** The SDK asks for one more than its submission count and gets
  three. Acquiring the next image has to block when the compositor still holds all of them, so the next image is
  acquired in advance with a zero timeout after presenting.
- **The device has a single queue.** Presenting costs the recording thread time that cannot be moved to another thread
  without a second Vulkan queue. A presentation thread of its own (`present_own_thread`, off) lowered the mean cost but
  widened the spread of frame times, and the spread is what a player feels: steady beats fast. With
  `present_thread_no_discards` the producer waits for the previous frame instead of dropping it.
- For Mass Effect the native renderer uses three work slots and asynchronous output, which removed the idle gap between
  the CPU and the GPU (207 ms to 2.5 ms in the first console run; see [performance-history.md](performance-history.md)).

## Things that did not work, or were not tried

- Running the start-up on an emulator (Ryujinx) as a test bed: it found two start-up bugs, but NVK cannot create its device
  there. Rejected.
- A fourth CPU core by a custom loader descriptor: researched, never run.
- Overclocking: not accepted. The two overclocked diagnostic runs (t210, t211) are never quoted as results.
