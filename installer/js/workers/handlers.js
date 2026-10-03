// The logic of the three kinds of worker, written as plain classes: `handle(message, post)` where post(message,
// transferList) sends an answer. The worker entry files (scan.worker.js, shader.worker.js, pack.worker.js) only wire
// them to postMessage, and the Node tests call them in-process.
//
// The WebAssembly programs are command line programs compiled with Emscripten (see shaders/wasm/): inputs are
// written into the module's in-memory file system (FS), callMain([...]) runs the program, outputs are read back.
// A trap inside WebAssembly (the documented crashes of the translator) leaves the instance unusable, so every
// handler drops a crashed instance and creates a fresh one for the next job.

async function createModule(url, extra = {}) {
  const factory = (await import(/* @vite-ignore */ url)).default;
  return factory(extra);
}

function describeError(e) {
  if (!e) return 'unknown error';
  return String(e.message ?? e).split('\n')[0].slice(0, 300);
}

/** Runs callMain and separates "the program ended with exit status N" from "the instance crashed". */
function runMain(module, args) {
  try {
    const code = module.callMain(args);
    return { code: typeof code === 'number' ? code : 0, crashed: false };
  } catch (e) {
    if (e && (e.name === 'ExitStatus' || (typeof e.status === 'number' && /terminated|exit/i.test(String(e.message))))) {
      return { code: e.status, crashed: false };
    }
    return { code: -1, crashed: true, error: describeError(e) };
  }
}

function tryUnlink(FS, path) {
  try { FS.unlink(path); } catch { /* not there */ }
}

function removeTree(FS, dir) {
  try {
    for (const name of FS.readdir(dir)) {
      if (name === '.' || name === '..') continue;
      tryUnlink(FS, `${dir}/${name}`);
    }
    FS.rmdir(dir);
  } catch { /* not there */ }
}

// ---- scan --------------------------------------------------------------------------------------------------------
/** ue3_shader_scan: finds the shader containers inside Unreal packages (LZO chunks and raw). */
export class ScanHandler {
  constructor() { this.module = null; this.url = null; this.lines = []; this.sent = new Set(); }

  async #module() {
    if (!this.module) {
      this.lines = [];
      this.module = await createModule(this.url, { print: (l) => this.lines.push(l), printErr: (l) => this.lines.push(l) });
      this.module.FS.mkdir('/out');
      this.sent = new Set();
    }
    return this.module;
  }

  async handle(msg, post) {
    if (msg.type === 'init') {
      this.url = msg.urls.scan;
      try { await this.#module(); post({ type: 'ready' }); } catch (e) { post({ type: 'fatal', code: 'wasm', file: msg.urls.scan, error: describeError(e) }); }
      return;
    }
    if (msg.type !== 'scan') return;
    const { id, name } = msg;
    try {
      const data = new Uint8Array(await msg.blob.arrayBuffer());
      const m = await this.#module();
      this.lines = [];
      m.FS.writeFile('/pkg.xxx', data);
      const r = runMain(m, ['/out', '/pkg.xxx']);
      tryUnlink(m.FS, '/pkg.xxx');
      if (r.crashed) {
        this.module = null; // a fresh instance (and an empty /out) for the next package
        post({ type: 'scanned', id, name, ok: false, reason: `scanner crashed: ${r.error}`, containers: [] });
        return;
      }
      const summary = this.lines.find((l) => /containers seen/.test(l)) ?? '';
      const fresh = /(\d+) new files/.exec(summary);
      const containers = [];
      const transfer = [];
      if (!fresh || Number(fresh[1]) > 0) {
        for (const file of m.FS.readdir('/out')) {
          if (file === '.' || file === '..' || this.sent.has(file)) continue;
          this.sent.add(file);
          const bytes = m.FS.readFile(`/out/${file}`).slice();
          containers.push({ name: file, data: bytes });
          transfer.push(bytes.buffer);
        }
      }
      post({ type: 'scanned', id, name, ok: r.code === 0, reason: r.code === 0 ? '' : `scanner exit ${r.code}`, containers, summary }, transfer);
    } catch (e) {
      post({ type: 'scanned', id, name, ok: false, reason: describeError(e), containers: [] });
    }
  }
}

// ---- translate + compile -----------------------------------------------------------------------------------------
/** One container in, one SPIR-V module (or a reason to skip it) out: translator (hlsl) then DXC. */
export class ShaderHandler {
  constructor() { this.hlsl = null; this.dxc = null; this.urls = null; this.common = null; this.counter = 0; this.log = []; }

