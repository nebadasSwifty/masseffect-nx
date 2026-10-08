# English edition: console test next to the Russian install (2026-10-08)

Goal: run the English build (`tools/edition.sh en all` -> `out/nx/masseffect-nx.nro`) on the same Switch that has the
Russian install in `sdmc:/switch/masseffect-nx/`, without touching that install.

## 1. Can two installs live side by side? Yes

Every runtime path is derived from the NRO's own path (`argv[0]`, filled in by hbloader), not from a fixed folder:

| What | Where it is resolved | Path |
|---|---|---|
| Executable folder | `sdk/src/core/filesystem_posix.cpp` `GetExecutablePath` (Switch branch: `__system_argv[0]`) | `<NRO folder>` |
| Config | `sdk/src/ui/rex_app.cpp` `SetupEnvironment` | `<NRO folder>/masseffect.toml` |
| Game data (VFS `game:`/`d:`) | `app/src/masseffect_app.h` `OnConfigurePaths` | `<NRO folder>/game_root` |
| User data: saves and DLC (`ContentManager`) | `GetUserFolder()` / `GetName()` (Switch: the executable folder) | `<NRO folder>/masseffect/` |
| SDK shader storage | `user_dir / "cache"` | `<NRO folder>/masseffect/cache` |
| Pipeline cache, prewarm list, cold cache | `masseffect_native_draws.cpp` | `<NRO folder>/cache/` |
| VFS index | `host_path_index.cpp` (parent of the game folder) | `<NRO folder>/cache/vfs_index_game_root.bin` |
| Shader package | `me_native_system.cpp` | `<NRO folder>/masseffect_shaders.mesp` + `.idx` |
| Logs | `rex_app.cpp` (`exe_dir / "logs"`) | `<NRO folder>/logs/` |
| Crash, assert, hang, profile files | `switch_crash_hooks.c` `RexSwitchLogDir` | `<NRO folder>/logs/rex/` |

Literal `/switch/masseffect-nx/` paths that remain, none of which matters for a second install:

* `app/src/main.cpp`: the default `argv[0]` used only when the loader passes none (Ryujinx). hbloader always passes it.
* `sdk/src/core/exception_handler_switch.cpp`: `sdmc:/switch/rex_crash.log`, only if `<NRO folder>/logs/rex/` cannot
  be translated.
* `tools/build_nsp.sh`: the forwarder NSP points at `sdmc:/switch/masseffect-nx/masseffect-nx.nro` (the Russian
  install on this console). Start the English NRO from hbmenu instead (see 3).
* `app/src/me_pgo.cpp` (PGO "generate" builds only) wrote to `sdmc:/switch/masseffect-nx/pgo/`. Changed on 2026-10-08
  to `<NRO folder>/pgo/` from `argv[0]`, same fallback (syntax-checked on the host; not in any built NRO yet).
* Comments and cvar help texts that use `sdmc:/switch/masseffect-nx/...` as an example.

So `sdmc:/switch/masseffect-nx-en/masseffect-nx.nro` gets its own config, game data, shaders, saves, DLC, caches and
logs. The Russian folder is never read or written by it.

## 2. Staging folder (local only, `out/` is ignored by git)

`out/en_stage/` holds exactly what the SD card needs, except the NRO (take `out/nx/masseffect-nx.nro` from the EN
build). The game data and DLC files are APFS clones (`cp -c`), so the staging folder costs almost no Mac disk space.

### game_root

Same selection as the installer (`installer/config.js` `disc.skip`, `installer/js/plan.js` `planGameFiles`): everything
under the disc root except `$SystemUpdate/`, `FillerFiles/` and `nxeart`.

* `default.xex` (SHA-256 `db14a72a5a24a573...`, the `editions/en` one), `Layer0/` (1,472 files), `Layer1/` (1,382 files)
* 2,855 files, 6,807,462,936 bytes; 2,194 `*.xxx` packages.

### Shader package

Built from the English packages on top of `out/shader-dlc` (the Russian disc + missing2 + DLC package), as in
`docs/image-defects-feros.md` 6.6 and `docs/dlc.md` section 4:

