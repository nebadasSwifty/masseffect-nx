// Everything the installer page needs to know about the project lives here. Change a name, a URL, an edition or a
// wasm file name in this file only; nothing else in the page hard-codes them.

export const CONFIG = {
  project: {
    title: 'Mass Effect for Nintendo Switch',
    author: 'NebadasSwifty',
    repoUrl: 'https://github.com/NebadasSwifty/masseffect-nx',
    releasesUrl: 'https://github.com/NebadasSwifty/masseffect-nx/releases',
    issuesUrl: 'https://github.com/NebadasSwifty/masseffect-nx/issues',
  },

  // Names inside the zip. The zip is extracted into sdmc:/switch/, so `root` becomes sdmc:/switch/masseffect-nx/.
  zip: {
    root: 'masseffect-nx',
    fullName: 'masseffect-nx.zip',
    updateName: 'masseffect-nx-update.zip',
    gameRootDir: 'game_root',
  },

  // File names inside <zip.root>/. The shader package names are fixed by the game (see shaders/README.md).
  files: {
    nro: 'masseffect-nx.nro',
    toml: 'masseffect.toml',
    shaders: 'masseffect_shaders.mesp',
    shadersIndex: 'masseffect_shaders.mesp.idx',
  },

  // Where the page gets the build. GitHub release assets cannot be fetched by a browser from another origin
  // (github.com answers the download URL with a redirect without CORS headers, and the storage host behind the
  // redirect sends none either), so the deploy workflow copies the latest release's assets into the site, next to
  // the page, and writes `manifest.json` there. See installer/README.md ("Where the NRO comes from").
  build: {
    // Same-origin folder of the site that holds the release assets and the manifest.
    siteDir: 'releases/',
    manifest: 'releases/manifest.json',
    // masseffect.toml is copied into the site by the workflow from app/masseffect.toml.
    toml: 'masseffect.toml',
  },

  // Known game editions, recognised by the SHA-256 of default.xex. Only editions listed here can be installed:
  // the NRO is the recompiled program of one exact default.xex.
  //   nro: the asset name in the release (and in siteDir) of the build for this edition.
  editions: [
    {
      id: 'usa-eur-en-es-pl-rev1',
      name: 'Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)',
      xexSha256: 'db14a72a5a24a57309b8c51bd0311c676b7018ca6524e3eb85fbc891c2dfc70b',
      nro: 'masseffect-nx.nro',
    },
  ],

  // Which disc files go to <root>/game_root/. Paths are relative to the disc root, matched case-insensitively.
  // The default is to include a file; only what is listed here is left out. See installer/README.md for the reasons.
  disc: {
    xex: 'default.xex',
    skip: [
      { prefix: '$SystemUpdate/', why: 'Xbox 360 console system update, installed by the console, never read by the game' },
      { prefix: 'FillerFiles/', why: 'padding files of the disc layout (*.jnk), no content' },
      { prefix: 'nxeart', why: 'dashboard artwork of the disc for the Xbox 360 home screen' },
    ],
    // Unreal packages searched for shaders (the scanner decides by content, this only picks candidates).
    scanExtensions: ['.xxx'],
  },

  // The WebAssembly tools (built by shaders/wasm/*.sh and published by the workflow into installer/wasm/).
  wasm: {
    dir: 'wasm/',
    scan: 'scan.mjs',
    hlsl: 'hlsl.mjs',
    dxc: 'dxc_web.mjs',
    pack: 'pack.mjs',
    // shader_common.h of the translator (shaders/XenosRecomp/shader_common.h), copied next to the tools.
    shaderCommon: 'shader_common.h',
    // Supplemental runtime containers for D3D immediate mode and Scaleform UI shaders.
    runtimeContainers: 'runtime_containers.json',
    // Each .mjs loads its own .wasm from the same folder.
    extraFiles: ['scan.wasm', 'hlsl.wasm', 'dxc_web.wasm', 'pack.wasm'],
  },

  limits: {
    // Parallel translate+compile workers: min(hardwareConcurrency - 1, this). Each holds a DXC instance (~0.5 GB).
    maxShaderWorkers: 6,
    scanWorkers: 2,
    // A container whose job produces no answer in this time is skipped and its worker restarted.
    jobTimeoutMs: 180000,
    // Read size when copying disc files into the zip.
    copyChunkBytes: 4 * 1024 * 1024,
    // Expected result of a complete run on the supported edition (the documented counts of shaders/README.md);
    // only used to tell the user when a run looks incomplete, never to stop one.
    expectedContainers: 30191,
    // Rough sizes for the up-front estimate (a complete shader package is about 920 MB plus a 48 MB index).
    expectedShaderBytes: 970 * 1000 * 1000,
    expectedNroBytes: 70 * 1000 * 1000,
    // Without a disk-backed save target the whole zip is built in memory: warn above this size.
    blobWarnBytes: 1.5 * 1024 * 1024 * 1024,
  },
};

export default CONFIG;
