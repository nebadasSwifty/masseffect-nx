# Downloadable content (Bring Down the Sky, Pinnacle Station)

Status: runtime support written behind `dlc_enable` (default off), host extractor `tools/stfs_extract.py` ready,
installer step written (`installer/js/stfs.js`, section 4). Nothing tested on the console yet.

DLC files are copyrighted game data. Never commit them, never put them in a release or an artifact. The player
supplies their own packages.

## 1. The packages

Xbox 360 Marketplace content for title `4D5307E8`, content type `00000002`. Both are single-file STFS packages with
`LIVE` magic, metadata v2, read-only STFS volumes (one hash table per level). `tools/stfs_extract.py info` prints all
of this:

| File name (42 hex chars) | Display name | DLC id | Package size | Files | Payload |
|---|---|---|---|---|---|
| `A275890E35D31622A6AB8D43C53F932A0342DEF64D` | Bring Down the Sky [ENPLES] | `DLC_UNC` (map `UNC52`) | 409.5 MB | 73 files, 100 entries | 388 MB |
| `2F0186372DDADB3B2C8557688F02D2587EEAFBC84D` | Pinnacle Station [ENPLES] | `DLC_Vegas` (maps `PRC2`, `PRC2AA`) | 402.0 MB | 176 files, 198 entries | 381 MB |

* Content ids: BDtS `C823528729EC4E1A8A21D5EBB89F5B8245E986A4`, Pinnacle `7697174923C8DAC41156F3070ABCAED001099271`.
* Licenses in the header: licensee `FFFFFFFFFFFFFFFF`, bit 1 (BDtS has flags 0, Pinnacle flags 1).
* Language: `[ENPLES]` = English, Polish, Spanish. Default text (`DLC_*_GlobalTlk`) is English, plus
  `Localised/ES` and `Localised/PL`. There is no Russian. AutoLoad.ini also names `_DE`, `_IT`, `_FR` tables that are
  not in these packages.
* BDtS files are all stored contiguously; Pinnacle has fragmented files, so a reader must follow the hash chain.

Tree (abridged):

```text
AutoLoad.ini                    what the game reads first (see section 2)
BIOCredits_DLC_UNC.ini / BIOCredits_DLC_Vegas.ini
spa.bin                         SPA (achievements, title strings) of the DLC
Xbox360ToC.bin                  BDtS only: UE3 binary table of contents
Content/Maps/UNC52/{,CIN,DSG,LAY,SND}/*.xxx      BDtS: 31 map packages
Content/Maps/PRC2/*.xxx, PRC2AA/*.xxx            Pinnacle: 90 map packages
Content/Packages/2DAs/BIOG_2DA_<DLC>_*_X.xxx     2DA merges: galaxy map, area map, treasure, talents, UI...
Content/Packages/Dialog/DLC_<DLC>_GlobalTlk.xxx  the DLC's talk table (+ Localised/ES, PL)
Content/Packages/GUI/*.xxx, GameObjects/**, Textures/, VFX/, Audio_Content/
Content/Packages/ISACT/*.isb, *.icb              audio banks
Movies/DLC_UNC_Opening.bik, DLC_UNC_Ending.bik   BDtS only
Script/, ScriptFinalRelease/                     BIOC_BaseDLC_<DLC>.xxx, PlotManagerDLC_<DLC>.xxx
```

AutoLoad.ini lists, under `[Packages]`, the 2DA packages to merge (`2DA1..9`), the script package (`DotU1`), the plot
manager maps and the talk tables per language; under `[GUI]` the name/description string refs, the save-load image and
the credits file.

## 2. How the game finds DLC

Strings in the executable (UTF-16) show the stock UE3 Xbox flow plus BioWare's layer:

1. `UOnlineSubsystemLive` calls `XContentCreateEnumerator()` for content type 2 (Marketplace) and enumerates
   `XCONTENT_DATA` items.
2. For each item it opens the package with `XContentCreate` under the root `DLC%d` (`DLC0`, `DLC1`, ...), so the
   files appear as `DLC0:\AutoLoad.ini`, `DLC0:\Content\...`.
3. `ABioWorldInfo::InitDownloadableContent` / `LoadDLCTlkFiles` read `AutoLoad.ini` (`DLC_%s`, `%s\%s\*`, `%s:\*`,
   sections `Packages` and `GUI`, keys `NameStrRef`, `DescriptionStrRef`, `ImagePackage`, `ImageFrame`,
   `CreditsFile`), add the DLC folders to the package search path, merge the 2DAs and load the talk tables. The
   binary table of contents (`Xbox360ToC.bin`) is read when present.
