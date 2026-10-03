// Streaming ZIP writer: "store" only (no compression: the game data is compressed already), ZIP64 whenever a size,
// an offset or the entry count needs it. The CRC-32 of a file is only known after its bytes went out, so every entry
// uses a data descriptor (general purpose bit 3); the sizes are known in advance and are written in the local
// header too, which keeps streaming extractors happy.
//
//   const zip = new ZipWriter(sink);               // sink: { write(Uint8Array): Promise|void }
//   await zip.addFile('a/b.bin', size, asyncIterableOfUint8Arrays);
//   await zip.finish();                            // central directory + end records; the sink is not closed

// ---- CRC-32 (IEEE), slicing-by-8 -----------------------------------------------------------------------------
const T = (() => {
  const t = new Int32Array(256 * 8);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
    t[i] = c;
  }
  for (let i = 0; i < 256; i++) {
    let c = t[i];
    for (let s = 1; s < 8; s++) {
      c = t[c & 255] ^ (c >>> 8);
      t[s * 256 + i] = c;
    }
  }
  return t;
})();

/** Continues a CRC-32: crc32(b, crc32(a)) == crc32(a + b). Returns an unsigned 32-bit value. */
export function crc32(bytes, previous = 0) {
  let c = ~previous;
  let i = 0;
  const n = bytes.length;
  for (const end = n - 7; i < end; i += 8) {
    const lo = c ^ (bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) | (bytes[i + 3] << 24));
    c =
      T[1792 + (lo & 255)] ^ T[1536 + ((lo >>> 8) & 255)] ^ T[1280 + ((lo >>> 16) & 255)] ^ T[1024 + (lo >>> 24)] ^
      T[768 + bytes[i + 4]] ^ T[512 + bytes[i + 5]] ^ T[256 + bytes[i + 6]] ^ T[bytes[i + 7]];
  }
  for (; i < n; i++) c = T[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return ~c >>> 0;
}

// ---- helpers ---------------------------------------------------------------------------------------------------
const U32_MAX = 0xffffffff;
const U16_MAX = 0xffff;
const FLAG_DESCRIPTOR = 1 << 3;
const FLAG_UTF8 = 1 << 11;
const textEncoder = new TextEncoder();

function dosDateTime(d) {
  const year = Math.min(Math.max(d.getFullYear(), 1980), 2107);
  const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  const date = ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  return { time, date };
}

class Writer {
  constructor(size) {
    this.u8 = new Uint8Array(size);
    this.dv = new DataView(this.u8.buffer);
    this.p = 0;
  }
  u16(v) { this.dv.setUint16(this.p, v, true); this.p += 2; return this; }
  u32(v) { this.dv.setUint32(this.p, v >>> 0, true); this.p += 4; return this; }
  u64(v) { this.dv.setBigUint64(this.p, BigInt(v), true); this.p += 8; return this; }
  bytes(b) { this.u8.set(b, this.p); this.p += b.length; return this; }
}

export class ZipWriter {
  /**
   * @param sink { write(Uint8Array) }
   * @param options.mtime        Date used for every entry (default: now)
   * @param options.zip64Threshold  sizes/offsets at or above this value use ZIP64 fields (default 0xFFFFFFFF; tests lower it)
   */
  constructor(sink, options = {}) {
    this.sink = sink;
    this.entries = [];
    this.offset = 0;
    this.threshold = options.zip64Threshold ?? U32_MAX;
    this.stamp = dosDateTime(options.mtime ?? new Date());
    this.names = new Set();
    this.finished = false;
  }

  get bytesWritten() { return this.offset; }

