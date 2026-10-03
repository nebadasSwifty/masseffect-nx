// The installer's work, start to finish. Runs in the page's main thread and drives Web Workers; it never touches the
// DOM, so the Node tests run it with in-process workers.
//
//   download  the build (NRO) and masseffect.toml from the site, checked against manifest.json
//   scan      read the disc's Unreal packages and find the shader containers          (workers: scan.mjs)
//   translate every container: Xbox 360 microcode -> HLSL -> SPIR-V                   (workers: hlsl.mjs + dxc_web.mjs)
//   pack      make masseffect_shaders.mesp and its .idx                               (worker: pack.mjs)
//   zip       stream the zip: nro, toml, shader package, then the disc files (full mode)
import { ZipWriter } from './zip.js';
import { planGameFiles, scanCandidates, sha256Hex } from './plan.js';

export class Cancelled extends Error {
  constructor() { super('Cancelled'); this.name = 'Cancelled'; }
}

export class UserError extends Error {
  constructor(message, code = 'error') { super(message); this.name = 'UserError'; this.code = code; }
}

const stageIds = ['download', 'scan', 'translate', 'pack', 'zip'];
export { stageIds };

function abs(rel, baseUrl) {
  return new URL(rel, baseUrl).href;
}

export function wasmUrls(config, baseUrl) {
  const w = config.wasm;
  return {
    scan: abs(w.dir + w.scan, baseUrl),
    hlsl: abs(w.dir + w.hlsl, baseUrl),
    dxc: abs(w.dir + w.dxc, baseUrl),
    pack: abs(w.dir + w.pack, baseUrl),
    shaderCommon: abs(w.dir + w.shaderCommon, baseUrl),
  };
}

/** HEAD-checks every file of the shader toolchain. Returns the list of missing URLs (empty = all present). */
export async function checkToolchain(config, baseUrl, fetchImpl = fetch) {
  const w = config.wasm;
  const names = [w.scan, w.hlsl, w.dxc, w.pack, w.shaderCommon, ...w.extraFiles];
  const missing = [];
  await Promise.all(names.map(async (n) => {
    const url = abs(w.dir + n, baseUrl);
    try {
      const r = await fetchImpl(url, { method: 'HEAD' });
      if (!r.ok) missing.push(w.dir + n);
    } catch {
      missing.push(w.dir + n);
    }
  }));
  return missing.sort();
}

async function fetchWithProgress(url, fetchImpl, signal, onBytes) {
  let r;
  try {
    r = await fetchImpl(url, { signal, cache: 'no-cache' });
  } catch (e) {
    if (signal?.aborted) throw new Cancelled();
    throw new UserError(`Could not download ${url}: ${e.message}. Check your connection and try again.`, 'network');
  }
  if (!r.ok) {
    throw new UserError(r.status === 404
      ? `${url} was not found on the site (HTTP 404). The build for this edition has not been published yet.`
      : `Could not download ${url} (HTTP ${r.status}).`, 'download');
  }
  const total = Number(r.headers.get('content-length')) || 0;
  const parts = [];
  let got = 0;
  if (r.body?.getReader) {
    const reader = r.body.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
      got += value.length;
      onBytes?.(got, total);
    }
  } else {
    const b = new Uint8Array(await r.arrayBuffer());
    parts.push(b);
    got = b.length;
    onBytes?.(got, total);
  }
  const out = new Uint8Array(got);
  let p = 0;
  for (const part of parts) { out.set(part, p); p += part.length; }
  return out;
}

/** Fetches the NRO of the edition and masseffect.toml; verifies both against releases/manifest.json when present. */
export async function fetchBuild(config, edition, { baseUrl, fetchImpl = fetch, signal, progress, log }) {
  let manifest = null;
  try {
    const r = await fetchImpl(abs(config.build.manifest, baseUrl), { signal, cache: 'no-cache' });
    if (r.ok) manifest = await r.json();
  } catch (e) {
    if (signal?.aborted) throw new Cancelled();
  }
  const files = [
    { key: 'nro', label: edition.nro, url: abs(config.build.siteDir + edition.nro, baseUrl), manifestName: edition.nro },
    { key: 'toml', label: config.files.toml, url: abs(config.build.toml, baseUrl), manifestName: config.files.toml },
  ];
  const out = {};
  let doneBytes = 0;
  const totalBytes = files.reduce((a, f) => a + (manifest?.assets?.[f.manifestName]?.size ?? 0), 0) || 0;
  for (const f of files) {
    const base = doneBytes;
    const data = await fetchWithProgress(f.url, fetchImpl, signal, (got, total) => {
      progress({ done: base + got, total: totalBytes || base + (total || got), label: `Downloading ${f.label}` });
    });
    const entry = manifest?.assets?.[f.manifestName];
    if (entry) {
      if (entry.size !== data.length) throw new UserError(`${f.label} has the wrong size (${data.length}, expected ${entry.size}). Try again in a minute; the site may be updating.`, 'integrity');
      if (entry.sha256 && (await sha256Hex(data)) !== entry.sha256.toLowerCase()) {
        throw new UserError(`${f.label} does not match its checksum in manifest.json. Try again in a minute; the site may be updating.`, 'integrity');
      }
    } else if (f.key === 'nro') {
      log?.('No manifest.json on the site: the build could not be verified against a checksum.', 'warn');
    }
    out[f.key] = data;
    doneBytes += data.length;
  }
  out.tag = manifest?.tag ?? null;
  progress({ done: 1, total: 1, label: 'Build downloaded' });
  return out;
}

