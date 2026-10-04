// End-to-end run of the real pipeline in Node: the real scan/hlsl/pack WebAssembly (built from shaders/wasm), the
// installer's worker handlers and orchestration, the zip writer, over a few packages of your own disc.
// DXC can run natively through a shim, or use real WASM with MASSEFFECT_TEST_DXC_WASM=1.
//   MASSEFFECT_TEST_WASM=<folder with scan/hlsl/pack .mjs+.wasm>  MASSEFFECT_TEST_DISC=<extracted disc>
//   (needs `dxc` on PATH or DXC=...)  node --test test/e2e.test.js
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, copyFileSync, readFileSync, writeFileSync, rmSync, existsSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { CONFIG } from '../config.js';
import { run, Cancelled, UserError } from '../js/pipeline.js';
import { MemorySink } from '../js/zip.js';
import { ScanHandler, ShaderHandler, PackHandler } from '../js/workers/handlers.js';

const here = dirname(fileURLToPath(import.meta.url));
const wasmDir = process.env.MASSEFFECT_TEST_WASM;
const disc = process.env.MASSEFFECT_TEST_DISC;
const haveDxc = spawnSync(process.env.DXC || 'dxc', ['--version']).status === 0 || spawnSync(process.env.DXC || 'dxc', ['-?']).status === 0;
const realDxcWasm = process.env.MASSEFFECT_TEST_DXC_WASM === '1';
const skip = !wasmDir || !disc || (!realDxcWasm && !haveDxc) ? 'set MASSEFFECT_TEST_WASM and MASSEFFECT_TEST_DISC and have dxc' : false;

function makeSite() {
  const site = mkdtempSync(join(tmpdir(), 'site-'));
  const w = join(site, 'wasm');
  spawnSync('mkdir', ['-p', w, join(site, 'releases')]);
  for (const f of ['scan.mjs', 'scan.wasm', 'hlsl.mjs', 'hlsl.wasm', 'pack.mjs', 'pack.wasm']) copyFileSync(join(wasmDir, f), join(w, f));
  if (realDxcWasm) {
    for (const f of ['dxc_web.mjs', 'dxc_web.wasm']) copyFileSync(join(wasmDir, f), join(w, f));
  } else {
    copyFileSync(join(here, 'fixtures/dxc_native_shim.mjs'), join(w, 'dxc_web.mjs'));
    writeFileSync(join(w, 'dxc_web.wasm'), '');
  }
  copyFileSync(join(here, '../../shaders/XenosRecomp/shader_common.h'), join(w, 'shader_common.h'));
  const nro = Buffer.from('NRO0-fake-build-bytes'.repeat(100));
  const toml = Buffer.from('# fake toml\n');
  writeFileSync(join(site, 'releases/masseffect-nx.nro'), nro);
  writeFileSync(join(site, 'masseffect.toml'), toml);
  const sha = (b) => createHash('sha256').update(b).digest('hex');
  writeFileSync(join(site, 'releases/manifest.json'), JSON.stringify({ tag: 'vtest', assets: { 'masseffect-nx.nro': { size: nro.length, sha256: sha(nro) }, 'masseffect.toml': { size: toml.length, sha256: sha(toml) } } }));
  return { site, nro, toml };
}

