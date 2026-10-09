// run() with output 'nsp' (stand-in workers, no WebAssembly, no game data): the stages, the RomFS entries the page
// packs (the same files as the full zip), a full NSP, a program-only update from its base metadata or from the base
// NSP itself, and an update with game data. Each NSP is compared with js/nsp.js buildNsp on the same entries.
import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { CONFIG } from '../config.js';
import { run, stagesFor, UserError } from '../js/pipeline.js';
import {
  buildNsp, sourceFromBytes, pythonJson, parseBaseMetadata, metadataFromNsp, PartsReader, PackError, checkUpdateTitle, validDataDir,
} from '../js/nsp.js';
import { MemoryNspSink, UsbNspSink } from '../js/nsp_sink.js';
import { usbLink, fakeSphairaInstall } from './usb_fake_console.js';
import { deterministicPssSigner } from '../js/nsp_crypto.js';
import { prng, makeNro, makeKeys, makeRsa } from './nsp_fixtures.js';
import { pattern } from './stfs_builder.js';

const enc = (s) => new TextEncoder().encode(s);
const dec = (b) => new TextDecoder().decode(b);
const TOML = 'video_mode_width = 960\n';

function fakeSite(nro, list = null) {
  const toml = enc(TOML);
  const fetchImpl = async (url, init = {}) => {
    const u = String(url).replace(/\?.*$/, ''); // without the ?v= cache tag
    let body = null;
    if (u.endsWith('/masseffect.toml')) body = toml;
    else if (u.endsWith('.nro')) body = nro;
    else if (u.endsWith('runtime_containers.json')) body = enc('{}');
    else if (u.endsWith(`/releases/${CONFIG.editions[0].prewarmList}`)) body = list;
    const ok = init.method === 'HEAD' || body !== null;
    return { ok, status: ok ? 200 : 404, headers: { get: () => null }, json: async () => JSON.parse(dec(body)), arrayBuffer: async () => body.slice().buffer, body: null };
  };
  const handlers = {
    scan: async (m) => (m.type === 'init' ? { type: 'ready' } : { type: 'scanned', id: m.id, name: m.name, ok: true, containers: [{ name: `c_${m.name}`, data: new Uint8Array(4) }] }),
    shader: async (m) => (m.type === 'init' ? { type: 'ready' } : { type: 'result', id: m.id, ok: true, spirv: new Uint8Array(8) }),
    pack: async (m) => {
      if (m.type === 'init') return { type: 'ready' };
      if (m.type === 'add') return { type: 'added' };
      if (m.type === 'pack') return { type: 'packed', ok: true, sizes: { package: 70000, index: 64 }, summary: 'packed' };
      if (m.type === 'read') return { type: 'chunk', data: shaderBytes(m.file, m.offset, m.length) };
      return null;
    },
  };
  const createWorker = (kind) => {
    const w = { onmessage: null, onerror: null, postMessage(msg) { handlers[kind](msg).then((r) => r && setImmediate(() => w.onmessage?.({ data: r }))); }, terminate() {} };
    return w;
  };
  return { fetchImpl, createWorker, toml };
}

const SHADERS = { 'masseffect_shaders.mesp': pattern(70000, 9), 'masseffect_shaders.mesp.idx': pattern(64, 10) };
const shaderBytes = (file, offset, length) => SHADERS[file].slice(offset, offset + length);

const discFiles = (variant = 0) => [
  { path: 'default.xex', size: 64, blob: new Blob([pattern(64, 1)]) },
  { path: 'Layer0/MEInit/Core.xxx', size: 100000, blob: new Blob([pattern(100000, 2 + variant)]) },
  { path: 'Layer1/Splash.bmp', size: 10, blob: new Blob([pattern(10, 3)]) },
  { path: 'FillerFiles/x.jnk', size: 10, blob: new Blob([pattern(10, 4)]) }, // left out, like in the zip
];