// ---- worker plumbing -------------------------------------------------------------------------------------------
/** One outstanding request at a time per worker: send a message, wait for the next message of an expected type. */
class Remote {
  constructor(worker) {
    this.worker = worker;
    this.waiting = null;
    worker.onmessage = (e) => this.#message(e.data);
    worker.onerror = (e) => this.#fail(new Error(e?.message || 'worker error'));
  }
  #message(m) {
    if (!this.waiting) return;
    const w = this.waiting;
    if (m.type === 'fatal') { this.waiting = null; w.reject(Object.assign(new UserError(`${m.file}: ${m.error}`, 'wasm'), { file: m.file })); return; }
    if (m.type === w.type) { this.waiting = null; clearTimeout(w.timer); w.resolve(m); }
  }
  #fail(e) {
    if (!this.waiting) return;
    const w = this.waiting;
    this.waiting = null;
    clearTimeout(w.timer);
    w.reject(e);
  }
  call(message, transfer, replyType, timeoutMs) {
    return new Promise((resolve, reject) => {
      const w = { type: replyType, resolve, reject, timer: null };
      if (timeoutMs) w.timer = setTimeout(() => { this.waiting = null; reject(Object.assign(new Error('timeout'), { timeout: true })); }, timeoutMs);
      this.waiting = w;
      this.worker.postMessage(message, transfer ?? []);
    });
  }
  terminate() { this.worker.terminate?.(); }
  cancel() { this.#fail(new Cancelled()); this.terminate(); }
}

class Workers {
  constructor(create) { this.create = create; this.live = new Set(); }
  spawn(kind) { const r = new Remote(this.create(kind)); this.live.add(r); return r; }
  kill(r) { r.terminate(); this.live.delete(r); }
  killAll() { for (const r of [...this.live]) r.cancel(); this.live.clear(); }
}

function throwIfCancelled(signal) { if (signal?.aborted) throw new Cancelled(); }

// ---- stages ----------------------------------------------------------------------------------------------------
async function scanStage({ workers, urls, files, config, signal, progress, log }) {
  const candidates = scanCandidates(files, config.disc);
  if (candidates.length === 0) throw new UserError('The disc has no Unreal package files (*.xxx), so there are no shaders to build.', 'disc');
  const totalBytes = candidates.reduce((a, f) => a + f.size, 0);
  const containers = new Map();
  const queue = [...candidates];
  let doneBytes = 0, doneFiles = 0, skipped = 0;
  const n = Math.max(1, Math.min(config.limits.scanWorkers, candidates.length));
  const runners = [];
  for (let i = 0; i < n; i++) {
    const remote = workers.spawn('scan');
    runners.push((async () => {
      await remote.call({ type: 'init', urls }, [], 'ready');
      for (let f = queue.shift(); f; f = queue.shift()) {
        throwIfCancelled(signal);
        const r = await remote.call({ type: 'scan', id: doneFiles, name: f.path, blob: f.blob }, [], 'scanned');
        if (!r.ok) { skipped++; log(`Package ${f.path} skipped: ${r.reason}`, 'warn'); }
        for (const c of r.containers) if (!containers.has(c.name)) containers.set(c.name, c.data);
        doneBytes += f.size; doneFiles++;
        progress({ done: doneBytes, total: totalBytes, label: `Reading the disc: ${doneFiles} of ${candidates.length} packages, ${containers.size.toLocaleString('en-US')} shaders found` });
      }
      workers.kill(remote);
    })());
  }
  await Promise.all(runners);
  log(`Scan: ${candidates.length} packages read, ${containers.size} distinct shader containers${skipped ? `, ${skipped} packages skipped` : ''}.`, 'info');
  return containers;
}

