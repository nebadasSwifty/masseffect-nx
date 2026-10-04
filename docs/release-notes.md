Native Nintendo Switch port of Mass Effect (Xbox 360), work in progress.

This update rebuilds the NRO with the project's Mesa/NVK patches and rejects older
Vulkan archives during future builds. Default settings use synchronous logging to
avoid async queue starvation and recheck streamed textures every four frames.
A stationary Eden Prime console comparison stayed around 26 fps; this is not a
claim that all flicker or movement-related frame drops are fixed. See
[the console verification notes](https://github.com/nebadasSwifty/masseffect-nx/blob/main/docs/renderer-console-check.md).

- `masseffect-nx.nro`: application for `/switch/masseffect-nx/`.
- `masseffect-nx-forwarder.nsp`: installable HOME menu launcher, 39-bit full application mode. Requires the NRO, game files and shaders on SD.
- `masseffect-nx-starter.zip`: extract into the SD card root; contains the NRO, default settings and NSP launcher.
- `SHA256SUMS`: checksums of the release downloads.

Create the game and shader package from your own supported Xbox 360 disc at [the browser installer](https://nebadasSwifty.github.io/masseffect-nx/), then extract its ZIP into `sdmc:/switch/`.

Supported edition: **Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)**. The port needs custom firmware. No Xbox game data or prebuilt game shaders are included. Performance and remaining issues are documented in the repository.
