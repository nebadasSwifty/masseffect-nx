# Installer page

A static web page (plain HTML, CSS and ES modules, no build step, no CDN) that builds the SD card package of the
port from the user's own Mass Effect disc. It is meant for GitHub Pages. **Everything runs in the browser: the game
files are never uploaded.** The only downloads are the page itself, the build (`masseffect-nx.nro`) and
`masseffect.toml`, both from the same site.

The user picks **Disc image (.iso)** or **XEX format** (the extracted disc folder), the page detects the edition and
creates either

* `masseffect-nx.zip` (full install): `masseffect-nx/{masseffect-nx.nro, masseffect.toml, masseffect_shaders.mesp,
  masseffect_shaders.mesp.idx, game_root/...}`, to be extracted into `sdmc:/switch/`, or
* `masseffect-nx-update.zip` (update): the same without `game_root/`, to be extracted over an existing install. It
  overwrites `masseffect.toml`. The disc is still needed, because the shaders are made from it.

## How it works

```
 source (ISO or folder) --> file list [{path, size, lazy Blob}]            js/xdvdfs.js, js/source.js
        |  default.xex  --> SHA-256 --> edition table (config.js)           js/plan.js
        v
 1 download   releases/<nro> + masseffect.toml, checked against releases/manifest.json
 2 scan       *.xxx packages -> shader containers        scan workers  (wasm/scan.mjs)
 3 translate  container -> HLSL -> SPIR-V                shader workers (wasm/hlsl.mjs + wasm/dxc_web.mjs)
 4 pack       (container, SPIR-V) pairs -> .mesp + .idx  pack worker   (wasm/pack.mjs)
 5 zip        nro, toml, .mesp, .idx, game_root/...      streaming ZIP64 "store" writer -> disk   js/zip.js, js/sink.js
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
* **Progress and cancel.** Five stages with their own bars. Cancel terminates every worker and aborts the output file
  (the half-written zip is discarded).
* **Reading the disc.** `js/xdvdfs.js` reads the XDVDFS volume descriptor, then each directory table, then files, all
  with `Blob.slice()`: a 7 GB image is never loaded whole. It looks for the game partition at offsets 0, `0x0FD90000`,
  `0x02080000` and `0x18300000` (what the xiso tools probe: plain, XGD2, XGD3, XGD1). Names are checked (no `..`, no
  separators). Folders come from `showDirectoryPicker()` where available, otherwise `<input webkitdirectory>`, or a
  drop. A folder pick may be the disc root or its parent: the shallowest `default.xex` marks the root.
* **The zip.** `js/zip.js` writes "store" entries with data descriptors (the CRC is only known after the bytes are
  out) and uses ZIP64 fields and end records whenever a size, an offset, the central directory or the entry count
  needs them. Output goes, in order of preference, to `showSaveFilePicker` (Chromium: straight to the chosen file, no
  buffering), to a temporary file in the origin-private file system and then a download (Firefox, Safari: needs free
  disk space but not memory), or to a Blob in memory (last resort; the page warns that several GB will most likely not
  fit). The file picker needs the click's user activation, so the page asks for the target before it starts working.

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

* **A new edition:** add an entry to `editions` with the SHA-256 of its `default.xex` and the release asset name of its
  NRO (`nro`). The NRO of every edition must be in the release; the workflow publishes all `*.nro`. An unknown
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
* `real-disc.test.js` (optional, read-only; `MASSEFFECT_TEST_ISO`, `MASSEFFECT_TEST_DISC`): the parser on a real image
  against the extracted disc, file by file (names, sizes, bytes of `default.xex` and of files beyond 4 GiB).
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
* Dual-layer images where the game partition is not at one of the four offsets are rejected ("not an Xbox 360 disc
  image").
* The page reads whole packages (up to about 28 MB) into memory, one per scan worker.
