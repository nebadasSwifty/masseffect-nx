Native Nintendo Switch port of Mass Effect (Xbox 360), work in progress.

### What's Changed in v0.2.0

**Performance**
- The scene renders at 960x544 and is upscaled to 720p with AMD FSR 1 (sharper than plain scaling at the same cost).
  Most locations now hold 30 fps; the heaviest scenes (Eden Prime after the opening cutscene, large fights) run at 20-26 fps.
- Large CPU and GPU savings in the renderer and the game code: native audio mixing and resampling (NEON), shader constants
  updated per changed vector, vertex data reused across frames, texture sign handling folded into shaders, much cheaper
  EDRAM emulation, fewer per-draw allocations, lighter kernel waits.
- Per-edition shader prewarm list: the game's pipelines are compiled in the background (from 25 s after launch), so the
  0.5-1 s stalls on the first visit of a place are gone.

**Stability**
- Fixed hangs and crashes at start: "Disc Read Error", a frozen intro / BioWare logo, a black screen. Cause: the first
  launch (no shader cache yet) compiled thousands of shaders while the game was loading, and the console ran out of
  system resources. The game now also leaves 64 MB more memory to the system.
- The screen no longer freezes on the intro when the console briefly refuses a frame.
- Long sessions no longer crash on memory fragmentation (shader cache saving, texture uploads); the shader memory use
  is about 200 MB lower.

**Image**
- Fixed missing letters in terminals, dialogues and the autosave label; characters "vanishing" on fast camera turns;
  the black start screen; Shepard rendered black.

**Controls and audio**
- Nintendo Switch button names in the game texts and the Switch layout in the decryption mini-game.
- Rumble on both Joy-Cons, scaled to the game's motor speeds.
- Experimental touch screen (handheld): drag to turn the camera, tap a dialogue option to pick it.
- Fixed a constant buzz that could replace music (XMA loops).

**Installation**
- Installable full NSP per edition (Russian and English install side by side, separate saves): the browser installer
  can build it from your own discs.
- Russian two-disc merge verified on real discs. The unused, broken `GlobalTlk_ES.xxx` of the Russian discs is now a
  short note instead of a "re-dump your discs" warning.

**Known issues**
- Pressing HOME during the first ~30 seconds after launch, while the game is loading, can freeze the console. Wait until
  the main menu.
- Rarely (about 1 launch in 10) the game can stop at the intro. Close it and start it again.
- Sysmodules and overlays use the same system memory the game's display and file access need; with many of them
  installed, the risk of the issues above grows.

### What's Changed in v0.1.3
- **Russian Edition Support**: Added full support for the Russian release (1C) with complete Russian voiceover and translated text.
- **Multi-Disc ISO Support**: The web installer now supports multi-disc ISO dumps (Disc 1 + Disc 2) and automatically combines packages.
- **Bilingual Interface**: Added an English / Russian language toggle to the web installer.

- `masseffect-nx.nro`: application for `/switch/masseffect-nx/` (English edition).
- `masseffect-nx-rus.nro`: the same for the Russian edition (1C); rename it to `masseffect-nx.nro` in its folder, or let the installer do it.
- `masseffect_prewarm_list-en.bin`, `masseffect_prewarm_list-ru.bin`: per-edition shader prewarm lists (the installer adds the right one as `masseffect_prewarm_list.bin`).
- `masseffect-nx-forwarder.nsp`: installable HOME menu launcher, 39-bit full application mode. Requires the NRO, game files and shaders on SD.
- `masseffect-nx-starter.zip`: extract into the SD card root; contains the NRO, default settings and NSP launcher.
- `SHA256SUMS`: checksums of the release downloads.

Create the game and shader package from your own supported Xbox 360 disc at [the browser installer](https://nebadasSwifty.github.io/masseffect-nx/), then extract its ZIP into `sdmc:/switch/`.

Supported editions: **Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)** and **Mass Effect (Russian Edition - 1C)**. The port needs custom firmware. No Xbox game data or prebuilt game shaders are included. Performance and remaining issues are documented in the repository.