async function translateStage({ workers, urls, containers, config, signal, progress, log, packRemote }) {
  const names = [...containers.keys()];
  const total = names.length;
  if (total === 0) throw new UserError('No shader containers were found on the disc. Is this a complete Mass Effect disc?', 'disc');
  const queue = names.slice();
  const failures = [];
  let done = 0, ok = 0;
  let batch = [];
  let addChain = Promise.resolve();
  const flush = () => {
    const items = batch;
    batch = [];
    if (items.length) {
      addChain = addChain.then(() => packRemote.call({ type: 'add', id: 0, items }, items.flatMap((i) => [i.spirv.buffer]), 'added'));
    }
  };
  const jobMs = config.limits.jobTimeoutMs;
  const n = Math.max(1, Math.min(config.limits.maxShaderWorkers, total, workerBudget()));
  const startWorker = async () => {
    const remote = workers.spawn('shader');
    await remote.call({ type: 'init', urls }, [], 'ready', 120000);
    return remote;
  };
  const runners = [];
  for (let i = 0; i < n; i++) {
    runners.push((async () => {
      let remote = await startWorker();
      for (let name = queue.shift(); name; name = queue.shift()) {
        throwIfCancelled(signal);
        const data = containers.get(name);
        let r;
        try {
          r = await remote.call({ type: 'job', id: done, name, data }, [], 'result', jobMs);
        } catch (e) {
          if (!e.timeout) throw e;
          workers.kill(remote);
          r = { ok: false, stage: 'translate', reason: `no answer after ${Math.round(jobMs / 1000)} s` };
          remote = await startWorker();
        }
        done++;
        if (r.ok) {
          ok++;
          batch.push({ name, container: data, spirv: r.spirv });
          if (batch.length >= 64) flush();
        } else {
          failures.push({ name, stage: r.stage, reason: r.reason });
        }
        if (done % 50 === 0 || done === total) {
          progress({ done, total, label: `Translating shaders: ${done.toLocaleString('en-US')} of ${total.toLocaleString('en-US')}${failures.length ? ` (${failures.length} skipped)` : ''}` });
        }
      }
      workers.kill(remote);
    })());
  }
  await Promise.all(runners);
  flush();
  await addChain;
  progress({ done: total, total, label: `Translating shaders: ${total.toLocaleString('en-US')} of ${total.toLocaleString('en-US')}${failures.length ? ` (${failures.length} skipped)` : ''}` });
  for (const f of failures) log(`Skipped ${f.name} (${f.stage}): ${f.reason}`, 'skip');
  log(`Translated ${ok} of ${total} containers; ${failures.length} skipped (a few skipped containers are expected).`, failures.length ? 'warn' : 'info');
  return { ok, failures, total };
}

function workerBudget() {
  const hc = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
  return Math.max(1, hc - 1);
}

async function* readChunks(blob, chunkBytes, signal) {
  let next = null;
  const read = (pos) => blob.slice(pos, Math.min(pos + chunkBytes, blob.size)).arrayBuffer().then((b) => new Uint8Array(b));
  if (blob.size === 0) return;
  next = read(0);
  for (let pos = 0; pos < blob.size; pos += chunkBytes) {
    throwIfCancelled(signal);
    const chunk = await next;
    const after = pos + chunkBytes;
    next = after < blob.size ? read(after) : null;
    yield chunk;
  }
}

async function* bytesOnce(bytes) { if (bytes.length) yield bytes; }

async function* packageChunks(packRemote, file, size, chunkBytes, signal) {
  for (let offset = 0; offset < size; offset += chunkBytes) {
    throwIfCancelled(signal);
    const length = Math.min(chunkBytes, size - offset);
    const r = await packRemote.call({ type: 'read', id: offset, file, offset, length }, [], 'chunk');
    yield r.data;
  }
}

/**
 * Runs the whole thing.
 * options: { config, edition, files (disc files, re-rooted), mode: 'full'|'update', sink, createWorker(kind),
 *            baseUrl, fetchImpl, signal, onProgress(stageId, {done,total,label}), onLog(text, level) }
 * Returns { zipBytes, shaders: {ok, failures, total}, sinkResult }.
 */
