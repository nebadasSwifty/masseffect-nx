# Supplemental Runtime Shader Containers

This folder contains synthetic 2008 Xenos shader containers for Direct3D immediate mode and Scaleform UI shaders.

## Why are these containers here?
Unlike standard Unreal Engine 3 world, material, and character shaders, which are pre-compiled into the disc's Unreal packages (`*.xxx`), Direct3D immediate-mode UI shaders and Scaleform runtime shaders are generated dynamically at runtime by `default.xex` / the game engine.

Because they do not exist inside any `*.xxx` package on the game disc:
1. An offline disc scanner cannot find them by inspecting packages alone.
2. Without these shaders in `masseffect_shaders.mesp`, the game's UI, HUD, "Press START" screen, and main menus cannot render, dropping draw calls with `unidentified pixel shader` (e.g. `PS=706F57115BB33790`, `PS=7CA16A6F1E83B4ED`, `PS=259080E95E4E6EE0`, etc.) and resulting in a black screen with background audio.

These containers wrap the exact microcode emitted by the runtime into 2008 Xenos container format (`0x102A1100`) so that they can be translated through the standard `masseffect_hlsl` -> `dxc` -> `pack` toolchain.

2026-10-07: 8 table-less containers that the v24-v27 packages had but this folder lacked were restored (among them
`ps_c5840b564b1d37b0.bin`, the Scaleform textured fill `tex * c2 + c3` that draws the combat heat meter). See
`docs/image-defects-feros.md`, section "Overheat bar missing".