/** The RomFS entries a full run must pack, built independently from the inputs. */
function expectedEntries(toml, variant = 0, list = null) {
  const files = discFiles(variant).filter((f) => !f.path.startsWith('FillerFiles/'));
  return [
    { path: 'masseffect.toml', ...sourceFromBytes(toml) },
    ...(list ? [{ path: 'masseffect_prewarm_list.bin', ...sourceFromBytes(list) }] : []),
    { path: 'masseffect_shaders.mesp', ...sourceFromBytes(SHADERS['masseffect_shaders.mesp']) },
    { path: 'masseffect_shaders.mesp.idx', ...sourceFromBytes(SHADERS['masseffect_shaders.mesp.idx']) },
    ...files.map((f) => ({ path: `game_root/${f.path}`, size: f.size, chunks: async function* () { yield new Uint8Array(await f.blob.arrayBuffer()); } })),
  ];
}

let fixed;
function deterministic() {
  if (fixed) return fixed;
  const rsa = makeRsa();
  const salt = new Uint8Array(crypto.randomBytes(32));
  const aes = { 0: new Uint8Array(crypto.randomBytes(16)), 1: new Uint8Array(crypto.randomBytes(16)), 2: new Uint8Array(crypto.randomBytes(16)) };
  const { keys } = makeKeys();
  fixed = { keys, signer: deterministicPssSigner(rsa, salt), aesKeyFor: (t) => aes[t], createdUtc: '2026-10-08T00:00:00Z' };
  return fixed;
}

async function runNsp(site, nsp, files = discFiles(), edition = CONFIG.editions[0], logs = null) {
  const det = deterministic();
  const sink = new MemoryNspSink();
  const seen = [];
  const result = await run({
    config: CONFIG, edition, files, dlc: [], mode: 'full', sink, output: 'nsp', onLog: logs ? (t, level) => logs.push([level, t]) : undefined,
    nsp: { keys: det.keys, signer: det.signer, aesKeyFor: det.aesKeyFor, createdUtc: det.createdUtc, ...nsp },
    createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl,
    onProgress: (id) => { if (seen.at(-1) !== id) seen.push(id); },
  });
  return { result, bytes: sink.bytes(), stages: seen };
}

async function direct(options) {
  const det = deterministic();
  const sink = new MemoryNspSink();
  const result = await buildNsp({ keys: det.keys, signer: det.signer, aesKeyFor: det.aesKeyFor, createdUtc: det.createdUtc, sink, ...options });
  return { result, bytes: sink.bytes() };
}

test('stagesFor: zip, full NSP, update, program-only update', () => {
  assert.deepEqual(stagesFor('zip'), ['download', 'scan', 'translate', 'pack', 'zip']);
  assert.deepEqual(stagesFor('nsp', { kind: 'full' }), ['download', 'scan', 'translate', 'pack', 'nsp_hash', 'nsp_write']);
  assert.deepEqual(stagesFor('nsp', { kind: 'update' }), ['download', 'nsp_base', 'scan', 'translate', 'pack', 'nsp_hash', 'nsp_write']);
  assert.deepEqual(stagesFor('nsp', { kind: 'update', programOnly: true }), ['download', 'nsp_base', 'nsp_write']);
});

