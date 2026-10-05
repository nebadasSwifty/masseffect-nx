Native Nintendo Switch port of Mass Effect (Xbox 360), work in progress.

### What's Changed in v0.1.2
- **Runtime UI Shaders**: Bundled 288 supplemental runtime containers for Scaleform UI, menus, and HUD in the web installer package generator, fixing missing UI draw calls and black screen issues caused by dynamic containers not found in static package scans.
- **Documentation & Attribution**: Completely rewrote project documentation and web installer to accurately represent Mass Effect NX, properly crediting StevensND and the NFSMW-NX project with non-affiliation disclaimers (#1).

- `masseffect-nx.nro`: application for `/switch/masseffect-nx/`.
- `masseffect-nx-forwarder.nsp`: installable HOME menu launcher, 39-bit full application mode. Requires the NRO, game files and shaders on SD.
- `masseffect-nx-starter.zip`: extract into the SD card root; contains the NRO, default settings and NSP launcher.
- `SHA256SUMS`: checksums of the release downloads.

Create the game and shader package from your own supported Xbox 360 disc at [the browser installer](https://nebadasSwifty.github.io/masseffect-nx/), then extract its ZIP into `sdmc:/switch/`.

Supported edition: **Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)**. The port needs custom firmware. No Xbox game data or prebuilt game shaders are included. Performance and remaining issues are documented in the repository.
