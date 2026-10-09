# Full NSP: the game installed as one application (experimental)

Status (2026-10-08): the runtime "packaged" mode, a host-side packer (full NSPs and updates) and the same packer in
the browser installer (section 6) exist and are tested on the host; the browser writer produces byte-identical NSPs to
the host packer. A full RU NSP and an update (v2) made by the host packer were installed and run on the console (4.1,
4.2). Browser-made NSPs have not been installed on a console yet. The forwarder NSP and the SD install are unchanged.

Today the port is an NRO plus the user's data on the SD card (`sdmc:/switch/masseffect-nx/`), started from the HOME
menu by a small forwarder NSP (`tools/build_nsp.sh`, title `01a5eec700000000`). A **full NSP** is a real installable
application that contains the program **and** the user's own game data. It is built on the user's computer from the
user's own disc and the user's own console keys, so the project still distributes nothing copyrighted and no keys.

## 1. What is inside

```
masseffect-nx.nsp  (PFS0)
├── <sha256[:16]>.nca         Program NCA (content type 0), AES-CTR, key area encrypted with key_area_key_application_00
│   ├── section 0  ExeFS (PFS0, HierarchicalSha256, 64 KiB blocks)
│   │     main        NSO made from the NRO's segments (uncompressed, segment hashes checked by the loader)
│   │     main.npdm   39-bit address space, application pool, hbloader's permissions, ACID key = our signing key
│   └── section 1  RomFS (IVFC: 6 levels, 16 KiB blocks)
│         masseffect-nx-package.txt          marker + data_dir= (switches the program to packaged mode)
│         masseffect.toml                    the installer's settings file
│         masseffect_prewarm_list.bin        the edition's shipped pipeline prewarm list (when the release has one)
│         masseffect_shaders.mesp, .mesp.idx the user's shader package
│         game_root/...                      the user's disc files (as in the zip)
│         masseffect/0000000000000000/...    DLC packages, if the user added them
├── <sha256[:16]>.nca         Control NCA (content type 2): RomFS with control.nacp + icon_AmericanEnglish.dat
└── <sha256[:16]>.cnmt.nca    Meta NCA (content type 1): PFS0 with Application_<title>.cnmt
                              (records: Program and Control with their SHA-256 and size; patch ID = title + 0x800)
```

Details that matter on the console:

* **NSO from the NRO, not from the ELF.** The NRO's text/ro/data are the ELF's load segments page-padded; the NRO
  header sits in `text[0x10:0x80]`, a gap libnx's crt0 leaves empty, and is cleared. Checked against `elf2nso` on the
  real build: same segment bytes, same build ID, same memory layout (only zero padding differs). So the release NRO is
  the only program input; no ELF and no devkitPro are needed to pack.
* **NPDM.** The forwarder runs the NRO inside hbloader's process, whose NPDM is Forwarder-Mod `hbl.json` patched by
  `tools/build_nsp.sh` (address space type 3 = 39 bits, `force_debug`, application pool, all syscalls, all services,
  handle table 512, 1 MB main thread stack, priority 44). The NSO gets exactly that NPDM, only the program ID differs.
  The Python writer is a port of switch-tools `npdmtool`; its output is byte-identical to `npdmtool`'s for that JSON.
* **Signatures.** The NCA header's first signature (Nintendo's fixed key) and the ACID signature cannot be made by
  anyone else; consoles accept them with the usual signature patches, exactly like the forwarder and every homebrew
  NSP. The second header signature (the one checked against the NPDM) is real: each pack generates a fresh RSA-2048
  key, puts its modulus in the ACID and signs the Program NCA header with RSA-PSS/SHA-256. No ticket and no titlekey:
  the content keys sit in the NCA key area (key generation 0, so only `key_area_key_application_00` is needed).
* **NACP and icon** come from the NRO's assets, patched like `tools/build_nsp.sh` (no account selection, screenshots
  and video on, no account save data). `--name` can rename the title.

### Title ID policy: one application per game edition

Each game edition is its own application with its own writable SD folder, so the English and the Russian NSP install
side by side (and next to the forwarder `01a5eec700000000`):

| Edition (`installer/config.js` id) | `--edition` | Title ID | Update (patch) ID | Data folder (`data_dir`) |
|---|---|---|---|---|
| Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1) (`usa-eur-en-es-pl-rev1`) | `en` | `01a5eec700020000` | `01a5eec700020800` | `sdmc:/switch/masseffect-nx-en` |
| Mass Effect (Russia) (Ru) (`rus-rev0`) | `ru` | `01a5eec700010000` | `01a5eec700010800` | `sdmc:/switch/masseffect-nx` |
| (forwarder, `tools/build_nsp.sh`) | | `01a5eec700000000` | | NRO folder |

The values live in `installer/config.js` (`editions[].nsp`) and `tools/build_full_nsp.py` (`EDITIONS`); a test checks
that no application ID, patch ID (`+0x800`) or add-on range (`+0x1000 .. +0x1FFF`) overlaps another. The RU title ID is
the one the first RU NSPs were installed with; never change a released edition's ID, since an update must carry the
installed title's ID. The data folders match the editions' NRO install folders, so NRO and NSP share saves (note: a
`masseffect.toml` in that folder then overrides the NSP's packaged one, see section 2).

`--edition` sets the defaults of `--title-id` and `--data-dir`; explicit flags still win. Without `--edition` the old
defaults apply (`01a5eec700010000`, `sdmc:/switch/masseffect-nx-nsp`). `--title-id 01a5eec700000000` replaces the
forwarder instead. The low three digits must be `000` (`+0x800` is the update ID, `+0x1000..` the add-on range).

The NRO's NACP has no ID fields set (PresenceGroupId, SaveDataOwnerId, LocalCommunicationId, SeedForPseudoDeviceId and
AddOnContentBaseId are 0: the port keeps no account save data). Should an NRO ever carry them (`nacptool --titleid`),
both packers rewrite every non-zero one to the NSP's title ID (AddOnContentBaseId to ID + 0x1000), so a control.nacp
never names the other edition.

