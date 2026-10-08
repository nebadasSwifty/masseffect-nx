# Installer page

A static web page (plain HTML, CSS and ES modules, no build step, no CDN) that builds the SD card package of the
port from the user's own Mass Effect disc. It is meant for GitHub Pages. **Everything runs in the browser: the game
files are never uploaded.** The only downloads are the page itself, the build (`masseffect-nx.nro`),
`masseffect.toml` and the edition's pipeline prewarm list (`masseffect_prewarm_list-<edition>.bin`), all from the
same site.

The user picks **Disc image (.iso)** or **XEX format** (the extracted disc folder), the page detects the edition and
creates either

* `masseffect-nx.zip` (full install): `masseffect-nx/{masseffect-nx.nro, masseffect.toml,
  masseffect_prewarm_list.bin, masseffect_shaders.mesp, masseffect_shaders.mesp.idx, game_root/...}`, to be extracted
  into `sdmc:/switch/`, or
* `masseffect-nx-update.zip` (update): the same without `game_root/`, to be extracted over an existing install. It
  overwrites `masseffect.toml` and `masseffect_prewarm_list.bin`. The disc is still needed, because the shaders are
  made from it.

Optionally the user adds Xbox 360 **DLC packages** (Bring Down the Sky, Pinnacle Station: STFS files of title
`4D5307E8`, content type `00000002`). Both zips then also contain
`masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/<package>/...` and
`masseffect-nx/masseffect/0000000000000000/4D5307E8/Headers/00000002/<package>.header`, the shader package includes
the DLC shaders, and `masseffect.toml` gets `dlc_enable = true`. See [../docs/dlc.md](../docs/dlc.md) section 4.

Optionally (step 4, "Installable NSP", experimental) the page writes an **installable NSP** instead of the zip: the
whole game as one application, or a small **update** for an NSP installed earlier. See "Installable NSP" below and
[../docs/full-nsp.md](../docs/full-nsp.md).

## How it works

```
 source (ISO or folder) --> file list [{path, size, lazy Blob}]            js/xdvdfs.js, js/source.js
        |  two ISOs --> merged per file (best copy, see below)               js/source.js, js/pkgcheck.js
        |  *.xxx/upk/u/sfm --> structural check, warning if bad/swapped      js/pkgcheck.js
        |  default.xex  --> SHA-256 --> edition table (config.js)           js/plan.js
 DLC packages (optional) --> STFS file list [{path, size, lazy Blob}] + .header, SHA-1 checked   js/stfs.js
        v
 1 download   releases/<nro> + masseffect.toml + releases/<prewarm list> (optional), checked against manifest.json
 2 scan       *.xxx packages -> shader containers        scan workers  (wasm/scan.mjs)
 3 translate  container -> HLSL -> SPIR-V                shader workers (wasm/hlsl.mjs + wasm/dxc_web.mjs)
 4 pack       (container, SPIR-V) pairs -> .mesp + .idx  pack worker   (wasm/pack.mjs)
 5 zip        nro, toml, list, .mesp, .idx, game_root/...  streaming ZIP64 "store" writer -> disk js/zip.js, js/sink.js
```

* `shaders/README.md` describes the pipeline, the container, the package and the index. The page runs the same three
  command line programs that the native build uses (compiled with Emscripten by `shaders/wasm/*.sh`) by writing their
  inputs into the module's in-memory file system and calling `callMain([...])`, exactly as the native command lines.
* **Workers.** Scanning uses `limits.scanWorkers` workers (one package at a time each). Translating uses
  `min(hardwareConcurrency - 1, limits.maxShaderWorkers)` workers, each with its own translator and DXC instance, one
  container per job. SPIR-V goes straight into the pack worker as it is produced, so the page never holds the package.
