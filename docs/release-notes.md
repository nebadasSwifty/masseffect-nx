Native Nintendo Switch port of Mass Effect (Xbox 360), work in progress.

### What's Changed in v0.1.3
- **Russian Edition Support**: Added full support for the Russian release (1C) with complete Russian voiceover and translated text.
- **Multi-Disc ISO Support**: The web installer now supports multi-disc ISO dumps (Disc 1 + Disc 2) and automatically combines packages.
- **Bilingual Interface**: Added an English / Russian language toggle to the web installer.

- `masseffect-nx.nro`: application for `/switch/masseffect-nx/`.
- `masseffect-nx-forwarder.nsp`: installable HOME menu launcher, 39-bit full application mode. Requires the NRO, game files and shaders on SD.
- `masseffect-nx-starter.zip`: extract into the SD card root; contains the NRO, default settings and NSP launcher.
- `SHA256SUMS`: checksums of the release downloads.

Create the game and shader package from your own supported Xbox 360 disc at [the browser installer](https://nebadasSwifty.github.io/masseffect-nx/), then extract its ZIP into `sdmc:/switch/`.

Supported editions: **Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)** and **Mass Effect (Russian Edition - 1C)**. The port needs custom firmware. No Xbox game data or prebuilt game shaders are included. Performance and remaining issues are documented in the repository.
