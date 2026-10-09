// The DLC parts of js/pipeline.js: the toml edit, the zip paths, and a whole run() with stand-in workers (no WebAssembly,
// no game data) that checks what is scanned and what ends up in the zip.
import test from 'node:test';
import assert from 'node:assert/strict';
import { CONFIG } from '../config.js';
import { run, enableTomlFlag, dlcZipEntries, dlcScanFiles, fetchBuild, UserError } from '../js/pipeline.js';
import { createHash } from 'node:crypto';
import { MemorySink } from '../js/zip.js';
import { openStfs } from '../js/stfs.js';
import { buildStfs, pattern } from './stfs_builder.js';

const enc = (s) => new TextEncoder().encode(s);
const dec = (b) => new TextDecoder().decode(b);

const BDTS = 'A275890E35D31622A6AB8D43C53F932A0342DEF64D';
const PINN = '2F0186372DDADB3B2C8557688F02D2587EEAFBC84D';

async function syntheticDlc(name, display, seed) {
  const files = [
    { path: 'AutoLoad.ini', data: enc(`[Packages]\r\nDLCName=${display}\r\n`) },
    { path: `Content/Maps/M${seed}/M${seed}.xxx`, data: pattern(9000 + seed, seed) },
    { path: `Content/Packages/P${seed}.xxx`, data: pattern(5000, seed + 1), fragmented: true },
    { path: 'Movies/intro.bik', data: pattern(300, seed + 2) },
  ];
  const { bytes } = buildStfs({ files, names: { en: display }, contentId: pattern(20, seed + 50) });
  return { pkg: await openStfs(new File([bytes], name), { expect: { titleId: CONFIG.dlc.titleId, contentType: CONFIG.dlc.contentType } }), files };
}