test('run(output nsp): full NSP, then updates against it (metadata file, base NSP, with game data)', async () => {
  const rand = prng(42);
  const nro = makeNro(rand);
  const site = fakeSite(nro);
  const full = await runNsp(site, { kind: 'full' });
  assert.deepEqual(full.stages, stagesFor('nsp', { kind: 'full' }));
  const en = CONFIG.editions[0].nsp;
  const want = await direct({ nro, entries: expectedEntries(site.toml), titleId: en.titleId, dataDir: en.dataDir });
  assert.deepEqual(full.bytes, want.bytes, 'the page packs the same files as the zip');
  assert.equal(pythonJson(full.result.nsp.baseMeta), pythonJson(want.result.baseMeta));
  assert.equal(full.result.sinkResult, 'memory');

  // Program-only update from the saved metadata (as the page reads it back).
  const meta = parseBaseMetadata(pythonJson(full.result.nsp.baseMeta));
  const nro2 = makeNro(rand, 'Mass Effect');
  const site2 = fakeSite(nro2);
  const upd = await runNsp(site2, { kind: 'update', programOnly: true, version: 1, base: { meta } }, []);
  assert.deepEqual(upd.stages, ['download', 'nsp_base', 'nsp_write']);
  const wantUpd = await direct({ nro: nro2, update: { baseMeta: meta, programOnly: true }, version: 1 });
  assert.deepEqual(upd.bytes, wantUpd.bytes);
  assert.ok(upd.bytes.length < nro2.length + 0x30000, `small: ${upd.bytes.length}`);

  // The same from the base NSP file (split into two parts, like 00 and 01 of a FAT32 folder).
  const half = 0x20000;
  const parts = [new Blob([full.bytes.subarray(0, half)]), new Blob([full.bytes.subarray(half)])];
  const upd2 = await runNsp(site2, { kind: 'update', programOnly: true, version: 1, base: { parts } }, []);
  assert.deepEqual(upd2.bytes, wantUpd.bytes);
  assert.equal(upd2.result.baseMeta.kind, 'masseffect-nx base RomFS (from the NSP)');

  // An update with game data (a changed disc file and new shaders would be stored; the rest maps to the base).
  const upd3 = await runNsp(site2, { kind: 'update', version: 2, base: { meta } }, discFiles(1));
  assert.deepEqual(upd3.stages, stagesFor('nsp', { kind: 'update' }));
  const wantUpd3 = await direct({ nro: nro2, entries: expectedEntries(site.toml, 1), update: { baseMeta: meta }, version: 2, dataDir: meta.data_dir });
  assert.deepEqual(upd3.bytes, wantUpd3.bytes);
  assert.ok(upd3.result.nsp.stats.baseBytes > 0);
});

test('run(output nsp, target usb): the console gets the same NSP; the pack worker serves the shaders again by offset', async () => {
  const rand = prng(44);
  const nro = makeNro(rand);
  const site = fakeSite(nro);
  const want = await runNsp(site, { kind: 'full' });
  const det = deterministic();
  const { host, console: con } = usbLink();
  const sink = new UsbNspSink(async () => host, CONFIG.nsp.fullName);
  const serve = sink.serve.bind(sink);
  let image;
  sink.serve = (img, o) => { image = img; return serve(Object.assign(img, { windowBytes: 0x4000, skipAheadBytes: 0x4000 }), o); };
  const seen = [];
  const labels = [];
  const consoleRun = fakeSphairaInstall(con, { readSize: 0x5000, midReads: [{ after: 0.3, offset: -0x9000, length: 0x3000 }, { after: 0.7, offset: -0x9000, length: 0x3000 }] });
  const [result, got] = await Promise.all([run({
    config: CONFIG, edition: CONFIG.editions[0], files: discFiles(), dlc: [], mode: 'full', sink, output: 'nsp',
    nsp: { keys: det.keys, signer: det.signer, aesKeyFor: det.aesKeyFor, createdUtc: det.createdUtc, kind: 'full', target: 'usb', chunkBytes: 0x4000 },
    createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl,
    onProgress: (id, p) => { if (seen.at(-1) !== id) seen.push(id); if (id === 'nsp_usb') labels.push(p.label); },
  }), consoleRun]);
  assert.deepEqual(seen, stagesFor('nsp', { kind: 'full', target: 'usb' }));
  assert.ok(Buffer.from(got.bytes).equals(Buffer.from(want.bytes)));
  assert.equal(result.sinkResult, 'installed');
  assert.equal(pythonJson(result.nsp.baseMeta), pythonJson(want.result.nsp.baseMeta));
  assert.match(labels.at(-1), /^Sending to the Switch: /);
  assert.ok(image.stats.reopens >= 2, 'sources were re-read from inside (disc file slices, pack worker reads at an offset)');
  assert.equal(image.stats.verified, true);
});

