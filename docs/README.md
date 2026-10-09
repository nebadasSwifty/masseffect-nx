# Documentation

## Start here

These documents explain how the port works, how to build it, and how to use it as a guide to port another Xbox 360 game.
They are written for someone who does not know the project yet. If a word is new to you, look it up in the
[glossary](glossary.md). The port is a work in progress (about 25 fps at stock clocks instead of 30): the honest state is
in [known-issues.md](known-issues.md).

## How the whole port fits together

This is the path the game follows, from the Xbox 360 disc to the Switch screen:

1. **The game's code is translated.** The game's program, `default.xex`, is written for the Xbox 360's processor. It is
   translated to C++ with [ReXGlue](glossary.md#rexglue-the-sdk) and compiled into an NRO for the Switch. See
   [toolchain.md](toolchain.md).
2. **The game gets a fake Xbox 360 around it.** When the game runs, ReXGlue gives it what it would find on an Xbox 360:
   files, memory, threads, audio and controllers. The Switch part of that is in `sdk/`. See
   [platform-notes.md](platform-notes.md).
3. **The game draws as usual, and the port draws the same thing on the Switch.** The game writes its list of GPU commands,
   as it always did. The port's renderer reads that list and draws the same frame with Vulkan, imitating the Xbox 360's
   10 MB of EDRAM. See [native-renderer.md](native-renderer.md).
4. **The shaders are translated ahead of time.** The game's small GPU programs are found in its data and converted before
   playing. The installer page is meant to do it from the user's own disc. See [shaders.md](shaders.md).
5. **Vulkan runs on a driver inside the NRO.** The driver is NVK, from the Mesa project, with this port's changes. See
   [mesa.md](mesa.md).
6. **Audio is decoded on the Switch, and the movies are played by the game.** See
   [audio-and-video.md](audio-and-video.md).

## What to read, depending on what you want

| You want to... | Read |
|---|---|
| Build the port yourself | [building.md](building.md) |
| Port another Xbox 360 game | [porting-another-game.md](porting-another-game.md), then [native-renderer.md](native-renderer.md), [shaders.md](shaders.md) and [platform-notes.md](platform-notes.md) |
| Make the port faster | [measuring.md](measuring.md) first, then [performance-history.md](performance-history.md), [optimization-paths.md](optimization-paths.md), [toolchain.md](toolchain.md) and [mesa.md](mesa.md) |
| Shorten the start-up time | [startup.md](startup.md) (timeline, ranked options, the read trace and preload) |
| Understand the SD reads during play | [streaming-io.md](streaming-io.md) (who reads, what is reread, the block cache) |
| Know what is broken or unproven | [known-issues.md](known-issues.md) |
| Support another edition of the game | [editions.md](editions.md) |
| Install the game as one NSP (program + your data in RomFS) | [full-nsp.md](full-nsp.md) (experimental: packer, packaged mode, keys, sizes, browser plan) |
| Understand a word | [glossary.md](glossary.md) |

## All documents

| Document | What it explains |
|---|---|
| [glossary.md](glossary.md) | Every technical word, in plain words |
| [building.md](building.md) | How to build the code generator, the NRO, the driver and the shader library, step by step |
| [porting-another-game.md](porting-another-game.md) | What you can reuse for another game, and in which order to work |
| [native-renderer.md](native-renderer.md) | How the port draws the game with Vulkan |
| [shaders.md](shaders.md) | How the game's shaders are translated, and what had to be fixed |
| [toolchain.md](toolchain.md) | How the game's code is translated and compiled, and the build options |
| [mesa.md](mesa.md) | The graphics driver and this port's changes to it |
| [platform-notes.md](platform-notes.md) | Things about the Switch system that cost a lot of time to find out |
| [audio-and-video.md](audio-and-video.md) | The game's audio and movies on the Switch |
| [audio-cpu.md](audio-cpu.md) | What the audio costs on the CPU, the native audio code and how it is verified |
| [hot-guard.md](hot-guard.md) | The hot-hook self-check guard: private-copy checks (no guest writes), per-hook handling |
| [editions.md](editions.md) | How the supported edition is identified, and how to add another |
| [measuring.md](measuring.md) | How to measure performance on the console without being misled |
| [performance-history.md](performance-history.md) | How the frame rate went from a few fps to about 25, step by step |
| [optimization-paths.md](optimization-paths.md) | Every optimization idea tried or considered, by area, with its result and setting |
| [known-issues.md](known-issues.md) | What is not working, not finished or not proven, with the evidence |
| [streaming-io.md](streaming-io.md) | The game's package reads during play, why they are reread, and the RAM block cache for them |
| [kernel-waits.md](kernel-waits.md) | Cost of the guest's kernel waits and releases in the SDK, the render-thread priority choice, the timer-spin probe |

Outside this folder: [../README.md](../README.md) (how to install and use the port), [../shaders/README.md](../shaders/README.md)
(the shader pipeline and file formats), [../mesa/README.md](../mesa/README.md) (the driver patch table),
[../tools/README.md](../tools/README.md) (the build and analysis scripts),
[../tools/console-test/README.md](../tools/console-test/README.md) (deploying to a console and measuring),
[../tests/README.md](../tests/README.md) and [../installer/README.md](../installer/README.md).
