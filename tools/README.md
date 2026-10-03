# Tools

The scripts that build the port from a clean checkout: they get the missing libraries, extract your disc, build the
code generator, translate the game to C++, fix up and check the generated code, and build the NRO. The tests of the
port are in [../tests](../tests/README.md); the scripts that deploy to a console and measure are in
[console-test/](console-test/README.md).

Everything here is run from the repository root and uses paths relative to it. Outputs go to `out/`,
`app/generated/` and `assets/`; none of them is committed, and none contains anything of the game except what you
extract yourself.

## Build, in order

| File | What it does |
|---|---|
| `fetch_thirdparty.py` | Fetches the third-party sources of the SDK into `sdk/thirdparty`. The repository only keeps the files this port changed. `build_host.sh` runs it when needed. |
| `extract_iso.py` | Extracts your Xbox 360 `.iso` into `assets/game_root` (a pure Python XDVDFS reader, no other tool needed) and prints the information of `default.xex`: title id, version, image base address, entry point. `--list` only lists the disc, `--xex-only` extracts only the executable, `--info` prints the header of an extracted `default.xex`. |
| `build_host.sh` | Builds the code generator `rexglue` for your computer (macOS or Linux) from `sdk/`. The result is `sdk/out/host/rexglue`. `--configure` only checks the toolchain. |
| `codegen.sh` | Runs `rexglue codegen` on `app/masseffect_manifest.toml` (it translates `assets/game_root/default.xex` to C++ in `app/generated/`), then the post-codegen steps below and the register-use checks. It builds nothing itself. `ARGS_IN_REGISTERS=0\|1`, `EXTRA_TOMLS`, `STRICT=1` and `REXGLUE` are described at the top of the script. |
| `build_nro.sh` | Builds `out/nx/masseffect-nx.nro` inside the Docker image `devkitpro/devkita64` (plus glslang for the utility shaders; the script builds that small image once). Needs `app/generated/` from `codegen.sh` and the NVK driver from `mesa/build_mesa_docker.sh` (`MESA_SDK`, found by itself in `out/mesa-sdk` or `../mesa-sdk`). `MASSEFFECT_LTO`, `MASSEFFECT_PGO`, `MASSEFFECT_GEN_OPT` and `JOBS` are described at the top of the script. |

The whole sequence on a fresh clone:

    python3 tools/extract_iso.py "Mass Effect.iso"
    tools/build_host.sh
    tools/codegen.sh
    mesa/build_mesa_docker.sh          # an hour or more, once
    tools/build_nro.sh

## After every code generation

`codegen.sh` runs these for you, right after the code generator, because a new generation silently loses them. If you
run the generator another way, run them in this order, then `verify_pch.sh`. All are idempotent and accept `--gen DIR`
(default `app/generated/default`), `--dry-run` and `--check`.

| File | What it does |
|---|---|
| `direct_calls.py` | Turns calls between recompiled functions into direct calls, except for calls to functions with a hook, so that the compiler can inline them (needed for LTO). |
| `pch_no_volatile.py` | Makes the guest loads and stores in `masseffect_pch.h` non-volatile, so the compiler can keep values in registers. |
| `pch_no_global_lock.py` | Lets the guest's interrupt-disable brackets skip the host global lock (switchable with a setting): they only surround compare-and-swap sequences. |
| `pch_ui_viewport.py` | Replaces the 1280x720 viewport the UI builds in one function with the internal resolution of the app. |
| `pch_ui_world_to_screen.py` | Makes the world-to-screen Y of the UI follow the internal resolution. |
| `pch_common.py` | Shared helpers of the `pch_*.py` scripts (file lookup, flags, errors). |
| `verify_pch.sh` | Fails if any patch above is missing from a generated tree. |
| `args_in_registers.py` | Fallback of the code generator option `args_in_registers`: rewrites already generated code so direct calls pass the arguments as C++ arguments instead of through the context. `--write-hooked FILE` writes the list of functions that keep the old convention; `--fix-ordering FILE` adds the fast entries to the function-order linker script. `codegen.sh` calls it only when the generator lacks the option. |
| `restore_hook_calls.py` | After adding a hook, turns the direct calls to the given addresses back into hookable calls without regenerating (a full `codegen.sh` does the same for every hook found in the sources). |

## Checks and analysis of the generated code

| File | What it does |
|---|---|
| `read_before_write.py` | Lists, per recompiled function, the registers it reads before writing them. With registers as C++ locals those start at zero, so a function that appears here must be marked `share_registers` in `app/perf_overrides.toml`. `codegen.sh` runs it and warns about unexpected names. |
| `find_gaps.py` | Finds code the generator's analysis left without a function (gaps between functions) and writes a report and a TOML block of candidates in the format of `app/overrides.toml`. Explains in its header why a gap is not always one function. Adapted from the gap scanner of the reference ReXGlue Switch port. |
| `gap_classify.py` | Classifies gap candidates from a scratch generation: only complete, self-contained leaves and thunks are accepted. |
| `gap_fixpoint.py` | Repeats the structural gap scan (find, scratch generation, classify, add to `app/overrides.toml`, regenerate) until no new gap qualifies. |
| `runtime_gap_resolver.py` | Reads logs of the game that stopped with "Call to invalid or unregistered function at guest address", checks with `find_gaps.py` whether each address lies in an unclaimed gap, and with `--apply` adds exactly one proven entry to `app/overrides.toml`. |
| `pointer_scan.py` | Finds code pointers stored in data (vtables, callback tables) that point into gaps, from a guest image dump (`MASSEFFECT_DUMP_IMAGE`). |
| `callgraph.py` | Call graph of the generated code (`build`, `callers`, `callees`, `grep`), for finding where the game's Direct3D is. |
| `pm4_ops.py` | Lists the PM4 packet headers a generated function builds (uses `callgraph.py`). |
| `coalesced.py` | Reads and writes the game's `Coalesced.ini` (`check`, `set-map`, `no-startup-movies`, `set`): start straight in a map, skip the logo movies. The game accepts a modified file only through the `masseffect_coalesced_sha1` setting. |
| `function_order.py` | Regenerates `app/function_order.ld`, the list of hot functions placed first in the program, from the stack-sampling profile of a console run (needs Docker for `addr2line`). |

## Folders

| Folder | What it has |
|---|---|
| `switch/cmake/` | The CMake toolchain file for devkitA64 and libnx, `switch-devkitA64.cmake`: the file that tells CMake how to compile for the Switch. `app/CMakeLists.txt` and `build_nro.sh` use it. |
| `console-test/` | Deploying to a real console and measuring there: upload, unattended cycle, live log, location sweep, CPU cost per frame. See its README. |

## Licence

The scripts adapted from the reference ReXGlue Switch port by StevenSND (`find_gaps.py`, `read_before_write.py`,
`direct_calls.py`, `fetch_thirdparty.py`, the toolchain file) keep GPL-3.0. Everything is GPL-3.0 like the rest of the
port, see `../LICENSE`.
