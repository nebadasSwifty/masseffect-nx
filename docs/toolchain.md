# From the game's code to the NRO (the toolchain)

## In short

The game's program, `default.xex`, is machine code for the Xbox 360's processor. The build turns it into C++ ahead of
time ([static recompilation](glossary.md#static-recompilation)), compiles that C++ for the Switch together with the
port's own code, and packs the result into an NRO. How fast the result runs depends a lot on how the C++ is written, so
most of this page is about options of the code generator. The measured effect of each one is in
[optimization-paths.md](optimization-paths.md) and the story of the speed work in
[performance-history.md](performance-history.md). The commands are in [building.md](building.md).

The main lessons:

- The options that paid off change **what the generated code does**: keeping the processor's registers in host
  registers instead of in memory, dropping stores nobody reads, and passing values directly between functions. Together
  they cut the main game thread from 32.4 to about 29 ms per frame.
- The options that change **how the compiler works** (link-time optimization, profile-guided optimization, `-Os`,
  function ordering) gave no measurable gain on this code, so they are off or not shipped. With millions of generated
  lines and hot functions that are already called directly, the compiler has little left to see across files.
- `-ffp-contract=off` is required for correctness, not for speed.

## The path from the game to the NRO

```
assets/game_root/default.xex                    your disc (never committed)
        |  tools/build_host.sh                  builds the code generator `rexglue` for your computer, from sdk/
        |  tools/codegen.sh                     rexglue codegen app/masseffect_manifest.toml
        |      + app/overrides.toml             code that automatic discovery misses (hand-written, with evidence)
        |      + app/perf_overrides.toml        code generation options for speed
        v
app/generated/default/*.cpp, *.h                the game as C++ (not committed), and app/generated/rexglue.cmake
        |  post-generation patches (tools/*.py) found by search, idempotent, verified by tools/verify_pch.sh
        v
CMake (app/CMakeLists.txt) in the Docker image devkitpro/devkita64:
        masseffect_recomp  = the generated code
        masseffect         = app/src + the SDK (sdk/) + the NVK driver (libvulkan.a from mesa/)
        v
out/nx/masseffect-nx.nro   (+ out/nx/masseffect, the ELF with symbols)
```

The generator is the host build of the SDK (`sdk/`), so it has to run on your computer: a cross build cannot execute it.
The Switch build itself runs in Docker with devkitA64 (GCC for AArch64) and libnx, with the toolchain file
`tools/switch/cmake/switch-devkitA64.cmake`.

Some numbers to give the idea of scale: the generated code is about 7 million lines of C++, about 50 MB of `.text`, and
the executable is about 78 MB. Generation sets two environment variables for the generator, `REX_MAX_JUMP_TABLE_ENTRIES`
(the most entries it accepts per detected jump table: 1024 here, the default is 512) and
`REX_REGISTER_SHARD_SPAN_BYTES` (the registration of the functions is split into shards of that many bytes of guest
address space: 262144 here, and 0 would disable sharding).

### Finding all the code

The generator discovers functions by analysing the executable and misses some: code reached only through pointers in
data, thunks, and split-off pieces. The missing pieces show up as "Call to invalid or unregistered function at guest
address ..." in a log of the running game. The tools that deal with it:

| Tool | What it does |
|---|---|
| `tools/find_gaps.py` | Finds code the analysis left without a function and writes a report and a toml block of candidates |
| `tools/gap_classify.py`, `tools/gap_fixpoint.py` | Accept only complete, self-contained leaves and thunks, and repeat until nothing new qualifies |
| `tools/pointer_scan.py` | Finds code pointers stored in data (vtables, callback tables) that point into gaps, from a dump of the guest image (`MASSEFFECT_DUMP_IMAGE`) |
| `tools/runtime_gap_resolver.py` | Reads the log of a game that stopped on an unregistered function and, with `--apply`, adds exactly one proven entry to `app/overrides.toml` |

## Registers as C++ locals

The generated code keeps the PowerPC registers in a structure in memory, the [ctx](glossary.md#ctx). About 20 % of the
instructions of the generated code were loads and stores of it, and the Switch's slow cores feel that. The options in
`app/perf_overrides.toml` make the compiler keep registers in real registers. They change only how the C++ is written;
the guest-visible result is the same. Measured together: about 13 % fewer instructions and 3 % less main thread time.

| Option | What it does | Measured |
|---|---|---|
| `cr_as_local`, `xer_as_local`, `ctr_as_local` | The condition, carry/overflow and count registers as locals | part of the locals result |
| `non_volatile_as_local` | r14-r31 as locals: 1.09 million uses of `ctx.r14`-`ctx.r31` down to 4 thousand, and 35 thousand save and restore helper calls removed | +9 % (t219) |
| `non_argument_as_local` | r0, r2, r11, r12, f0 and the vector registers v32-v63 as locals too | main thread 32.4 to 31.7 ms (-2 %) |
| `skip_msr`, `reserved_as_local` | No `mfmsr`/`mtmsrd` fences, no `ctx.msr` stores and no global lock around guest atomic sequences, and the `lwarx`/`stwcx.` reservation in a local | main thread 31.3 ms (-3.4 % in all) |
| `elide_lr_stores`, `lr_keep_returns` | No `ctx.lr = <return address>;` before calls, except at the four listed return addresses that hooks compare | the "diet" run: main thread 32.4 to 29.1 ms (-10 %) |
| `dform_disp_split` | D-form loads and stores as base plus zero-extended register plus displacement | in the diet run |
| `fctiwz_inline` | `fctiwz` as `fcvtzs` plus a NaN select | in the diet run |

### What can go wrong: `share_registers`

A local register is not visible to anything outside its function, so a function that reads a register another function
set (a stack probe that takes its size in r12, a fragment entered with a jump) must keep that register in `ctx`. That is
what `share_registers` marks (the five entries at the end of `app/perf_overrides.toml`). `tools/read_before_write.py`
lists, per function, the registers it reads before writing them, and `tools/codegen.sh` runs it after every generation
and warns about unexpected names. **Do not mark ordinary functions**: marking 61 functions that a loop-blind scan chose
zeroed a register in a caller and crashed the game at start-up. The comment at the top of `perf_overrides.toml`
explains the rule. The recompiled guest `setjmp`/`longjmp` is not supported (the longjmp returns to its caller instead
of to the setjmp site); it is only reached on error paths of the game's shader compiler library.

## Arguments in registers

`args_in_registers` makes a generated function `sub_X(ctx, base)` a thin wrapper around `__fast_sub_X(ctx, base, r3, r5,
f1, lr, ...)`. Direct calls between recompiled functions pass r3-r10, f1-f13, r0/r11/r12/f0 and `lr` as C++ arguments and
take r3 back as the return value. Functions that a hook may replace keep the old convention
(`args_in_registers_exclude_file`, written by `tools/codegen.sh` as `app/hooked_functions.txt`). Where a function has to
leave a register in `ctx` for a caller or an unknown callee, the pass exports it at the exits.

It is **off by default** (`args_in_registers = false`): statically it removes about 10 % of the instructions of the hot
files and 65 % of the `ctx` stores, but on top of the locals and the diet it gave no measurable gain on the console
(100.1 core-ms against 98.6). `ARGS_IN_REGISTERS=1 tools/codegen.sh` turns it on for one generation, and
`tools/args_in_registers.py` applies the same transformation to already generated code when the generator lacks the
option. One lesson is kept in the code: every fast body starts with an empty `asm volatile("" ::: "memory")`, because
GCC decided a read-only fast body was `pure` and a polling loop around it was deleted (the first console run froze).

## Direct calls

By default a call between two recompiled functions goes through a weak symbol, because a hook may replace the target.
`tools/direct_calls.py` rewrites every call to a function that has **no** hook into a direct call to the strong
`__imp__sub_X` symbol, so the compiler can see it and inline it (157,617 calls in 190 files). Any `0x82xxxxxx` address
that appears in `app/src` counts as hooked, so keep hook addresses in the sources. It runs on every generation: a new
generation silently loses it, and `tools/verify_pch.sh` fails if it is missing. After adding a hook without
regenerating, `tools/restore_hook_calls.py` turns the direct calls to that address back.

The other post-generation patches are small and specific: `pch_no_volatile.py` makes guest loads and stores
non-volatile so values stay in registers (+15-20 %; the risk is a loop that only polls memory), `pch_no_global_lock.py`
lets the interrupt-disable brackets skip the host global lock, and the two `pch_ui_*.py` scripts make the UI follow the
internal resolution. All are idempotent and take `--gen DIR`, `--dry-run` and `--check`.

## Compiler flags

Set in `app/CMakeLists.txt` for the generated code (the SDK compiles itself with the same two first flags):

| Flag | Why |
|---|---|
| `-ffp-contract=off` | GCC's default for GNU C++ fuses a separate PowerPC `fmul` and `fadd` into an AArch64 fused multiply-add, and the results drift from the Xbox 360. Required for correctness. The executable grew by 1.2 MB when the flag was applied, which showed that fusion had been happening |
| `-fno-strict-aliasing` | The recompiled code reads and writes guest memory through casts |
| `-fno-math-errno -fno-trapping-math` | No `errno` side effect on `sqrt` and friends and no trap assumptions: removes the libm slow-path call after every `fsqrt` (-1.1 % instructions on the hot functions). Results are identical. Option `MASSEFFECT_FAST_MATH`, on by default |
| `-g1` (the app's own sources) | Line tables only, no code change, so profiles can be turned into source lines |

## LTO

Link-time optimization of the game and app code (`MASSEFFECT_LTO=ON`, not the SDK libraries) compiles with
`-flto -fno-fat-lto-objects` and links with `-flto=2 -flto-partition=balanced`. With direct calls, small functions inline
across files. **Measured: no gain** (226 core-ms per frame against 218-226 without), builds take far longer and the build
container needs about 16 GB, so it is off by default.

## PGO

Profile-guided optimization works in two builds of the same build folder, with `MASSEFFECT_PGO`:

1. `MASSEFFECT_PGO=generate tools/build_nro.sh` builds an instrumented NRO. GCC embeds the path of each `.gcda` file;
   `app/src/me_pgo.cpp` redirects them to `sdmc:/switch/masseffect-nx/pgo/` and dumps the counters every 60 seconds from a
   thread, because the system does not always close the program cleanly (HOME kills it).
2. Play for a while, then copy the files from `sdmc:/switch/masseffect-nx/pgo/` into a folder named `pgo/` at the root of
   the repository.
3. `MASSEFFECT_PGO=use tools/build_nro.sh` rebuilds the **same** build folder with the profile. Do not regenerate the
   code or edit sources in between.

The flags handle two Switch facts: `-fno-profile-values`, because libgcov's indirect-call profiling reads thread-local
storage through `tpidr_el0`, which is 0 on Horizon, and `-fprofile-partial-training`, because a profile from a short
session does not cover the whole game. **Measured: no gain.** A first run showed 19.6 fps against 18.7, but a
back-to-back pair in the same session gave 16.7 against 16.6, so the first number was session noise. For that reason the
repository has **no `pgo/` folder and no profile is shipped**. The profile of any run you make is for experiments only.

## Function ordering

The Cortex-A57 has 48 KB of instruction cache and a small TLB, and the game code is tens of megabytes.
`app/function_order.ld`, passed to the linker with `--section-ordering-file` (`MASSEFFECT_FUNCTION_ORDER`, on by default),
puts the hottest functions first in `.text` with `crt0` first of all (the NRO does not boot otherwise). The list comes
from stack sampling on the console: `tools/function_order.py PROFILE_LOG ELF` resolves the sampled addresses with
`addr2line` inside the devkitA64 Docker image and keeps the 2000 hottest functions. **Measured: no measurable gain**
(18.7 against 18.9 fps); it is kept because it costs nothing. Its names are function addresses, so the list is specific to
one executable and one generation.

## Smaller code

Because 50 MB of code does not fit any cache, `MASSEFFECT_GEN_OPT=-Os` builds the generated code for size. Measured:
**worse** (21.8 against 24.9 fps, the main thread at 98-102 % of its core). Keep `-O3`.

## Debugging crashes on the console

- A fatal fault writes the registers, address and stack to `sdmc:/switch/masseffect-nx/logs/rex/rex_crash.log` and shows
  an error 2345-0102. `abort()` shows 2345-0101, `exit()` 2345-0103 and an uncaught exception 2345-0104, and standard
  error goes to `logs/rex/rex_stderr.log`. All are written by `sdk/src/ui/switch_crash_hooks.c`.
- For a guest access violation the SDK also writes the guest registers, the guest stack chain and the memory the registers
  point to, which is what finds where a bad guest pointer came from.
- The profiler (`logs/rex/rex_profile.log`, every 10 seconds) samples where each thread is at 1 kHz; addresses come out as
  `image+0x...` and are translated with `addr2line` on the unstripped ELF of the same build (`out/nx/masseffect`). See
  [measuring.md](measuring.md).
- `tools/callgraph.py` and `tools/pm4_ops.py` help to find where the game's Direct3D is in the generated code.