1. `out/tools/ue3_shader_scan out/en_stage_work/scan <all 2,194 *.xxx>` (list `out/en_stage_work/scanned_packages.txt`):
   403,857 containers seen, 30,191 distinct (the installer's `expectedContainers` for this edition).
2. `out/tools/extract_shader_package out/shader-dlc/masseffect_shaders.mesp` (30,875 containers) and comparison by
   content: 30,111 byte-identical, 80 not in the base.
3. 56 of the 80 are scanner false positives (no `00000000 00000024` words at offset 12): set aside in
   `out/en_stage_work/rejected_false_positives/`.
4. The remaining 24: 20 PS whose microcode is already in the base but with another container header (constant
   table), and 4 VS with new microcode (`vs_11b8c972fc24af0a`, `vs_664739fc5097d095`, `vs_b80a5d6fd38298fb`,
   `vs_e17c82a1e9171000`, the ones that failed translation in missing2/DLC).
5. `shaders/tools/translate_all.py` + `shaders/tools/compile_spirv_all.sh`: 24 translated, 24 SPIR-V, 0 failed (the
   4 VS now translate as well).
6. `out/tools/me_pack_shaders --replace-exact out/shader-dlc/masseffect_shaders.mesp out/en_stage_work/containers
   out/en_stage_work/spirv out/en_stage/masseffect_shaders.mesp`: 0 old entries replaced, 30,899 shaders
   (281 vertex, 30,618 pixel), 951,393,704 bytes, index 49,849,292 bytes.
7. `validate_shader_package`: `invalid=0`. `test_shader_index <mesp> <idx> --other out/shader-dlc/...idx`: OK.

`shaders/runtime_containers` (the D3D immediate-mode and Scaleform shaders, 290 containers): all already in the base
package byte for byte, so nothing to add.

The package is a superset of the Russian one, so it also covers the DLC and the Russian-only containers (harmless).

### DLC

The Marketplace packages are the same files for both editions (title 4D5307E8, English text). `ContentManager` reads
them from `<NRO folder>/masseffect/0000000000000000/`, so the English install needs its own copy:
`out/en_stage/masseffect/0000000000000000/` = `out/dlc_content/0000000000000000/` (251 files, 806,124,234 bytes,
BDtS + Pinnacle Station + both headers). Pointing `user_data_root` at the Russian folder would avoid the copy but also
share saves, so it is not used.

### masseffect.toml

`out/en_stage/masseffect.toml` = `app/masseffect.toml` with `dlc_enable = true` and, for the first runs,
`masseffect_diag_missing_shader_draws = true` (any EN-only shader gap is logged with its microcode). Remove that line
for timed runs.

## 3. Upload plan

Target folder: `sdmc:/switch/masseffect-nx-en/` (new; nothing in `sdmc:/switch/masseffect-nx/` changes).

| Mac source (under `masseffect-nx/`) | SD destination | Files | Bytes |
|---|---|---|---|
| `out/nx/masseffect-nx.nro` (EN build) | `/switch/masseffect-nx-en/masseffect-nx.nro` | 1 | 65,350,867 (2026-10-08 01:00 build) |
| `out/en_stage/masseffect.toml` | `/switch/masseffect-nx-en/masseffect.toml` | 1 | 15,292 |
| `out/en_stage/masseffect_shaders.mesp` | `/switch/masseffect-nx-en/masseffect_shaders.mesp` | 1 | 951,393,704 |
| `out/en_stage/masseffect_shaders.mesp.idx` | `/switch/masseffect-nx-en/masseffect_shaders.mesp.idx` | 1 | 49,849,292 |
| `out/en_stage/game_root/` | `/switch/masseffect-nx-en/game_root/` | 2,855 | 6,807,462,936 |
| `out/en_stage/masseffect/0000000000000000/` (only for a DLC test) | `/switch/masseffect-nx-en/masseffect/0000000000000000/` | 251 | 806,124,234 |

Total: 2,859 files, 7,874,072,091 bytes (7.33 GiB) without DLC; 3,110 files, 8,680,196,325 bytes (8.08 GiB) with it.
Without the DLC copy, set `dlc_enable = false` in the toml. `cache/`, `logs/` and `masseffect/` (saves) are created by
the game. Check that the copy tool adds no `.DS_Store` / `._*` files.

Start: hbmenu, `masseffect-nx-en/masseffect-nx.nro`, launched in title-override mode (hold R while starting a game),
as the forwarder only knows the Russian path. The first start is cold (no `cache/`). Expect in
`/switch/masseffect-nx-en/logs/`: `shaders: library with 30899 shaders`, the game root at
`.../masseffect-nx-en/game_root`, and with DLC on, both packages listed.

Work files: `out/en_stage_work/` (scan, containers, hlsl, spirv, rejected false positives; game data, local only).