function fileFetch(siteDir) {
  return async (url, init = {}) => {
    const u = new URL(url);
    const path = join(siteDir, decodeURIComponent(u.pathname.replace(/^.*\/site-[^/]+\//, '')));
    const ok = existsSync(path);
    const body = ok && init.method !== 'HEAD' ? readFileSync(path) : null;
    return {
      ok, status: ok ? 200 : 404,
      headers: { get: (k) => (k.toLowerCase() === 'content-length' && body ? String(body.length) : null) },
      json: async () => JSON.parse(body.toString()),
      arrayBuffer: async () => body.buffer.slice(body.byteOffset, body.byteOffset + body.length),
      body: null,
    };
  };
}

function inProcessWorkers() {
  const kinds = { scan: ScanHandler, shader: ShaderHandler, pack: PackHandler };
  return (kind) => {
    const handler = new kinds[kind]();
    let chain = Promise.resolve();
    let dead = false;
    const w = {
      onmessage: null, onerror: null,
      postMessage(msg) {
        chain = chain.then(() => dead ? null : handler.handle(msg, (m) => setImmediate(() => !dead && w.onmessage?.({ data: m }))));
      },
      terminate() { dead = true; },
    };
    return w;
  };
}

function discFile(rel) {
  const p = join(disc, rel);
  const b = readFileSync(p);
  return { path: rel, size: b.length, blob: new Blob([b]) };
}

const edition = CONFIG.editions[0];
const base = (site) => pathToFileURL(site + '/').href;

const smallDisc = () => [
  'default.xex', 'Layer0/MEInit/Coalesced.ini', 'Layer0/MEInit/BIOG_Powers.xxx', 'Layer0/MEInit/GameFramework.xxx', 'Layer1/Splash.bmp',
  '$SystemUpdate/system.manifest', 'nxeart',
].map(discFile).concat([{ path: 'FillerFiles/0042sb3o.jnk', size: 3, blob: new Blob([new Uint8Array(3)]) }]);

test('full run: scan, translate, compile with DXC, pack, zip', { skip, timeout: 600000 }, async () => {
  const { site, nro, toml } = makeSite();
  try {
    const sink = new MemorySink();
    const stages = new Set();
    const logs = [];
    const result = await run({
      config: CONFIG, edition, files: smallDisc(), mode: 'full', sink, createWorker: inProcessWorkers(),
      baseUrl: base(site), fetchImpl: fileFetch(site), onProgress: (id) => stages.add(id), onLog: (t, l) => logs.push([l, t]),
    });
    assert.deepEqual([...stages].sort(), ['download', 'pack', 'scan', 'translate', 'zip']);
    assert.ok(result.shaders.ok > 50, `translated ${result.shaders.ok}`);
    assert.equal(result.tag, 'vtest');
    const zipBytes = sink.concat();
    assert.equal(zipBytes.length, result.zipBytes);
    const out = join(site, 'out.zip');
    writeFileSync(out, zipBytes);
    const py = spawnSync('python3', ['-c', `
import zipfile, json, sys
z = zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None
print(json.dumps({i.filename: i.file_size for i in z.infolist()}))`, out], { encoding: 'utf8' });
    assert.equal(py.status, 0, py.stderr);
    const listing = JSON.parse(py.stdout);
    const names = Object.keys(listing);
    assert.deepEqual(names.slice(0, 4), ['masseffect-nx/masseffect-nx.nro', 'masseffect-nx/masseffect.toml', 'masseffect-nx/masseffect_shaders.mesp', 'masseffect-nx/masseffect_shaders.mesp.idx']);
    assert.equal(listing['masseffect-nx/masseffect-nx.nro'], nro.length);
    assert.equal(listing['masseffect-nx/masseffect.toml'], toml.length);
    assert.ok(listing['masseffect-nx/masseffect_shaders.mesp'] > 10000);
    assert.ok(names.includes('masseffect-nx/game_root/default.xex'));
    assert.ok(names.includes('masseffect-nx/game_root/Layer0/MEInit/Coalesced.ini'));
    assert.ok(names.includes('masseffect-nx/game_root/Layer1/Splash.bmp'));
    assert.ok(!names.some((n) => /SystemUpdate|FillerFiles|nxeart/.test(n)), 'skipped files must not be in the zip');
    // keep the outputs for the external format check
    if (process.env.MASSEFFECT_TEST_KEEP) writeFileSync(process.env.MASSEFFECT_TEST_KEEP, zipBytes);
    // the update zip has only the four build files
    const sink2 = new MemorySink();
    await run({ config: CONFIG, edition, files: smallDisc(), mode: 'update', sink: sink2, createWorker: inProcessWorkers(), baseUrl: base(site), fetchImpl: fileFetch(site) });
    writeFileSync(out, sink2.concat());
    const py2 = spawnSync('python3', ['-c', 'import zipfile,sys;z=zipfile.ZipFile(sys.argv[1]);print(len(z.namelist()))', out], { encoding: 'utf8' });
    assert.equal(py2.stdout.trim(), '4');
  } finally {
    rmSync(site, { recursive: true, force: true });
  }
});

test('a missing DXC module gives a clear message', { skip }, async () => {
  const { site } = makeSite();
  try {
    rmSync(join(site, 'wasm/dxc_web.mjs'));
    await assert.rejects(
      run({ config: CONFIG, edition, files: smallDisc(), mode: 'update', sink: new MemorySink(), createWorker: inProcessWorkers(), baseUrl: base(site), fetchImpl: fileFetch(site) }),
      (e) => e instanceof UserError && e.code === 'wasm-missing' && /dxc_web\.mjs/.test(e.message));
  } finally { rmSync(site, { recursive: true, force: true }); }
});

test('a checksum mismatch of the NRO is refused', { skip }, async () => {
  const { site } = makeSite();
  try {
    writeFileSync(join(site, 'releases/masseffect-nx.nro'), Buffer.from('tampered'.repeat(300)));
    await assert.rejects(
      run({ config: CONFIG, edition, files: smallDisc(), mode: 'update', sink: new MemorySink(), createWorker: inProcessWorkers(), baseUrl: base(site), fetchImpl: fileFetch(site) }),
      (e) => e instanceof UserError && e.code === 'integrity');
  } finally { rmSync(site, { recursive: true, force: true }); }
});

test('cancelling stops the run and aborts the sink', { skip }, async () => {
  const { site } = makeSite();
  try {
    const ac = new AbortController();
    let aborted = false;
    const sink = { write() {}, close() {}, abort() { aborted = true; } };
    await assert.rejects(
      run({ config: CONFIG, edition, files: smallDisc(), mode: 'update', sink, createWorker: inProcessWorkers(), baseUrl: base(site), fetchImpl: fileFetch(site), signal: ac.signal,
        onProgress: (id) => { if (id === 'translate') ac.abort(); } }),
      (e) => e instanceof Cancelled);
    assert.equal(aborted, true);
  } finally { rmSync(site, { recursive: true, force: true }); }
});
