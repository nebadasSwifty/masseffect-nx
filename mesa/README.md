# The graphics driver (Mesa NVK for the Switch)

The Switch has no Vulkan driver that homebrew can use, so the NRO carries its own: **NVK**, the open source Vulkan
driver for NVIDIA GPUs from the **Mesa** project, running on Horizon (the Switch OS). The result of the build in this
folder is one static library, `libvulkan.a` (NVK + the NAK shader compiler + its Rust runtime), which `tools/build_nro.sh`
links into the NRO.

## Exact base and patch

| | |
|---|---|
| Upstream | [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch), `main` |
| Pinned commit | `d4a00ea0ab3f59afb967cc5d779e4263d237bd77` ("Merge NaGaa95/mesa-switch main: Mesa 26.2.3, NVK and NAK fixes, shared shader cache") |
| Mesa version | 26.2.3 |
| Patch | `mesa-switch-masseffect.patch`, one `git apply`-able diff: 18 files, +1955 / -41, about 110 KB |

The patch is the sum of two things, applied on that commit:

1. the Mesa patch of the reference Switch port of this recompilation framework (written by [StevensND](https://github.com/StevensND)), ported onto
   the Mesa 26.2.3 base. Its ZCULL work, the SM50 `FADD32I.SAT` fix, the uniform-buffer-through-cbuf policy, uncached
   CPU-write memory and other fixes were already merged upstream, so the ported patch only carries what upstream still
   lacks (see the table);
2. our own changes for Mass Effect (submit tail reservation, fragment barrier and per-stage cache switches).

The text of the source comments was cleaned of project-specific names; no code was changed by that.

## What the patch changes

| Change | Why | Effect | Files |
|---|---|---|---|
| WSI recovery from unknown NWindow ownership (ours) | One failed Binder call of the swapchain (queue, cancel, request, release) "poisoned" the NWindow for the rest of the process: every new swapchain was refused, the old buffers stayed registered (`nwindowSetDimensions` then fails with 0xF59, `LibnxError_AlreadyInitialized`) and the screen froze on the last queued image while the game kept running (the intro-movie frame at start, about 1 launch in 5-10, 2026-10-08/09). | `vkCreateSwapchainKHR` on a poisoned window calls `nwindowReleaseBuffers` (cancel the dequeued slot, disconnect the producer); if libnx then reports no configured slot, the poison is lifted and the swapchain is created normally. The replaced chain keeps its memory (about 11 MB at 1280x720, once). Logs `wsi/switch: NWindow ownership was unknown; all buffers released`. | `src/vulkan/wsi/wsi_common_switch.c` |
| Submit tail reservation (ours) | The GPFIFO holds 0x800 entries. `exec_locked` reserved only the final skid, then `submit_locked` needed up to 3 more entries (fence, report, cross-channel wait). A batch filling the queue to the skid made the next submit fail with `no queue space`, and NVK treats that as a lost device. | No more "Graphics device lost" after minutes of play. The queue now keeps 3 entries free. | `src/nouveau/horizon/nouveau_horizon_channel.c` |
| Per-stage shader cache keys, `NVK_SWITCH_STAGE_CACHE=1` (ours) | The Vulkan runtime hashes the vertex and fragment shader of a linked pipeline into one cache key, so every new VS/FS pair recompiled both. | Setting `disable_lto` gives one key per stage. Cold pipeline compile went from 137 to 81 ms. Off unless the variable is set (the app sets it). | `nvk_device.c` |
| Fragment barrier, `NVK_SWITCH_FRAG_BARRIER=0/1/2` (ours) | A barrier between fragment work and later fragment work does not need a full pipeline wait (what deko3d does). 1 = colour attachment sources, 2 = colour and depth/stencil. | Experimental, off (0) by default. | `nvk_cmd_buffer.c` |
| NAK latencies | Texture and global-load results take hundreds of cycles on the shared-memory GM20B; the scheduler assumed 32. Now 200, so samples are separated from their consumers. | Scheduler priority only, scoreboard barriers unchanged. | `opt_instr_sched_common.rs` |
| NAK `peephole_select` limit 8 | NAK only flattened empty `if`s, and every other `if` became a real branch (SSY/BRA/SYNC) on Maxwell. | Small branches become straight code. Watch `NVK_SHADER_STATS=1` for local-memory spills. | `nak_nir.c` |
| Dead colour output removal | Writes to colour attachments that do not exist in the pipeline are discarded by Vulkan; removing them lets dead code elimination drop what only fed them. | Smaller, faster fragment shaders. | `nvk_shader.c` |
| Shader cache revision | NAK changed, and on Horizon the build id is the package version, so old compiled shaders would be reused. A bit of the compiler ABI revision (bit 24) is set. | Shaders compiled by a different NAK are not reused. Raise it with every NAK change. | `nvk_shader.c` |
| `NVK_SHADER_STATS`, `NVK_SUBTILING_KNOB` | Diagnostics and a GPU tuning knob that can be tried without rebuilding. | Off / the GPU's own value by default. | `nvk_shader.c` |
| ZCULL trace | One log line per size says whether a depth image really got a ZCULL plane. ZCULL itself (the GPU skips hidden pixels before shading them) is upstream. | Diagnostics only. | `nvk_image.c` |
| Faster draws | Per-draw measurement (1 of every N draws), batched emission of commands, "nothing dirty" shortcut for constant buffers, dynamic state by dirty groups, guard for the dynamic UBO delta. A structure shared with the app (`nvk_switch_draw`, version 1) controls and reads them. | Less CPU time in the driver per draw. `NVK_SWITCH_DRAW=false` turns it off. | `nvk_cmd_buffer.c/h`, `nvk_cmd_draw.c` |
| Set 4 by differences | Only the game constants that changed since the previous draw are sent. The mechanism is upstream's dynamic UBO delta; the app-visible contract `nvk_switch_set4` (version 1) and its self-check (compares with the full way on some draws and turns itself off on a mismatch) are kept. | Fewer constant uploads. `NVK_SWITCH_DYN_UBO_DELTA=false` turns it off. | `nvk_cmd_buffer.c/h`, `nvk_cmd_draw.c`, `nvk_device.c` |
| Pipeline prefetch | `PRFM` of the pipeline data read at bind time, requested all at once. | Fewer cache misses at `vkCmdBindPipeline`. | `src/vulkan/runtime/vk_pipeline.c/h`, `nvk_cmd_buffer.c` |
| Copy engine switch, `NVK_COPY_ENGINE=1` | Buffer and image copies on the GPU's copy unit instead of meta shaders. | Measured slower for us, so off; kept to measure again. | `nvk_cmd_meta.c` |
| Build fixes | `bindgen` is run with `--no-layout-tests` and `__sFILE` opaque (devkitA64's `FILE` fails bindgen's size check; used in the Docker build too). MinGW/MSYS2 fixes: LLVM `demangle` module, `-Dshared-llvm`, rustc wrapper linker fallback. | The build works in Docker and on Windows. | `bindgen-switch-wrapper.sh`, `rustc-switch-wrapper.sh`, `rustc-native-msys2-wrapper.sh`, `build-unified.sh`, `meson.build` |

Almost everything is inside `#ifdef HAVE_SWITCH_PLATFORM` (Mesa's build file defines it for the `switch` platform, in the
same way it defines `HAVE_X11_PLATFORM` on Linux), so building this source for another platform leaves it out.
The NAK compiler changes and the build changes apply everywhere.

## Environment variables

Written, for the app, in its Mesa environment setting. All are optional.

| Variable | Default | Effect |
|---|---|---|
| `NVK_SWITCH_STAGE_CACHE` | off | `1`: per-stage shader cache keys. |
| `NVK_SWITCH_FRAG_BARRIER` | `0` | `1` or `2`: fragment-to-fragment barrier softening. |
| `NVK_SWITCH_DRAW` | on | `false`: turn the faster-draws path off. |
| `NVK_SWITCH_DYN_UBO_DELTA` | on | `false`: turn set 4 by differences off. |
| `NVK_SWITCH_NO_UBO_CBUF` | off | `1`: upstream way of reading uniform buffers. |
| `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED` | on | `0`: put the CPU cache back on write-only memory. |
| `NVK_COPY_ENGINE` | off | `1`: copy engine for copies. |
| `NVK_SHADER_STATS` | off | `1`: log registers and local memory per shader. |
| `NVK_SUBTILING_KNOB` | GPU value | A number, for experiments. |

## Apply and build (macOS or Linux, Docker)

This is what we did, on macOS arm64 (Apple silicon) with the official `devkitpro/devkita64` image running natively.

```sh
mesa/build_mesa_docker.sh             # clone, pin, patch, build, merge; result in ../mesa-sdk (next to the repo)
OUT=/path/to/mesa-sdk mesa/build_mesa_docker.sh
```

Needs `git` and `docker`; the first build takes an hour or more and about 15 GB. What the script does:

1. `git clone https://github.com/danfromtico/mesa-switch.git`, `git checkout d4a00ea0ab3f59afb967cc5d779e4263d237bd77`,
   `git apply mesa/mesa-switch-masseffect.patch`.
2. Runs upstream `./build-switch.sh`, which:
   - builds the image `devkitpro-mesa-rust` from `Docker.rust` (`devkitpro/devkita64:latest` + SPIRV-Tools 1.3.290 +
     Rust nightly with `rust-src` + `bindgen-cli` and `cbindgen`);
   - builds the native host tools `mesa_clc` and `vtn_bindgen2` (`meson setup builddir-native`);
   - configures the cross build (`meson setup builddir-switch --cross-file switch_cross_file.txt --buildtype=release
     -Doptimization=2 -Db_lto=false -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau -Dplatforms=switch -Dllvm=disabled ...`);
   - builds with ninja the archives `libnvk.a`, `libvulkan.a`, `libnak.a`, `libnak_rs.a` and their dependencies.
3. Merges the Rust runtime of the shader compiler into the library (`libvulkan.a` alone does not contain it):
   `CREATE merged.a / ADDLIB libvulkan.a / ADDLIB libnak_rs.a / SAVE / END` through `aarch64-none-elf-ar -M`, and puts the
   result at `<OUT>/opt/devkitpro/portlibs/switch/lib/libvulkan.a` (about 122 MB).

Then build the NRO with `MESA_SDK=<OUT> tools/build_nro.sh` (it passes
`-DREXGLUE_SWITCH_NVK_SDK=<OUT>/opt/devkitpro/portlibs/switch`). The NRO link depends on that archive (`LINK_DEPENDS`
in `sdk/cmake/rexglue_switch.cmake`), so a rebuilt driver is relinked; delete NRO build directories configured before
2026-10-07 once.

### Optimization level (-O2 by default, -O1 fallback)

Upstream `build-switch.sh` configures `-Doptimization=1`. `build_mesa_docker.sh` rewrites that line to `MESA_OPT`
(default `2`; `1`, `2`, `3` or `O1`, `O2`, `O3`) before building, and records the level in `<OUT>/SOURCE.txt`. -O2
was measured on the Switch on 2026-10-07: +5 % draw throughput in heavy scenes (15.7k -> 16.6k draws/s, 25.8 -> 26.4 fps,
frames over 60 ms 160 -> 108 per route), same image.

```sh
mesa/build_mesa_docker.sh                         # -O2, the default driver
OPT=1 mesa/build_mesa_opt.sh                      # -O1 fallback from an existing tree: out/mesa-sdk-o1, ~1 minute
MESA_OPT=O1 tools/build_nro.sh                    # link the -O1 fallback (out/mesa-sdk-o1 or ../mesa-sdk-o1)
OPT=3 mesa/build_mesa_opt.sh                      # experiments: out/mesa-sdk-o3, link with MESA_SDK=out/mesa-sdk-o3
```

`build_mesa_opt.sh` configures its own build directory (`builddir-switch-o<N>`) in an already built tree (it reuses
`builddir-native` and the Docker image). `--incremental` picks the build directory of the requested level and refuses
to merge an archive of another level.

After editing the driver source: `SRC=<tree> mesa/build_mesa_docker.sh --incremental` (needs the Docker image and a
previous full build in the same tree). If you change NAK, raise the revision number in `nvk_shader.c`.

Reproducibility notes: `Docker.rust` takes the latest Rust nightly and `devkita64:latest`. Our reference build used
`rustc 1.101.0-nightly (75a75c3e0 2026-09-26)` (LLVM 23.1.1). If a newer nightly fails to build NAK, install that one
(`rustup toolchain install nightly-2026-09-26`, then `rustup default` it in the image).

## Windows (MSYS2)

`build_mesa_msys2.sh` rebuilds incrementally after a first full build with mesa-switch's own `build-unified.sh`. It
has not been tested with this patch; the build-file changes of the patch (see "Build fixes") are the ones that earlier
Windows builds needed.

## Credits

- **[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)**: the Mesa port to Horizon (platform backend,
  NVK on the Switch GPU, build scripts), the base of everything here.
- **NaGaa95**: the Mesa 26.2.3 update, NVK/NAK fixes and the shared shader cache merged into mesa-switch main.
- **[StevensND](https://github.com/StevensND)**: the original Mesa patch for the reference port (ZCULL, Horizon channel and memory changes, NAK
  scheduling, the faster-draws and set-4-by-differences work), which is the starting point of this patch. (Note: StevensND is not affiliated with this Mass Effect project).
- **The Mesa project** (NVK, NAK, NIL and the Vulkan runtime, MIT licensed). All file headers and licences in the
  patched files are unchanged.
