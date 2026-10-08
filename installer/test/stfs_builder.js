// Builds small synthetic STFS (LIVE/CON/PIRS) packages for the tests: an independent writer, laid out from the format
// description (docs/dlc.md, js/stfs.js header comment), not from the reader's block math. No game data.
//
// Physical layout of the data area (packages below 0xAA * 0xAA blocks), in blocks after the header, with
// T = 1 hash-table copy for read-only packages and 2 otherwise:
//   [L0 table 0: T] [data 0..169] [L1 table: T] [L0 table 1: T] [data 170..339] [L0 table 2: T] [data 340..509] ...
// (the L1 table exists only when there are more than 170 data blocks).
import { createHash } from 'node:crypto';

const BLOCK = 0x1000;
const GROUP = 0xaa;
const END = 0xffffff;

export function pattern(n, seed) {
  const b = new Uint8Array(n);
  let x = (seed * 2654435761) >>> 0 || 1;
  for (let i = 0; i < n; i++) {
    x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0;
    b[i] = x & 0xff;
  }
  return b;
}

const sha1 = (b) => new Uint8Array(createHash('sha1').update(b).digest());

function putU24le(b, at, v) { b[at] = v & 0xff; b[at + 1] = (v >> 8) & 0xff; b[at + 2] = (v >> 16) & 0xff; }

function putUtf16be(b, at, s, max) {
  for (let i = 0; i < s.length && (i + 1) * 2 <= max; i++) {
    b[at + i * 2] = s.charCodeAt(i) >> 8;
    b[at + i * 2 + 1] = s.charCodeAt(i) & 0xff;
  }
}

/**
 * options:
 *   files: [{path, data: Uint8Array, fragmented?: bool}]   (directories are derived from the paths)
 *   readOnly (default true), rootActiveIndex (default false), magic ('LIVE'), titleId (0x4D5307E8),
 *   contentType (2), metaVersion (2), volumeType (0), names {en, de, pl, ...}, descriptions {en},
 *   licenses [{id: bigint, bits, flags}], contentId (20 bytes), headerSize (0xAD0E),
 *   fileTableBlocks: block numbers for the file table (default: allocated first, chained in order),
 *   rawNames: entries' names are used as given (for the unsafe-name test)
 * Returns {bytes, entries, totalBlocks}.
 */