Updates always keep the **base's** title ID. With `--edition` (the installer always passes the selected edition), a
base of the other known edition is refused with an error naming both IDs (it would put one edition's program over the
other's game data); a base with an ID of no edition (made with an explicit `--title-id`) only gives a warning. NSPs
made before per-edition IDs all carry `01a5eec700010000`: an English one of those has to be updated with the Python
packer without `--edition` (no check), or replaced by a new full English NSP.

### Keys

Only two keys of the user's `prod.keys` are read: `header_key` (AES-XTS of the NCA headers) and
`key_area_key_application_00` (AES-ECB of the key area). The packer and the page never print, log, store or upload
them; the page keeps them in memory only and zeroes them on `pagehide`. `titlekek` and master keys are not needed.

## 2. The program in packaged mode

`app/src/me_packaged.{h,cpp}`. Detected once in libnx's `userAppInit` (end of `__appInit`: services and the SD are up,
static constructors not run yet):

1. `envIsNso()` (started by the system loader, not hbloader), and
2. `romfsMountSelf("romfs")` succeeds, and
3. `romfs:/masseffect-nx-package.txt` exists.

Otherwise nothing changes: the NRO under hbloader never gets past step 1, and an NSO without the marker behaves like
before. In packaged mode:

* The writable SD folder is `data_dir=` from the marker (per edition, see the table in section 1; the runtime's fallback
  for a marker without a valid one is `sdmc:/switch/masseffect-nx-nsp`; the packer's `--edition` / `--data-dir` set it). It is created, and a small `masseffect-nx-nsp.txt` is written there. `argv[0]` (the loader
  passes none to applications) is set to that file. Every writable path derives from the executable folder, so saves,
  caches and logs keep working without further changes (`docs/en-console-test.md` lists them).
* `OnConfigurePaths` (`me::packaged::ConfigurePaths`):

| What | NRO mode (unchanged) | Packaged mode |
|---|---|---|
| Game data (VFS `game:` / `d:`) | `<NRO folder>/game_root` | `romfs:/game_root` |
| Config | `<NRO folder>/masseffect.toml` | `<data_dir>/masseffect.toml` if the user put one there, else `romfs:/masseffect.toml` (read-only) |
| Shader package + index | `<NRO folder>/masseffect_shaders.mesp(.idx)` | `romfs:/masseffect_shaders.mesp(.idx)` |
| Shipped prewarm list (`masseffect_native_pipelines_shipped_list`, relative name) | `<NRO folder>/masseffect_prewarm_list.bin` | `<data_dir>/masseffect_prewarm_list.bin` if the user put one there, else `romfs:/masseffect_prewarm_list.bin` (`me::packaged::ShippedFile`); missing = one warning |
| DLC (Marketplace content) | `<NRO folder>/masseffect/0000000000000000/` | `romfs:/masseffect/0000000000000000/` when packed (cvar `content_marketplace_root`), else the SD folder |
| Saves, SDK shader storage | `<NRO folder>/masseffect/` | `<data_dir>/masseffect/` |
| Pipeline cache, cold cache | `<NRO folder>/cache/` | `<data_dir>/cache/` |
| VFS index | `<NRO folder>/cache/vfs_index_game_root.bin` | off (`vfs_index = false`): RomFS tables are in memory |
| Logs, crash files | `<NRO folder>/logs/` | `<data_dir>/logs/` |

  The two cvars are set with command-line priority, so the packaged toml cannot turn them back.
* `romfs:/...` paths are kept as device paths. libstdc++ sees no root directory in `device:/x`, so
  `std::filesystem::absolute` would prefix the current directory; `Runtime::SetupVfs` now leaves device paths alone
  on Switch (`AbsoluteHostRoot` in `sdk/src/system/runtime.cpp`). Rooted SD paths still go through `absolute()`.
* **Own process handle.** The guest memory model calls `svcMapProcessCodeMemory` and friends on its own process.
  hbloader passes a real handle; an NSO has none (`envGetOwnProcessHandle()` is `INVALID_HANDLE`). `Proc()` in
  `sdk/src/core/guest_memory_switch.cpp` now makes one the way nx-hbloader does (send the current-process
  pseudo-handle to itself as a copy handle over a private session), at load time; the pseudo-handle is the fallback.
* **Heap.** Under hbloader the NRO always used hbloader's heap (all application memory minus 2 MB, the NRO image and
  96 MB for automatic recording): libnx ignores `__nx_heap_size` (1 GiB, `runtime_switch.cpp`) when a loader
  overrides the heap, so the 1 GiB cap described in `docs/platform-notes.md` does not apply under hbloader. An NSO would get the 1 GiB, a configuration the game has never run with, so
  `__libnx_initheap` is overridden: libnx's code for the override case (NRO: unchanged), hbloader's computation for
  the NSO case.

Files touched: `app/src/me_packaged.{h,cpp}` (new), `app/src/masseffect_app.h`, `app/src/native/me_native_system.cpp`,
`app/CMakeLists.txt`, `sdk/src/system/runtime.cpp`, `sdk/src/system/xam/content_manager.cpp`,
`sdk/src/core/guest_memory_switch.cpp`. All syntax-checked with the devkitA64 compiler and the build's own flags
(`-fsyntax-only`); not built.

## 3. Host packer: `tools/build_full_nsp.py`

Pure Python 3 + `cryptography` (no devkitPro, no Docker, no downloads):