  async #out(bytes) {
    if (bytes.length === 0) return;
    await this.sink.write(bytes);
    this.offset += bytes.length;
  }

  /**
   * Adds one file whose size is known. `chunks` is an (async) iterable of Uint8Array whose lengths add up to `size`.
   * Rejects (and the archive must be abandoned) when they do not.
   */
  async addFile(name, size, chunks, options = {}) {
    if (this.finished) throw new Error('zip already finished');
    if (this.names.has(name)) throw new Error(`duplicate zip entry ${name}`);
    if (name.startsWith('/') || name.split('/').includes('..') || name.includes('\\')) throw new Error(`unsafe zip entry name ${name}`);
    this.names.add(name);
    const nameBytes = textEncoder.encode(name);
    if (nameBytes.length > U16_MAX) throw new Error('entry name too long');
    const stamp = options.mtime ? dosDateTime(options.mtime) : this.stamp;
    const bigSize = size >= this.threshold;
    const headerOffset = this.offset;

    // Local file header
    const extraLen = bigSize ? 20 : 0;
    const h = new Writer(30 + nameBytes.length + extraLen);
    h.u32(0x04034b50).u16(bigSize ? 45 : 20).u16(FLAG_DESCRIPTOR | FLAG_UTF8).u16(0).u16(stamp.time).u16(stamp.date);
    h.u32(0); // crc: in the descriptor
    h.u32(bigSize ? U32_MAX : size).u32(bigSize ? U32_MAX : size);
    h.u16(nameBytes.length).u16(extraLen).bytes(nameBytes);
    if (bigSize) h.u16(0x0001).u16(16).u64(size).u64(size);
    await this.#out(h.u8);

    let crc = 0;
    let written = 0;
    for await (const chunk of chunks) {
      if (chunk.length === 0) continue;
      written += chunk.length;
      if (written > size) throw new Error(`${name}: more data than the declared ${size} bytes`);
      crc = crc32(chunk, crc);
      await this.#out(chunk);
    }
    if (written !== size) throw new Error(`${name}: expected ${size} bytes, got ${written}`);

    // Data descriptor (8-byte sizes when the local header carries the ZIP64 extra field)
    const d = new Writer(bigSize ? 24 : 16);
    d.u32(0x08074b50).u32(crc);
    if (bigSize) d.u64(size).u64(size); else d.u32(size).u32(size);
    await this.#out(d.u8);

    this.entries.push({ nameBytes, crc, size, headerOffset, stamp, bigSize });
  }

  /** Writes the central directory and the end records. */
  async finish() {
    if (this.finished) throw new Error('zip already finished');
    this.finished = true;
    const cdStart = this.offset;
    for (const e of this.entries) {
      const bigOffset = e.headerOffset >= this.threshold;
      const fields = [];
      if (e.bigSize) fields.push(e.size, e.size); // uncompressed, compressed (the order the spec fixes)
      if (bigOffset) fields.push(e.headerOffset);
      const extraLen = fields.length ? 4 + fields.length * 8 : 0;
      const needsZip64 = e.bigSize || bigOffset;
      const w = new Writer(46 + e.nameBytes.length + extraLen);
      w.u32(0x02014b50).u16((3 << 8) | 45).u16(needsZip64 ? 45 : 20).u16(FLAG_DESCRIPTOR | FLAG_UTF8).u16(0);
      w.u16(e.stamp.time).u16(e.stamp.date).u32(e.crc);
      w.u32(e.bigSize ? U32_MAX : e.size).u32(e.bigSize ? U32_MAX : e.size);
      w.u16(e.nameBytes.length).u16(extraLen).u16(0).u16(0).u16(0);
      w.u32(((0o100644) << 16) >>> 0);
      w.u32(bigOffset ? U32_MAX : e.headerOffset).bytes(e.nameBytes);
      if (fields.length) {
        w.u16(0x0001).u16(fields.length * 8);
        for (const f of fields) w.u64(f);
      }
      await this.#out(w.u8);
    }
    const cdSize = this.offset - cdStart;
    const count = this.entries.length;
    const zip64 = count >= U16_MAX || cdStart >= this.threshold || cdSize >= this.threshold;
    if (zip64) {
      const z = new Writer(56 + 20);
      const zip64EndOffset = this.offset;
      z.u32(0x06064b50).u64(44).u16((3 << 8) | 45).u16(45).u32(0).u32(0).u64(count).u64(count).u64(cdSize).u64(cdStart);
      z.u32(0x07064b50).u32(0).u64(zip64EndOffset).u32(1);
      await this.#out(z.u8);
    }
    const end = new Writer(22);
    end.u32(0x06054b50).u16(0).u16(0);
    end.u16(zip64 ? U16_MAX : count).u16(zip64 ? U16_MAX : count);
    end.u32(zip64 ? U32_MAX : cdSize).u32(zip64 ? U32_MAX : cdStart).u16(0);
    await this.#out(end.u8);
  }
}

/** Sink that keeps everything in memory (tests, small archives). */
export class MemorySink {
  constructor() { this.parts = []; this.size = 0; }
  write(bytes) { this.parts.push(bytes.slice()); this.size += bytes.length; }
  async close() { return 'memory'; }
  async abort() { this.parts = []; }
  concat() {
    const out = new Uint8Array(this.size);
    let p = 0;
    for (const part of this.parts) { out.set(part, p); p += part.length; }
    return out;
  }
}
