# The graphics driver: Mesa, NVK and NAK on the Switch

## In short

The Switch has no Vulkan driver that homebrew can use, so the NRO carries its own: **NVK**, the open source Vulkan driver
for NVIDIA GPUs from the [Mesa](glossary.md#mesa-nvk-nak) project, running on Horizon. **NAK** is its shader compiler,
written in Rust. The whole driver is built into one static library, `libvulkan.a`, which `tools/build_nro.sh` links into
the NRO, so the game needs nothing installed on the console.

The base is [mesa-switch](https://github.com/danfromtico/mesa-switch) (the port of Mesa to Horizon by danfromtico, with
NaGaa95's updates), pinned at one commit (Mesa 26.2.3). This port adds one patch, `mesa/mesa-switch-masseffect.patch`
(17 files, +1861 / -31). The patch is the sum of two things: a Mesa patch by StevenSND (the work this port's driver
changes start from), ported onto the newer base (its ZCULL work, a Maxwell `FADD32I.SAT` fix, reading uniform buffers
through the constant bank and uncached CPU-write memory were already merged upstream, so the ported patch only carries
what upstream still lacks), and our own changes for Mass Effect. The full table of changes, the environment variables and
the exact pins are in [../mesa/README.md](../mesa/README.md); this page explains what they are for.

Almost everything is inside `#ifdef HAVE_SWITCH_PLATFORM`, which Mesa's build file defines for the `switch` platform, so
building the source for another platform leaves it out. The NAK compiler changes and the build changes apply everywhere.

## Building the driver

The build has been done on macOS arm64 (Apple silicon) in Docker, with the official `devkitpro/devkita64` image. The steps
are in [building.md](building.md), step 5. In short, `mesa/build_mesa_docker.sh`:

1. clones mesa-switch, checks out the pinned commit (`d4a00ea0ab3f59afb967cc5d779e4263d237bd77`) and applies the patch;
2. runs the upstream `build-switch.sh`: builds the image `devkitpro-mesa-rust` (devkitA64, SPIRV-Tools, Rust nightly with
   `rust-src`, `bindgen-cli`, `cbindgen`), the host tools `mesa_clc` and `vtn_bindgen2`, and then the cross build for the
   Switch (`-Dvulkan-drivers=nouveau -Dplatforms=switch -Dllvm=disabled`, release, `-Db_lto=false`);
3. merges the Rust runtime of the shader compiler (`libnak_rs.a`) into `libvulkan.a`, which alone does not contain it, and
   puts the result in `<OUT>/opt/devkitpro/portlibs/switch/lib/libvulkan.a` (about 122 MB).

It takes an hour or more and about 15 GB the first time. Ninja does not track the driver archive: after a driver change,
**delete the NRO build folder**, or the old driver stays linked. If you change NAK, raise the shader cache revision in
`nvk_shader.c`, because on Horizon the cache's build id is the package version and shaders compiled by an older NAK would
be reused. The Docker image takes the latest Rust nightly; the reference build used `rustc 1.101.0-nightly (2026-09-26)`,
and if a newer nightly breaks the NAK build, install that one. `build_mesa_msys2.sh` (Windows) was not tested with this
patch.

## ZCULL: skipping hidden pixels

ZCULL is hardware that lets the GPU throw away hidden pixels before it shades them (see
[glossary](glossary.md#zcull)). It is part of upstream NVK now. Two things matter for this port:

- It is easy to lose. A shader that writes the depth by hand disables both early depth testing and ZCULL on this GPU. The
  setting that made every depth-writing shader do so (`masseffect_native_float24_ps_mode`) made the scene run at under 1 fps
  and must stay 0 (see [performance-history.md](performance-history.md)).
- The patch adds a trace: one log line per depth size says whether the image really got a ZCULL plane. Stencil moved through
  the copy engine costs the 1x depth view its ZCULL plane (see [native-renderer.md](native-renderer.md)). Making ZCULL
  work for the scene depth was measured: no gain (the scene is vertex bound).

## NAK: the shader compiler

- **Latencies.** Texture and global-load results take hundreds of cycles on the GM20B, and the scheduler assumed 32. Now it
  assumes 200, so samples are separated from their consumers. This changes scheduler priority only; the scoreboard barriers
  are unchanged.
- **Small branches become straight code.** NAK only flattened empty `if`s and every other `if` became a real branch
  (`SSY`/`BRA`/`SYNC`) on Maxwell. The select-peephole limit is now 8. Watch `NVK_SHADER_STATS=1` for local-memory spills.
- **Dead colour outputs.** Vulkan discards writes to colour attachments that do not exist in the pipeline. Removing them in
  the compiler lets dead-code elimination drop what only fed them: smaller, faster fragment shaders.
- **Two optional diagnostics.** `NVK_SHADER_STATS` logs registers and local memory per shader, and `NVK_SUBTILING_KNOB`
  is a GPU tuning knob to try without rebuilding.

## Memory and synchronization

- **Submit tail reservation (ours).** The GPFIFO holds 0x800 entries. The queue code reserved only the final skid, but a
  submit then needed up to three more entries (fence, report, cross-channel wait). A batch that filled the queue to the
  skid made the next submit fail with `no queue space`, which NVK treats as a lost device ("Graphics device lost" after
  minutes of play). The queue now keeps three entries free.
- **Fragment barrier, `NVK_SWITCH_FRAG_BARRIER` (ours).** A barrier between fragment work and later fragment work does not
  need a full pipeline wait. Level 1 covers colour attachment sources, 2 adds depth and stencil. It gave no measurable
  gain on the console, so it is off (0) by default.
- **Copy engine, `NVK_COPY_ENGINE=1`.** Buffer and image copies on the GPU's copy unit instead of meta shaders. Measured
  slower, so it is off and kept to measure again.
- **Uncached CPU-write memory** (upstream, on by default; `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED=0` puts the CPU cache back).

## The draw path: less CPU per draw

The CPU is the limit of this port, and a game frame has thousands of draws, so the driver's per-draw cost counts.

- **Faster draws.** Per-draw measurement (one of every N draws), batched emission of commands, a "nothing dirty"
  shortcut for constant buffers, dynamic state by dirty groups. A structure shared with the app (`nvk_switch_draw`,
  version 1) controls and reads them. `NVK_SWITCH_DRAW=false` turns it off.
- **Set 4 by differences.** Only the game constants that changed since the previous draw are sent. The mechanism is
  upstream's dynamic uniform buffer delta; the contract with the app (`nvk_switch_set4`, version 1) and a self-check that
  compares with the full way on some draws and turns itself off on a mismatch are kept. `NVK_SWITCH_DYN_UBO_DELTA=false`
  turns it off.
- **Pipeline prefetch.** `PRFM` of the pipeline data read at bind time, requested all at once.
- **Per-stage shader cache keys, `NVK_SWITCH_STAGE_CACHE=1` (ours).** The Vulkan runtime hashes the vertex and fragment
  shader of a linked pipeline into one key, so every new vertex/fragment pair recompiled both. With one key per stage a
  cold pipeline compile went from 137 to 81 ms. The app sets the variable when `masseffect_nvk_cache_per_stage` is on (it
  is in the shipped configuration), and the setting needs this patched driver.

The measured effect of each of these on the frame is in [optimization-paths.md](optimization-paths.md), section 6; several
were measured as "no visible change" at the time.

## Environment variables

All optional; the app writes them for the driver before the Vulkan device is created.

| Variable | Default | Effect |
|---|---|---|
| `NVK_SWITCH_STAGE_CACHE` | off | `1`: per-stage shader cache keys |
| `NVK_SWITCH_FRAG_BARRIER` | `0` | `1` or `2`: fragment-to-fragment barrier softening |
| `NVK_SWITCH_DRAW` | on | `false`: turn the faster-draws path off |
| `NVK_SWITCH_DYN_UBO_DELTA` | on | `false`: turn set 4 by differences off |
| `NVK_SWITCH_NO_UBO_CBUF` | off | `1`: upstream's way of reading uniform buffers |
| `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED` | on | `0`: put the CPU cache back on write-only memory |
| `NVK_COPY_ENGINE` | off | `1`: copy engine for copies |
| `NVK_SHADER_STATS` | off | `1`: log registers and local memory per shader |
| `NVK_SUBTILING_KNOB` | GPU value | A number, for experiments |

`MESA_SHADER_CACHE_DISABLE` is set by the app when `masseffect_cold_startup` is true, to test a cold start.

## What was not done

- Shader stencil export: Maxwell has none, so stencil goes through the copy engine (see
  [native-renderer.md](native-renderer.md)).
- Exact 2x multisampled images were not integrated: earlier experience says multisampling can hang this driver on the
  console ([optimization-paths.md](optimization-paths.md)).
- Push descriptors and fewer descriptor-set binds (pre-Turing GPUs fetch descriptor sets unprefetched at each non-push
  bind) are open ideas.

## Credits

- [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch): the Mesa port to Horizon (platform backend, NVK on
  the Switch GPU, build scripts), the base of everything here.
- NaGaa95: the Mesa 26.2.3 update, NVK/NAK fixes and the shared shader cache merged into mesa-switch main.
- StevenSND: the original Mesa patch this one starts from (ZCULL, Horizon channel and memory changes, NAK scheduling, the
  faster-draws and set-4-by-differences work).
- The Mesa project (NVK, NAK, NIL and the Vulkan runtime, MIT licensed). All file headers and licenses in the patched files
  are unchanged.