```
python3 tools/build_full_nsp.py --input <installer output folder> --keys <prod.keys> --output masseffect-nx.nsp \
    [--edition en|ru] [--nro out/nx/masseffect-nx.nro] [--title-id <id>] [--data-dir sdmc:/switch/<folder>] \
    [--name "Mass Effect"] [--no-dlc] [--split] [--emulator-compatible]
```

`--input` is the `masseffect-nx/` folder of the installer zip (or a staging/SD folder: only `masseffect.toml`, the
shader package, `masseffect_prewarm_list.bin`, `game_root/` and `masseffect/0000000000000000/` are taken; saves,
`cache/`, `logs/`, `._*`, `.DS_Store` are not). `--nro` packs another build than the one in the folder. The prewarm
list is optional: without it the packer logs a warning and the NSP has none (cold starts then compile more pipelines
during play). The browser packs the same file (`PREWARM_LIST_NAME` in `js/nsp.js` and in the packer; byte-identical
outputs, `test/nsp_python.test.js`).

How it streams (peak memory: a few tens of MB):

1. Pass 1 reads the RomFS image once and keeps only its 16 KiB block hashes (32 bytes each: 17 MB for 8.6 GB), then
   builds the upper IVFC levels and the master hash. The NCA header and every size are now known.
2. Pass 2 reads the data again, encrypts it (AES-CTR), writes it into the NSP after a placeholder header and hashes it.
3. The Control and Meta NCAs are small; the CNMT gets the Program NCA's hash, and finally the NSP header with the
   NCA names (their hashes) is written over the placeholder (the names have fixed lengths, so the header size is known).

Measured on the Mac (M-series, files in the page cache, output discarded) with the English edition + both DLC:
3,110 files, 8,614,845,467 bytes of payload, Program NCA 8,693,450,752 bytes (8.10 GiB). Pass 1: 5.7 s (1.5 GB/s).
Pass 2 (AES-CTR + SHA-256): 6.1 s (1.4 GB/s). A real run is bound by the disk write of 8.1 GB.

### Tests

* `python3 tests/tools/test_full_nsp.py` (also run by `tests/run_all.sh`): a synthetic installer output and a fake
  NRO, throwaway random keys, then an independent reader takes the NSP apart: header XTS, key area, AES-CTR, PFS0
  hash tables, all IVFC levels, a RomFS lookup through the hash chains as libnx does it, `readdir` order, the marker,
  the NACP patches, the CNMT records, the NSO segment hashes, the NPDM (39-bit, program ID) and the Program NCA's
  second signature against the ACID key. Also: FAT32 split parts, `--no-dlc`, bad key file, missing input, and the
  DebugFlags capability in the console and `--emulator-compatible` layouts.