/** Reads a store-only zip without ZIP64 (enough for these small outputs): [{name, data}] in central-directory order. */
function readZip(zip) {
  const dv = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  const eocd = zip.length - 22;
  assert.equal(dv.getUint32(eocd, true), 0x06054b50);
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const out = [];
  for (let i = 0; i < count; i++) {
    assert.equal(dv.getUint32(p, true), 0x02014b50);
    const size = dv.getUint32(p + 24, true);
    const nameLen = dv.getUint16(p + 28, true);
    const extraLen = dv.getUint16(p + 30, true);
    const commentLen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = dec(zip.subarray(p + 46, p + 46 + nameLen));
    const lName = dv.getUint16(local + 26, true);
    const lExtra = dv.getUint16(local + 28, true);
    const start = local + 30 + lName + lExtra;
    out.push({ name, data: zip.subarray(start, start + size) });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return out;
}

const TOML = '# settings\nvideo_mode_width = 960\n\n# dlc_enable = false\npresent_effect = "fsr"\n';

test('enableTomlFlag: replaces the commented example, an existing value, or appends', () => {
  assert.equal(dec(enableTomlFlag(enc(TOML), 'dlc_enable')), '# settings\nvideo_mode_width = 960\n\ndlc_enable = true\npresent_effect = "fsr"\n');
  assert.equal(dec(enableTomlFlag(enc('a = 1\ndlc_enable = false\n# dlc_enable = false\n'), 'dlc_enable')), 'a = 1\ndlc_enable = true\n# dlc_enable = false\n');
  const appended = dec(enableTomlFlag(enc('a = 1\n'), 'dlc_enable'));
  assert.match(appended, /^a = 1\n\n# Downloadable content added by the installer[^\n]*\ndlc_enable = true\n$/);
  // a [table] keeps the key top-level
  const tabled = dec(enableTomlFlag(enc('a = 1\n[section]\nb = 2\n'), 'dlc_enable'));
  assert.ok(tabled.indexOf('dlc_enable = true') < tabled.indexOf('[section]'));
  // the key inside a table does not count as the top-level key
  const inTable = dec(enableTomlFlag(enc('a = 1\n[x]\ndlc_enable = false\n'), 'dlc_enable'));
  assert.ok(inTable.indexOf('dlc_enable = true') < inTable.indexOf('[x]'));
  // a similar key is not touched
  assert.match(dec(enableTomlFlag(enc('dlc_enable_extra = 1\n'), 'dlc_enable')), /dlc_enable_extra = 1\n[\s\S]*\ndlc_enable = true\n$/);
});

test('dlcZipEntries: the SD paths of the runtime ContentManager', async () => {
  const { pkg } = await syntheticDlc(BDTS, 'Bring Down the Sky [ENPLES]', 1);
  const entries = dlcZipEntries(CONFIG, [pkg]);
  const names = entries.map((e) => e.name);
  assert.deepEqual(names, [
    `masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/${BDTS}/AutoLoad.ini`,
    `masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/${BDTS}/Content/Maps/M1/M1.xxx`,
    `masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/${BDTS}/Content/Packages/P1.xxx`,
    `masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/${BDTS}/Movies/intro.bik`,
    `masseffect-nx/masseffect/0000000000000000/4D5307E8/Headers/00000002/${BDTS}.header`,
  ]);
  assert.equal(entries.at(-1).size, 332);
  assert.equal(dlcScanFiles([pkg]).filter((f) => f.path.endsWith('.xxx')).length, 2);
  assert.throws(() => dlcZipEntries(CONFIG, [pkg, pkg]), UserError);
});

// ---- a whole run with stand-in workers ----------------------------------------------------------------------------
function fakeSite({ packageBytes = 4096, list = null, manifest = null, shader = null } = {}) {
  const nro = enc('NRO0'.repeat(50));
  const toml = enc(TOML);
  const scanned = [];
  const fetched = [];
  const fetchImpl = async (url, init = {}) => {
    const u = String(url).replace(/\?.*$/, ''); // without the ?v= cache tag
    fetched.push(u);
    let body = null;
    if (u.endsWith('/masseffect.toml')) body = toml;
    else if (u.endsWith('.nro')) body = nro;
    else if (u.endsWith(`/releases/${CONFIG.editions[0].prewarmList}`)) body = list;
    else if (u.endsWith('/releases/manifest.json') && manifest) body = enc(JSON.stringify(manifest(nro, toml)));
    else if (u.endsWith('runtime_containers.json')) body = enc('{}');
    const ok = init.method === 'HEAD' || body !== null;
    return {
      ok, status: ok ? 200 : 404,
      headers: { get: () => null },
      json: async () => JSON.parse(dec(body)),
      arrayBuffer: async () => body.slice().buffer,
      body: null,
    };
  };
  const handlers = {
    scan: async (m) => {
      if (m.type === 'init') return { type: 'ready' };
      scanned.push(m.name);
      const head = new Uint8Array(await m.blob.slice(0, 16).arrayBuffer());
      return { type: 'scanned', id: m.id, name: m.name, ok: true, containers: [{ name: `c_${m.name}`, data: head }] };
    },
    shader: async (m) => (m.type === 'init' ? { type: 'ready' } : (shader?.(m) ?? { type: 'result', id: m.id, ok: true, spirv: new Uint8Array(8) })),
    pack: async (m) => {
      if (m.type === 'init') return { type: 'ready' };
      if (m.type === 'add') return { type: 'added' };
      if (m.type === 'pack') return { type: 'packed', ok: true, sizes: { package: packageBytes, index: 64 }, summary: 'packed' };
      if (m.type === 'read') return { type: 'chunk', data: new Uint8Array(m.length).fill(0x5a) };
      return null;
    },
  };
  const createWorker = (kind) => {
    const w = {
      onmessage: null, onerror: null,
      postMessage(msg) { handlers[kind](msg).then((r) => r && setImmediate(() => w.onmessage?.({ data: r }))); },
      terminate() {},
    };
    return w;
  };
  return { nro, toml, scanned, fetched, fetchImpl, createWorker };
}

const discFiles = () => [
  { path: 'default.xex', size: 64, blob: new Blob([pattern(64, 1)]) },
  { path: 'Layer0/MEInit/Core.xxx', size: 100, blob: new Blob([pattern(100, 2)]) },
  { path: 'Layer1/Splash.bmp', size: 10, blob: new Blob([pattern(10, 3)]) },
];

async function runWith(site, mode, dlc, extra = {}) {
  const sink = new MemorySink();
  const logs = [];
  const result = await run({
    config: CONFIG, edition: CONFIG.editions[0], files: discFiles(), dlc, mode, sink,
    createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl,
    onLog: (t, l) => logs.push([l, t]), ...extra,
  });
  return { result, zip: readZip(sink.concat()), logs };
}

test('run with two DLC packages: scanned, extracted to the content folder, dlc_enable = true', async () => {
  const a = await syntheticDlc(BDTS, 'Bring Down the Sky [ENPLES]', 1);
  const b = await syntheticDlc(PINN, 'Pinnacle Station [ENPLES]', 7);
  for (const mode of ['full', 'update']) {
    const site = fakeSite();
    const { zip } = await runWith(site, mode, [a.pkg, b.pkg]);
    // the scan saw the disc's and both packages' .xxx files
    assert.deepEqual(site.scanned.slice().sort(), [
      `DLC/${PINN}/Content/Maps/M7/M7.xxx`, `DLC/${PINN}/Content/Packages/P7.xxx`,
      `DLC/${BDTS}/Content/Maps/M1/M1.xxx`, `DLC/${BDTS}/Content/Packages/P1.xxx`,
      'Layer0/MEInit/Core.xxx',
    ].sort());
    const byName = new Map(zip.map((e) => [e.name, e.data]));
    const toml = dec(byName.get('masseffect-nx/masseffect.toml'));
    assert.match(toml, /^dlc_enable = true$/m);
    assert.doesNotMatch(toml, /dlc_enable = false/);
    assert.equal(byName.has('masseffect-nx/game_root/default.xex'), mode === 'full');
    for (const { pkg, files } of [a, b]) {
      const dir = `masseffect-nx/masseffect/0000000000000000/4D5307E8/00000002/${pkg.folderName}`;
      for (const f of files) assert.deepEqual(byName.get(`${dir}/${f.path}`), f.data, `${dir}/${f.path}`);
      const header = byName.get(`masseffect-nx/masseffect/0000000000000000/4D5307E8/Headers/00000002/${pkg.folderName}.header`);
      assert.deepEqual(header, pkg.header);
    }
    const names = zip.map((e) => e.name);
    assert.deepEqual(names.slice(0, 4), ['masseffect-nx/masseffect-nx.nro', 'masseffect-nx/masseffect.toml', 'masseffect-nx/masseffect_shaders.mesp', 'masseffect-nx/masseffect_shaders.mesp.idx']);
    assert.equal(names.length, 4 + (mode === 'full' ? 3 : 0) + 2 * 5);
  }
});

test('run without DLC leaves masseffect.toml and the content folder alone', async () => {
  const site = fakeSite();
  const { zip } = await runWith(site, 'full', []);
  const byName = new Map(zip.map((e) => [e.name, e.data]));
  assert.deepEqual(byName.get('masseffect-nx/masseffect.toml'), site.toml);
  assert.ok(!zip.some((e) => e.name.startsWith('masseffect-nx/masseffect/')));
  assert.deepEqual(site.scanned, ['Layer0/MEInit/Core.xxx']);
});

test('a shader package above 1 GiB is refused', async () => {
  const { pkg } = await syntheticDlc(BDTS, 'Bring Down the Sky [ENPLES]', 1);
  const site = fakeSite({ packageBytes: CONFIG.limits.maxShaderPackageBytes + 1 });
  let aborted = false;
  await assert.rejects(
    run({ config: CONFIG, edition: CONFIG.editions[0], files: discFiles(), dlc: [pkg], mode: 'update',
      sink: { write() {}, close() {}, abort() { aborted = true; } },
      createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl }),
    (e) => e instanceof UserError && e.code === 'shader-cap' && /1 GiB/.test(e.message));
  assert.equal(aborted, true);
  assert.equal(CONFIG.limits.maxShaderPackageBytes, 1073741824);
});

// ---- the edition's shipped pipeline prewarm list -----------------------------------------------------------------
const LIST = new Uint8Array([...enc('NFPL'), 5, 0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, ...pattern(8, 77)]);
const asset = (b) => ({ size: b.length, sha256: createHash('sha256').update(b).digest('hex') });
const fullManifest = (list) => (nro, toml) => ({
  tag: 'vtest',
  assets: { [CONFIG.editions[0].nro]: asset(nro), 'masseffect.toml': asset(toml), [CONFIG.editions[0].prewarmList]: asset(list) },
});

test('config: every edition names its own prewarm list; it is installed under the toml name', () => {
  const names = CONFIG.editions.map((e) => e.prewarmList);
  assert.ok(names.every((n) => /^masseffect_prewarm_list-[a-z]+\.bin$/.test(n)), names.join());
  assert.equal(new Set(names).size, names.length);
  assert.equal(CONFIG.files.prewarmList, 'masseffect_prewarm_list.bin');
});

test('run: the prewarm list goes next to the NRO in the full and the update zip', async () => {
  for (const mode of ['full', 'update']) {
    const site = fakeSite({ list: LIST, manifest: fullManifest(LIST) });
    const { zip, logs } = await runWith(site, mode, []);
    const names = zip.map((e) => e.name);
    assert.deepEqual(names.slice(0, 5), ['masseffect-nx/masseffect-nx.nro', 'masseffect-nx/masseffect.toml',
      'masseffect-nx/masseffect_prewarm_list.bin', 'masseffect-nx/masseffect_shaders.mesp', 'masseffect-nx/masseffect_shaders.mesp.idx']);
    assert.deepEqual(new Map(zip.map((e) => [e.name, e.data])).get('masseffect-nx/masseffect_prewarm_list.bin'), LIST);
    assert.equal(names.length, 5 + (mode === 'full' ? 3 : 0));
    assert.ok(!logs.some(([l, t]) => l === 'warn' && /prewarm/.test(t)), JSON.stringify(logs));
  }
});

test('fetchBuild: a release without the list installs without it (warning only, no download attempt)', async () => {
  const site = fakeSite({ list: LIST, manifest: (nro, toml) => ({ tag: 't', assets: { [CONFIG.editions[0].nro]: asset(nro), 'masseffect.toml': asset(toml) } }) });
  const logs = [];
  const build = await fetchBuild(CONFIG, CONFIG.editions[0], { baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl, progress() {}, log: (t, l) => logs.push([l, t]) });
  assert.equal(build.prewarmList, undefined);
  assert.ok(!site.fetched.some((u) => u.includes('prewarm')));
  assert.ok(logs.some(([l, t]) => l === 'warn' && t.includes(CONFIG.editions[0].prewarmList)));
  // No manifest and no list on the site (HTTP 404): also a warning, and the zip has no list.
  const bare = fakeSite();
  const { zip, logs: logs2 } = await runWith(bare, 'update', []);
  assert.ok(!zip.some((e) => e.name.endsWith('masseffect_prewarm_list.bin')));
  assert.ok(logs2.some(([l, t]) => l === 'warn' && t.includes('prewarm list')));
});

test('fetchBuild: the list is verified against the manifest like the NRO; a file that is not a list is dropped', async () => {
  const other = LIST.slice();
  other[20] ^= 1;
  const site = fakeSite({ list: other, manifest: fullManifest(LIST) });
  await assert.rejects(fetchBuild(CONFIG, CONFIG.editions[0], { baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl, progress() {} }),
    (e) => e instanceof UserError && e.code === 'integrity' && e.message.includes(CONFIG.editions[0].prewarmList));
  const notList = enc('<html>not found</html>');
  const logs = [];
  const build = await fetchBuild(CONFIG, CONFIG.editions[0], { baseUrl: 'https://example.test/', fetchImpl: fakeSite({ list: notList }).fetchImpl, progress() {}, log: (t, l) => logs.push([l, t]) });
  assert.equal(build.prewarmList, undefined);
  assert.ok(logs.some(([l, t]) => l === 'warn' && t.includes('not a pipeline prewarm list')));
});

// ---- shader failures: never a package with holes ------------------------------------------------------------------
const fail = (m, reason, stage = 'translate') => ({ type: 'result', id: m.id, name: m.name, ok: false, stage, reason });

test('a container that fails twice stops the run: no package is written', async () => {
  const site = fakeSite({ shader: (m) => (m.name === 'c_Layer0/MEInit/Core.xxx' ? fail(m, 'translator crashed (memory access out of bounds)') : null) });
  const { pkg } = await syntheticDlc(BDTS, 'Bring Down the Sky [ENPLES]', 1);
  let aborted = false;
  const logs = [];
  await assert.rejects(
    run({ config: CONFIG, edition: CONFIG.editions[0], files: discFiles(), dlc: [pkg], mode: 'update',
      sink: { write() {}, close() {}, abort() { aborted = true; } },
      createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl, onLog: (t, l) => logs.push([l, t]) }),
    (e) => e instanceof UserError && e.code === 'shaders-incomplete' && /1 of 3 shaders could not be made/.test(e.message) && /Core\.xxx/.test(e.message));
  assert.equal(aborted, true);
  assert.ok(logs.some(([l, t]) => l === 'error' && /^FAILED c_Layer0\/MEInit\/Core\.xxx \(translate\)/.test(t)));
  assert.ok(!logs.some(([, t]) => /expected/.test(t)), 'no "expected" wording for failures');
});

test('a container that fails once is retried in a fresh worker and the run succeeds', async () => {
  let calls = 0;
  const site = fakeSite({ shader: (m) => (++calls === 1 ? fail(m, 'no answer after 180 s') : null) });
  const { result, logs } = await runWith(site, 'update', []);
  assert.equal(calls, 2);
  assert.equal(result.shaders.ok, 1);
  assert.deepEqual(result.shaders.failures, []);
  assert.ok(logs.some(([l, t]) => l === 'warn' && /trying them once more/.test(t)));
});

test('a scan match that is not a shader container is ignored, not a failure', async () => {
  const site2 = fakeSite({ shader: (m) => (m.name.endsWith('P1.xxx') ? fail(m, 'not a shader container: microcode outside the container') : null) });
  const { pkg } = await syntheticDlc(BDTS, 'Bring Down the Sky [ENPLES]', 1);
  const r2 = await runWith(site2, 'update', [pkg]);
  assert.equal(r2.result.shaders.ignored, 1);
  assert.deepEqual(r2.result.shaders.failures, []);
  assert.ok(r2.logs.some(([l, t]) => l === 'info' && /1 scan matches are not shader containers/.test(t)));
});

test('a container on the known list is left out with a note; the run succeeds', async () => {
  const name = 'c_Layer0/MEInit/Core.xxx';
  const config = { ...CONFIG, knownShaderFailures: { [name]: 'DXC rejects it natively too' } };
  const { pkg } = await syntheticDlc(BDTS, 'Bring Down the Sky [ENPLES]', 1);
  const site = fakeSite({ shader: (m) => (m.name === name ? fail(m, 'DXC rejected it (code 4)', 'dxc') : null) });
  const sink = new MemorySink();
  const logs = [];
  const result = await run({ config, edition: config.editions[0], files: discFiles(), dlc: [pkg], mode: 'update', sink,
    createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl, onLog: (t, l) => logs.push([l, t]) });
  assert.deepEqual(result.shaders.failures, []);
  assert.deepEqual(result.shaders.known.map((f) => f.name), [name]);
  assert.ok(logs.some(([l, t]) => l === 'skip' && /Known untranslatable/.test(t)));
});