test('run(output nsp): the edition\'s prewarm list is in the RomFS of the full NSP and of an update with data', async () => {
  const rand = prng(43);
  const nro = makeNro(rand);
  const list = new Uint8Array([...enc('NFPL'), 5, 0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, ...rand(8)]);
  const site = fakeSite(nro, list);
  const logs = [];
  const full = await runNsp(site, { kind: 'full' }, discFiles(), CONFIG.editions[0], logs);
  const en = CONFIG.editions[0].nsp;
  const want = await direct({ nro, entries: expectedEntries(site.toml, 0, list), titleId: en.titleId, dataDir: en.dataDir });
  assert.deepEqual(full.bytes, want.bytes);
  assert.ok(full.result.nsp.baseMeta.files['masseffect_prewarm_list.bin'], 'the list is a RomFS file of the base');
  assert.ok(!logs.some(([l, t]) => l === 'warn' && /prewarm/.test(t)), JSON.stringify(logs));
  // A new release's list reaches an update with game data (the program-only update keeps the base's RomFS).
  const meta = parseBaseMetadata(pythonJson(full.result.nsp.baseMeta));
  const list2 = list.slice();
  list2[20] ^= 0xff;
  const upd = await runNsp(fakeSite(nro, list2), { kind: 'update', version: 1, base: { meta } }, discFiles());
  const wantUpd = await direct({ nro, entries: expectedEntries(site.toml, 0, list2), update: { baseMeta: meta }, version: 1, dataDir: meta.data_dir });
  assert.deepEqual(upd.bytes, wantUpd.bytes);
  assert.ok(upd.result.nsp.stats.patchBytes > 0);
  // Without a list on the site: packed without it, with a warning.
  const bareLogs = [];
  const bare = await runNsp(fakeSite(nro), { kind: 'full' }, discFiles(), CONFIG.editions[0], bareLogs);
  const wantBare = await direct({ nro, entries: expectedEntries(site.toml), titleId: en.titleId, dataDir: en.dataDir });
  assert.deepEqual(bare.bytes, wantBare.bytes);
  assert.ok(bareLogs.some(([l, t]) => l === 'warn' && t.includes('masseffect_prewarm_list')));
});

test('run(output nsp): errors are UserErrors and the output is discarded', async () => {
  const site = fakeSite(makeNro(prng(5)));
  let aborted = 0;
  const sink = { write() {}, writeAt() {}, close() {}, abort() { aborted++; } };
  const base = { config: CONFIG, edition: CONFIG.editions[0], files: [], dlc: [], mode: 'full', sink, output: 'nsp', createWorker: site.createWorker, baseUrl: 'https://example.test/', fetchImpl: site.fetchImpl };
  await assert.rejects(run({ ...base, nsp: { kind: 'full' } }), (e) => e instanceof UserError && e.code === 'nsp-keys');
  const { keys } = makeKeys();
  await assert.rejects(run({ ...base, nsp: { keys, kind: 'update', programOnly: true, version: 1, base: {} } }), (e) => e instanceof UserError && e.code === 'nsp-base');
  const notNsp = [new Blob([pattern(4096, 1)])];
  await assert.rejects(run({ ...base, nsp: { keys, kind: 'update', programOnly: true, version: 1, base: { parts: notNsp } } }), (e) => e instanceof UserError && /not an NSP/.test(e.message));
  assert.equal(aborted, 3);
});

const EN = CONFIG.editions.find((e) => e.id === 'usa-eur-en-es-pl-rev1');
const RU = CONFIG.editions.find((e) => e.id === 'rus-rev0');

test('config: every edition has its own NSP title ID and data folder, clear of the forwarder and of patch/AOC IDs', () => {
  assert.deepEqual(EN.nsp, { titleId: '01a5eec700020000', dataDir: 'sdmc:/switch/masseffect-nx-en' });
  assert.deepEqual(RU.nsp, { titleId: '01a5eec700010000', dataDir: 'sdmc:/switch/masseffect-nx' });
  const apps = [0x01A5EEC700000000n, ...CONFIG.editions.map((e) => BigInt(`0x${e.nsp.titleId}`))];
  // Each application owns its ID, its patch (+0x800) and its add-on range (+0x1000 .. +0x1FFF).
  const ranges = apps.map((id) => [id, id + 0x1FFFn]);
  for (let i = 0; i < ranges.length; i++) {
    for (let j = i + 1; j < ranges.length; j++) assert.ok(ranges[i][1] < ranges[j][0] || ranges[j][1] < ranges[i][0], `${i} and ${j} overlap`);
  }
  for (const id of apps) assert.equal(id & 0xFFFn, 0n);
  const dirs = CONFIG.editions.map((e) => e.nsp.dataDir);
  assert.equal(new Set(dirs).size, dirs.length);
  for (const d of dirs) assert.ok(validDataDir(d), d);
});