* `HACTOOL=/path/to/hactool python3 tests/tools/test_full_nsp.py` also runs hactool (ISC, built from
  [SciresM/hactool](https://github.com/SciresM/hactool) with `make`) with `-y` on every NCA: all hashes GOOD, NPDM
  signature GOOD; only the Nintendo fixed-key signature and the ACID signature fail, as expected.
* `npm test` in `installer/` (section 6): the browser port against this packer, byte for byte.

### Emulators (Eden, yuzu): `--emulator-compatible`

The console NSP does not start in Eden v0.2.1 (macOS, 2026-10-08): "Unable to completely parse the kernel metadata
when loading the emulated process" (`Failed to load ROM (Error 32)` = `ResultStatus::ErrorUnableToParseKernelMetadata`
in the log). Cause: the DebugFlags kernel capability. Since 19.0.0 its layout is `allow_debug` (bit 17),
`force_debug_prod` (bit 18), `force_debug` (bit 19); `npdmtool` (and this packer) write `force_debug` to bit 19
(`0x0008FFFF`), which the console needs. Eden/yuzu's `KCapabilities` still has the old layout `allow_debug` (17),
`force_debug` (18), `reserved` (19..31), and `SetDebugFlagsCapability` (src/core/hle/kernel/k_capabilities.cpp)
returns `ResultReservedUsed` for any reserved bit. `KProcess::LoadFromMetadata` then fails and
`AppLoader_DeconstructedRomDirectory::Load` reports error 32. Eden master removed the check on 2026-05-19
(commit 300a646a34, "make HBLoader work"), after v0.2.1. Every other capability is accepted as is: core/priority
(cores 0..2, priorities 28..59), syscall masks 0..0xBF (`NumSupervisorCalls` = 0xC0), program type 1, kernel version
3.0, handle table 512, MapRegion type 1 (KernelTraceBuffer, skipped because KTrace is disabled); the 39-bit address
space and the application pool are also fine.

`--emulator-compatible` changes only that capability: `force_debug` goes to the old bit 18 (`0x0004FFFF`), everything
else is byte-identical. Do not install that NSP on the console (there bit 18 means `force_debug_prod`). Example:

```
python3 tools/build_full_nsp.py --input out/ru_stage --nro ../mass-effect-recomp/run/me1/ru_glob10.nro \
    --keys ~/Downloads/ProdKeys/prod.keys --data-dir sdmc:/switch/masseffect-nx \
    --output out/nsp/masseffect-nx-ru-full-emu.nsp --emulator-compatible
```

Side note: Eden's NCA loader reads a "heap size" as the u64 at NPDM offset 0x28 (inside the 16-byte name field) and
only uses it to size the IPC pointer buffer; for the name "Mass Effect" it reads 0x746365 ("ect"), which is harmless.

## 4. Testing on the console

1. Build the NRO with the changes above (`tools/build_nro.sh`, or `tools/edition.sh en all`).
2. Pack. The input can be the EN staging folder (`out/en_stage`, see `docs/en-console-test.md`) with the new NRO:

   ```
   python3 tools/build_full_nsp.py --input out/en_stage --nro out/nx/masseffect-nx.nro \
       --keys ~/path/to/prod.keys --output /Volumes/<disk with 9 GB free>/masseffect-nx-en.nsp
   ```

   `prod.keys` is your own console's dump (Lockpick_RCM). It can be anywhere: pass it with `--keys` or
   `SWITCH_PROD_KEYS`; the default is `~/.switch/prod.keys`. It must never be inside the repository (`*.keys` is in
   `.gitignore` anyway). The Mac had 13 GiB free when this was written: write the NSP to another disk, or directly to an
   exFAT SD card, or use `--split` for a FAT32 one.
3. Install (DBI recommended: shows errors clearly). The install needs about 8.1 GB on the target storage (SD or NAND)
   and the NSP itself needs no space on the console if it is streamed over USB/MTP.
4. Start "Mass Effect" from the HOME menu. Expect in `<data_dir>/logs/` (the edition's data folder, section 1):
   `[package] installed NSP: read-only data from romfs:/ ...`, `[io] mounted 'romfs:/game_root' ...` with the full file
   count, and `[native] shader library romfs:/masseffect_shaders.mesp`.
5. Measure cold start, loading screens and streaming hitches against the NRO on the same build (both titles can be
   installed side by side). RomFS reads go through the FS service, AES-CTR decryption and IVFC hash verification of
   every 16 KiB block; the SD install reads FAT32 directly. Which is faster is unknown until measured.

If it fails at start, read `<data_dir>/logs/rex/rex_stderr.log` first. It always has the packaged-mode lines (since the
fix below):

```
[package] userAppInit: NSO, argc 0, romfsMountSelf rc 0x0
[package] userAppInit: marker found, data_dir sdmc:/switch/..., anchor stat 0, rooted stat 0 (errno 0), argv[0] ... (set here)
[package] ConfigurePaths: romfs:/game_root directory 1, config '...', DLC in RomFS 1
[package] paths: packaged 1, executable folder '/switch/...', game_data_root 'romfs:/game_root' (directory 1), config '...' (exists 1), user data '...'
```

(an NRO prints `userAppInit: NRO (hbloader), packaged mode off` and its `paths:` line). Then the crash files in the same
folder, then `sdmc:/switch/rex_crash.log`. Remaining suspects: the own-process handle (guest memory mapping errors),
the heap size.

### 4.1 First console run (2026-10-08): "--game_data_root was not provided"

The RU NSP installed and started, created the Vulkan device, then `ConstructRuntime` stopped with
`--game_data_root was not provided`, and the log had no `[package]` line. Cause: the Russian edition replaces
`app/src/masseffect_app.h` with its own copy (`editions/ru/overlay/app/src/masseffect_app.h`, applied by
`tools/edition.sh`), and that copy did not have the packaged-mode hooks. The RU build therefore compiled
`me_packaged.cpp` (userAppInit ran, so `argv[0]` and the logs pointed at the data dir) but never called
`ConfigurePaths` or `LogStatus`. Fixed by:

* the same hooks in the overlay copy (`OnConfigurePaths` and `OnPostInitLogging` are now identical in both files);
* `tools/edition.sh` refusing to stage an edition whose `masseffect_app.h` lacks `me::packaged::ConfigurePaths`,
  `ReportPaths` or `LogStatus`;
* always-on diagnostics in `rex_stderr.log` (above): every step of `userAppInit` (kept in a buffer and written once
  stderr is redirected, constructor priority 150), `ConfigurePaths`, and the final paths with the reason.

Why the overlay's own fallback (`<data_dir>/game_root`, which exists on that SD) did not apply is not known from the
first log alone; the new `paths:` line answers it. The NSP must be rebuilt with the new NRO (the program is inside).


### 4.2 Second run (update v1): title screen, no main menu, defaults instead of the toml

The game started from the RomFS, but the log showed default values (`masseffect_native_textures_mb_max` 512 instead of
the toml's 128, `dlc_enable` off), an empty save folder and a pipeline cache of 0 KB although the SD has all three.

* **The packaged toml did not parse.** `out/ru_stage/masseffect.toml` has `dlc_enable` twice (line 94 `false`, line
  274 `true`): TOML forbids that, toml++ throws, and `cvar::LoadConfig` ignores the whole file. Its error was logged
  before logging starts (`SetupEnvironment` loads the config first), so nothing showed. Fixed: `LoadConfig` also writes
  `[config] loaded <path>` / `[config] FAILED to parse <path> ...` to stderr (`rex_stderr.log`), and the packer refuses
  an invalid `masseffect.toml` (`tomllib`). The romfs toml was used at all because the SD one was not found, next point.
* **SD paths under the NSO.** hbloader starts an NRO after `chdir()` to its folder (libnx `__libnx_init_cwd`), which
  also makes the SD newlib's default device; an NSO never gets that call, and the SD files the game opens through
  rooted paths (`/switch/masseffect-nx/...`) were not found (toml, pipeline cache, saves). Packaged mode now does the
  same `chdir(<data_dir>)` in `userAppInit` and logs the default device before and after it. `ReportPaths` adds a
  `[package] files:` line: default device, cwd, and `stat` of `<data_dir>/masseffect.toml` rooted and with `sdmc:` (they
  must agree).

## 5. Size, FAT32 and how to install

* About 7.3 GB of game data + 0.95 GB of shaders (+ 0.8 GB DLC): the NSP is 8.1-8.9 GB.
* **FAT32 cannot hold a file of 4 GiB or more.** Options, best first:
  1. **Install over USB or the network**, nothing stored on the SD: the installer page's own USB install to Sphaira
     (Chrome/Edge), [Switch USB Installer](https://github.com/nebadasSwifty/switch-usb-installer) for a saved NSP on a
     Mac (Sphaira), DBI (MTP "Install title from computer" or `dbibackend`), Tinfoil/Awoo (USB/network),
     Goldleaf + Quark. Recommended.
  2. **exFAT SD card**: copy the single NSP and install it from the SD.
  3. **Split NSP** (`--split`): a folder `masseffect-nx.nsp/` with parts `00`, `01`, ... of `0xFFFF0000` bytes. The
     folder needs the FAT "archive" attribute (Hekate's archive bit fixer, or `attrib +a` on Windows); macOS cannot
     set it. DBI, Tinfoil and Goldleaf read such folders.
* The installed title lives encrypted in `Nintendo/Contents` (SD) or NAND; the data dir on the SD stays small
  (saves, ~100 MB of caches, logs).

## 6. In the browser (installer)

The installer page builds the same NSPs (step 4, "Installable NSP"): `installer/js/nsp.js` is a JavaScript port of
this packer, module by module, and for the same inputs, the same AES keys and the same RSA-PSS salt it writes the
same bytes (tested against this script, see below). No server, nothing uploaded, no third-party code.

| Piece | Browser implementation |
|---|---|
| Keys | `parseProdKeys` keeps only `header_key` and `key_area_key_application_00`, in memory; the page overwrites them on `pagehide`. Never uploaded, stored or logged; errors name missing keys, never values. |
| Input | The pipeline's entries (`{path, size, chunks()}` in `js/pipeline.js`): disc files are lazy `Blob` slices, the shader package is read back from the pack worker, DLC files are slices of the STFS packages, so every entry can be read twice. The RomFS gets exactly the files of the full zip (paths relative to its `masseffect-nx/` folder) plus the marker. |
| NSO, NPDM, NACP, icon, PFS0, RomFS, IVFC, CNMT, BKTR | Ported line by line (`nsoFromNro`, `buildNpdm`, `patchNacp`, `Romfs`, `IvfcPlan`, `bucketTree`, ...). |
| SHA-256 | `js/nsp_crypto.js`: an incremental JS implementation (the whole-NCA hash, chunks that span read blocks) and WebCrypto `digest` for independent 16/64 KiB blocks (batched per 4 MiB read). |
| AES-CTR | WebCrypto `AES-CTR` (counter = section upper IV ‖ offset/16, `length: 64`), 4 MiB per call. |
| AES-XTS (headers), AES-ECB (key area) | Our own AES-128 block cipher in `js/nsp_crypto.js` (FIPS-197); XTS with Nintendo's big-endian sector tweak. Only 3 KiB per NCA. |
| RSA-PSS | WebCrypto `generateKey({name: 'RSA-PSS', modulusLength: 2048})` (private key not extractable), `sign({saltLength: 32})`, modulus from the public JWK. Tests inject a deterministic signer (`deterministicPssSigner`: fixed salt, BigInt). |
| Output | `js/nsp_sink.js`: `showSaveFilePicker` + positioned writes (placeholder header, stream, then the real header at 0), or a FAT32 split folder (`showDirectoryPicker`: `<name>.nsp/00, 01, ...` of `0xFFFF0000` bytes, part 00 stays open for the header), or memory + download (small outputs only: updates), or the connected console over USB (`UsbNspSink`, WebUSB; see "Install over USB" below). |

Streaming is the packer's: pass 1 reads the RomFS once (IVFC block hashes + 64 KiB file chunk hashes, ~17 MB for
8 GB), pass 2 reads it again, encrypts, hashes and writes. Measured in Chromium (M-series Mac, data in memory): pass 1
~890 MB/s, pass 2 ~260 MB/s (bound by the JS SHA-256 of the whole NCA, which cannot be parallelised): about 45 s of
CPU for 8.7 GB on top of the reads and the disk write.

### What the page offers

* **Full NSP** (after the shader stages, like the zip): one `.nsp` file or a FAT32 split folder. Afterwards the page
  offers `<name>.nsp.basemeta.json` (written next to a split folder automatically, a "Save" button otherwise).
* **Update for an installed NSP**: inputs are the base (its `.basemeta.json`, or the base NSP itself: one file or all
  parts of a split folder, read once with the keys), the release NRO the page downloads anyway, and `prod.keys`.
  Output `masseffect-nx-update-vN.nsp`. By default a **program-only update** (below): no disc reads, no shaders,
  about the size of the NRO. With "Also rebuild the shaders and settings from the disc" it is the full update of
  section 8 (shaders made again, disc read twice, only changed chunks stored).
* **Install over USB** (full NSP or update): the same package, sent to Sphaira's USB install screen instead of a
  file; nothing is written to the computer. Below.
* **Update number**: one more than `last_update_version` in the base metadata, or than the last update made for that
  base in this browser (`localStorage`, keyed by title and Program NCA; only the number), and editable. After an
  update the page offers the base metadata again with `last_update_version` set, so the next update counts on.
  (`last_update_version` is an optional key; the Python packer ignores it.)

### Browser support

| | Full NSP (8-9 GB) | Update NSP |
|---|---|---|
| Chrome, Edge, Opera (desktop) | single file, FAT32 split folder, or installed over USB (WebUSB) | saved to a chosen file, or installed over USB |
| Firefox, Safari | not offered: no page API can write a multi-GB file with positioned writes, and there is no WebUSB; the page says so and points to the zip + this script | built in memory, then downloaded |

The origin-private file system could hold the NSP in Firefox and Safari, but needs the space twice (temporary file
plus the download) and Safari has no `createWritable` there; not used.

### Install over USB (WebUSB to Sphaira)

Goal: install the NSP on the console while it is generated, without writing 8.7 GB to the computer. Pieces:
`installer/js/usb_install.js` (protocol, WebUSB transport), `installer/js/crc32c.js`, `UsbNspSink` in
`installer/js/nsp_sink.js`, `NspImage` in `installer/js/nsp.js`, the UI in `installer/js/app.js`.

**Protocol.** Sphaira's own (the console is the USB device 057E:3000, vendor-specific interface 0 with one bulk IN
and one bulk OUT endpoint; the host only answers). Every packet is 6 little-endian u32: magic `SPH0` (0x53504830),
`arg2..arg5`, CRC-32C of the first 20 bytes.

```
console: hello (arg2 = 0)                      host: result(OK, length of the name table), "name\n" per file
console: cmd 1 open, arg3 = index              host: result(OK, size >> 32 & 0xFFFF | flags << 16, size & 0xFFFFFFFF)
console: range (offset hi, offset lo, length)  host: result(OK, length, CRC-32C of the data), the data
console: range (0, 0)                          host: result(OK): file closed, back to the commands
console: cmd 0 quit                            host: result(OK)
```

It is the protocol of `tools/usb_install.py` in the recomp repository (verified on a console with host-built NSPs).
Sphaira ships a Python host and a WebUSB host of its own (`tools/usb_install.py`, `tools/webusb` in
[ITotalJustice/sphaira](https://github.com/ITotalJustice/sphaira)); we read them and Sphaira's installer
(`sphaira/source/usb/usb_installer.cpp`, `sphaira/include/usb/usb_api.hpp`, `sphaira/source/yati/yati.cpp`,
`sphaira/source/yati/container/nsp.cpp`) for the protocol and the read pattern, and wrote our own code. Credit to
ITotalJustice for Sphaira and its protocol. CRC-32C in JS: slicing-by-8 tables, measured ~2 GB/s in Node 26 on an
M-series Mac (`test/usb.test.js` prints the rate), far above any USB link.

**How Sphaira reads an NSP** (Sphaira 1.0.x, `InstallInternal`, flags 0): the PFS0 header (16 bytes, then the file
table, then the string table), then each `.cnmt.nca` (the Meta NCA, at the end of our NSP), then the NCAs the CNMT
lists, sorted by content type descending (Control, then Program), each one from its start to its end in increasing
reads (0x4000 bytes first, then its read buffer size). Every NCA's SHA-256 must match its name (unless "skip NCA hash
verify" is set), and the console's transfer timeout is 3 s per transfer (`usb_menu.cpp`).

**Choice: (b), a hashing pre-pass, then ranges served by regenerating.** The NSP header holds the NCA names, the
SHA-256 of the encrypted NCAs, and Sphaira reads it first. So the header must be final before the first byte goes
out, and on-demand generation without a pre-pass (option a) cannot work, however sequential the reads are. The
existing passes become three:

1. pass 1 (as before): RomFS hashes (IVFC levels, 64 KiB chunk hashes);
2. pass 2: the Program NCA is encrypted and hashed but not written (~260 MB/s, the JS SHA-256); the Control and Meta
   NCAs and the header are then final and stay in memory (a few hundred KB);
3. pass 3: Sphaira reads; `NspImage.read(offset, length)` answers from memory for the header, the Control and the
   Meta NCA, and makes the Program NCA again from `NcaBuilder.chunks(from)`.

The Program NCA, the 8 GB, is read by Sphaira strictly in order, so pass 3 is one more sequential read of the disc: a
cursor continues for sequential requests, a window of recent blocks (16 MiB) answers re-reads, and any other request
reopens the cursor at the requested offset (rounded down to a 16 KiB IVFC block). Reopening is cheap because every
stream can start at an offset: `NcaBuilder.chunks(from)` (AES-CTR restarts at any 16-byte block),
`IvfcPlan.chunks(data, from)` (hash levels from memory), `Romfs.chunks(from)` (files before the offset are not read),
and the sources (`chunks(from)` on disc file slices, DLC slices and the pack worker's package reads). Patch sections of
updates are not seekable and fall back to reading from the start and dropping the head (they are small or rarely
reopened). The NCA header is made once and kept (its RSA-PSS signature is randomised; every read must see the same
bytes). While the Program NCA is produced in one run from its start, its SHA-256 is compared with pass 2's, so a disc
file that changed or became unreadable between the passes fails the install before the last block reaches the console
(the page sends an error result; Sphaira stops).

**Not used: `FLAG_STREAM`** (open result `flags` bit 0). With it Sphaira installs in file order and skips the NCA hash,
header signature and NPDM checks, so content IDs need not be hashes and the pre-pass could go. But it is meant for
sources that cannot seek (Sphaira-to-Sphaira, compressed archives), it leaves the content ID / SHA-256 relation broken
for anything that checks it later, and it depends on a Sphaira version that has the flag. The pre-pass costs ~35 s of
CPU plus one disc read; the safe path wins.

**Costs.** The disc is read three times (pass 3 at USB speed, so it is not the limit). Memory: the window (16 MiB),
the 4 MiB AES block, the small NCAs. The 8.7 GB NSP is never stored.

**Page flow.** "Install to the connected Switch (USB)" (full NSP) or the update checkbox. The page tells the user to
start Sphaira -> Install -> USB (MTP off) and to connect the cable, then click the button. When the package is ready
the page uses a console this site was allowed before (`navigator.usb.getDevices()`), else shows "Choose the Switch"
(the chooser needs a click; it also continues when an allowed console is plugged in), then waits for Sphaira's hello.
Progress shows GB sent and MB/s, Cancel closes the device (pending transfers fail, Sphaira reports a USB error), a
console that quits before it has read every byte (cancelled, no space, already installed) is an error, and the base
metadata is offered as after a file output.

**Drivers.** macOS: nothing. Windows: Chrome can only open devices bound to WinUSB; if the console is missing from the
chooser, install WinUSB for 057E:3000 once with [Zadig](https://zadig.akeo.ie/) (the driver other USB install tools
use; NS-USBloader/DBI setups often have it already). Linux: the user needs access to the device node (a udev rule for
057E:3000). Another program holding the interface (an open `usb_install.py`, a DBI/NS-USBloader host) makes
`claimInterface` fail; the page says so.

**Needs the console** (not tested yet, the device was busy): a full install from Chrome on macOS; the USB throughput
from the browser; that no regeneration (reopen) is ever slow enough to hit the 3 s transfer timeout (it should not
happen with Sphaira's read order); Windows with WinUSB; Edge; cancelling from both sides; the update over USB; an
install onto the SD card and onto system memory.

### Tests (`npm test` in `installer/`)

* `test/nsp_python.test.js`: synthetic installer outputs (`test/nsp_fixtures.js`: fake NRO, game files, DLC, a UTF-8
  name, junk files that must be skipped), throwaway keys and RSA key generated in the test, and this script run through
  `test/fixtures/nsp_oracle.py` (fixed AES keys per NCA type, fixed PSS salt). Byte-identical: a full NSP and its
  `.basemeta.json`; a full NSP with title ID, name, display version, emulator NPDM, no DLC, version; `--edition en` and
  `ru` (title ID, data folder, the base metadata read back from the NSP, an update without `--data-dir`); an NRO whose
  NACP carries ID fields; the same with
  16/48 KiB read blocks (chunks spanning blocks); FAT32 split parts; an update from the `.basemeta.json`; an update
  from the base NSP (the JS base metadata equals `metadata_from_nsp`'s); program-only updates from either. Also: a pack
  with the WebCrypto key and random AES keys whose header signature verifies against the NPDM's ACID modulus.
  Skipped without `python3` + `cryptography`.
* `test/nsp_crypto.test.js`: AES (FIPS-197 vector), ECB, XTS, CTR, SHA-256, RSA-PSS, base64 against Node's OpenSSL.
* `test/nsp_pipeline.test.js`: `run()` with `output: 'nsp'` and stand-in workers: the stages, the files packed (the
  zip's), a full NSP, program-only updates from the metadata and from a two-part base NSP, an update with game data,
  errors (no keys, no base, not an NSP) discarding the output; per edition: the title ID and data folder of each
  edition (config IDs do not overlap), an update for the other edition's base refused before any work, an unknown base
  title kept with a warning. The USB target: the console gets the same NSP, with sources re-read from inside.
* `test/usb.test.js` with `test/usb_fake_console.js` (a stand-in console reading in Sphaira's order and checking the
  NCA hashes against their names): CRC-32C (check values, bitwise reference, speed), packet bytes (vectors made with
  Python), `serveFiles`, whole installs byte-identical to the file output (out-of-order reads, a tiny window, random
  ranges with seekable and non-seekable sources, a program-only update), a source changing between the passes, a
  console stopping early, and `WebUsbTransport` over a fake `USBDevice`.

## 7. Risks and open questions

* **Console coverage.** Full NSPs and updates made by the host packer install and run (4.1, 4.2). NSPs made in the
  browser are byte-identical to the host packer's for the same keys (tests), but have not been installed yet; the
  program-only update has not been installed yet either.
* **Signature patches** are required (fs for the NCA fixed-key signature, loader for the ACID signature), the same as
  for the forwarder. A console without them refuses to install or start it.
* **RomFS read speed** vs SD (see 4.5). If RomFS is slower, the game data could stay on the SD while the NSP carries
  only program + shaders (`--no-game-data` would be a small change: without `romfs:/game_root` the program already
  uses `<data_dir>/game_root`).
* **Settings.** The packaged toml is read-only; the settings overlay can only save once the user copies a
  `masseffect.toml` into the data dir. A new NSP brings its own toml, but a user copy in the data dir wins and may be
  stale.
* **Updates** (section 8): an update v2 made by the host packer installed and ran on the console. The patch CNMT has
  no extended data (no patch history). A **program-only** update (all RomFS ranges from the base, no patch data) is
  verified on the host only (the virtual section equals the base's byte for byte).
* **Disk space** on the packing machine: the NSP is 8-9 GB.

## 8. Updates (patch NSPs)

A new build should not mean re-installing 8 GB. `--update` makes a Nintendo-style **patch**: title
`<base> + 0x800` (`01a5eec700010800` RU, `01a5eec700020800` EN), CNMT type Patch (0x81, extended header {application ID, required
system version, extended data size 0, reserved}), title version `N * 0x10000`. The system applies it on top of the
installed base like any game update; the base must stay installed.

```
python3 tools/build_full_nsp.py --update --version 1 --input <new installer output> [--nro <new NRO>] \
    --base <base.nsp | base.nsp.basemeta.json> --keys <prod.keys> [--edition en|ru] [--data-dir <folder>] \
    [--display-version 1.1] --output masseffect-nx-update-v1.nsp
```

Version N must grow with every update (N = 1, 2, ...); each update is made against the **base** (not the previous
update), like Nintendo's, so installing v3 over v1 is fine.

**Program-only update** (`--update --program-only --nro <new NRO>`, no `--input`): only the program changes; the RomFS
section is the base's, mapped 1:1 (one indirect entry, storage 0, no patch data: the section is just the two tables,
64 KiB). It needs only the base metadata (or the base NSP), the NRO and the keys, so the browser installer makes it
without the disc. The RomFS keeps the base's `masseffect.toml`, shaders and marker (the runtime reads only `data_dir=`
from the marker); a user `masseffect.toml` in the data dir still overrides the packaged one. The same holds for the
prewarm list: a program-only update keeps the base's `masseffect_prewarm_list.bin` (both packers log a note). A list
whose `kVersionPipelinesList` no longer matches is ignored with a warning (harmless, only the cold-start benefit is
lost); to use the new release's list, copy the edition's `masseffect_prewarm_list-<ed>.bin` from the release into the
data dir as `masseffect_prewarm_list.bin` (it wins over the RomFS copy) or make a normal update. When a release changes
the shaders, the settings or the prewarm list, make a normal update with `--input`: its RomFS carries the new list.

```
python3 tools/build_full_nsp.py --update --program-only --version 2 --nro <new NRO> \
    --base <base.nsp | base.nsp.basemeta.json> --keys <prod.keys> --output masseffect-nx-update-v2.nsp
```

### What is inside

* **Program NCA** (program ID = the application's): a new ExeFS (new `main` + `main.npdm`, signed like a full pack)
  and a RomFS section of type **BKTR** (encryption type 4, AesCtrEx). That section describes the *new* RomFS section
  (IVFC levels + image, byte for byte what a full pack of the new input would contain; the IVFC superblock and master
  hash are the new ones) as a virtual storage made of ranges of the base and ranges stored in the patch:

  ```
  physical: [patch data][indirect table][AES-CTR-EX table]          FS header 0x100: indirect {offset, size, BKTR header}
  virtual:  indirect entries {virtual, physical, storage 0=base/1=patch}      0x120: AES-CTR-EX {offset, size, header}
            -> base Program NCA's RomFS section (decrypted) or patch data    0x140: upper IV {generation = N, secure 0}
  ```

  Both tables are bucket trees with 16 KiB nodes (an L1 node with the entry-set start offsets, then the entry sets);
  indirect entries are 0x14 bytes, AES-CTR-EX entries 0x10 (`{offset, encryption 0, generation}`). One AES-CTR-EX entry
  covers the data and the indirect table (generation N); the AES-CTR-EX table itself uses the FS header's upper IV. All
  boundaries are 16-byte aligned. References: Atmosphère `fssystem` (bucket tree, indirect storage,
  aes_ctr_counter_extended_storage, NCA driver), switchbrew, hactool `bktr.c`; the code is our own.
* **Control NCA**: the new NACP (`--display-version` sets the version string) and icon.
* **Meta NCA**: `Patch_<id>.cnmt` listing both.

### What goes into the patch data

Each file of the new RomFS is compared with the base file of the **same path**, in 64 KiB chunks (SHA-256 of
`[offset + k*64K, min(+64K, offset + align16(size)))`): equal chunks are mapped to the base (wherever the file sits in
the base, so files may move), the others are stored. Always stored: the IVFC hash levels (~16 MB for 8 GB of data),
the RomFS header and tables, and the marker file. So a program-only update is the ExeFS (~60 MB) + ~16 MB; a new
shader package adds the changed chunks of `masseffect_shaders.mesp`; changed game files add their changed chunks.

### What the update needs from the base

Not the base's game bytes, only, per file: its offset in the base RomFS image, its size and the chunk hashes, plus the
base IVFC data offset and title ID. A full pack writes them next to the NSP: `<output>.basemeta.json` (about 6 MB for
the whole game; offsets and hashes only, no keys, no game data; keep it with the base NSP). For a base without it (any
full NSP from this packer, including the first ones), `--base <base.nsp>` computes the same by decrypting the base's
Program NCA with the keys (one read of the base's RomFS). The base's AES key, NCA ID or generation counter are not
needed: storage 0 of the patch is "the base Program NCA's RomFS section, decrypted", which the system resolves itself.

### Verification

* `tests/tools/test_full_nsp.py` (`UpdateNspTests`): a synthetic base, then an update that changes one 64 KiB chunk
  of a 330 KB file, the toml and the program, adds a file and removes one. The independent reader rebuilds the
  virtual section from the base NCA + the patch's tables and checks it is **byte-identical to a full pack of the new
  input**, that every IVFC level verifies against the patch's master hash, that the unchanged chunks come from the
  base, and the Patch CNMT. The update made from `--base base.nsp` has the same tables as the one made from the
  metadata file. With `HACTOOL` set, hactool applies the patch (`--basenca`, `-y`): all IVFC levels GOOD, and its saved
  RomFS equals the full pack's.
* `test_program_only_update`: the virtual section of a program-only update equals the base's RomFS section byte for
  byte, nothing is stored in the patch, the ExeFS has the new NSO, the CNMT is a Patch of version N * 0x10000, and the
  same section header comes from `--base base.nsp` and from the metadata file.
* Real data (2026-10-08): base `out/nsp/masseffect-nx-ru-full.nsp` (8.68 GB, made by the first version of the packer),
  input `out/ru_stage` + the same NRO, version 1: update NSP 75.1 MB (3 ranges: 16.3 MB stored, 8.0 GiB from the base),
  packed in 16 s. A streaming check (no extraction) read base + update as the console would: all IVFC levels of the
  virtual section GOOD, and the virtual RomFS image equal to the image of `out/ru_stage` byte for byte (8,604,521,660
  bytes). The test update was deleted afterwards.

### Console notes

* Install the update with DBI/Tinfoil/Goldleaf like any update; the HOME menu shows the new version.
* The data dir must be the same as the base's: without `--data-dir` an update takes the base's (recorded in the
  `.basemeta.json`, or read from the marker in the base NSP's RomFS); a different `--data-dir` gives a warning (saves,
  caches and logs would move).
* A 16 B change in the RomFS layout (here: the marker file grew) shifts every later file, which is why files are
  mapped by path, not by position.
