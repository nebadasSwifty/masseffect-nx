// Where the NSP goes. The writer (js/nsp.js) streams the NSP once and finally writes its header over a placeholder at
// offset 0, so a target needs positioned writes:
//   1. one file from showSaveFilePicker (Chromium: Chrome, Edge, Opera): FileSystemWritableFileStream with
//      write({type: 'write', position, data}). Nothing is buffered in memory.
//   2. a FAT32 split folder from showDirectoryPicker (Chromium): <name>.nsp/00, 01, ... of 0xFFFF0000 bytes each, the
//      layout DBI, Tinfoil and Goldleaf install from a FAT32 card.
//   3. memory (any browser), then a download: only for small outputs (an update NSP is ~75 MB).
//   4. the connected Switch over USB (Chromium, WebUSB): UsbNspSink. Nothing is written anywhere: js/nsp.js sees
//      serve() and hands over an NspImage that the console reads by range (Sphaira's USB install, js/usb_install.js).
// Firefox and Safari have neither picker nor WebUSB: a full NSP (8-9 GB) cannot be written there; the page says so.
import { FAT32_PART } from './nsp.js';
import { serveFiles, webUsbSupport } from './usb_install.js';

function triggerDownload(blob, name) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.style.display = 'none';
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60 * 60 * 1000);
}

/** One file written through a FileSystemWritableFileStream (or anything with the same write() forms). */
export class FileNspSink {
  constructor(writable, name) { this.writable = writable; this.name = name; this.kind = 'file'; this.bytes = 0; }
  async write(bytes) { await this.writable.write(bytes); this.bytes += bytes.length; }
  async writeAt(position, bytes) { await this.writable.write({ type: 'write', position, data: bytes }); }
  async close() { await this.writable.close(); return 'saved'; }
  async abort() { try { await this.writable.abort(); } catch { /* already closed */ } }
}

/**
 * A FAT32 split NSP: parts of `partSize` bytes. openPart(index) -> a writable with write({type:'write', position,
 * data}) and close(); remove() deletes what was written (on cancel or error).
 */
export class SplitNspSink {
  constructor(openPart, { partSize = FAT32_PART, remove = null, name = '' } = {}) {
    this.openPart = openPart;
    this.partSize = partSize;
    this.remove = remove;
    this.name = name;
    this.parts = new Map();
    this.pos = 0;
    this.kind = 'split';
  }

  async #part(index) {
    if (!this.parts.has(index)) this.parts.set(index, await this.openPart(index));
    return this.parts.get(index);
  }

  async #writeRange(position, bytes) {
    let off = 0;
    while (off < bytes.length) {
      const index = Math.floor((position + off) / this.partSize);
      const inner = (position + off) % this.partSize;
      const n = Math.min(bytes.length - off, this.partSize - inner);
      const part = await this.#part(index);
      await part.write({ type: 'write', position: inner, data: bytes.subarray(off, off + n) });
      off += n;
    }
  }

  async write(bytes) {
    // Parts before the current one are complete: close them (part 0 stays open for the final header).
    const index = Math.floor(this.pos / this.partSize);
    for (const [i, p] of this.parts) {
      if (i > 0 && i < index && !p.closed) { await p.close(); p.closed = true; }
    }
    await this.#writeRange(this.pos, bytes);
    this.pos += bytes.length;
  }

  async writeAt(position, bytes) { await this.#writeRange(position, bytes); }

  async close() {
    for (const p of this.parts.values()) if (!p.closed) { await p.close(); p.closed = true; }
    return 'saved';
  }

  async abort() {
    for (const p of this.parts.values()) {
      if (!p.closed) { try { await p.abort?.(); } catch { /* ignore */ } p.closed = true; }
    }
    try { await this.remove?.(); } catch { /* ignore */ }
  }
}

/** Builds the NSP in memory, then downloads it (or returns it, for tests). For small outputs only. */
export class MemoryNspSink {
  constructor(name = 'out.nsp') { this.name = name; this.chunks = []; this.size = 0; this.kind = 'memory'; }
  write(bytes) { this.chunks.push(bytes.slice()); this.size += bytes.length; }
  writeAt(position, bytes) {
    let base = 0;
    for (const c of this.chunks) {
      const lo = Math.max(position, base), hi = Math.min(position + bytes.length, base + c.length);
      if (lo < hi) c.set(bytes.subarray(lo - position, hi - position), lo - base);
      base += c.length;
    }
    if (position + bytes.length > base) throw new Error('MemoryNspSink: write past the end');
  }
  bytes() {
    const out = new Uint8Array(this.size);
    let o = 0;
    for (const c of this.chunks) { out.set(c, o); o += c.length; }
    return out;
  }
  async close() {
    if (typeof document === 'undefined') return 'memory';
    const blob = new Blob(this.chunks, { type: 'application/octet-stream' });
    this.chunks = [];
    triggerDownload(blob, this.name);
    return 'downloaded';
  }
  async abort() { this.chunks = []; }
}