test('run(output nsp): each edition packs its own title ID and data folder; an update keeps the base title', async () => {
  const rand = prng(77);
  const nro = makeNro(rand);
  const site = fakeSite(nro);
  for (const ed of [EN, RU]) {
    const full = await runNsp(site, { kind: 'full' }, discFiles(), ed);
    assert.equal(full.result.nsp.titleId, ed.nsp.titleId);
    assert.equal(full.result.nsp.baseMeta.data_dir, ed.nsp.dataDir);
    const want = await direct({ nro, entries: expectedEntries(site.toml), titleId: BigInt(`0x${ed.nsp.titleId}`), dataDir: ed.nsp.dataDir });
    assert.deepEqual(full.bytes, want.bytes);
    // The marker inside the RomFS: read back from the NSP itself (metadataFromNsp), the same data folder.
    const fromNsp = await metadataFromNsp(new PartsReader([new Blob([full.bytes])]), deterministic().keys);
    assert.equal(fromNsp.title_id, ed.nsp.titleId);
    assert.equal(fromNsp.data_dir, ed.nsp.dataDir);
    // A program-only update of the same edition: patch title = base + 0x800.
    const upd = await runNsp(site, { kind: 'update', programOnly: true, version: 1, base: { parts: [new Blob([full.bytes])] } }, [], ed);
    assert.equal(upd.result.nsp.patchId, (BigInt(`0x${ed.nsp.titleId}`) + 0x800n).toString(16).padStart(16, '0'));
  }
});

test('run(output nsp): an update for a base of the other edition is refused before any work; an unknown base title warns', async () => {
  const rand = prng(78);
  const site = fakeSite(makeNro(rand));
  const ruFull = await runNsp(site, { kind: 'full' }, discFiles(), RU);
  const ruMeta = parseBaseMetadata(pythonJson(ruFull.result.nsp.baseMeta));
  await assert.rejects(runNsp(site, { kind: 'update', programOnly: true, version: 1, base: { meta: ruMeta } }, [], EN),
    (e) => e instanceof UserError && e.code === 'nsp' && /Russia/.test(e.message) && /01a5eec700010000/.test(e.message) && /01a5eec700020000/.test(e.message));
  await assert.rejects(runNsp(site, { kind: 'update', version: 1, base: { parts: [new Blob([ruFull.bytes])] } }, discFiles(1), EN),
    (e) => e instanceof UserError && /same edition/.test(e.message));
  // A base with a title ID of no edition (packed with an explicit --title-id): its ID is kept, with a warning.
  const custom = await direct({ nro: makeNro(rand), entries: expectedEntries(site.toml), titleId: 0x01A5EEC700030000n, dataDir: 'sdmc:/switch/me-custom' });
  const logs = [];
  const upd = await runNsp(site, { kind: 'update', version: 1, base: { meta: custom.result.baseMeta } }, discFiles(1), EN, logs);
  assert.equal(upd.result.nsp.patchId, '01a5eec700030800');
  assert.ok(logs.some(([level, t]) => level === 'warn' && /01a5eec700030000/.test(t) && /keeps the base/.test(t)), JSON.stringify(logs));
  assert.equal(upd.result.nsp.titleId, '01a5eec700030000');
  // ... and the base's data folder, not the edition's: no "saves move" warning.
  assert.ok(!logs.some(([, t]) => /data_dir=/.test(t)), JSON.stringify(logs));
  // buildNsp itself: a title ID given for an update is only checked against the base.
  await assert.rejects(direct({ nro: makeNro(rand), update: { baseMeta: ruMeta, programOnly: true }, version: 1, titleId: EN.nsp.titleId, knownTitleIds: { [RU.nsp.titleId]: 'ru', [EN.nsp.titleId]: 'en' } }),
    (e) => e instanceof PackError && /the ru edition/.test(e.message));
  assert.equal(checkUpdateTitle('01a5eec700010000', 0x01A5EEC700010000n), null);
});
