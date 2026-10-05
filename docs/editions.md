# Supporting other editions

A game is often sold in several editions: versions for other regions, other languages, or later pressings. Each edition
can have its own `default.xex` (the game's program), and every different `default.xex` is a different program that
needs its own generated code and its own build of the NRO. This page says which edition is supported, how an edition is
recognised, and what it would take to add another one. Words you do not know are in the [glossary](glossary.md).

## What is supported

The supported editions are:

| Edition | `default.xex` SHA-256 | NRO in the installer |
|---|---|---|
| Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1) | `db14a72a5a24a57309b8c51bd0311c676b7018ca6524e3eb85fbc891c2dfc70b` | `masseffect-nx.nro` |
| Mass Effect (Russia) (Ru) | `20c619de3c31f5c4448e7c54062577df839d59b2eb5ac5c633a2953dbbe525a3`<br>`4beb582540010b25032e3a51e4ce84a6fb1a9f381adddd8b2def0dc5d6bf715d` | `masseffect-nx-rus.nro` |

Its title id is `4D5307E8` (both English and Russian share title id `4D5307E8`; Russian Media ID is `572BA75D`, version 0.0.0.5). Every address in the sources (the `0x82......` numbers in `app/`, `tools/` and the toml files) refers to the specific executable build. Detailed documentation of the Russian edition port and address remapping is in [russian-edition.md](russian-edition.md).

## How an edition is recognised

The edition is recognised by the SHA-256 of `default.xex`:

- **The installer page** (`installer/`) hashes the file and looks the hash up in the `editions` table of
  `installer/config.js`. An unknown hash shows the hash and an explanation, and cannot be installed. Each entry gives the
  release asset name of the NRO for that edition (`nro`), and the NRO is downloaded from the site next to the page.
- **`tools/extract_iso.py`** prints the title id, version, image base address and entry point of `default.xex` when it
  extracts a disc, and warns if the title id is not Mass Effect's.
- **The NRO itself** contains the code recompiled from one exact `default.xex`. This documentation does not claim that
  the program refuses to run with another `default.xex`; the installer is what enforces the match. Do not mix an NRO
  with the files of another edition.

The shader package does not depend on the edition: it is keyed by the fingerprint of each shader's microcode container,
so editions that carry the same shaders share them. Even so, the installer always builds the package from the user's own
disc.

## Adding an edition

This is the work, in order. Nothing here has been done for a second edition yet, so every step is a plan, not a record.

1. **Get the executable and look at it.** Run `python3 tools/extract_iso.py assets/game_root/default.xex --info` for
   the title id, version, image base, entry point and image size, and take the SHA-256 of the file. If the image base and size match the supported edition, the code may be close to identical; if not, treat
   it as a different program.
2. **Generate its code in a separate work folder.** Run `tools/build_host.sh` and `tools/codegen.sh` with the new
   `default.xex` in `assets/game_root`. Expect the function boundaries to differ. The hand-written `app/overrides.toml`
   lists code that the automatic discovery misses, found with `tools/find_gaps.py`, `tools/gap_classify.py` and
   `tools/gap_fixpoint.py`, and `tools/pointer_scan.py` finds code pointers stored in data (use `MASSEFFECT_DUMP_IMAGE`
   to dump the loaded image). Repeat the gap work for the new executable.
3. **Re-find every address.** These are tied to one executable and must be checked, one by one, for the new one:
   - hooks and native replacements in `app/src/` (`me_resolution.cpp`, `me_ring_wait.cpp`, `me_physx.cpp`,
     `me_audio_hooks.cpp`, `me_d3d_trace.cpp`, `me_hot_guest.cpp`, and `app/src/native/hot/n_*.h`, whose names are the
     addresses they replace; the fuzzers in `tests/hot_fuzz` compare each against the recompiled original);
   - `lr_keep_returns` and the `share_registers` marks in `app/perf_overrides.toml`;
   - the post-generation patches in `tools/` (`pch_ui_viewport.py`, `pch_ui_world_to_screen.py`; `verify_pch.sh` tells you
     if a pattern is missing);
   - the `Coalesced.ini` check: the address of its SHA-1 table is in `app/src/masseffect_app.h`, and the override is
     skipped with a warning if the bytes there are not the expected ones;
   - the guest addresses the renderer recognises (`app/src/native/masseffect/`).
4. **Regenerate the hot-function order.** `app/function_order.ld` is made from a stack profile of one build, by
   `tools/function_order.py`, and its names are addresses, so each edition needs its own list (it gave no measurable gain
   here, so an empty or shared list is acceptable at first).
5. **Check the game on the console.** Use [measuring.md](measuring.md) and `tools/console-test/`: a location sweep and
   captures of many places, not two or three.
6. **Publish it in the installer.** Add an entry to `editions` in `installer/config.js` with the SHA-256 of the new
   `default.xex` and a distinct `nro` file name, and put that NRO in the release. The workflow publishes every `*.nro` of
   the latest release. Check `installer/test/plan.test.js` for the edition lookup tests.

## What to record

For each edition: the title id, version and SHA-256 of `default.xex`, the disc size and layout, which addresses moved,
which hooks needed a new address, and the console results. A table like the one at the top of this page is enough.