/**
 * The connected console as the target. connect({ signal, log }) -> a transport ({ read, write, close }, see
 * js/usb_install.js WebUsbTransport); the page supplies it (it finds an allowed device or asks the user for one, which
 * needs a click). serve(image) is called by js/nsp.js once the NSP header is final; it returns when the console quits.
 */
export class UsbNspSink {
  constructor(connect, name, { onWaiting = null } = {}) {
    this.connect = connect;
    this.name = name;
    this.kind = 'usb';
    this.onWaiting = onWaiting;
    this.transport = null;
    this.installed = false;
    this.stats = null;
  }

  write() { throw new Error('UsbNspSink takes no writes: the NSP is served to the console by range (serve)'); }
  writeAt() { throw new Error('UsbNspSink takes no writes: the NSP is served to the console by range (serve)'); }

  async serve(image, { signal = null, log = () => {}, progress = () => {} } = {}) {
    this.transport = await this.connect({ signal, log });
    try {
      const r = await serveFiles(this.transport, [{ name: this.name, size: image.size, read: (o, n) => image.read(o, n) }], {
        signal, log, onWaiting: this.onWaiting ?? undefined,
        onProgress: (sent, total, info) => progress(Math.min(sent, total), total, info),
      });
      this.stats = { ...r.files[0], reopens: image.stats.reopens, verified: image.stats.verified };
      if (!image.complete()) {
        throw new Error('the console stopped before it had read the whole package: the install was cancelled or failed on the console (see its message; not enough free space?), or Sphaira skipped it because this version is already installed');
      }
      this.installed = true;
    } finally {
      await this.transport.close?.();
    }
  }

  async close() { return this.installed ? 'installed' : 'not-installed'; }
  async abort() { try { await this.transport?.close?.(); } catch { /* ignore */ } }
}

/**
 * What this browser can write an NSP to: { file, split, memory, usb } (memory is always possible, for small outputs;
 * usb: WebUSB in a secure context, usbReason says why not).
 */
export function nspSinkSupport() {
  const usb = webUsbSupport();
  return {
    file: typeof globalThis.showSaveFilePicker === 'function',
    split: typeof globalThis.showDirectoryPicker === 'function',
    memory: true,
    usb: usb.ok,
    usbReason: usb.reason,
  };
}

/**
 * Asks for the target (call it from the click handler: the pickers need the user gesture).
 * how: 'file' | 'split' | 'memory' | 'usb' (with connect, see UsbNspSink). Returns the sink, or null when the user
 * cancelled.
 * For 'split', sink.dir is the picked folder (the base metadata can be written next to the NSP folder).
 */
export async function openNspSink(how, name, { connect = null, onWaiting = null } = {}) {
  if (how === 'usb') {
    if (!connect) throw new Error('openNspSink: usb needs connect()');
    return new UsbNspSink(connect, name, { onWaiting });
  }
  if (how === 'file') {
    try {
      const handle = await globalThis.showSaveFilePicker({
        suggestedName: name,
        types: [{ description: 'Nintendo Switch package', accept: { 'application/octet-stream': ['.nsp'] } }],
      });
      const sink = new FileNspSink(await handle.createWritable(), handle.name ?? name);
      return sink;
    } catch (e) {
      if (e?.name === 'AbortError') return null;
      throw e;
    }
  }
  if (how === 'split') {
    let dir;
    try {
      dir = await globalThis.showDirectoryPicker({ mode: 'readwrite' });
    } catch (e) {
      if (e?.name === 'AbortError') return null;
      throw e;
    }
    const folder = await dir.getDirectoryHandle(name, { create: true });
    const sink = new SplitNspSink(async (index) => {
      const fh = await folder.getFileHandle(String(index).padStart(2, '0'), { create: true });
      return fh.createWritable();
    }, { name, remove: () => dir.removeEntry(name, { recursive: true }) });
    sink.dir = dir;
    return sink;
  }
  return new MemoryNspSink(name);
}

/**
 * Saves a small text file (the base metadata) where the user chooses: next to a split NSP folder when that folder's
 * parent is known, else a save picker, else a download. Must run in a click handler when it opens a picker.
 */
export async function saveTextFile(name, text, { dir = null } = {}) {
  const blob = new Blob([text], { type: 'application/json' });
  if (dir) {
    const fh = await dir.getFileHandle(name, { create: true });
    const w = await fh.createWritable();
    await w.write(blob);
    await w.close();
    return 'saved';
  }
  if (typeof globalThis.showSaveFilePicker === 'function') {
    try {
      const handle = await globalThis.showSaveFilePicker({ suggestedName: name, types: [{ description: 'JSON', accept: { 'application/json': ['.json'] } }] });
      const w = await handle.createWritable();
      await w.write(blob);
      await w.close();
      return 'saved';
    } catch (e) {
      if (e?.name === 'AbortError') return null;
      throw e;
    }
  }
  triggerDownload(blob, name);
  return 'downloaded';
}