* **Skipping.** A container that fails is skipped, never fatal: the 56 containers that crash the translator
  (a WebAssembly trap: the worker drops that instance and creates a new one), the 4 that DXC rejects, and any job that
  gets no answer within `limits.jobTimeoutMs` (the worker is terminated and replaced). Skips are listed in the
  Details log. A complete disc gives 30,191 containers and 30,131 shaders; the page warns (but does not stop) if far
  fewer come out.
* **Progress and cancel.** Five stages with their own bars, each with a percent and an estimate of the time left
  (from the rate since the stage's first report; the packing stage reports no steps and shows an animated bar).
  A step indicator above the cards shows each of the four steps as pending, active, done, error or optional. Cancel
  terminates every worker and aborts the output file (the half-written zip is discarded).
* **Page look.** `style.css` only; the banner (`assets/banner.png`) is the visible title, the `h1` is for screen
  readers. The fonts (Russo One, Golos Text, SIL OFL 1.1) are served from `assets/fonts/` with their licenses, so the
  page makes no third-party requests. All texts are in `js/i18n.js` (English and Russian); `index.html` uses
  `data-i18n`, `data-i18n-html`, `data-i18n-alt` and `data-i18n-aria`. After changing `style.css`, `js/app.js` or
  `js/i18n.js`, bump the `?v=` tags in `index.html` and `js/app.js`.
* **Reading the disc.** `js/xdvdfs.js` reads the XDVDFS volume descriptor, then each directory table, then files, all
  with `Blob.slice()`: a 7 GB image is never loaded whole. It looks for the game partition at offsets 0, `0x0FD90000`,
  `0x02080000` and `0x18300000` (what the xiso tools probe: plain, XGD2, XGD3, XGD1). Names are checked (no `..`, no
  separators). Folders come from `showDirectoryPicker()` where available, otherwise `<input webkitdirectory>`, or a
  drop. A folder pick may be the disc root or its parent: the shallowest `default.xex` marks the root.
* **Two discs and the package check.** `js/pkgcheck.js` is a port of `tools/check_packages.py` (same problem
  texts; `test/pkgcheck.test.js` compares both on synthetic packages, and on a real folder with
  `MASSEFFECT_TEST_PACKAGES=<game_root>`; on the EN disc and the RU `game_root`, 2194 packages each, the results are
  identical). It reads only the package summary (4 KB, 64 KB when the summary is longer) and the 16-byte header and
  block table of every compressed chunk, with `Blob.slice()`, so it works on ISO entries and folder files alike (all
  ~2200 packages of a disc take well under a second from a local disk). A package is bad when its summary does not
  parse, a table offset lies outside it, or a chunk or one of its blocks lies past the end of the file; two packages
  of one game root (`Layer0`, `Layer1`) with the same GUID under different names are a copy of another map.
  * **Several ISOs** (the RU two-disc repack) are sorted by file name and merged per path. Every copy of a path
    present on several discs gets a rank: package passes and its GUID is not used by another valid package name on
    that disc (3), passes but shares its GUID with another name (2, a copy of another map), has the tag but fails the
    check (1), no tag (0); Bink movies 3 with the `BIK` tag, 0 without. The highest rank wins, the earliest disc on a
    tie. This takes the Feros `BIOA_WAR00` from Disc 1 (Disc 2's copy is the Ilos map cut short) and the Ilos
    `BIOA_LOS00` from Disc 2 (Disc 1's is a valid Feros copy with WAR00's GUID, which a tag check could not see).
    Every decision for a package or movie is written to the Details log of a run (`<path>: <disc> (why) over
    <disc> (why)`).
  * **Every source** (folder, one ISO, merged ISOs) then gets the check on all packages (`disc.packageCheck.scope`
    in `config.js`, `'maps'` limits it to `Maps/*.xxx`). Bad or same-GUID packages are listed in a warning in step 2
    with advice (choose both disc images together; for merged discs: re-get the images) and in the Details log.
    It does not block unless `disc.packageCheck.block` is `true`.
* **DLC packages.** `js/stfs.js` is a port of `tools/stfs_extract.py`: it reads the STFS header, the file table and
  the hash tables with `Blob.slice()` and returns each file as a Blob made of slices of the package (fragmented files
  follow the hash chain). Packages of another title or content type, SVOD packages and damaged blocks (SHA-1) are
  rejected when they are added. Their `*.xxx` files join the shader scan; the shader package must stay at or below
  1 GiB (`limits.maxShaderPackageBytes`, the runtime's limit; base + both DLCs is about 951 MB).
* **The zip.** `js/zip.js` writes "store" entries with data descriptors (the CRC is only known after the bytes are
  out) and uses ZIP64 fields and end records whenever a size, an offset, the central directory or the entry count
  needs them. Output goes, in order of preference, to `showSaveFilePicker` (Chromium: straight to the chosen file, no
  buffering), to a temporary file in the origin-private file system and then a download (Firefox, Safari: needs free
  disk space but not memory), or to a Blob in memory (last resort; the page warns that several GB will most likely not
  fit). The file picker needs the click's user activation, so the page asks for the target before it starts working.

## Installable NSP

`js/nsp.js` is a JavaScript port of `tools/build_full_nsp.py` (byte-identical output for the same keys; tested against
it), `js/nsp_crypto.js` has the cryptography WebCrypto lacks (our own AES-128 for the XTS headers and the ECB key area,
an incremental SHA-256, a deterministic RSA-PSS signer for tests), `js/nsp_sink.js` the outputs. `run()` in
`js/pipeline.js` takes `output: 'nsp'` and an `nsp` option; the stages are `stagesFor(output, nsp)`.

```
 full NSP             download, scan, translate, pack,   nsp_hash (pass 1: hashes), nsp_write (pass 2: AES-CTR + write)
 update (with data)   download, nsp_base, scan, translate, pack, nsp_hash, nsp_write
 update (program)     download, nsp_base, nsp_write          (no shaders, no disc reads; ~ the NRO's size)
```

* **Keys.** The user picks `prod.keys`; only `header_key` and `key_area_key_application_00` are kept, in memory, and
  overwritten on `pagehide`. Never uploaded, stored or logged.
* **Per-edition identity.** Each edition in `config.js` has `nsp: { titleId, dataDir }`: English
  `01a5eec700020000` + `sdmc:/switch/masseffect-nx-en`, Russian `01a5eec700010000` + `sdmc:/switch/masseffect-nx`
  (table in `docs/full-nsp.md` section 1), so both editions install side by side. The page passes the detected
  edition's values to the packer.
* **Full NSP.** The RomFS holds exactly the files of the full zip (toml, shader package and index, `game_root/`, DLC)
  plus the marker (with the edition's data folder). The disc is read twice; nothing large is kept in memory. Output: one `.nsp` via
  `showSaveFilePicker` (positioned writes: the header is written last, over a placeholder) or a FAT32 split folder via
  `showDirectoryPicker` (`masseffect-nx.nsp/00, 01, ...`, 0xFFFF0000-byte parts). Then the page offers
  `masseffect-nx.nsp.basemeta.json` (offsets and hashes only): it is written next to a split folder automatically,
  otherwise a button saves it.
* **Update.** Base = that `.basemeta.json`, or the base NSP itself (one file or all parts 00, 01, ...; read once with
  the keys, a few minutes for 8 GB). The update number defaults to one more than `last_update_version` in the metadata
  or than the last update made for that base in this browser (`localStorage`: title + Program NCA -> number); after an
  update the page offers the metadata again with the new `last_update_version`. The update keeps the base's own title
  ID and data folder; a base of the other edition is refused before any work, a base with an ID of no edition only
  gives a warning. Output
  `masseffect-nx-update-vN.nsp` to a chosen file, or in memory + download where there is no file picker. By default a
  program-only update; the checkbox "Also rebuild the shaders and settings from the disc" makes a full update (only
  changed 64 KiB chunks are stored).
* **Browser support.** Full NSP: desktop Chrome, Edge, Opera (File System Access API). Firefox and Safari: the full
  NSP button stays disabled with an explanation (use the zip and `tools/build_full_nsp.py`); updates work (memory).
* **Installing.** The page explains DBI (USB/MTP install recommended) and Goldleaf, the 4 GB FAT32 limit, the split
  folder and its archive bit. Signature patches are needed, as for the launcher NSP.
* **Speed** (Chromium, M-series Mac, data in memory): pass 1 ~890 MB/s, pass 2 ~260 MB/s (the JS SHA-256 of the
  whole NCA); for 8.7 GB roughly 45 s of CPU plus the reads and the disk write.

## Which disc files go to `game_root/`

Everything, except the entries of `config.js` -> `disc.skip`:

| Left out | Size on the supported disc | Why |
|---|---|---|
| `$SystemUpdate/` | 12 MB | The Xbox 360 system update the console installs from the disc. It is not game data and the game does not open it. |
| `FillerFiles/` (`*.jnk`) | 446 MB, 1,654 files | Padding the disc layout uses; no content. |
| `nxeart` | 1.7 MB | The disc's artwork for the Xbox 360 home screen. |

Everything else is copied, including `default.xex`, `Layer0/` and `Layer1/` (Unreal packages `*.xxx`, `ISACT` audio,
`MEInit` with `Coalesced.ini`, and **all** `Movies/*.bik`: the game reads the logo, loading and attract movies, and
the startup movies are part of the original `Coalesced.ini`) and `Layer1/Splash.bmp`. `Coalesced.ini` is copied
unmodified: the game checks it against a SHA-1 stored in the executable. The full zip is therefore about 7.8 GB for the
supported edition (7.29 GB disc minus 0.46 GB left out = 6.8 GB of game files, plus the shader package of about
0.92 GB, its 0.05 GB index and the build); the update zip is about 1 GB. No file is ever skipped by size or by guess; to include one of the three again, delete its line
in `config.js`.

**Not verified:** the three exclusions are based on what the files are (their role on the supported disc), not on a run of
the game on a console with those files absent. If a game ever fails to find one of them, remove the rule.

## Where the NRO comes from (CORS)

The browser has to fetch the build, and GitHub's release asset URLs cannot be fetched from another origin. Probed on
2026-10-03 with `curl` and an `Origin` header against a public release of another project:

* `https://github.com/<owner>/<repo>/releases/download/<tag>/<file>` answers `302` **without**
  `Access-Control-Allow-Origin`: a browser `fetch()` fails.
* `https://api.github.com/repos/<owner>/<repo>/releases/assets/<id>` with `Accept: application/octet-stream` does send
  `Access-Control-Allow-Origin: *`, but the redirect it returns (an Azure blob URL on
  `release-assets.githubusercontent.com`) answers `200` with no CORS header either, so the final response is blocked.
  That endpoint is also rate-limited (60 requests per hour per IP without a token).

So the page uses a **copy of the release assets hosted next to the page**. `.github/workflows/installer.yml` downloads
`*.nro` from the latest release, `installer/tools/stage_site.sh` copies them to `releases/` and writes
`releases/manifest.json` (`{tag, assets: {name: {size, sha256}}}`, also for `masseffect.toml`); the page checks size
and SHA-256 against it. Publish a release, and the workflow (it also runs on `release: published`) redeploys the site.
Mind the Pages limits (1 GB site, 100 MB per file); an NRO is well below that.

`masseffect.toml` is copied into the site from `app/masseffect.toml` at deploy time.

**Shipped pipeline prewarm list.** Each edition has its own list (`editions[].prewarmList` in `config.js`:
`masseffect_prewarm_list-en.bin`, `masseffect_prewarm_list-ru.bin`), because it holds fingerprints of that edition's own
shader package. The sources live in `app/prewarm/` (pipeline keys, vertex formats, shader fingerprints: our data, no
game data, no compiled code); `.github/workflows/build.yml` and `tools/publish_release.sh` attach them to the release,
`installer.yml` downloads them with the NROs and `stage_site.sh` copies them to `releases/` and into `manifest.json`.
The page installs the edition's list next to the NRO as `masseffect_prewarm_list.bin` (`files.prewarmList`, the name
`masseffect.toml` gives in `masseffect_native_pipelines_shipped_list`), in the full zip, the update zip and the NSP
RomFS. It is checked against the manifest like the NRO. A release without it (not in the manifest, or a 404 without a
manifest) or a file that does not start with `NFPL` is left out with a warning in the log; the game then only logs
`shipped list ... not found`. Regenerate the list per edition after a shader package or a pipeline key/list version
(`kVersionPipelinesList`) change: `tools/extract_prewarm_list.py` (docs/cold-start-hitches.md, section C).

## Hosting (GitHub Pages)

1. Repository settings -> Pages -> Source: **GitHub Actions**.
2. Check `config.js`: `project.repoUrl` and friends point at the real repository.
3. Push to `main` (touching `installer/`, `shaders/wasm|tools|XenosRecomp`, `app/masseffect.toml`) or publish a
   release, or run the workflow by hand. The workflow builds `scan`, `hlsl`, `pack` with Emscripten, tries to build
   `dxc_web` (see below), stages the site with `stage_site.sh` and deploys it.

Local run:

```sh
installer/tools/stage_site.sh /tmp/site --wasm <folder with the wasm outputs> --releases <folder with *.nro>
cd /tmp/site && python3 -m http.server 8000      # http://localhost:8000 (a secure context)
```

The page needs a secure context (`https://` or `localhost`): it hashes `default.xex` with `crypto.subtle`. Opening
`index.html` from `file://` does not work (module workers).

### Building the WebAssembly tools

```sh
# with Emscripten active (emsdk_env.sh or `brew install emscripten`), XXHASH_DIR/FMT_DIR as in shaders/README.md
OUT=installer/wasm shaders/wasm/build_wasm_tools.sh hlsl pack scan      # a few seconds to build
```

`dxc_web.mjs`/`dxc_web.wasm` come from the pinned, SHA-256 verified DXC v2025.1 build
of [StevensND's](https://github.com/StevensND) [NFSMW-NX](https://github.com/StevensND/NFSMW-NX) installer. The mandatory `dxc` job can also rebuild them
from the pinned C++ source by selecting `rebuild_dxc` in a manual run. Deployment loads every module and compiles vertex/pixel shaders to SPIR-V.
A missing compiler blocks deployment rather than publishing an incomplete installer.
See [../docs/releases.md](../docs/releases.md) for the release and Pages setup.

## Changing things

Everything lives in `config.js`:

* **DLC** (`dlc`): accepted title id and content type, the content and header folders inside the zip root, the
  license mask written into each `.header`, the toml key.

* **A new edition:** add an entry to `editions` with the SHA-256 of its `default.xex` and the release asset name of its
  NRO (`nro`), and its own `nsp` title ID and data folder (also in `EDITIONS` of `tools/build_full_nsp.py`; the IDs,
  their `+0x800` patch IDs and `+0x1000` add-on ranges must not overlap, `test/nsp_pipeline.test.js` checks). The NRO
  of every edition must be in the release; the workflow publishes all `*.nro`. An unknown
  `default.xex` shows its hash and an explanation, and cannot be installed.
* **A new release:** publish it on GitHub. Nothing in the page changes.
* **Names** (`zip`, `files`), the **wasm file names** (`wasm`), what is left out of `game_root` (`disc.skip`), the
  repository links (`project`), limits and rough size estimates (`limits`).
* The shader package/index names must stay `files.shaders` and `files.shaders + ".idx"` (the packer derives the index
  name); the page refuses to run otherwise.

## Tests

```sh
node --test installer/test/          # Node 20+
```

* `xdvdfs.test.js`: the parser on synthetic images written by an independent builder (`test/helpers.js`), including
  the three XGD partition offsets, truncated images, loops and unsafe names.
* `zip.test.js`: CRC-32 against zlib, the writer against its own reader and against Python's `zipfile`, the ZIP64 paths
  (forced with a low threshold) and the 70,000-entry end record.
* `plan.test.js`: edition lookup, disc root detection, what is left out of the zip.
* `stfs.test.js`: the STFS reader on synthetic packages written by an independent builder (`test/stfs_builder.js`;
  read-only and read-write volumes, fragmented files, multi-block file tables), rejections, SHA-1 verification, the
  `.header` bytes, and (with `python3`) a byte-for-byte comparison with `tools/stfs_extract.py --verify`.
* `dlc_pipeline.test.js`: `run()` with stand-in workers and DLC: what is scanned, the zip paths and bytes, the
  `dlc_enable = true` toml edit, the 1 GiB shader package refusal; the edition's prewarm list in the full and the
  update zip, its manifest check, and installing without it when the release has none.
* `real-disc.test.js` (optional, read-only; `MASSEFFECT_TEST_ISO`, `MASSEFFECT_TEST_DISC`): the parser on a real image
  against the extracted disc, file by file (names, sizes, bytes of `default.xex` and of files beyond 4 GiB).
* `nsp.test.js`, `nsp_crypto.test.js`: the key file parser and RomFS primitives; AES (FIPS-197), ECB, XTS, CTR,
  SHA-256, RSA-PSS and base64 against Node's OpenSSL.
* `nsp_python.test.js` (needs `python3` with `cryptography`, else skipped): synthetic inputs (`test/nsp_fixtures.js`),
  throwaway keys and RSA key made in the test, `tools/build_full_nsp.py` run deterministically through
  `test/fixtures/nsp_oracle.py` (fixed AES keys, fixed PSS salt): full NSPs (with options, small read blocks, FAT32
  parts), `.basemeta.json`, updates from the metadata and from the base NSP, program-only updates, all byte for byte;
  plus a real pack (WebCrypto RSA key) whose header signature verifies against the NPDM key.
* `nsp_pipeline.test.js`: `run()` with `output: 'nsp'` and stand-in workers (stages, packed files incl. the prewarm
  list, full NSP, updates,
  errors discard the output).
* `e2e.test.js` (optional; `MASSEFFECT_TEST_WASM`, `MASSEFFECT_TEST_DISC`, native `dxc`): the whole pipeline over a few
  real packages with the real scan/hlsl/pack WebAssembly, the installer's worker code and either the real DXC WASM (`MASSEFFECT_TEST_DXC_WASM=1`) or a native DXC behind the
  `dxc_web` interface (`test/fixtures/dxc_native_shim.mjs`).

## Limitations

* The long step is translating about 30,000 shaders. Native, it takes about 6 minutes on a 10-core machine; in
  WebAssembly it will be slower. **The duration of a complete run in a browser has not been measured.**
* Packing holds the whole shader package (about 920 MB) in the 4 GB address space of WebAssembly, plus the in-memory
  copies of its inputs and output. A complete run needs several GB of free memory; **it has not been tried with the full
  30,191 containers**. If it fails, the page says that packing ran out of memory.
* Each translate worker holds a DXC instance (several hundred MB); lower `limits.maxShaderWorkers` on small machines.
* Without `showSaveFilePicker` the zip is first written to temporary browser storage (free disk space about the zip
  size needed, quota permitting), and without that it is built in memory and will fail above a few GB.
* The NSP writer runs on the page's main thread (in 4 MiB steps, so the page stays responsive); a full NSP from the
  browser has not been installed on a console yet (the same bytes from the host packer have).
* Dual-layer images where the game partition is not at one of the four offsets are rejected ("not an Xbox 360 disc
  image").
* The page reads whole packages (up to about 28 MB) into memory, one per scan worker.
