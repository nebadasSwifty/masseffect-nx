# Things that broke the picture before (do not repeat)

Each rule below cost hours of console bisection. Check this list before changing the renderer, the internal resolution,
the edition overlays or the test tooling.

## Rendering

1. **Never shrink the game's front buffer.** With an internal resolution (`masseffect_scene_width/height`), the front buffer
   (the texture the back buffer is resolved into at Present, `masseffect_scene_parts` bit 16) must stay 1280x720.
   A 960x544 front buffer made Shepard a black silhouette in the character-creation cutscene ("Every soldier has scars")
   and the title planet black. `masseffect_scene_front_full = true` (default) keeps it full size and the output pass shows
   only the WxH corner (`MeResolutionOutputSize`), texel for texel (`PaintOutput(..., cropped)`): scaling the whole
   1280x720 buffer into the output shows the picture shrunk into the top-left corner. Do not set it to false, do not
   add bit 16 back.
2a. **Check that the internal resolution is really active.** The log must show the hook lines ("internal resolution:
   device", "back buffer", "viewport", "scene render targets"), not only "internal resolution WxH (parts ...)". Without
   them the game renders at 1280x720 (about 1.5x the GPU time) and every comparison is wrong.
2. **Every resolution part is all-or-nothing for the picture.** Leaving out the viewport (bit 1) or the back buffer (bit 8)
   gives a zoomed or corner-only image. Bisect with whole configurations, not single parts, unless you also crop the output.
3. **Ruled out as causes of black characters** (each tested alone on the console, 2026-10-07): ZCULL, all EDRAM mode 4 stencil
   shortcuts, the shadow map PCF/bias changes, the scene render-target allocation hook, `masseffect_native_depth_samples_x`
   (it is already false in the shipped config). Do not re-test them for that symptom.

## Edition builds (English / Russian)

4. **Overlay files replace English files.** Any change to an English file that has a copy in `editions/<id>/overlay/` must be
   made in the copy too (it missed a hook call and a watchdog start). `tools/edition.sh` prints `tools/edition_drift.py`;
   the only accepted drift is guest addresses, the disabled hot functions of `me_hot_guest.cpp` and the shader dump cvar.
4a. **Staging must copy overlay files by content and with a fresh mtime** (`rsync -rlpD --checksum` in `tools/edition.sh`,
   then a `cmp` check). An overlay file has the same size as the English one, rsync's quick check skipped it, and with
   `-a` its old mtime made ninja keep the English object: the Russian build hooked English addresses.
5. **Never build an edition NRO from stale generated code.** The direct-call patch bakes in the set of hooked functions; a new
   hook added after codegen is silently bypassed (black characters in an early Russian build). `tools/edition.sh` regenerates
   when the set changes and refuses a `build`-only step; do not bypass it by copying `generated/` around.

## Testing on the console

6. **Verify the uploaded masseffect.toml.** A failed upload leaves no toml and the game runs on defaults (full 1280x720, other
   behaviour), which looks like a fix. The console routes in `mass-effect-recomp/tools/me1_anderson.sh` and `me1_anderson_timed.sh` compare the bytes.
7. **Some launches stay black after the logo movies** (about 1 in 3 at times; renderer presents Swaps with 0 draws). Treat a
   black start as a launch failure, not a result: the routes relaunch up to 3 times.
8. **Do not press A while waiting for a screen**: it can pick the default profile and skip character creation. Wait for the
   screen, then press once (`switch_bot.py advance`).
9. **HOME ignores input while it appears**: press A until the screen leaves HOME.
10. **The hang watchdog runs on the ring thread** (`masseffect_hang_watchdog = true`, optional
    `masseffect_hang_peek`). Its first version created its own std::thread and the game did not start on the Switch:
    do not add threads to it. Useful peek for the D3D device owner thread: `*82EA0F24+2A08` (Russian),
    `*82EA0F04+2A08` (English).
11. **The black start was a render command buffer deadlock** (fixed by `masseffect_render_ring_kb`, default 4096): while
    the loading movie thread owns the Direct3D device, the rendering thread does not read commands; with the game's
    256 KB buffer the game thread could fill it before the movie stopped and spin forever. Do not set it back to 256.
    Checked: 12 launches out of 12 reached the title (before: black in 1 of 6, once 3 in a row).
