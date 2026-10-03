// Builds small synthetic XDVDFS images for the tests (an independent writer, written from the format description).
import { SECTOR, MAGIC, VOLUME_DESCRIPTOR_OFFSET } from '../js/xdvdfs.js';

function dirTable(entries) {
  // entries: [{name, sector, size, isDir}] -> Uint8Array padded with 0xFF to a sector multiple.
  const sorted = [...entries].sort((a, b) => a.name.toUpperCase().localeCompare(b.name.toUpperCase()));
  const offsets = [];
  let p = 0;
  for (const e of sorted) {
    offsets.push(p);
    p += (14 + e.name.length + 3) & ~3;
  }
  const size = Math.max(SECTOR, Math.ceil(p / SECTOR) * SECTOR);
  const t = new Uint8Array(size).fill(0xff);
  const dv = new DataView(t.buffer);
  // balanced binary tree over the sorted list; the root must be at offset 0, so build it by index and place the
  // median at index 0 by re-ordering offsets: simplest is to lay entries out in tree pre-order.
  const order = [];
  const links = new Map();
  const build = (lo, hi) => {
    if (lo > hi) return 0;
    const mid = (lo + hi) >> 1;
    order.push(mid);
    const self = mid;
    const l = build(lo, mid - 1);
    const r = build(mid + 1, hi);
    links.set(self, [l, r]);
    return -1 - self; // placeholder, resolved below
  };
  build(0, sorted.length - 1);
  const pos = new Map();
  let q = 0;
  for (const idx of order) {
    pos.set(idx, q);
    q += (14 + sorted[idx].name.length + 3) & ~3;
  }
  const unit = (ref) => (ref === 0 ? 0 : pos.get(-1 - ref) / 4);
  for (const idx of order) {
    const e = sorted[idx];
    const at = pos.get(idx);
    const [l, r] = links.get(idx);
    dv.setUint16(at, unit(l), true);
    dv.setUint16(at + 2, unit(r), true);
    dv.setUint32(at + 4, e.sector, true);
    dv.setUint32(at + 8, e.size, true);
    t[at + 12] = e.isDir ? 0x10 : 0x20;
    t[at + 13] = e.name.length;
    for (let i = 0; i < e.name.length; i++) t[at + 14 + i] = e.name.charCodeAt(i);
  }
  return t;
}

/**
 * tree: { 'default.xex': Uint8Array, Layer0: { 'a.bin': Uint8Array, sub: {...} } }
 * Returns a Uint8Array with the partition starting at `base` (the bytes before it are zero).
 */
export function buildXiso(tree, base = 0) {
  const chunks = new Map(); // sector -> Uint8Array
  let next = 34; // 32 = volume descriptor, 33 = first free sector after it
  const alloc = (bytes) => {
    const sector = next;
    chunks.set(sector, bytes);
    next += Math.max(1, Math.ceil(bytes.length / SECTOR));
    return sector;
  };
  const writeDir = (node) => {
    // children first so their sectors are known; the directory table itself goes after them
    const entries = [];
    for (const [name, v] of Object.entries(node)) {
      if (v instanceof Uint8Array) entries.push({ name, sector: v.length ? alloc(v) : 0, size: v.length, isDir: false });
      else {
        const d = writeDir(v);
        entries.push({ name, sector: d.sector, size: d.size, isDir: true });
      }
    }
    if (entries.length === 0) return { sector: 0, size: 0 };
    const table = dirTable(entries);
    return { sector: alloc(table), size: table.length };
  };
  const root = writeDir(tree);
  const total = base + next * SECTOR;
  const img = new Uint8Array(total);
  for (const [sector, bytes] of chunks) img.set(bytes, base + sector * SECTOR);
  const vd = base + VOLUME_DESCRIPTOR_OFFSET;
  for (let i = 0; i < MAGIC.length; i++) {
    img[vd + i] = MAGIC.charCodeAt(i);
    img[vd + 0x7ec + i] = MAGIC.charCodeAt(i);
  }
  const dv = new DataView(img.buffer);
  dv.setUint32(vd + 20, root.sector, true);
  dv.setUint32(vd + 24, root.size, true);
  return img;
}

export function pattern(n, seed = 1) {
  const a = new Uint8Array(n);
  let x = seed >>> 0;
  for (let i = 0; i < n; i++) {
    x = (Math.imul(x, 1664525) + 1013904223) >>> 0;
    a[i] = x >>> 24;
  }
  return a;
}
