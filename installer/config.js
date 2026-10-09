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

  // Installable NSP output (js/nsp.js, docs/full-nsp.md): file names the page suggests. {n} is the update number.
  nsp: {
    fullName: 'masseffect-nx.nsp',
    updateName: 'masseffect-nx-update-v{n}.nsp',
  },

  // File names inside <zip.root>/. The shader package names are fixed by the game (see shaders/README.md).
  files: {
    nro: 'masseffect-nx.nro',
    toml: 'masseffect.toml',
    shaders: 'masseffect_shaders.mesp',
    shadersIndex: 'masseffect_shaders.mesp.idx',
    // The edition's shipped pipeline prewarm list (editions[].prewarmList in the release), installed under this name
    // next to the NRO; masseffect.toml names it in masseffect_native_pipelines_shipped_list.
    prewarmList: 'masseffect_prewarm_list.bin',
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
  //   prewarmList: the asset name of this edition's shipped pipeline prewarm list (app/prewarm/, made by
  //        tools/extract_prewarm_list.py; it holds fingerprints of this edition's own shaders, so each edition has
  //        its own). Optional: a release without it installs without it (a warning in the log).
  //   nsp: the installable NSP of this edition (docs/full-nsp.md): its application title ID (an update is title ID +
  //        0x800) and the writable SD folder (saves, caches, logs). Each edition has its own, so they install side by
  //        side; tools/build_full_nsp.py --edition uses the same values. Never change the title ID of a released
  //        edition: updates must match the installed title.
  editions: [
    {
      id: 'usa-eur-en-es-pl-rev1',
      name: 'Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)',
      xexSha256: 'db14a72a5a24a57309b8c51bd0311c676b7018ca6524e3eb85fbc891c2dfc70b',
      header: {
        titleId: 0x4D5307E8,
        mediaId: 0x38575660,
        version: 0x00000015,
        imageSize: 16515072,
        entryPoint: 0x828121D0,
      },
      nro: 'masseffect-nx.nro',
      prewarmList: 'masseffect_prewarm_list-en.bin',
      nsp: { titleId: '01a5eec700020000', dataDir: 'sdmc:/switch/masseffect-nx-en' },
    },
    {
      id: 'rus-rev0',
      name: 'Mass Effect (Russia) (Ru)',
      xexSha256: [
        '20c619de3c31f5c4448e7c54062577df839d59b2eb5ac5c633a2953dbbe525a3',
        '4beb582540010b25032e3a51e4ce84a6fb1a9f381adddd8b2def0dc5d6bf715d',
      ],
      header: {
        titleId: 0x4D5307E8,
        mediaId: 0x572BA75D,
        version: 0x00000005,
        imageSize: 16515072,
        entryPoint: 0x82812A00,
      },
      nro: 'masseffect-nx-rus.nro',
      prewarmList: 'masseffect_prewarm_list-ru.bin',
      nsp: { titleId: '01a5eec700010000', dataDir: 'sdmc:/switch/masseffect-nx' },
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
    // Structural check of the disc's UE3 packages when a source is loaded (js/pkgcheck.js, the port of
    // tools/check_packages.py: summary, chunk table, every chunk's block table; and one package GUID under two names).
    // scope 'all' reads a few KB of each of the ~2200 packages (seconds), 'maps' only Maps/*.xxx.
    // block: false only warns; true also keeps the create step hidden while bad or duplicate packages are found.
    // known: packages that are broken on every copy of an edition and that the game never reads. They are listed as a
    // note (not as damaged files) and never block. path: relative to the disc root (case-insensitive); editions: the
    // editions[].id values it applies to (on any other edition a broken copy is a normal finding).
    packageCheck: {
      scope: 'all',
      block: false,
      known: [
        {
          path: 'Layer0/MEInit/GlobalTlk_ES.xxx',
          editions: ['rus-rev0'],
          why: 'the Spanish text table; junk on both discs of the Russian release, which only reads GlobalTlk.xxx',
        },
      ],
    },
  },

  // Optional downloadable content (Xbox 360 Marketplace STFS packages the user supplies; see docs/dlc.md).
  // The runtime ContentManager reads shared (xuid 0) content from the user folder next to the NRO:
  //   <root>/<contentDir>/<package file name>/...  and  <root>/<headersDir>/<package file name>.header
  dlc: {
    titleId: 0x4D5307E8,
    contentType: 0x00000002,
    contentDir: 'masseffect/0000000000000000/4D5307E8/00000002',
    headersDir: 'masseffect/0000000000000000/4D5307E8/Headers/00000002',
    // License mask stored in each .header (every license bit granted; same default as tools/stfs_extract.py).
    licenseMask: 0xFFFFFFFF,
    // masseffect.toml key that makes the runtime report the packages to the game.
    tomlKey: 'dlc_enable',
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
    // Extra shader package bytes when both DLC packages are scanned (base 925 MB -> 951 MB combined).
    expectedDlcShaderBytes: 26 * 1000 * 1000,
    // The runtime and the packer refuse a shader package above 1 GiB (kMaxFile in masseffect_shader_library.cpp).
    maxShaderPackageBytes: 1073741824,
    // Without a disk-backed save target the whole zip is built in memory: warn above this size.
    blobWarnBytes: 1.5 * 1024 * 1024 * 1024,
  },
};

export default CONFIG;
