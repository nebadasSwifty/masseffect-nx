// Builds small synthetic ME1 Xbox 360 UE3 packages for the tests (written from the format description in
// tools/check_packages.py, independent of js/pkgcheck.js).
const TAG = 0x9e2a83c1;

class W {
  constructor() { this.bytes = []; }
  u32(v) { this.bytes.push((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255); return this; }
  i32(v) { return this.u32(v >>> 0); }
  raw(arr) { for (const b of arr) this.bytes.push(b & 255); return this; }
  get length() { return this.bytes.length; }
  out() { return new Uint8Array(this.bytes); }
}

/** guid: 32 hex digits or a number used to fill it. */
function guidBytes(guid) {
  if (typeof guid === 'string') return guid.match(/../g).map((h) => parseInt(h, 16));
  return Array.from({ length: 16 }, (_, i) => (guid * 31 + i * 7) & 255);
}

/**
 * options: {guid, chunks: [blockCount...] (compressed, LZX) or [] (uncompressed), version, gens, folderLen,
 * blockSize (raw value written in the chunk header, default the tag), seed}. Returns Uint8Array.
 */
export function buildPackage(options = {}) {
  const {
    guid = 1, chunks = [2, 1], version = 0x005c0187, gens = 1, folderLen = 5, blockSize = TAG, seed = 1,
    compression = chunks.length ? 2 : 0,
  } = options;
  // summary length: 0x0C + 4 + folder + 4 flags + 24 tables + 16 guid + 4 + 12*gens + 8 + 28 + 8 + 16*chunks (+4)
  const summaryLen = 16 + folderLen + 4 + 24 + 16 + 4 + 12 * gens + 8 + 28 + 8 + 16 * chunks.length + (chunks.length ? 0 : 4);
  const uBlock = 0x20000;
  const chunkInfo = [];
  let uo = summaryLen;
  let co = summaryLen;
  for (const [ci, n] of chunks.entries()) {
    const blocks = Array.from({ length: n }, (_, b) => [100 + ((seed + ci * 13 + b * 7) % 50), b === n - 1 ? 0x8000 + ci : uBlock]);
    const us = blocks.reduce((a, [, u]) => a + u, 0);
    const csz = blocks.reduce((a, [c]) => a + c, 0);
    const cs = 16 + 8 * n + csz;
    chunkInfo.push({ uo, us, co, cs, csz, blocks });
    uo += us;
    co += cs;
  }
  const streamSize = chunks.length ? uo : summaryLen + 64;
  const headerSize = chunks.length ? Math.min(summaryLen + 0x9000, streamSize) : summaryLen + 32;
  const w = new W();
  w.u32(TAG).u32(version).u32(headerSize).i32(folderLen);
  w.raw(Array.from({ length: folderLen }, (_, i) => (i === folderLen - 1 ? 0 : 'None'.charCodeAt(i % 4))));
  w.u32(0x00080000); // flags
  w.u32(10).u32(summaryLen + 8).u32(5).u32(summaryLen + 24).u32(3).u32(summaryLen + 16);
  w.raw(guidBytes(guid));
  w.u32(gens);
  for (let g = 0; g < gens; g++) w.u32(5).u32(10).u32(0);
  w.u32(2674).u32(33);
  for (const v of [0x12345678, 0xba53, 0, 0x02830000, 0, 0, 0]) w.u32(v);
  w.u32(compression).u32(chunks.length);
  for (const c of chunkInfo) w.u32(c.uo).u32(c.us).u32(c.co).u32(c.cs);
  if (!chunks.length) {
    w.u32(0);
    w.raw(new Array(64).fill(0x5a));
    return w.out();
  }
  for (const c of chunkInfo) {
    w.u32(TAG).u32(blockSize).u32(c.csz).u32(c.us);
    for (const [cb, ub] of c.blocks) w.u32(cb).u32(ub);
    for (const [cb] of c.blocks) w.raw(Array.from({ length: cb }, (_, i) => (i * 17 + seed) & 255));
  }
  return w.out();
}

/** Byte offset of chunk `i`'s header in a package made by buildPackage (reads the chunk table). */
export function chunkOffset(pkg, i) {
  const dv = new DataView(pkg.buffer, pkg.byteOffset, pkg.byteLength);
  const flen = dv.getInt32(12);
  let pos = 16 + flen + 4 + 24 + 16;
  const gens = dv.getUint32(pos);
  pos += 4 + 12 * gens + 8 + 28 + 8;
  return dv.getUint32(pos + 16 * i + 8);
}