export function buildStfs(options) {
  const o = {
    readOnly: true, rootActiveIndex: false, magic: 'LIVE', titleId: 0x4d5307e8, contentType: 2, metaVersion: 2,
    volumeType: 0, names: { en: 'Synthetic DLC' }, descriptions: { en: 'Synthetic description' },
    licenses: [{ id: 0xffffffffffffffffn, bits: 1, flags: 1 }], contentId: pattern(20, 99), headerSize: 0xad0e,
    ...options,
  };
  const T = o.readOnly ? 1 : 2;

  // ---- entries (directories first, then files; parent = index of the directory entry) ----
  const entries = [];
  const dirIndex = new Map();
  const dirOf = (parts) => {
    if (!parts.length) return 0xffff;
    const key = parts.join('/');
    if (dirIndex.has(key)) return dirIndex.get(key);
    const parent = dirOf(parts.slice(0, -1));
    const idx = entries.length;
    entries.push({ name: parts[parts.length - 1], isDir: true, parent, data: new Uint8Array(0) });
    dirIndex.set(key, idx);
    return idx;
  };
  for (const f of o.files) dirOf(f.path.split('/').slice(0, -1));
  for (const f of o.files) {
    const parts = f.path.split('/');
    entries.push({ name: parts[parts.length - 1], isDir: false, parent: dirOf(parts.slice(0, -1)), data: f.data, fragmented: !!f.fragmented });
  }

  // ---- block allocation ----
  const ftCount = Math.max(1, Math.ceil((entries.length + 1) / 64));
  const blocks = new Map(); // data block index -> {data: Uint8Array(4096), next}
  let nextFree = 0;
  const ftBlocks = o.fileTableBlocks ?? Array.from({ length: ftCount }, () => nextFree++);
  if (o.fileTableBlocks) nextFree = Math.max(...ftBlocks) + 1;
  for (const e of entries) {
    if (e.isDir) continue;
    const n = Math.ceil(e.data.length / BLOCK);
    let list = Array.from({ length: n }, (_, k) => nextFree + k);
    if (e.fragmented && n > 2) {
      // a permutation of the range: jumps forwards and backwards, across hash-table groups
      const step = [7, 11, 13, 17, 19].find((s) => n % s !== 0);
      list = Array.from({ length: n }, (_, k) => nextFree + ((k * step) % n));
    }
    nextFree += n;
    e.blocks = list;
    e.start = n ? list[0] : 0;
    list.forEach((b, k) => {
      const chunk = new Uint8Array(BLOCK);
      chunk.set(e.data.subarray(k * BLOCK, Math.min((k + 1) * BLOCK, e.data.length)));
      // the unused tail of the last block is not zero on real packages either
      if ((k + 1) * BLOCK > e.data.length) chunk.fill(0xcd, e.data.length - k * BLOCK);
      blocks.set(b, { data: chunk, next: k + 1 < n ? list[k + 1] : END });
    });
  }
  // file table blocks, chained in the given order
  const ftData = ftBlocks.map(() => new Uint8Array(BLOCK));
  entries.forEach((e, i) => {
    const t = ftData[Math.floor(i / 64)];
    const at = (i % 64) * 0x40;
    const name = e.name;
    for (let k = 0; k < name.length; k++) t[at + k] = name.charCodeAt(k);
    t[at + 0x28] = (name.length & 0x3f) | (e.isDir ? 0x80 : 0) | (!e.isDir && !e.fragmented ? 0x40 : 0);
    const n = e.isDir ? 0 : e.blocks.length;
    putU24le(t, at + 0x29, n);
    putU24le(t, at + 0x2c, n);
    putU24le(t, at + 0x2f, e.isDir ? 0 : e.start);
    t[at + 0x32] = e.parent >> 8; t[at + 0x33] = e.parent & 0xff;
    new DataView(t.buffer).setUint32(at + 0x34, e.isDir ? 0 : e.data.length, false);
  });
  ftBlocks.forEach((b, i) => blocks.set(b, { data: ftData[i], next: i + 1 < ftBlocks.length ? ftBlocks[i + 1] : END }));

  const totalBlocks = Math.max(...blocks.keys()) + 1;
  if (totalBlocks >= GROUP * GROUP) throw new Error('builder: too many blocks for this simple layout');
  for (let b = 0; b < totalBlocks; b++) if (!blocks.has(b)) blocks.set(b, { data: pattern(BLOCK, 1000 + b), next: END });

  // ---- physical layout ----
  const groups = Math.ceil(totalBlocks / GROUP);
  const hasL1 = totalBlocks > GROUP;
  const l0Pos = (g) => (g === 0 ? 0 : T + GROUP * g + T + (g - 1) * T);
  const l1Pos = T + GROUP;
  const dataPos = (i) => { const g = Math.floor(i / GROUP); return i + T + (g >= 1 ? T + g * T : 0); };
  const physical = Math.max(dataPos(totalBlocks - 1), l0Pos(groups - 1)) + 1;
  const base = Math.ceil(o.headerSize / BLOCK) * BLOCK;
  const out = new Uint8Array(base + physical * BLOCK);
  const at = (p) => base + p * BLOCK;

  for (const [i, b] of blocks) out.set(b.data, at(dataPos(i)));

  // level-0 tables; which copy is active: root index bit if there is no L1, else the L1 record's bit 30
  const l0Secondary = (g) => (o.readOnly ? false : hasL1 ? g % 2 === 0 : o.rootActiveIndex);
  for (let g = 0; g < groups; g++) {
    const table = new Uint8Array(BLOCK);
    const dv = new DataView(table.buffer);
    for (let r = 0; r < GROUP; r++) {
      const i = g * GROUP + r;
      if (i >= totalBlocks) break;
      const b = blocks.get(i);
      table.set(sha1(b.data), r * 0x18);
      dv.setUint32(r * 0x18 + 0x14, (0x80 << 24) | b.next, false);
    }
    const sec = l0Secondary(g);
    out.set(table, at(l0Pos(g)) + (sec ? BLOCK : 0));
    if (!o.readOnly) out.set(pattern(BLOCK, 5000 + g), at(l0Pos(g)) + (sec ? 0 : BLOCK)); // the stale copy: garbage
  }
  if (hasL1) {
    const table = new Uint8Array(BLOCK);
    const dv = new DataView(table.buffer);
    for (let g = 0; g < groups; g++) dv.setUint32(g * 0x18 + 0x14, l0Secondary(g) ? 0x40000000 : 0, false);
    const sec = !o.readOnly && o.rootActiveIndex;
    out.set(table, at(l1Pos) + (sec ? BLOCK : 0));
    if (!o.readOnly) out.set(pattern(BLOCK, 7000), at(l1Pos) + (sec ? 0 : BLOCK));
  }

  // ---- header ----
  const h = out;
  const dv = new DataView(h.buffer);
  for (let k = 0; k < 4; k++) h[k] = o.magic.charCodeAt(k);
  o.licenses.forEach((l, i) => {
    dv.setBigUint64(0x22c + i * 0x10, l.id, false);
    dv.setUint32(0x22c + i * 0x10 + 8, l.bits, false);
    dv.setUint32(0x22c + i * 0x10 + 12, l.flags, false);
  });
  h.set(o.contentId, 0x32c);
  dv.setUint32(0x340, o.headerSize, false);
  dv.setUint32(0x344, o.contentType, false);
  dv.setUint32(0x348, o.metaVersion, false);
  dv.setBigUint64(0x34c, BigInt(physical * BLOCK), false);
  dv.setUint32(0x354, 0, false);      // media id
  dv.setUint32(0x360, o.titleId, false);
  const vd = 0x379;
  h[vd] = 0x24;
  h[vd + 2] = (o.readOnly ? 1 : 0) | (o.rootActiveIndex ? 2 : 0);
  h[vd + 3] = ftBlocks.length & 0xff; h[vd + 4] = ftBlocks.length >> 8;
  putU24le(h, vd + 5, ftBlocks[0]);
  dv.setUint32(vd + 0x1c, totalBlocks, false);
  dv.setUint32(vd + 0x20, 0, false);
  dv.setUint32(0x3a9, o.volumeType, false);
  const v1 = ['en', 'ja', 'de', 'fr', 'es', 'it', 'ko', 'zh', 'pt'];
  const v2 = ['pl', 'ru', 'sv'];
  v1.forEach((l, i) => {
    if (o.names[l]) putUtf16be(h, 0x411 + i * 0x100, o.names[l], 0x100);
    if (o.descriptions[l]) putUtf16be(h, 0xd11 + i * 0x100, o.descriptions[l], 0x100);
  });
  v2.forEach((l, i) => {
    if (o.names[l]) putUtf16be(h, 0x541a + i * 0x100, o.names[l], 0x100);
  });
  putUtf16be(h, 0x1691, 'Mass Effect', 0x80);
  return { bytes: out, entries, totalBlocks };
}

/** A package with every case the reader has to handle: nested folders, a two-block file table, a contiguous file
 * that crosses a hash-table group, a fragmented file, an empty file and a partial last block. */
export function sampleFiles() {
  const files = [
    { path: 'AutoLoad.ini', data: new TextEncoder().encode('[Packages]\r\n2DA1=BIOG_2DA_Synthetic_X\r\n') },
    { path: 'Content/Maps/SYN01/SYN01_big.xxx', data: pattern(250 * BLOCK + 123, 1) },
    { path: 'Content/Packages/frag.xxx', data: pattern(60 * BLOCK - 7, 2), fragmented: true },
    { path: 'Content/Packages/empty.bin', data: new Uint8Array(0) },
    { path: 'Movies/one_block.bik', data: pattern(BLOCK, 3) },
  ];
  for (let i = 0; i < 70; i++) files.push({ path: `Content/Packages/Small/s${String(i).padStart(2, '0')}.xxx`, data: pattern(100 + i * 13, 10 + i) });
  return files;
}