export async function run(options) {
  const { config, edition, files, mode, sink, createWorker, baseUrl, signal } = options;
  const fetchImpl = options.fetchImpl ?? fetch;
  const log = options.onLog ?? (() => {});
  const stage = (id) => (p) => options.onProgress?.(id, p);
  const workers = new Workers(createWorker);
  const urls = wasmUrls(config, baseUrl);
  const onAbort = () => workers.killAll();
  signal?.addEventListener('abort', onAbort);
  const names = config.files;
  const root = config.zip.root;
  try {
    if (names.shadersIndex !== `${names.shaders}.idx`) {
      throw new UserError('config.js: files.shadersIndex must be files.shaders + ".idx" (the packer derives the index name).', 'config');
    }
    throwIfCancelled(signal);
    const missing = await checkToolchain(config, baseUrl, fetchImpl);
    if (missing.length) {
      throw new UserError(`The shader tools are not on this site: missing ${missing.join(', ')}. ` +
        'The site build did not include the WebAssembly tools (see installer/README.md); the shaders cannot be made in the browser until it does.', 'wasm-missing');
    }
    const build = await fetchBuild(config, edition, { baseUrl, fetchImpl, signal, progress: stage('download'), log });
    log(build.tag ? `Build ${build.tag} downloaded.` : 'Build downloaded.', 'info');

    throwIfCancelled(signal);
    const containers = await scanStage({ workers, urls, files, config, signal, progress: stage('scan'), log });

    // The pack worker starts now so SPIR-V goes straight into it as the translators finish.
    const packRemote = workers.spawn('pack');
    await packRemote.call({ type: 'init', urls }, [], 'ready');
    const shaders = await translateStage({ workers, urls, containers, config, signal, progress: stage('translate'), log, packRemote });
    containers.clear();
    if (shaders.ok === 0) throw new UserError('No shader could be translated. The WebAssembly tools may be broken; see the log.', 'shaders');
    if (shaders.ok < config.limits.expectedContainers * 0.9) {
      log(`Only ${shaders.ok} shaders were made (a complete disc gives about ${config.limits.expectedContainers - 60}). The game may be missing graphics.`, 'warn');
    }

    throwIfCancelled(signal);
    stage('pack')({ done: 0, total: 1, label: 'Packing the shader library (this takes a while and a lot of memory)' });
    const packed = await packRemote.call({ type: 'pack', names: { package: names.shaders, index: names.shadersIndex } }, [], 'packed');
    if (!packed.ok) {
      throw new UserError(packed.memory
        ? `Packing the shaders ran out of memory (${packed.error}). Close other tabs and programs and try again in a desktop browser with at least 8 GB of RAM.`
        : `Packing the shaders failed: ${packed.error}`, 'pack');
    }
    stage('pack')({ done: 1, total: 1, label: `Shader library packed: ${packed.sizes.package.toLocaleString('en-US')} bytes + ${packed.sizes.index.toLocaleString('en-US')} index` });
    log(packed.summary || 'Packed.', 'info');

    // ---- the zip ----
    throwIfCancelled(signal);
    const plan = mode === 'full' ? planGameFiles(files, config.disc) : null;
    const entries = [
      { name: `${root}/${names.nro}`, size: build.nro.length, chunks: () => bytesOnce(build.nro) },
      { name: `${root}/${names.toml}`, size: build.toml.length, chunks: () => bytesOnce(build.toml) },
      { name: `${root}/${names.shaders}`, size: packed.sizes.package, chunks: () => packageChunks(packRemote, names.shaders, packed.sizes.package, 8 * 1024 * 1024, signal) },
      { name: `${root}/${names.shadersIndex}`, size: packed.sizes.index, chunks: () => packageChunks(packRemote, names.shadersIndex, packed.sizes.index, 8 * 1024 * 1024, signal) },
    ];
    if (plan) {
      for (const f of plan.copy) {
        entries.push({ name: `${root}/${config.zip.gameRootDir}/${f.path}`, size: f.size, chunks: () => readChunks(f.blob, config.limits.copyChunkBytes, signal) });
      }
    }
    const totalBytes = entries.reduce((a, e) => a + e.size, 0);
    let written = 0;
    const zip = new ZipWriter({ write: async (b) => { await sink.write(b); written += b.length; } });
    let fileNo = 0;
    for (const e of entries) {
      throwIfCancelled(signal);
      const tick = async function* () {
        for await (const c of e.chunks()) {
          yield c;
          stage('zip')({ done: written, total: totalBytes, label: `Writing the zip: file ${fileNo + 1} of ${entries.length}` });
        }
      };
      await zip.addFile(e.name, e.size, tick());
      fileNo++;
    }
    await zip.finish();
    stage('zip')({ done: totalBytes, total: totalBytes, label: 'Finishing the zip' });
    const sinkResult = await sink.close();
    return { zipBytes: zip.bytesWritten, shaders, sinkResult, tag: build.tag };
  } catch (e) {
    try { await sink.abort?.(); } catch { /* ignore */ }
    throw e;
  } finally {
    signal?.removeEventListener('abort', onAbort);
    workers.killAll();
  }
}
