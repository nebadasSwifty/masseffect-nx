# Building the port yourself

## In short

Players do not need this page: the [installer page](https://nebadasswifty.github.io/masseffect-nx/) makes the package
from their own copy of the game, with the released NRO. This page is for building the NRO (the Switch program), the
graphics driver and the shader library yourself, from the source code in this repository.

The steps, in order: get the missing libraries, extract the game, build the code generator, translate the game to
C++, build the graphics driver, build the NRO, build the shader library, and copy everything to the SD card. Each step
says what to do and why. If a word is new to you, it is in the [glossary](glossary.md).

Everything here was done on macOS (Apple silicon). The scripts are bash and Python and Linux should work too, but
nobody has tried it. Windows is not covered: the driver has an MSYS2 script that was never tested with this port's patch
(see [mesa.md](mesa.md)). All commands are run from the repository root.

## What you need

- **Docker.** The NRO and the graphics driver are built inside Docker images (`devkitpro/devkita64`), so no local
  devkitPro install is needed.
- **CMake 3.25 or newer, Ninja, Git, Python 3**, and a C++ compiler for your computer (`clang` and `clang++` by default;
  the SDK is written for clang). They build the code generator, which has to run on your computer.
- Your own copy of Mass Effect for Xbox 360: an `.iso` (plain, XGD1, XGD2 or XGD3 layout) or the extracted disc. Nothing
  from the game is in this repository, and nothing made from it may be added to it.
- **Memory and disk.** About 15 GB of disk for the first driver build. Every compile job of the NRO build needs about
  1-2 GB of Docker memory, and `JOBS` is 6 by default. The translated game code is huge (millions of lines of C++), so
  give Docker as much memory as you can; the optional LTO build needs about 16 GB (see [toolchain.md](toolchain.md)).

Only for the shader library on your computer (instead of the installer page): `xxhash`, `fmt` (headers only),
SPIRV-Tools and a DXC binary, for example `brew install xxhash fmt spirv-tools` and DXC from the LunarG Vulkan SDK.

## 1. Get the missing libraries

The repository keeps only the third-party files this port changed. The rest comes from the ReXGlue SDK release the SDK
fork is based on (v0.10.0) and its submodules:

```sh
python3 tools/fetch_thirdparty.py                  # clones the release and its submodules (needs git and network)
python3 tools/fetch_thirdparty.py --source DIR     # or copy from a local rexglue-sdk checkout, no network
```

Files already in `sdk/thirdparty` are kept. `tools/build_host.sh` runs the fetch for you if the libraries are missing.

## 2. Extract the game

```sh
python3 tools/extract_iso.py "Mass Effect.iso"            # extracts into assets/game_root
python3 tools/extract_iso.py "Mass Effect.iso" --list     # only lists the disc
```

`extract_iso.py` is a pure Python XDVDFS reader (no other tool needed). It finds the game partition, extracts every
file into `assets/game_root` (or the folder given with `-o`), and prints the information of `default.xex`: title id,
version, image base address and entry point. Compressed formats (CCI, GOD, ZAR) must be converted to a plain `.iso`
first. If you already have the extracted disc, put it in `assets/game_root` yourself (`default.xex` at its root) and, to
check the file, run `python3 tools/extract_iso.py assets/game_root/default.xex --info`.

Check that your `default.xex` is the supported edition: its SHA-256 must be the one listed in
[editions.md](editions.md) (`shasum -a 256 assets/game_root/default.xex`). The recompiled code is only valid for that
exact file.

## 3. Build the code generator

```sh
tools/build_host.sh                 # fetches the libraries if missing, configures, builds
tools/build_host.sh --configure     # only configure: a quick check of the toolchain
```

This builds `rexglue`, the ReXGlue code generator, for your computer from `sdk/`. It cannot be built for the Switch:
the translation has to run on your machine. The result is `sdk/out/host/rexglue`. `CC`, `CXX`, `JOBS` and `BUILD_DIR`
are described at the top of the script.

## 4. Translate the game to C++

```sh
tools/codegen.sh
```

This runs `rexglue codegen` on `app/masseffect_manifest.toml`: it translates `assets/game_root/default.xex` to C++ in
`app/generated/` (the log is `out/codegen.log`) and then applies and checks the **post-generation patches** that the
build depends on. The manifest includes two hand-written files:

- `app/overrides.toml`: code that the automatic function discovery misses (gaps between functions, found with
  `tools/find_gaps.py` and `tools/gap_fixpoint.py`), each with its evidence.
- `app/perf_overrides.toml`: the code generation options that make the code faster (see [toolchain.md](toolchain.md)).

The patches that run after the generator are `direct_calls.py`, `pch_no_volatile.py`, `pch_no_global_lock.py`,
`pch_ui_viewport.py` and `pch_ui_world_to_screen.py`, then `verify_pch.sh` fails the run if any of them did not take
effect: a new generation silently loses them. At the end `read_before_write.py` checks the register use of the
generated functions and prints a warning for unexpected names.

Options, all environment variables: `ARGS_IN_REGISTERS=0|1` for one run, `EXTRA_TOMLS` (extra toml files merged after
`perf_overrides.toml`, for experiments), `STRICT=1` (the checks fail the run instead of warning) and `REXGLUE` (another
generator executable). The generated code is not committed: `app/generated/` is ignored by git.

## 5. Build the graphics driver

```sh
mesa/build_mesa_docker.sh           # an hour or more and about 15 GB, once
```

This clones [mesa-switch](https://github.com/danfromtico/mesa-switch) at a pinned commit, applies
`mesa/mesa-switch-masseffect.patch`, builds NVK and its shader compiler in Docker, and lays out the result as
`<OUT>/opt/devkitpro/portlibs/switch/lib/libvulkan.a`. By default `OUT` is `mesa-sdk` next to the repository folder
(`../mesa-sdk`), which is where the next step looks for it. After editing the driver source, use
`SRC=<tree> mesa/build_mesa_docker.sh --incremental`. Details, the patch contents and the reference toolchain versions
are in [mesa.md](mesa.md) and [../mesa/README.md](../mesa/README.md). `APPLY_ONLY=1` only clones and applies the patch.

## 6. Build the NRO

```sh
tools/build_nro.sh
```

This builds `out/nx/masseffect-nx.nro` inside the Docker image `devkitpro/devkita64` (the script builds a small image,
`masseffect-nx-build`, once, that adds glslang for the renderer's utility shaders). It needs `app/generated/` from
step 4, `sdk/thirdparty` from step 1 and the driver from step 5. The script finds the driver in `out/mesa-sdk` or
`../mesa-sdk`; set `MESA_SDK` if it is elsewhere. The unstripped ELF, with symbols for the profiler, stays next to it
as `out/nx/masseffect`. The script also checks that `crt0` is first in `.text`, because the NRO does not boot otherwise.

Environment variables (all optional, described at the top of the script):

| Variable | Default | Meaning |
|---|---|---|
| `JOBS` | 6 | Parallel compile jobs inside the container (1-2 GB each) |
| `MESA_SDK` | `out/mesa-sdk` or `../mesa-sdk` | Folder produced by the driver build |
| `NRO_OUT` | `out/nx` | Build folder |
| `MASSEFFECT_LTO` | OFF | Link-time optimization of the game and app code. Measured as no gain, needs about 16 GB |
| `MASSEFFECT_PGO` | empty | `generate` or `use`: profile-guided optimization (see [toolchain.md](toolchain.md)) |
| `MASSEFFECT_GEN_OPT` | empty | Extra optimization flag for the generated code, for example `-Os` (measured as worse) |
| `MASSEFFECT_FAST_MATH` | ON | `-fno-math-errno -fno-trapping-math` on the generated code |

The stock configuration is the one that measured best: no LTO, no PGO, function ordering on.

## 7. Build the shader library

The installer page does this in the browser, but its in-browser DXC is not built yet (see
[../installer/README.md](../installer/README.md)), so for now this is the way to get the shaders. The pipeline and the
file formats are in [shaders.md](shaders.md) and [../shaders/README.md](../shaders/README.md). The tools find xxHash and
fmt through `shaders/tools/env.sh`; after step 1 point them to the fetched copies, and give the paths of DXC and
`spirv-val` if they are not in `PATH` or the Vulkan SDK folder:

```sh
export XXHASH_DIR="$PWD/sdk/thirdparty/xxHash" FMT_DIR="$PWD/sdk/thirdparty/fmt/include"
# export DXC=/path/to/dxc SPIRV_VAL=/path/to/spirv-val        (if they are not found by themselves)

# 1. containers from the game's Unreal packages (about 30 s for the whole disc, 118 MB of output)
shaders/tools/build_extra_tools.sh ue3_shader_scan
find assets/game_root -type f -iname '*.xxx' -print0 | xargs -0 out/tools/ue3_shader_scan run/shaders

# 2. containers -> HLSL -> SPIR-V -> package + index (about 6 minutes on a 10-core Mac)
ALLOW_FAILURES=1 shaders/tools/build_shader_spirv.sh run/shaders run/shaderlib
#    -> run/shaderlib/masseffect_shaders.mesp  and  masseffect_shaders.mesp.idx

# 3. checks
shaders/tools/validate_shader_package.sh run/shaderlib/masseffect_shaders.mesp
```

On a complete disc, 30,191 distinct containers are found and 30,131 shaders end up in the package (56 containers crash
the translator and 4 are rejected by DXC; they are skipped, which is why `ALLOW_FAILURES=1` is needed). The package
is about 920 MB. `<out>` must not exist when `build_shader_spirv.sh` starts. Every step of the shader pipeline has more
options in [../shaders/README.md](../shaders/README.md).

## 8. Copy it to the SD card

Make this folder on the SD card, with these exact names:

```text
sdmc:/switch/masseffect-nx/
  masseffect-nx.nro               from out/nx/masseffect-nx.nro
  masseffect.toml                 from app/masseffect.toml
  masseffect_shaders.mesp         from run/shaderlib/
  masseffect_shaders.mesp.idx     from run/shaderlib/
  game_root/                      the files of your disc (assets/game_root)
```

`game_root/` is the whole disc except three things the installer page also leaves out, because the game never reads them:
`$SystemUpdate/`, `FillerFiles/` and `nxeart`. Keep `Coalesced.ini` unmodified: the game checks it against a SHA-1 stored
in the executable. The shader library names are fixed, and the index must sit next to the package it was made for
(a package without a matching index still loads, but slowly and with much more memory).

Then start it as described in the [README](../README.md): hold **R** on an installed title, or use a forwarder.

## Tests

```sh
tests/run_all.sh                run the CPU and Python tests (no console, no game files)
tests/run_all.sh --gpu          also the headless Vulkan proofs (VULKAN_SDK must point to the LunarG SDK)
```

They check the logic of the app headers and the shader tools, not speed or image quality on the console. The tests that
include SDK headers need step 1 first. The differential fuzzers of the native replacements (`tests/hot_fuzz`,
`tests/audio_dsp`) need `app/generated/` from step 4. The installer page has its own tests: `node --test installer/test/`
(Node 20 or newer). See [../tests/README.md](../tests/README.md). The scripts that deploy to a console and measure are in
[../tools/console-test/README.md](../tools/console-test/README.md); how to read their results is in
[measuring.md](measuring.md).

## Other editions

A different `default.xex` is a different program and needs its own generation and its own NRO. What that takes is in
[editions.md](editions.md).