4. Messages 153723/153801 ("Downloadable Content is installed", "Initializing Downloadable Content") come from this
   path; 154280 is shown when a save needs a DLC that is not mounted.

On the runtime side (SDK, Xenia-derived):

* `XamContentCreateEnumerator` (`sdk/src/kernel/xam/xam_content.cpp`) lists both the profile's and the shared
  (`xuid 0`) content through `ContentManager::ListContent`.
* `XamContentCreate` forces `xuid 0` for Marketplace content and opens
  `<content_root>/0000000000000000/4D5307E8/00000002/<file name>` (`ContentManager::ResolvePackagePath`).
* The content root on the Switch is the user data folder: `sdmc:/switch/masseffect-nx/masseffect/`
  (`GetUserFolder()` = NRO folder, plus the app name `masseffect`). Saves already live there.
* Xenia's layout (extracted tree per package, plus `Headers/00000002/<name>.header` holding an
  `XCONTENT_AGGREGATE_DATA` and the license mask) works unchanged: it is the same code.

## 3. Runtime support (implemented, `dlc_enable`, default off)

Files: `sdk/src/system/xam/content_manager.cpp`, `sdk/src/filesystem/devices/stfs_container_file.cpp`.

* `dlc_enable = false` (default): `ListContent` reports no Marketplace content, the game sees no DLC. One log line
  `[dlc] the game asked for downloadable content; dlc_enable is off, reporting none` shows that the game asked.
* `dlc_enable = true`:
  * every folder under `.../4D5307E8/00000002/` is reported (display name from its `.header` if present);
  * a raw STFS package file in the same folder (magic `CON `/`LIVE`/`PIRS`) is also reported, with the display name
    from its own header, and is mounted read-only through `StfsContainerDevice` instead of `HostPathDevice`.
    This needs no extraction, but reads go 4 KB-block-run by run through one locked `FILE*`; the extracted form is the
    one to ship, the raw form is a quick test path;
  * when a Marketplace package is opened and its `.header` stores no license, the game gets `dlc_license_mask`
    (default `0xFFFFFFFF`, every bit granted);
  * closing a DLC root never copies it to `content_backup_root` (saves still are).
* Log lines (`[dlc]` tag):
  * `[dlc] list type 00000002 in <path>: N entry(ies)` and one `[dlc]   '<file name>' (displayed as '<name>')` each;
  * `[save] XamContentCreate root 'DLC0' content '<file name>' type 00000002 mode 3 -> open (result 00000000)`;
  * `[dlc] mount '<file name>' ('<name>') as DLC0: from <path> (extracted folder|STFS package), AutoLoad.ini found,
    license FFFFFFFF`;
  * `[dlc] unmount ...` when the game closes it.
* `StfsContainerFile::ReadSync` now takes a lock (seek + read on a shared `FILE*` are not thread-safe). STFS
  containers were only used by `ContentManager::InstallContent` before, so nothing else changes.

### Console test

Everything below is prepared on the Mac (2026-10-07); nothing of it is in git (`out/` is ignored).