  async #hlsl() {
    if (!this.hlsl) {
      this.log = [];
      this.hlsl = await createModule(this.urls.hlsl, { print: (l) => this.log.push(l), printErr: (l) => this.log.push(l) });
      this.hlsl.FS.writeFile('/shader_common.h', this.common);
      this.hlsl.FS.mkdir('/in');
    }
    return this.hlsl;
  }

  async #dxc() {
    if (!this.dxc) {
      this.dxc = await createModule(this.urls.dxc, { print: () => {}, printErr: (l) => { this.dxcErr = l; } });
    }
    return this.dxc;
  }

  async handle(msg, post) {
    if (msg.type === 'init') {
      this.urls = msg.urls;
      let which = msg.urls.shaderCommon;
      try {
        this.common = new Uint8Array(await fetchBytes(msg.urls.shaderCommon));
        which = msg.urls.hlsl;
        await this.#hlsl();
        which = msg.urls.dxc;
        await this.#dxc();
        post({ type: 'ready' });
      } catch (e) {
        post({ type: 'fatal', code: 'wasm', file: which, error: describeError(e) });
      }
      return;
    }
    if (msg.type !== 'job') return;
    const { id, name } = msg;
    const stem = name.replace(/\.bin$/, '');
    const fail = (stage, reason) => post({ type: 'result', id, name, ok: false, stage, reason });
    // 1. Xbox 360 microcode -> HLSL
    let hlslBytes;
    try {
      const h = await this.#hlsl();
      const out = `/o${++this.counter}`;
      h.FS.writeFile(`/in/${name}`, msg.data);
      this.log = [];
      const r = runMain(h, ['/in', out, '/shader_common.h']);
      tryUnlink(h.FS, `/in/${name}`);
      if (r.crashed) {
        this.hlsl = null;
        return fail('translate', `translator crashed (${r.error})`);
      }
      let produced = null;
      try { if (h.FS.readdir(out).includes(`${stem}.hlsl`)) produced = h.FS.readFile(`${out}/${stem}.hlsl`); } catch { /* none */ }
      removeTree(h.FS, out);
      if (!produced) {
        const why = this.log.find((l) => /rejected|unknown|produced nothing|truncated/.test(l)) ?? `translator exit ${r.code}`;
        return fail('translate', why.trim());
      }
      hlslBytes = produced;
    } catch (e) {
      this.hlsl = null;
      return fail('translate', describeError(e));
    }
    // 2. HLSL -> SPIR-V
    try {
      const d = await this.#dxc();
      d.FS.writeFile('/s.hlsl', hlslBytes);
      tryUnlink(d.FS, '/s.spv');
      this.dxcErr = '';
      let code;
      try {
        code = d.ccall('compile', 'number', ['string', 'string', 'number'], ['/s.hlsl', '/s.spv', stem.startsWith('vs_') ? 1 : 0]);
      } catch (e) {
        this.dxc = null; // crashed: new instance next time
        return fail('dxc', `DXC crashed (${describeError(e)})`);
      }
      if (code !== 0) return fail('dxc', `DXC rejected it (code ${code}${this.dxcErr ? `: ${this.dxcErr.slice(0, 160)}` : ''})`);
      const spirv = d.FS.readFile('/s.spv').slice();
      tryUnlink(d.FS, '/s.spv');
      post({ type: 'result', id, name, ok: true, spirv }, [spirv.buffer]);
    } catch (e) {
      this.dxc = null;
      fail('dxc', describeError(e));
    }
  }
}

async function fetchBytes(url) {
  if (url.startsWith('file:')) {
    const { readFile } = await import('node:fs/promises');
    const b = await readFile(new URL(url));
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.length);
  }
  const r = await fetch(url);
  if (!r.ok) throw new Error(`HTTP ${r.status} for ${url}`);
  return r.arrayBuffer();
}

// ---- pack --------------------------------------------------------------------------------------------------------
/** me_pack_shaders: collects (container, SPIR-V) pairs, writes the package and the index, serves them in chunks. */
export class PackHandler {
  constructor() { this.module = null; this.lines = []; this.added = 0; }

  async handle(msg, post) {
    const FS = this.module?.FS;
    switch (msg.type) {
      case 'init':
        try {
          this.module = await createModule(msg.urls.pack, { print: (l) => this.lines.push(l), printErr: (l) => this.lines.push(l) });
          this.module.FS.mkdir('/c'); this.module.FS.mkdir('/s'); this.module.FS.mkdir('/o');
          post({ type: 'ready' });
        } catch (e) { post({ type: 'fatal', code: 'wasm', file: msg.urls.pack, error: describeError(e) }); }
        return;
      case 'add':
        for (const item of msg.items) {
          const stem = item.name.replace(/\.bin$/, '');
          FS.writeFile(`/c/${stem}.bin`, item.container);
          FS.writeFile(`/s/${stem}.spv`, item.spirv);
          this.added++;
        }
        post({ type: 'added', id: msg.id, total: this.added });
        return;
      case 'pack': {
        this.lines = [];
        const names = msg.names;
        const r = runMain(this.module, ['/c', '/s', `/o/${names.package}`]);
        if (r.crashed || r.code !== 0) {
          const detail = r.crashed ? r.error : (this.lines.slice(-3).join(' ') || `exit ${r.code}`);
          post({ type: 'packed', ok: false, error: detail, memory: /memory|alloc|abort|grow/i.test(detail) });
          return;
        }
        // The inputs are no longer needed: free the in-memory copies before the files are read out.
        removeTree(FS, '/c'); removeTree(FS, '/s');
        const sizes = {};
        sizes.package = FS.stat(`/o/${names.package}`).size;
        sizes.index = FS.stat(`/o/${names.package}.idx`).size; // the packer names the index <package>.idx
        post({ type: 'packed', ok: true, sizes, summary: this.lines.filter((l) => /shaders,/.test(l)).join(' ') });
        return;
      }
      case 'read': {
        const path = `/o/${msg.file}`;
        const stream = FS.open(path, 'r');
        const buf = new Uint8Array(msg.length);
        const n = FS.read(stream, buf, 0, msg.length, msg.offset);
        FS.close(stream);
        const out = n === buf.length ? buf : buf.slice(0, n);
        post({ type: 'chunk', id: msg.id, data: out }, [out.buffer]);
        return;
      }
      case 'dispose':
        this.module = null;
        post({ type: 'disposed' });
        return;
      default:
    }
  }
}
