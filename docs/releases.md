# Releases and GitHub Pages

The public repository contains source and artwork. Game files, generated game code,
console keys and generated game shaders are excluded from Git.

## Release an NRO built on your computer

```sh
MASSEFFECT_VERSION=0.1.0 tools/build_nro.sh
# Commit and push all source changes first.
tools/publish_release.sh v0.1.0
```

This creates a tagged draft release and uploads `masseffect-nx.nro`. **Release NRO
and NSP** runs on a GitHub Ubuntu runner, compiles the pinned open-source forwarder
and hacBrewPack, and produces:

- `masseffect-nx.nro`, with the selected Mass Effect cover.
- `masseffect-nx-forwarder.nsp`, title ID `01a5eec700000000`, 39-bit application mode.
- `masseffect-nx-starter.zip`, extracted into the SD card root.
- `masseffect.toml` and `SHA256SUMS`.

The workflow publishes a normal release marked Latest by default (the manual
`prerelease` option is available for preview builds). It publishes the draft only
after packaging succeeds, then explicitly
dispatches **Installer page**. An Actions-created release does not by itself trigger
another workflow when using `GITHUB_TOKEN`.

Configure the encrypted Actions secret `SWITCH_PROD_KEYS` with your own `prod.keys`.
The packer uses `header_key` and `key_area_key_application_00`. The secret is written
to a restricted temporary file, mounted read-only, and removed even after failure.
Keys never enter the release artifacts. Only the two required key lines need to be
stored in the secret. Packing also runs when a release is published manually in
GitHub: attach `masseffect-nx.nro` to the draft before publishing it. Use the helper
above to keep a release in draft until every artifact succeeds. Locally:

```sh
SWITCH_PROD_KEYS=/path/to/prod.keys tools/build_nsp.sh
```

The NSP is a HOME menu forwarder, not a container for the Xbox game. It loads
`sdmc:/switch/masseffect-nx/masseffect-nx.nro`. First prepare the game and shader
files with the browser installer. Installing only the NSP cannot run the game.
Actual installation and launch on a Switch need a console test.

## Build everything through Actions

**Build Switch NRO** is manually dispatched with a new `vX.Y.Z` tag. It requires a
private self-hosted runner labelled `masseffect-build`, with Docker, Git, Python 3.11+,
CMake, Ninja, Clang and the dependencies listed in `docs/building.md`.

Set repository variables:

- `MASSEFFECT_XEX`: absolute path to your supported `default.xex` outside the runner checkout.
- `MESA_SDK`: absolute path to the SDK built by `mesa/build_mesa_docker.sh`.

The workflow validates the edition hash, builds the host generator, runs codegen,
builds the NRO, creates a tagged draft and dispatches the same NSP release workflow.
A standard GitHub runner cannot build the game executable from this public checkout
alone: its copyrighted `default.xex` is deliberately absent. Do not enable public PR
jobs on the private runner. Regular CI runs on GitHub-hosted runners.

## Installer hosting

Enable **Settings → Pages → Source: GitHub Actions**. The site is hosted at
https://nebadasswifty.github.io/masseffect-nx/.

**Installer page** compiles scan/hlsl/pack with Emscripten, fetches the SHA-256 verified
DXC v2025.1 WASM from the pinned reference installer commit, runs real WASM
vertex/pixel shader smoke tests, and stages the release NRO, NSP and matching settings
on the same origin. SHA-256 hashes and sizes are recorded in `releases/manifest.json`.
The NSP can be downloaded directly from the installer. The game stays on the user's
computer; all extraction and shader compilation happen inside the browser.

A missing WASM module or release asset fails deployment and keeps the last successful
site available. Manual deployment accepts `release_tag`; otherwise the newest
published release is selected, including prereleases. DXC is cached by compiler
commit, Emscripten version and wrapper sources. The manual `rebuild_dxc` option
compiles DXC from its C++ sources instead. The reference compiler was also tested
on real Mass Effect packages through the complete scan/translate/compile/pack/ZIP pipeline. Full disc processing still needs the
memory described in `installer/README.md`.