**Files.** Extracted with
`python3 -I tools/stfs_extract.py extract --verify --content-root out/dlc_content <both package files>` (every
block's SHA-1 checked, no mismatch). Shader package built as described in section 4 "Shaders".

| Mac source (under `masseffect-nx/`) | SD destination | Files | Bytes |
|---|---|---|---|
| `out/dlc_content/0000000000000000/4D5307E8/00000002/A275890E35D31622A6AB8D43C53F932A0342DEF64D/` (BDtS) | `sdmc:/switch/masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/A275890E35D31622A6AB8D43C53F932A0342DEF64D/` | 73 | 406,894,838 |
| `out/dlc_content/0000000000000000/4D5307E8/00000002/2F0186372DDADB3B2C8557688F02D2587EEAFBC84D/` (Pinnacle) | `sdmc:/switch/masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/2F0186372DDADB3B2C8557688F02D2587EEAFBC84D/` | 176 | 399,228,732 |
| `out/dlc_content/0000000000000000/4D5307E8/Headers/00000002/A275890E35D31622A6AB8D43C53F932A0342DEF64D.header` | `sdmc:/switch/masseffect-nx/masseffect/0000000000000000/4D5307E8/Headers/00000002/` | 1 | 332 |
| `out/dlc_content/0000000000000000/4D5307E8/Headers/00000002/2F0186372DDADB3B2C8557688F02D2587EEAFBC84D.header` | same folder | 1 | 332 |
| `out/shader-dlc/masseffect_shaders.mesp` | `sdmc:/switch/masseffect-nx/masseffect_shaders.mesp` (replaces the missing2 package) | 1 | 950,986,884 |
| `out/shader-dlc/masseffect_shaders.mesp.idx` | `sdmc:/switch/masseffect-nx/masseffect_shaders.mesp.idx` (always together with the `.mesp`) | 1 | 49,827,496 |

Simplest: copy the whole `out/dlc_content/0000000000000000/` folder into `sdmc:/switch/masseffect-nx/masseffect/`
(806,124,898 bytes, 251 files), plus the two shader files. About 1.81 GB in all; the old package (925 MB + 49 MB) is
overwritten, so the net SD growth is about 830 MB. Check that the copy tool did not add `.DS_Store`/`._*` files inside
`00000002/` (`ListContent` reports every folder there; stray files are only probed for an STFS magic, so they are
harmless, but keep the folder clean). The new shader package is a superset of missing2, so it is also the right
package with `dlc_enable = false`. For a one-DLC first run, upload only BDtS and its header (the shader package stays
the same).

**Settings.** `masseffect.toml`: `dlc_enable = true`; keep `masseffect_diag_missing_shader_draws = true` for this run.
Same NRO otherwise (the DLC code must be in it).

**Expected log lines.**

1. Shader package: `[native] shaders: library with 30875 shaders (277 vertex, 30598 pixel); ...` and
   `[native] shaders: library loaded in ... ms (index only; SPIR-V on demand, ...)`. A `index ... not used` warning
   means the `.idx` does not belong to the `.mesp` (copy both).
2. At boot / main menu: `[dlc] list type 00000002 in <...>/0000000000000000/4D5307E8/00000002: 2 entry(ies)`, then
   `[dlc]   'A275890E35D31622A6AB8D43C53F932A0342DEF64D' (displayed as 'Bring Down the Sky [ENPLES]')` and
   `[dlc]   '2F0186372DDADB3B2C8557688F02D2587EEAFBC84D' (displayed as 'Pinnacle Station [ENPLES]')`.
3. `[save] XamContentCreate root 'DLC0' content '<file name>' type 00000002 mode 3 -> open (result 00000000)` (and
   `DLC1` for the second), then
   `[dlc] mount '<file name>' ('<name>') as DLC0: from <path> (extracted folder), AutoLoad.ini found, license FFFFFFFF`.
4. With `dlc_enable = false` instead: one `[dlc] the game asked for downloadable content; dlc_enable is off, reporting
   none` line.

**In game.** Message 153801 "Initializing Downloadable Content"; on the Normandy galaxy map Asteroid X57 in the
Exodus Cluster (BDtS) and Pinnacle Station (its entry comes from `BIOG_2DA_Vegas_GalaxyMap_X`). The journal and the
save/load screen get the DLC images. On the DLC maps (UNC52, PRC2/PRC2AA): no `unidentified pixel/vertex shader` /
`missing shader draw` lines except, possibly, for the two vertex shaders that could not be translated (section 4).

**Things to watch.** `[dlc] ... AutoLoad.ini MISSING` (path case or mount failure), file-not-found lines for `DLC0:`,
any `missing shader draw` on DLC maps (with a rebuilt NRO they come with complete microcode), memory use on the
asteroid maps (UNC52 `_05_DSG` is 49 MB) and DLC map load times (section 4, "Read speed").

## 4. Installer and shaders

### Installer (`installer/`, implemented 2026-10-08)

The optional step "DLC packages (optional)" (step 3 of the page, between the edition and the create step) takes one
or more package files (file picker or drop). Nothing is uploaded or copied: everything is read lazily from the `File`.

* **`js/stfs.js`**: a JavaScript port of `tools/stfs_extract.py` (same block math as
  `StfsContainerDevice::BlockToOffsetSTFS`, same hash-table selection for read-only and read-write volumes, same
  file-table walk). `openStfs(file, {fileName, expect: {titleId, contentType}, licenseMask})` returns
  `{meta, folderName, entries, files, payloadBytes, header}`:
  * `meta`: magic, title id, content type, metadata version, content id, display names/descriptions (12 languages),
    licenses and the installer license rule, volume descriptor;
  * `files`: `{path, size, contiguous, runs, blob}`; `blob` is `new Blob([file.slice(...), ...])` over the file's
    block runs (adjacent blocks merged, fragmented files followed along the level-0 hash chain), so the scan workers
    and the zip writer read DLC files exactly like disc files;
  * `header`: the 332-byte `.header` file (`XCONTENT_AGGREGATE_DATA` 0x148 bytes big-endian + license mask
    `FFFFFFFF` little-endian), identical to what `stfs_extract.py extract` writes;
  * `folderName`: the package file name (as the Python tool uses it) when it is plain ASCII of at most 42
    characters, otherwise the content id (a browser may have renamed a download to `... (1)`).
  `verifyStfs(pkg)` checks the SHA-1 of every data block against the hash tables (`--verify` of the Python tool) and
  that each file's hash chain gives the same blocks as its Blob. Rejected with a message: not `CON `/`LIVE`/`PIRS`,
  another title (`belongs to another game (title ..., expected 4D5307E8)`), another content type (`is not
  downloadable content (content type 000B0000 GameTitle, ...)`, e.g. a title update or a save), SVOD, truncated
  packages, unsafe names, block hash mismatches, and the same package twice (by content id).
* **UI** (`index.html`, `js/app.js`, `js/i18n.js`): the list shows display name, file name, file count and size;
  each package is SHA-1 checked when added (about 0.5 s per 400 MB package in Node with the file in the OS cache; a browser and a cold disk are slower,
  not measured); the create
  buttons wait for it. The size estimates include the DLC payload plus ~26 MB of extra shaders. Offered in update
  mode too.
* **Pipeline** (`js/pipeline.js`, `run({..., dlc})`): the DLC `*.xxx` files (listed as `DLC/<folder>/<path>` in the
  log) are scanned together with the disc's, so one combined `masseffect_shaders.mesp`/`.idx` comes out. Then, in
  both the full and the update zip, after `game_root/`:
  `masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/<folder>/<path>` for every file and
  `masseffect-nx/masseffect/0000000000000000/4D5307E8/Headers/00000002/<folder>.header` per package (paths in
  `config.js` -> `dlc`). `masseffect.toml` gets `dlc_enable = true` (an existing or commented-out top-level
  `dlc_enable` line is replaced, otherwise the line is appended; the manifest checksum is checked before the edit).
  Directories without files are not written (the two real packages have none).
* **Shader package cap**: after packing, a package above `limits.maxShaderPackageBytes` (1,073,741,824 bytes, the
  runtime's `kMaxFile`) stops the run with a clear message (the packer has the same limit). Base + both DLCs is
  950,986,884 bytes (section "Shaders").
* **Update mode note**: the update zip overwrites `masseffect.toml`. An update made without the DLC packages
  selected writes the stock toml (`dlc_enable` off) and leaves the extracted DLC on the SD card unused; add the
  packages again in the update run (their shaders must be in the package anyway).
* **Tests** (`installer/test/`, no game data): `stfs.test.js` builds synthetic packages with an independent writer
  (`stfs_builder.js`: read-only and read-write volumes with both root-index settings, three hash-table groups, an L1
  table, a two-block and a non-adjacent file table, contiguous files across a hash table, fragmented files, empty and
  partial-block files) and checks the reader, the header bytes, rejections, SHA-1 verification (a flipped byte and a
  contiguous flag that disagrees with the chain), the block math against numbers printed by `stfs_extract.py`, and,
  when `python3` is present, extracts the same packages with `tools/stfs_extract.py --verify` and compares every
  file and the `.header` byte for byte. `dlc_pipeline.test.js` runs `run()` with stand-in workers and checks the
  scanned names, every zip path and its bytes, the toml edit and the 1 GiB refusal.
* **Checked against the real packages** (2026-10-08, local only, nothing copied): `openStfs` + Blob reads under
  Node 26 against `out/dlc_content` from `stfs_extract.py extract --verify`: BDtS 73 files, 406,894,838 bytes, and
  Pinnacle 176 files (all non-contiguous), 399,228,732 bytes, all identical; no file missing or extra; both 332-byte
  `.header` files identical; `verifyStfs` passes on both.

### Shaders

* The scanner finds containers by content (`shaders/tools/ue3_shader_scan.cpp`), so adding the DLC `.xxx` files
  (`Content/**`, `Script*/`) to the scan input is all that is needed; the result is one combined
  `masseffect_shaders.mesp`/`.idx`. Same for the command-line pipeline in `docs/building.md` (pass the extracted DLC
  folders next to `game_root`).
* At runtime a shader that is not in the package is not translated on the device: the draw is dropped and the log
  shows `[native] shaders: unidentified ... shader` and `[native] unresolved draw shader pairs`. So DLC maps without
  a rebuilt package will miss geometry. That is the first thing to check in a DLC console run.
* Only one library is loaded (`MASSEFFECT_SHADER_LIBRARY` or `masseffect_shaders.mesp`), limited to 1 GiB
  (`kMaxFile` = 1,073,741,824 bytes) and 65536 shaders (`kMaxShaders`), both in
  `app/src/native/masseffect/masseffect_shader_library.cpp`. That one file is the limit everywhere: the runtime reader
  (`Load`/`LoadIndexed`), the packer (`PackShaders`, used by `me_pack_shaders` and by the installer's pack worker,
  which is the same C++ compiled to WebAssembly) and the validators. The format itself is not the limit (package
  size and entry offsets in the `.idx` are u64); raising the cap would be a change of that constant (old packages
  still load), plus a look at the index bound (`kMaxIndex`), at the browser memory the installer's pack worker needs,
  and at the fallback without `.idx`, which reads the whole package into Switch memory. Not needed yet: the DLC package below
  is 950,986,884 bytes (122.7 MB / 11 % below the cap) and 30,875 shaders.
* **Built package: `out/shader-dlc/masseffect_shaders.mesp` + `.idx`** (2026-10-07, missing2 + DLC; game data, local
  only). Steps:
  1. `out/tools/ue3_shader_scan out/shader-dlc/scan <all 205 DLC *.xxx>` (list: `out/shader-dlc/scanned_packages.txt`,
     Maps, Packages, Script, ScriptFinalRelease of both DLCs): 34,888 containers seen, 5,697 distinct.
  2. Compared with the 30,160 containers of `out/shader-missing2` (`out/tools/extract_shader_package`): 4,797 are
     byte-identical to a package entry; 900 are not.
  3. Of the 900, 183 are scanner false positives (the `0x102A11xx` pattern found inside LZO-compressed bytes; their
     header does not have the `00000000 00000024` words that every real container has): 125 of them crash
     XenosRecomp, 58 "translate" to garbage. They were set aside (`out/shader-dlc/rejected_false_positives/`).
  4. The remaining 717 real containers: 645 PS and 2 VS with microcode not in missing2, plus 70 PS whose microcode is
     already in missing2 but with a different container header (other constant table). The 70 are packed too, as a
     full installer build would: the runtime picks the first entry in library order when identifying by microcode,
     so existing entries keep winning on disc maps, and exact-container lookups find the DLC variant.
  5. `translate_all.py` + `compile_spirv_all.sh`: 715 SPIR-V, 2 failed: both new VS (`vs_11b8c972fc24af0a`,
     `vs_b80a5d6fd38298fb`, duplicate `BLENDWEIGHT0` input, the known translator failure class, as in missing2).
     So 645 new PS, 0 new VS, 70 PS variants.
  6. `me_pack_shaders --replace-exact out/shader-missing2/masseffect_shaders.mesp out/shader-dlc/containers
     out/shader-dlc/spirv out/shader-dlc/masseffect_shaders.mesp`: 0 old entries replaced, 30,875 shaders,
     950,986,884 bytes, index 49,827,496 bytes, all packed ones found. `validate_shader_package.sh`: 30,875 shaders
     (277 vertex, 30,598 pixel), `invalid=0`; `test_shader_index`: OK.
  Kept: `out/shader-dlc/{scan,containers,spirv,rejected_false_positives}` and `translate_failed.txt` (it lists the
  false positives too).
* **Duplicate `BLENDWEIGHT0` fixed, package `out/shader-dlc-vs/masseffect_shaders.mesp` + `.idx`** (2026-10-08,
  game data, local only; supersedes `out/shader-dlc`, which stays as the rollback).
  * Cause: four UE3 terrain vertex shaders (`InvTerrainSize`, `InvMaxTesselationLevel`) list two vertex elements
    with the same usage+index, BLENDWEIGHT0, fetched into different registers (both read `.x`). XenosRecomp
    declared one `main()` input per element, so the HLSL had `iBlendWeight0 : BLENDWEIGHT0` twice and DXC failed
    with "redefinition of parameter". Affected: base game `vs_664739fc5097d095` (loaded hash `093FB7223F611ADC`),
    `vs_e17c82a1e9171000` (`2DD35B45B15E59FA`); BDtS `vs_11b8c972fc24af0a`, `vs_b80a5d6fd38298fb`.
  * Fix (`shaders/XenosRecomp/shader_recompiler.cpp`, vertex input declaration): an element whose usage+index is
    already declared gets no second input; its fetch address still maps to the element, so every fetch reads the
    one input. This matches D3D (a declaration element is bound by usage+index, so both fetches are patched to the
    same stream, offset and format) and the native renderer (`ComputeEntry` binds the first element at
    `LocationOfUsage` and skips the second, warning 22). Both fetches only read `.x`, so the first fetch's
    `g_InputRemap` is right for both. All 277 vertex shaders of `out/shader-dlc` translate to byte-identical HLSL
    with the old and the new translator. The installer's WebAssembly translator compiles the same file
    (`shaders/wasm/build_wasm_tools.sh`) and needs a rebuild to pick it up.
  * Build: the two base-game containers were rescanned from the 290 packages of
    `out/shader-missing2/scanned_packages.txt` (same 6,672 distinct containers); `translate_all.py` and
    `compile_spirv_all.sh`: 4 SPIR-V, 0 failed; `me_pack_shaders --replace-exact out/shader-dlc/masseffect_shaders.mesp
    out/shader-dlc-vs/containers out/shader-dlc-vs/spirv out/shader-dlc-vs/masseffect_shaders.mesp`: 4 added,
    0 replaced, 30,879 shaders (281 vertex, 30,598 pixel), 951,093,188 bytes, index 49,832,888 bytes;
    `validate_shader_package.sh`: `invalid=0`; `test_shader_index` (with `--other` the old index): OK.
  * The X57 console drops (`VS 83D232C56BD49D57`, 129 words, and `VS 0D0AB386D5018592`, 177 words, in
    `mass-effect-recomp/run/me1/x57/console.log`) are **not** these shaders. Their microcode matches, outside the
    fetch words, package entries that were already in `out/shader-dlc` (container XXH3 `d85a6766b65e299a` and
    `47960bda7104b5b9`; scanner names `vs_f2c316e557a97433` and `vs_22d4da7b37678e48`, present in both the base
    game and the BDtS scan). So the draws are dropped by runtime identification, not by a missing package entry.
    `ShadersNative::Identify` keeps the fetch destination swizzle (`kFetchKeeps[1]` = `0x80000FFF`), and the ring
    words carry D3D-patched swizzles (position `688` -> `A88`, texcoord5-7 `E88` -> `E0A`), so the exact
    microcode match fails; the address pairing then offered a wrong candidate (`VS identity mismatch ...
    selected_container=47960BDA7104B5B9` for the 129-word program) and the guard refused it.
  * **Fixed in the app (2026-10-08, not yet run on the console): second-stage VS lookup**, cvar
    `masseffect_native_vs_identify_patched` (bool, default true, init-only). Why the remap-tolerant paths did not
    pick them: the draw-time guard (`VertexShaderIdentityMatches`) already accepts representable swizzle patches,
    and `ComputeEntry` already turns them into `g_InputRemap` codes (the `C6: ... original swizzle 688, patched A88`
    lines), but a candidate has to be offered first. `ByCode` (exact hash) and `ShadersNative::Identify` (exact
    masked microcode, swizzle kept) both missed, and the only other offer was the address/object pairing, which
    pointed at another program. Now `Identify`, when the exact stage finds nothing for a vertex shader, scans the
    same-length library entries again with the guard's own test at ALU tolerance 0: every ALU/CF word identical,
    the declared fetch words identical under the loader masks (opcode, registers, predicate), the swizzle
    excluded but required to be representable by the input remap. Since it is the guard's predicate, an entry found
    here is never refused at draw time; several candidates keep the first in library order (as the exact stage).
    Checked offline with the real header code against the 281 VS of `out/shader-dlc-vs`: `83D232C56BD49D57` ->
    `d85a6766b65e299a` (only position `688 -> A88` differs), `0D0AB386D5018592` -> `47960bda7104b5b9` (position
    `688 -> A88`, texcoord5-7 `E88 -> E0A`), one candidate each. Base-game logs under `run/me1/**`: the same fix finds
    `30458CCA865ABD51` (192 words, Normandy and Wards, `ab/g3d_normandy`, `ab/g3d_wards`) -> `bdec8003b46754c7`, and,
    with the fetch permutation below, `CD057930742AFE84` (120 words, Eden Prime, 2524 dropped draws in
    `ab/g3d_eden`) -> `894453492fbd1d03`.
  * Fetch permutation, cvar `masseffect_native_vs_fetch_permutation` (bool, default true, init-only): in
    `CD057930742AFE84` Direct3D also swapped two back-to-back fetches into the same register (instructions 11 and
    12 write `r3.zw` and `r3.xy` in the library, `r3.xy` and `r3.zw` in the ring; all other words identical).
    `VertexShaderFetchPermutationMatches` (`app/src/native/me_shader_identity.h`) accepts exactly that: a run of
    contiguous declared fetches with the same opcode/source/destination register, non-zero pairwise disjoint written
    masks in both programs, one exec clause holding the whole run and no clause holding only part of it (nothing
    runs between them), each library fetch paired with the loaded fetch of the same written mask through a
    representable swizzle, every ALU/CF word identical. The draw guard (`VertexShaderProgramMatches`) and the
    second-stage lookup use it; `ComputeEntry` then takes each element's fetch from its own run
    (`SelectVertexFetchPermuted`, never a fetch outside the run, so a register reused later cannot be picked).
  * Counter: every 10 s report, when anything was found, `[native] VS patched-fetch lookup: N draws rescued (drawn
    with a VS only that lookup found; cumulative); M distinct programs found (P with same-register fetches
    reordered, A with several candidates)`, plus one line per program (at most 48):
    `[native] shaders: VS <hash> (<n> words) identified by the patched-fetch lookup as n<k> (container <xxh3>)...;
    swizzles library->loaded: position0@5:688->A88 ...`.
  * Console check (X57, same package `out/shader-dlc-vs`, `masseffect_diag_missing_shader_draws = true`): no
    `missing shader draw: VS 83D232C56BD49D57`/`0D0AB386D5018592` lines and neither hash in `unresolved draw shader
    pairs`; the two `identified by the patched-fetch lookup` lines (containers `D85A6766B65E299A`,
    `47960BDA7104B5B9`); `C6: VS n... position0 (format ...): original swizzle 688, patched A88, remap ...` and for the
    177-word program texcoord5-7 `original swizzle E88, patched E0A`; the rescued-draw counter growing; no
    `FINAL VS unproven`. A/B: `masseffect_native_vs_identify_patched = false` brings the drops back.
* A new library invalidates the pipeline prewarm list once (it is keyed by fingerprints); recapture it after a DLC run
  if cold-start hitches matter.

### Read speed (follow-up)

Extracted DLC is mounted as a read-write `HostPathDevice` (as saves are), which skips the read-only fast paths of
the game root (read windows, range/block cache, negative lookups, VFS index). DLC map loads will therefore be slower
than disc maps. Mounting Marketplace content read-only would enable them, but first: the VFS index would be written
to `.../00000002/cache/vfs_index_<package>.bin` (a folder that `ListContent` would then report as a package), and a
set `vfs_path_index` would make every read-only device share one index file. Both need handling before that change.

### Other

* Russian edition: the DLC text will be English (no `_RU` talk table; the game falls back to the default
  `GlobalTalkTable1`). A Russian DLC talk table would be a separate translation project. The Switch text rewrites
  (`me_switch_text*`) do not cover DLC string refs either, so DLC text may still say "Xbox"/"Xbox LIVE".

## 5. Game-side notes

* Galaxy map entries, journal, treasure, talents and music come from the DLC 2DA merges and plot-manager maps in
  AutoLoad.ini; nothing in our code needs to know about them once the package is mounted.
* Saves: a save made on a DLC map (or with DLC plot state) records the DLC; loading it without the DLC mounted gives
  message 154280. Turning `dlc_enable` off after playing DLC content can therefore lock those saves out until it is
  turned back on. Saves made before installing DLC load normally.
* `XamContentGetLicenseMask` (title license, `license_mask` cvar) is separate from the per-package license and is not
  involved.
