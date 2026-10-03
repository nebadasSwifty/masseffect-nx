// Where the zip goes. Three targets, best first:
//   1. showSaveFilePicker (Chromium): written straight to the file the user picks, nothing buffered in memory.
//   2. the origin-private file system (Firefox, Safari, ...): written to a temporary file on disk, then handed
//      to the browser as a download. Needs free disk space but not memory.
//   3. a Blob in memory: works everywhere, but the whole zip must fit in memory (browsers fail somewhere between
//      about 1 and 4 GB). Only used when neither of the above exists.

function triggerDownload(blob, name) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.style.display = 'none';
  document.body.appendChild(a);
  a.click();
  a.remove();
  return url;
}

export class FilePickerSink {
  constructor(writable) { this.writable = writable; this.kind = 'file'; }
  async write(bytes) { await this.writable.write(bytes); }
  async close() { await this.writable.close(); return 'saved'; }
  async abort() { try { await this.writable.abort(); } catch { /* already closed */ } }
}

export class OpfsSink {
  constructor(dir, handle, writable, name) {
    this.dir = dir; this.handle = handle; this.writable = writable; this.name = name; this.kind = 'opfs';
  }
  async write(bytes) { await this.writable.write(bytes); }
  async close() {
    await this.writable.close();
    const file = await this.handle.getFile();
    const url = triggerDownload(file, this.name);
    // Keep the temporary file for a while: the browser reads it while it "downloads". Removed on the next visit too.
    setTimeout(() => { URL.revokeObjectURL(url); this.dir.removeEntry(this.handle.name).catch(() => {}); }, 60 * 60 * 1000);
    return 'downloaded';
  }
  async abort() {
    try { await this.writable.abort(); } catch { /* ignore */ }
    try { await this.dir.removeEntry(this.handle.name); } catch { /* ignore */ }
  }
}

export class BlobSink {
  constructor(name) { this.parts = []; this.name = name; this.kind = 'memory'; }
  write(bytes) { this.parts.push(bytes.slice()); }
  async close() {
    const blob = new Blob(this.parts, { type: 'application/zip' });
    this.parts = [];
    const url = triggerDownload(blob, this.name);
    setTimeout(() => URL.revokeObjectURL(url), 60 * 60 * 1000);
    return 'downloaded';
  }
  async abort() { this.parts = []; }
}

export function describeSinkSupport() {
  if (typeof showSaveFilePicker === 'function') return 'file';
  if (navigator.storage?.getDirectory) return 'opfs';
  return 'memory';
}

/**
 * Must be called from the click handler (the file picker needs the user gesture, which is gone after a few
 * seconds of work). Returns a sink, or null when the user cancelled the picker.
 */
export async function openSink(name, expectedBytes) {
  if (typeof showSaveFilePicker === 'function') {
    try {
      const handle = await showSaveFilePicker({ suggestedName: name, types: [{ description: 'ZIP archive', accept: { 'application/zip': ['.zip'] } }] });
      return new FilePickerSink(await handle.createWritable());
    } catch (e) {
      if (e?.name === 'AbortError') return null;
      // fall through to the other targets
    }
  }
  if (navigator.storage?.getDirectory) {
    try {
      const est = await navigator.storage.estimate?.();
      if (!est?.quota || est.quota - (est.usage ?? 0) > expectedBytes * 1.05) {
        const dir = await navigator.storage.getDirectory();
        const handle = await dir.getFileHandle(`${Date.now()}-${name}`, { create: true });
        if (typeof handle.createWritable === 'function') return new OpfsSink(dir, handle, await handle.createWritable(), name);
        await dir.removeEntry(handle.name).catch(() => {});
      }
    } catch { /* fall through */ }
  }
  return new BlobSink(name);
}

/** Removes temporary zips left behind by an earlier visit. */
export async function cleanStaleTemporaryFiles() {
  try {
    const dir = await navigator.storage?.getDirectory?.();
    if (!dir) return;
    for await (const [name] of dir.entries()) if (name.endsWith('.zip')) await dir.removeEntry(name).catch(() => {});
  } catch { /* not available */ }
}
