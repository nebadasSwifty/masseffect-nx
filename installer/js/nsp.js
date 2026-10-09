// Installable NSP output (docs/full-nsp.md): a JavaScript port of tools/build_full_nsp.py. For the same inputs, the
// same AES keys and the same RSA signature it writes the same bytes as the Python packer (test/nsp_python.test.js
// compares both). Full NSPs, updates (patch titles with a BKTR RomFS against a base) and program-only updates.
//
// Streaming, like the Python packer: pass 1 reads the RomFS once and keeps only hashes (IVFC block hashes, 64 KiB
// chunk hashes per file), pass 2 reads it again, encrypts and writes it, then the NSP header (whose names are the
// NCA hashes) is written over a placeholder. Peak memory: a few tens of MB plus the program (~60 MB).
// A sink with serve() (the USB install, js/nsp_sink.js UsbNspSink) gets no writes: pass 2 only hashes, and the
// finished NSP is then served by range from an NspImage, which encrypts the requested region again (AES-CTR is
// seekable; every stream below can start at an offset).
//
// Keys: parseProdKeys() keeps only header_key and key_area_key_application_00, in memory, for the lifetime of the
// tab (the page drops them on pagehide). They are never uploaded, stored or logged, and error messages never contain
// key material.
import {
  aesEcb, aesXts, AesCtr, Sha256, sha256, sha256Many, rsaPssSigner, randomBytes,
  base64Encode, base64Decode, toHex, fromHex,
} from './nsp_crypto.js';

/** The browser writer is in place (kept for the page and the tests). */
export const NSP_BROWSER_READY = true;

export const NSP_DEFAULT_TITLE_ID = '01a5eec700010000';
export const DEFAULT_TITLE_ID = 0x01A5EEC700010000n;
export const FORWARDER_TITLE_ID = 0x01A5EEC700000000n;
export const DEFAULT_DATA_DIR = 'sdmc:/switch/masseffect-nx-nsp';
// The per-edition title IDs and data folders live in config.js (editions[].nsp); tools/build_full_nsp.py EDITIONS
// has the same values.
export const MARKER_NAME = 'masseffect-nx-package.txt';
export const TOML_NAME = 'masseffect.toml';
export const SHADERS_NAME = 'masseffect_shaders.mesp';
// Optional RomFS file: the edition's shipped pipeline prewarm list (tools/build_full_nsp.py PREWARM_LIST_NAME).
export const PREWARM_LIST_NAME = 'masseffect_prewarm_list.bin';
export const DLC_PREFIX = 'masseffect/0000000000000000/';

const MEDIA_UNIT = 0x200;
const IVFC_BLOCK_LOG2 = 14;
export const IVFC_BLOCK = 1 << IVFC_BLOCK_LOG2;
const IVFC_LEVELS = 6;
const EXEFS_HASH_BLOCK = 0x10000;
const META_HASH_BLOCK = 0x1000;
const SDK_VERSION = 0x000C1100;
/** Part size of a split NSP folder (the convention DBI, Tinfoil and Goldleaf read). */
export const FAT32_PART = 0xFFFF0000;
/** FAT32 cannot hold files of 4 GiB or more: such NSPs must be split, or installed over USB/LAN. */
export const FAT32_MAX_FILE = 0xFFFFFFFF;
const CHUNK = 4 << 20;

export const CONTENT_PROGRAM = 0, CONTENT_META = 1, CONTENT_CONTROL = 2;
const CNMT_PROGRAM = 1, CNMT_CONTROL = 3;
const CNMT_APPLICATION = 0x80, CNMT_PATCH = 0x81;
export const PATCH_CHUNK = 0x10000;
const BUCKET_NODE = 0x4000;
export const BASE_META_FORMAT = 1;
const ROMFS_EMPTY = 0xFFFFFFFF;
const ROMFS_DATA_OFFSET = 0x200;

const enc = new TextEncoder();
const dec = new TextDecoder();

export class PackError extends Error {
  constructor(message) { super(message); this.name = 'PackError'; }
}

export class KeyFileError extends Error {
  constructor(message, missing = []) { super(message); this.name = 'KeyFileError'; this.missing = missing; }
}

function align(value, alignment) {
  return Math.ceil(value / alignment) * alignment;
}

function concat(parts) {
  const total = parts.reduce((a, p) => a + p.length, 0);
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

function equalBytes(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

function compareBytes(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return a[i] - b[i];
  return a.length - b.length;
}

/** Little-endian field writer over a Uint8Array. */
class Fields {
  constructor(bytes) { this.b = bytes; this.dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength); }
  u8(o, v) { this.b[o] = v; return this; }
  u16(o, v) { this.dv.setUint16(o, v, true); return this; }
  u32(o, v) { this.dv.setUint32(o, Number(v) >>> 0, true); return this; }
  i32(o, v) { this.dv.setInt32(o, v, true); return this; }
  u64(o, v) { this.dv.setBigUint64(o, BigInt.asUintN(64, BigInt(v)), true); return this; }
  i64(o, v) { this.dv.setBigInt64(o, BigInt(v), true); return this; }
  bytes(o, v) { this.b.set(v, o); return this; }
}

const view = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const hex16 = (v) => BigInt.asUintN(64, BigInt(v)).toString(16).padStart(16, '0');

function throwIfAborted(signal) {
  if (signal?.aborted) { const e = new Error('Cancelled'); e.name = 'Cancelled'; throw e; }
}

// ---- keys ------------------------------------------------------------------------------------------------------------

const REQUIRED_KEYS = [
  ['header_key', 32],
  ['key_area_key_application_00', 16],
];

function hexBytes(value, size) {
  if (typeof value !== 'string' || value.length !== size * 2 || !/^[0-9a-fA-F]+$/.test(value)) return null;
  return fromHex(value);
}

/**
 * Parses a prod.keys text ("name = hex" lines) and returns only the keys the NSP needs:
 * { header_key: Uint8Array(32), key_area_key_application_00: Uint8Array(16) }.
 * Throws KeyFileError naming the missing or malformed keys (never their values).
 */
export function parseProdKeys(text) {
  const found = new Map();
  for (const line of String(text).split(/\r?\n/)) {
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    found.set(line.slice(0, eq).trim().toLowerCase(), line.slice(eq + 1).trim());
  }
  const keys = {};
  const missing = [];
  for (const [name, size] of REQUIRED_KEYS) {
    const bytes = hexBytes(found.get(name), size);
    if (!bytes) missing.push(name);
    else keys[name] = bytes;
  }
  found.clear();
  if (!missing.length) {
    const hk = keys.header_key;
    if (hk.subarray(0, 16).every((b, i) => b === hk[16 + i])) missing.push('header_key');
  }
  if (missing.length) {
    forgetKeys(keys);
    throw new KeyFileError(`missing or malformed: ${missing.join(', ')}`, missing);
  }
  return keys;
}

/** Overwrites the key bytes (best effort: the GC may hold copies of the original text). */
export function forgetKeys(keys) {
  if (!keys) return;
  for (const v of Object.values(keys)) if (v instanceof Uint8Array) v.fill(0);
}

// ---- NRO -> NSO, NACP, icon -------------------------------------------------------------------------------------------

export class Nro {
  constructor(data) {
    // A plain Uint8Array (a Node Buffer's slice() is a view, and the NSO conversion edits a copy of the text).
    if (data instanceof Uint8Array && data.constructor !== Uint8Array) data = new Uint8Array(data);
    if (!(data instanceof Uint8Array) || data.length < 0x80 || dec.decode(data.subarray(0x10, 0x14)) !== 'NRO0') {
      throw new PackError('not an NRO (no NRO0 header)');
    }
    const dv = view(data);
    this.data = data;
    this.size = dv.getUint32(0x18, true);
    this.segments = [0, 1, 2].map((i) => [dv.getUint32(0x20 + 8 * i, true), dv.getUint32(0x24 + 8 * i, true)]);
    this.bss = dv.getUint32(0x38, true);
    this.buildId = data.slice(0x40, 0x60);
    for (const [off, size] of this.segments) {
      if (off % 0x1000 || off + size > data.length) throw new PackError('NRO segments are outside the file or not page aligned');
    }
    if (this.segments[0][0] !== 0) throw new PackError('NRO text segment does not start at 0');
    this.assets = {};
    if (this.size + 0x38 <= data.length && dec.decode(data.subarray(this.size, this.size + 4)) === 'ASET') {
      for (const [name, pos] of [['icon', 8], ['nacp', 24], ['romfs', 40]]) {
        const off = Number(dv.getBigUint64(this.size + pos, true));
        const size = Number(dv.getBigUint64(this.size + pos + 8, true));
        if (size) {
          const start = this.size + off;
          if (start + size > data.length) throw new PackError('NRO asset section is truncated');
          this.assets[name] = data.slice(start, start + size);
        }
      }
    }
  }
}

/** An uncompressed NSO with the NRO's segments (tools/build_full_nsp.py nso_from_nro). */
export function nsoFromNro(nro) {
  const segs = nro.segments.map(([off, size], i) => {
    const seg = nro.data.slice(off, off + size);
    if (i === 0) {
      if (dec.decode(seg.subarray(0x10, 0x14)) !== 'NRO0') throw new PackError('unexpected NRO layout (header not at text+0x10)');
      seg.fill(0, 0x10, 0x80);
    }
    return seg;
  });
  const header = new Uint8Array(0x100);
  const f = new Fields(header);
  header.set(enc.encode('NSO0'), 0);
  f.u32(0x0C, 0x38);
  let fileOff = 0x100;
  segs.forEach((seg, i) => {
    const extra = i === 2 ? nro.bss : 1;
    f.u32(0x10 + 0x10 * i, fileOff).u32(0x14 + 0x10 * i, nro.segments[i][0]).u32(0x18 + 0x10 * i, seg.length).u32(0x1C + 0x10 * i, extra);
    f.u32(0x60 + 4 * i, seg.length);
    header.set(sha256(seg), 0xA0 + 0x20 * i);
    fileOff += seg.length;
  });
  header.set(nro.buildId, 0x40);
  return concat([header, ...segs]);
}

// NACP fields that carry the application's own ID (tools/build_full_nsp.py NACP_ID_FIELDS): PresenceGroupId,
// SaveDataOwnerId, LocalCommunicationId[8], SeedForPseudoDeviceId; AddOnContentBaseId is ID + 0x1000.
const NACP_ID_FIELDS = [0x3038, 0x3078, 0x30B0, 0x30B8, 0x30C0, 0x30C8, 0x30D0, 0x30D8, 0x30E0, 0x30E8, 0x30F8];
const NACP_AOC_BASE = 0x3070;

/**
 * The NRO's NACP patched like tools/build_nsp.sh (tools/build_full_nsp.py patch_nacp). With titleId (BigInt), the ID
 * fields the NRO set (non-zero) are rewritten to this title.
 */
export function patchNacp(nacp, name = null, displayVersion = null, titleId = null) {
  if (nacp.length !== 0x4000) throw new PackError("the NRO's NACP is not 0x4000 bytes");
  const out = nacp.slice();
  if (titleId !== null) {
    const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
    for (const off of NACP_ID_FIELDS) if (dv.getBigUint64(off, true)) dv.setBigUint64(off, BigInt(titleId), true);
    if (dv.getBigUint64(NACP_AOC_BASE, true)) dv.setBigUint64(NACP_AOC_BASE, BigInt(titleId) + 0x1000n, true);
  }
  out[0x3025] = 0;
  out[0x3034] = 0;
  out[0x3035] = 2;
  out.fill(0, 0x3080, 0x3090);
  if (displayVersion) {
    const raw = asciiBytes(displayVersion).subarray(0, 0xF);
    out.fill(0, 0x3060, 0x3070);
    out.set(raw, 0x3060);
  }
  if (name) {
    const raw = enc.encode(name).subarray(0, 0x1FF);
    for (let lang = 0; lang < 16; lang++) {
      const entry = lang * 0x300;
      out.fill(0, entry, entry + 0x200);
      out.set(raw, entry);
    }
  }
  return out;
}

function asciiBytes(s) {
  if (!/^[\x00-\x7f]*$/.test(s)) throw new PackError('the display version must be ASCII');
  return enc.encode(s);
}

// ---- NPDM (tools/build_full_nsp.py npdm_config / build_npdm; port of switch-tools npdmtool, ISC) ---------------------

export function npdmConfig(titleId, { emulatorCompatible = false } = {}) {
  return {
    name: 'Mass Effect',
    program_id: BigInt(titleId),
    main_thread_stack_size: 0x100000,
    main_thread_priority: 44,
    default_cpu_id: 0,
    version: 0,
    address_space_type: 3,
    is_64_bit: true,
    is_retail: true,
    pool_partition: 0,
    fs_permissions: 0xFFFFFFFFFFFFFFFFn,
    content_owner_ids: [0x0100000000001000n],
    service_host: ['*'],
    service_access: ['*'],
    kernel_flags: [59, 28, 2, 0],
    syscalls: Array.from({ length: 0xC0 }, (_, i) => i),
    application_type: 1,
    min_kernel_version: 0x30,
    handle_table_size: 512,
    debug_flags: [false, false, true],
    legacy_debug_flags: emulatorCompatible,
    map_regions: [[1, true]],
  };
}

export function buildNpdm(cfg, acidModulus = new Uint8Array(0x100)) {
  const caps = [];
  const [hiPrio, loPrio, hiCpu, loCpu] = cfg.kernel_flags;
  const realHi = Math.min(hiPrio, loPrio), realLo = Math.max(hiPrio, loPrio);
  const desc = (((((hiCpu << 8) | loCpu) << 6) | (realHi & 0x3F)) << 6) | (realLo & 0x3F);
  caps.push(desc * 16 + 0x7);
  const descriptors = new Array(8).fill(0);
  for (const sc of cfg.syscalls) descriptors[Math.floor(sc / 0x18)] |= 1 << (sc % 0x18);
  descriptors.forEach((d, i) => { if (d) caps.push((d + i * 0x1000000) * 32 + 0xF); });
  caps.push(((cfg.application_type & 7) << 14) | 0x1FFF);
  caps.push(((cfg.min_kernel_version & 0xFFFF) << 15) | 0x3FFF);
  caps.push((cfg.handle_table_size << 16) | 0x7FFF);
  const [allow, forceProd, force] = cfg.debug_flags.map((x) => (x ? 1 : 0));
  if (cfg.legacy_debug_flags) caps.push(((allow | ((force || forceProd) ? 1 : 0) << 1) << 17) | 0xFFFF);
  else caps.push(((allow | (forceProd << 1) | (force << 2)) << 17) | 0xFFFF);
  const regions = [...cfg.map_regions];
  while (regions.length < 3) regions.push([0, false]);
  let cap = 0x3FF;
  regions.slice(0, 3).forEach(([rtype, ro], i) => { cap += ((rtype & 0x3F) | ((ro ? 1 : 0) << 6)) * 2 ** (11 + 7 * i); });
  caps.push(cap);
  const kac = new Uint8Array(caps.length * 4);
  caps.forEach((c, i) => view(kac).setUint32(i * 4, c % 0x100000000, true));

  const sacParts = [];
  for (const name of cfg.service_host) sacParts.push(Uint8Array.of((name.length - 1) | 0x80), enc.encode(name));
  for (const name of cfg.service_access) sacParts.push(Uint8Array.of(name.length - 1), enc.encode(name));
  const sac = concat(sacParts);

  // ACI0
  const cois = cfg.content_owner_ids;
  const coi = new Uint8Array(cois.length ? 4 + 8 * cois.length : 0);
  if (cois.length) {
    const f = new Fields(coi).u32(0, cois.length);
    cois.forEach((c, i) => f.u64(4 + 8 * i, c));
  }
  const fah = new Uint8Array(0x1C + coi.length);
  new Fields(fah).u32(0, 1).u64(4, cfg.fs_permissions).u32(12, 0x1C).u32(16, coi.length).u32(20, 0x1C + coi.length).u32(24, 0);
  fah.set(coi, 0x1C);
  const aci0SacOff = align(0x40 + fah.length, 0x10);
  const aci0KacOff = align(aci0SacOff + sac.length, 0x10);
  const aci0 = new Uint8Array(aci0KacOff + kac.length);
  aci0.set(enc.encode('ACI0'), 0);
  new Fields(aci0).u64(0x10, cfg.program_id).u32(0x20, 0x40).u32(0x24, fah.length).u32(0x28, aci0SacOff)
    .u32(0x2C, sac.length).u32(0x30, aci0KacOff).u32(0x34, kac.length);
  aci0.set(fah, 0x40);
  aci0.set(sac, aci0SacOff);
  aci0.set(kac, aci0KacOff);

  // ACID
  const fac = new Uint8Array(44);
  new Fields(fac).u8(0, 1).u8(1, 0).u8(2, 0).u64(4, cfg.fs_permissions);
  const acidSacOff = align(0x240 + fac.length, 0x10);
  const acidKacOff = align(acidSacOff + sac.length, 0x10);
  const acid = new Uint8Array(acidKacOff + kac.length);
  acid.set(acidModulus, 0x100);
  const flags = (cfg.is_retail ? 1 : 0) | ((cfg.pool_partition & 3) << 2);
  acid.set(enc.encode('ACID'), 0x200);
  new Fields(acid).u32(0x204, acid.length - 0x100).u32(0x208, 0).u32(0x20C, flags).u64(0x210, cfg.program_id)
    .u64(0x218, cfg.program_id).u32(0x220, 0x240).u32(0x224, fac.length).u32(0x228, acidSacOff).u32(0x22C, sac.length)
    .u32(0x230, acidKacOff).u32(0x234, kac.length);
  acid.set(fac, 0x240);
  acid.set(sac, acidSacOff);
  acid.set(kac, acidKacOff);

  const header = new Uint8Array(0x80);
  header.set(enc.encode('META'), 0);
  const mmu = ((cfg.address_space_type & 3) << 1) | (cfg.is_64_bit ? 1 : 0);
  new Fields(header).u8(0x0C, mmu).u8(0x0E, cfg.main_thread_priority).u8(0x0F, cfg.default_cpu_id)
    .u32(0x18, cfg.version).u32(0x1C, cfg.main_thread_stack_size);
  header.set(enc.encode(cfg.name).subarray(0, 0xF), 0x20);
  const acidOff = 0x80;
  const aci0Off = align(acidOff + acid.length, 0x10);
  new Fields(header).u32(0x70, aci0Off).u32(0x74, aci0.length).u32(0x78, acidOff).u32(0x7C, acid.length);
  const out = new Uint8Array(aci0Off + aci0.length);
  out.set(header, 0);
  out.set(acid, acidOff);
  out.set(aci0, aci0Off);
  return out;
}

// ---- PFS0 ------------------------------------------------------------------------------------------------------------

/** entries: [[name, size]] -> the header (string table zero-padded so the header is 0x20-aligned). */
export function pfs0Header(entries, headerAlign = 0x20) {
  const nameParts = [];
  const offsets = [];
  let namesLen = 0;
  for (const [name] of entries) {
    offsets.push(namesLen);
    const b = enc.encode(name);
    nameParts.push(b, new Uint8Array(1));
    namesLen += b.length + 1;
  }
  const raw = 0x10 + 0x18 * entries.length + namesLen;
  const padded = namesLen + (align(raw, headerAlign) - raw);
  const out = new Uint8Array(0x10 + 0x18 * entries.length + padded);
  out.set(enc.encode('PFS0'), 0);
  const f = new Fields(out).u32(4, entries.length).u32(8, padded).u32(12, 0);
  let dataOff = 0;
  entries.forEach(([, size], i) => {
    const o = 0x10 + 0x18 * i;
    f.u64(o, dataOff).u64(o + 8, size).u32(o + 16, offsets[i]).u32(o + 20, 0);
    dataOff += size;
  });
  out.set(concat(nameParts), 0x10 + 0x18 * entries.length);
  return out;
}

export function pfs0Bytes(files) {
  return concat([pfs0Header(files.map(([n, d]) => [n, d.length])), ...files.map(([, d]) => d)]);
}

// ---- RomFS (switch-tools build_romfs layout) ----------------------------------------------------------------------------

/** The RomFS entry hash libnx uses for lookups: name is a Uint8Array (UTF-8 bytes). */
export function romfsHash(parentOffset, name) {
  let h = (parentOffset ^ 123456789) >>> 0;
  for (const c of name) {
    h = ((h >>> 5) | (h << 27)) >>> 0;
    h = (h ^ c) >>> 0;
  }
  return h;
}

export function romfsTableCount(n) {
  if (n < 3) return 3;
  if (n < 19) return n | 1;
  while ([2, 3, 5, 7, 11, 13, 17].some((p) => n % p === 0)) n++;
  return n;
}

/**
 * A RomFS file: bytes, or a size and a re-readable chunks() (async iterable of Uint8Array). A source with
 * `seekable: true` takes chunks(from) and starts at byte `from`; others are read from the start and the head dropped.
 */
export function sourceFromBytes(bytes) {
  return { size: bytes.length, seekable: true, chunks: async function* (from = 0) { if (bytes.length > from) yield from ? bytes.subarray(from) : bytes; } };
}

/** Drops the first `n` bytes of an async byte stream. */
export async function* skipBytes(stream, n) {
  for await (const block of stream) {
    if (n >= block.length) { n -= block.length; continue; }
    yield n ? block.subarray(n) : block;
    n = 0;
  }
}

/** A source's bytes from `from` (its own seek when it has one). */
export function sourceChunksFrom(src, from) {
  if (!from) return src.chunks();
  return src.seekable ? src.chunks(from) : skipBytes(src.chunks(), from);
}

class RomfsDir {
  constructor(name, parent) {
    this.name = name; // Uint8Array
    this.parent = parent;
    this.dirs = new Map(); // key: name string
    this.files = new Map();
    this.offset = 0;
  }
  sortedDirs() { return [...this.dirs.values()].sort((a, b) => compareBytes(a.name, b.name)); }
  sortedFiles() { return [...this.files.values()].sort((a, b) => compareBytes(a.name, b.name)); }
}

export class Romfs {
  constructor() {
    this.root = new RomfsDir(new Uint8Array(0), null);
    this.count = 0;
    this.dataBytes = 0;
  }

  add(path, source) {
    const parts = String(path).split('/').filter(Boolean);
    if (!parts.length) throw new PackError(`bad RomFS path '${path}'`);
    let node = this.root;
    for (const part of parts.slice(0, -1)) {
      if (node.files.has(part)) throw new PackError(`RomFS path conflict at '${path}'`);
      if (!node.dirs.has(part)) node.dirs.set(part, new RomfsDir(enc.encode(part), node));
      node = node.dirs.get(part);
    }
    const last = parts[parts.length - 1];
    if (node.files.has(last) || node.dirs.has(last)) throw new PackError(`duplicate RomFS path '${path}'`);
    node.files.set(last, { name: enc.encode(last), source, dir: node });
    this.count++;
    this.dataBytes += source.size;
  }

  layout() {
    const dirs = [];
    const visit = (d) => { dirs.push(d); for (const c of d.sortedDirs()) visit(c); };
    visit(this.root);
    let off = 0;
    for (const d of dirs) { d.offset = off; off += 0x18 + align(d.name.length, 4); }
    const dirTableSize = off;
    const files = [];
    let entryOff = 0;
    let dataOff = 0;
    for (const d of dirs) {
      for (const f of d.sortedFiles()) {
        dataOff = align(dataOff, 0x10);
        f.entryOff = entryOff;
        f.dataOff = dataOff;
        files.push(f);
        entryOff += 0x20 + align(f.name.length, 4);
        dataOff += f.source.size;
      }
    }
    const fileTableSize = entryOff;
    const dataSize = dataOff;
    const dirCount = romfsTableCount(dirs.length), fileCount = romfsTableCount(files.length);
    const dirHash = new Array(dirCount).fill(ROMFS_EMPTY);
    const fileHash = new Array(fileCount).fill(ROMFS_EMPTY);
    const dirTable = new Uint8Array(dirTableSize);
    const fileTable = new Uint8Array(fileTableSize);
    const ft = new Fields(fileTable);
    const firstFile = new Map();
    for (const f of files) if (!firstFile.has(f.dir)) firstFile.set(f.dir, f.entryOff);
    for (const d of dirs) {
      const list = d.sortedFiles();
      list.forEach((f, i) => {
        const sibling = i + 1 < list.length ? list[i + 1].entryOff : ROMFS_EMPTY;
        const h = romfsHash(d.offset, f.name) % fileCount;
        ft.u32(f.entryOff, d.offset).u32(f.entryOff + 4, sibling).u64(f.entryOff + 8, 0).u64(f.entryOff + 16, f.source.size)
          .u32(f.entryOff + 24, fileHash[h]).u32(f.entryOff + 28, f.name.length);
        fileHash[h] = f.entryOff;
        fileTable.set(f.name, f.entryOff + 0x20);
      });
    }
    for (const f of files) ft.u64(f.entryOff + 8, f.dataOff);
    const dt = new Fields(dirTable);
    for (const d of dirs) {
      const children = d.sortedDirs();
      const child = children.length ? children[0].offset : ROMFS_EMPTY;
      let parent, sibling;
      if (d.parent === null) {
        parent = 0; sibling = ROMFS_EMPTY;
      } else {
        parent = d.parent.offset;
        const peers = d.parent.sortedDirs();
        const i = peers.indexOf(d);
        sibling = i + 1 < peers.length ? peers[i + 1].offset : ROMFS_EMPTY;
      }
      const h = romfsHash(d.parent !== null ? parent : 0, d.name) % dirCount;
      dt.u32(d.offset, parent).u32(d.offset + 4, sibling).u32(d.offset + 8, child)
        .u32(d.offset + 12, firstFile.has(d) ? firstFile.get(d) : ROMFS_EMPTY).u32(d.offset + 16, dirHash[h])
        .u32(d.offset + 20, d.name.length);
      dirHash[h] = d.offset;
      dirTable.set(d.name, d.offset + 0x18);
    }
    const metaOff = align(ROMFS_DATA_OFFSET + dataSize, 0x10);
    const dh = new Uint8Array(dirCount * 4);
    dirHash.forEach((v, i) => view(dh).setUint32(i * 4, v, true));
    const fh = new Uint8Array(fileCount * 4);
    fileHash.forEach((v, i) => view(fh).setUint32(i * 4, v, true));
    const header = new Uint8Array(0x50);
    const hf = new Fields(header);
    [0x50, metaOff, dh.length, metaOff + dh.length, dirTable.length, metaOff + dh.length + dirTable.length, fh.length,
      metaOff + dh.length + dirTable.length + fh.length, fileTable.length, ROMFS_DATA_OFFSET]
      .forEach((v, i) => hf.u64(i * 8, v));
    this.header = header;
    this.meta = concat([dh, dirTable, fh, fileTable]);
    this.metaOff = metaOff;
    this.placements = files.map((f) => [f.dataOff, f.source]);
    this.fileRanges = files.map((f) => [RomfsPath(f), ROMFS_DATA_OFFSET + f.dataOff, f.source.size]);
    this.size = metaOff + this.meta.length;
    return this.size;
  }

  /**
   * The whole image, in order (call layout() first), or its tail from byte `from`: files before it are not read, the
   * file that contains it starts at its inner offset (sourceChunksFrom).
   */
  async *chunks(from = 0) {
    if (from < ROMFS_DATA_OFFSET) {
      const head = new Uint8Array(ROMFS_DATA_OFFSET);
      head.set(this.header);
      yield from ? head.subarray(from) : head;
    }
    let pos = Math.max(from, ROMFS_DATA_OFFSET);
    for (const [doff, src] of this.placements) {
      const target = ROMFS_DATA_OFFSET + doff;
      if (target + src.size <= pos && src.size) continue;
      if (target > pos) { yield new Uint8Array(target - pos); pos = target; }
      if (!src.size) continue;
      const inner = pos - target;
      let got = inner;
      for await (const block of sourceChunksFrom(src, inner)) {
        yield block;
        pos += block.length;
        got += block.length;
      }
      if (got !== src.size) throw new PackError(`a file of the RomFS changed size while packing (${got} != ${src.size})`);
    }
    if (this.metaOff > pos) { yield new Uint8Array(this.metaOff - pos); pos = this.metaOff; }
    if (pos - this.metaOff < this.meta.length) yield this.meta.subarray(pos - this.metaOff);
  }
}

function RomfsPath(f) {
  const parts = [dec.decode(f.name)];
  let d = f.dir;
  while (d.parent !== null) { parts.push(dec.decode(d.name)); d = d.parent; }
  return parts.reverse().join('/');
}

// ---- hashing helpers ---------------------------------------------------------------------------------------------------

/** Growable byte list (32-byte hashes of an 8 GB image: 17 MB, kept in one buffer). */
class ByteList {
  constructor(capacity = 1 << 16) { this.buf = new Uint8Array(capacity); this.length = 0; }
  push(bytes) {
    if (this.length + bytes.length > this.buf.length) {
      const next = new Uint8Array(Math.max(this.buf.length * 2, this.length + bytes.length));
      next.set(this.buf.subarray(0, this.length));
      this.buf = next;
    }
    this.buf.set(bytes, this.length);
    this.length += bytes.length;
  }
  bytes() { return this.buf.subarray(0, this.length); }
}

/** SHA-256 of every block of a stream; the last block zero-padded (IVFC) or not (HierarchicalSha256). */
class BlockHasher {
  constructor(block, padLast) {
    this.block = block; this.padLast = padLast;
    this.pending = new Uint8Array(block);
    this.pendingLen = 0;
    this.hashes = new ByteList();
    this.total = 0;
  }

  async update(data) {
    this.total += data.length;
    let off = 0;
    if (this.pendingLen) {
      const need = Math.min(this.block - this.pendingLen, data.length);
      this.pending.set(data.subarray(0, need), this.pendingLen);
      this.pendingLen += need;
      off = need;
      if (this.pendingLen < this.block) return;
      this.hashes.push(sha256(this.pending));
      this.pendingLen = 0;
    }
    const full = off + Math.floor((data.length - off) / this.block) * this.block;
    if (full > off) {
      const pieces = [];
      for (let p = off; p < full; p += this.block) pieces.push(data.subarray(p, p + this.block));
      for (const h of await sha256Many(pieces)) this.hashes.push(h);
    }
    if (full < data.length) {
      this.pending.set(data.subarray(full), 0);
      this.pendingLen = data.length - full;
    }
  }

  finish() {
    if (this.pendingLen) {
      const tail = this.padLast ? (this.pending.fill(0, this.pendingLen), this.pending) : this.pending.subarray(0, this.pendingLen);
      this.hashes.push(sha256(tail));
      this.pendingLen = 0;
    }
    return this.hashes.bytes().slice();
  }
}

async function hashBlocks(data, block, padLast) {
  const h = new BlockHasher(block, padLast);
  await h.update(data);
  return h.finish();
}

function hashBlocksSync(data, block, padLast) {
  const out = new ByteList(Math.ceil(data.length / block) * 32 + 32);
  for (let p = 0; p < data.length; p += block) {
    let piece = data.subarray(p, Math.min(p + block, data.length));
    if (padLast && piece.length < block) { const z = new Uint8Array(block); z.set(piece); piece = z; }
    out.push(sha256(piece));
  }
  return out.bytes().slice();
}

/** Re-chunks an async byte stream into blocks of `size` bytes (the last one shorter). */
async function* rechunk(stream, size, signal) {
  let buf = null;
  let fill = 0;
  for await (const block of stream) {
    throwIfAborted(signal);
    let off = 0;
    while (off < block.length) {
      if (fill === 0 && block.length - off >= size) {
        yield block.subarray(off, off + size);
        off += size;
        continue;
      }
      if (!buf) buf = new Uint8Array(size);
      const take = Math.min(size - fill, block.length - off);
      buf.set(block.subarray(off, off + take), fill);
      fill += take;
      off += take;
      if (fill === size) { yield buf; buf = null; fill = 0; }
    }
  }
  if (fill) yield buf.subarray(0, fill);
}

/**
 * SHA-256 of fixed-size chunks of given ranges of a stream: ranges = [[key, start, end]], sorted, disjoint.
 * Chunk k of a range covers [start + k*chunk, min(start + (k+1)*chunk, end)).
 */
class RangeChunkHasher {
  constructor(ranges, chunk) {
    this.ranges = ranges; this.chunk = chunk;
    this.index = 0;
    this.pos = 0;
    this.current = null;
    this.currentSlot = null;
    this.result = new Map();
  }

  async update(data) {
    let off = 0;
    const batch = [];
    while (off < data.length) {
      if (this.index >= this.ranges.length) { this.pos += data.length - off; break; }
      const [key, start, end] = this.ranges[this.index];
      if (this.pos < start) {
        const skip = Math.min(data.length - off, start - this.pos);
        off += skip;
        this.pos += skip;
        continue;
      }
      if (!this.result.has(key)) this.result.set(key, []);
      const list = this.result.get(key);
      const chunkEnd = Math.min(start + (Math.floor((this.pos - start) / this.chunk) + 1) * this.chunk, end);
      const take = Math.min(data.length - off, chunkEnd - this.pos);
      if (this.current === null && take === chunkEnd - this.pos) {
        // The whole chunk is in this block: hash it natively, in a batch.
        list.push(null);
        batch.push([list, list.length - 1, data.subarray(off, off + take)]);
      } else {
        if (this.current === null) { this.current = new Sha256(); list.push(null); this.currentSlot = [list, list.length - 1]; }
        this.current.update(data.subarray(off, off + take));
        if (this.pos + take === chunkEnd) {
          const [l, i] = this.currentSlot;
          l[i] = this.current.digest();
          this.current = null;
        }
      }
      off += take;
      this.pos += take;
      if (this.pos === chunkEnd && this.pos === end) this.index++;
    }
    if (batch.length) {
      const digests = await sha256Many(batch.map((b) => b[2]));
      batch.forEach(([l, i], k) => { l[i] = digests[k]; });
    }
  }
}

function fileHashRanges(romfs) {
  return romfs.fileRanges.filter(([, , size]) => size).map(([path, off, size]) => [path, off, off + align(size, 0x10)]);
}

// ---- NCA sections ------------------------------------------------------------------------------------------------------

class Section {
  /** chunkFn: () => async iterable of plaintext bytes; size: a multiple of 0x200. */
  constructor(fsHeader, size, chunkFn, generation = 0, secureValue = 0) {
    if (fsHeader.length !== 0x200 || size % MEDIA_UNIT) throw new Error('bad NCA section');
    const fs = fsHeader.slice();
    new Fields(fs).u32(0x140, generation).u32(0x144, secureValue);
    this.fsHeader = fs;
    this.size = size;
    this.chunks = chunkFn;
    this.ctrUpper = (BigInt(secureValue) << 32n) | BigInt(generation);
    // chunks(from) starts at byte `from` of the section (else NcaBuilder drops the head of a full read).
    this.seekable = false;
  }
}

async function pfs0Section(pfs0, hashBlock) {
  const table = hashBlocksSync(pfs0, hashBlock, false);
  const pfs0Off = align(table.length, MEDIA_UNIT);
  const bodyLen = align(pfs0Off + pfs0.length, MEDIA_UNIT);
  const body = new Uint8Array(bodyLen);
  body.set(table, 0);
  body.set(pfs0, pfs0Off);
  const fs = new Uint8Array(0x200);
  new Fields(fs).u16(0, 2).u8(2, 1).u8(3, 2).u8(4, 3)
    .bytes(0x08, sha256(table))
    .u32(0x28, hashBlock).u32(0x2C, 2).u64(0x30, 0).u64(0x38, table.length).u64(0x40, pfs0Off).u64(0x48, pfs0.length);
  const section = new Section(fs, body.length, async function* (from = 0) { if (from < body.length) yield body.subarray(from); });
  section.seekable = true;
  return section;
}

/** IVFC (HierarchicalIntegrity) over a data image (tools/build_full_nsp.py IvfcPlan). */
class IvfcPlan {
  /** dataHashes: level-6 block hashes (null for a plan rebuilt from sizes only, see forImage). */
  static async create(dataHashes, dataSize) {
    const plan = new IvfcPlan();
    const levels = new Array(IVFC_LEVELS);
    levels[IVFC_LEVELS - 1] = [dataSize, null];
    let below = dataHashes;
    for (let i = IVFC_LEVELS - 2; i >= 0; i--) {
      levels[i] = [below.length, below];
      below = i ? await hashBlocks(below, IVFC_BLOCK, true) : null;
    }
    const level1 = levels[0][1];
    if (level1.length > IVFC_BLOCK) throw new PackError('RomFS too large for 6 IVFC levels');
    const padded = new Uint8Array(IVFC_BLOCK);
    padded.set(level1);
    plan.master = sha256(padded);
    plan.#place(levels);
    return plan;
  }

  /** The plan of an existing section from its image size and master hash (levels not available). */
  static forImage(imageSize, master) {
    const plan = new IvfcPlan();
    const levels = new Array(IVFC_LEVELS);
    levels[IVFC_LEVELS - 1] = [imageSize, null];
    let below = imageSize;
    for (let i = IVFC_LEVELS - 2; i >= 0; i--) {
      const size = Math.ceil(below / IVFC_BLOCK) * 32;
      levels[i] = [size, null];
      below = size;
    }
    if (levels[0][0] > IVFC_BLOCK) throw new PackError('RomFS too large for 6 IVFC levels');
    plan.master = master;
    plan.#place(levels);
    return plan;
  }

  #place(levels) {
    this.levels = levels;
    this.offsets = [];
    let off = 0;
    for (const [size] of levels) {
      this.offsets.push(off);
      off = align(off + size, IVFC_BLOCK);
    }
    this.size = off;
  }

  fsHeader() {
    const fs = new Uint8Array(0x200);
    const f = new Fields(fs).u16(0, 2).u8(2, 0).u8(3, 3).u8(4, 3);
    fs.set(enc.encode('IVFC'), 0x08);
    f.u32(0x0C, 0x20000).u32(0x10, 0x20).u32(0x14, IVFC_LEVELS + 1);
    this.levels.forEach(([size], i) => {
      f.u64(0x18 + 0x18 * i, this.offsets[i]).u64(0x20 + 0x18 * i, size).u32(0x28 + 0x18 * i, IVFC_BLOCK_LOG2).u32(0x2C + 0x18 * i, 0);
    });
    fs.set(this.master, 0xC8);
    return fs;
  }

  /** The section image from byte `from`; dataChunks(inner) gives the data level (level 6) from its byte `inner`. */
  async *chunks(dataChunks, from = 0) {
    let pos = from;
    for (let i = 0; i < this.levels.length; i++) {
      const [size, blob] = this.levels[i];
      const start = this.offsets[i];
      if (start + size <= pos) continue;
      if (start > pos) { yield new Uint8Array(start - pos); pos = start; }
      const inner = pos - start;
      if (blob !== null) {
        yield inner ? blob.subarray(inner) : blob;
        pos = start + blob.length;
      } else {
        for await (const block of dataChunks(inner)) { yield block; pos += block.length; }
      }
    }
    if (this.size > pos) yield new Uint8Array(this.size - pos);
  }
}

/**
 * Pass 1: hashes the whole image (IVFC levels) and every file in 64 KiB chunks (for patches). Returns the Section;
 * romfs.plan is the IVFC plan and romfs.chunkHashes a Map path -> [Uint8Array(32)].
 */
async function romfsSection(romfs, { progress, signal, chunkBytes = CHUNK } = {}) {
  romfs.layout();
  const hasher = new BlockHasher(IVFC_BLOCK, true);
  const chunks = new RangeChunkHasher(fileHashRanges(romfs), PATCH_CHUNK);
  let done = 0;
  for await (const block of rechunk(romfs.chunks(), chunkBytes, signal)) {
    await hasher.update(block);
    await chunks.update(block);
    done += block.length;
    progress?.(done, romfs.size);
  }
  if (hasher.total !== romfs.size) throw new PackError('RomFS size changed while hashing (files modified?)');
  const plan = await IvfcPlan.create(hasher.finish(), romfs.size);
  romfs.plan = plan;
  romfs.chunkHashes = chunks.result;
  const section = new Section(plan.fsHeader(), plan.size, (from = 0) => plan.chunks((inner) => romfs.chunks(inner), from));
  section.seekable = true;
  return section;
}

// ---- NCA ---------------------------------------------------------------------------------------------------------------

class NcaBuilder {
  constructor(keys, titleId, contentType, sections, { aesKey = null, signer = null, signal, chunkBytes = CHUNK } = {}) {
    this.keys = keys;
    this.titleId = BigInt(titleId);
    this.contentType = contentType;
    this.sections = sections;
    this.aesKey = aesKey ?? randomBytes(16);
    this.signer = signer;
    this.signal = signal;
    this.chunkBytes = chunkBytes;
    this.starts = [];
    let off = 0xC00;
    for (const s of sections) { this.starts.push(off); off += s.size; }
    this.size = off;
  }

  /** The encrypted header. Made once: the RSA-PSS signature is randomised, and every read must see the same bytes. */
  header() {
    this.headerPromise ??= this.#makeHeader();
    return this.headerPromise;
  }

  async #makeHeader() {
    const h = new Uint8Array(0xC00);
    h.set(enc.encode('NCA3'), 0x200);
    const f = new Fields(h).u8(0x204, 0).u8(0x205, this.contentType).u8(0x206, 0).u8(0x207, 0)
      .u64(0x208, this.size).u64(0x210, this.titleId).u32(0x218, 0).u32(0x21C, SDK_VERSION);
    this.sections.forEach((s, i) => {
      const start = this.starts[i];
      f.u32(0x240 + 0x10 * i, start / MEDIA_UNIT).u32(0x244 + 0x10 * i, (start + s.size) / MEDIA_UNIT).u32(0x248 + 0x10 * i, 1).u32(0x24C + 0x10 * i, 0);
      h.set(sha256(s.fsHeader), 0x280 + 0x20 * i);
      h.set(s.fsHeader, 0x400 + 0x200 * i);
    });
    const keyArea = new Uint8Array(0x40);
    keyArea.set(this.aesKey, 0x20);
    h.set(aesEcb(this.keys.key_area_key_application_00, keyArea), 0x300);
    if (this.signer) h.set(await this.signer.sign(h.slice(0x200, 0x400)), 0x100);
    return aesXts(this.keys.header_key, h);
  }

  /** The encrypted NCA, in order, from byte `from` (a multiple of 16: AES-CTR restarts at any block). */
  async *chunks(from = 0) {
    if (from % 16) throw new Error('NcaBuilder.chunks: the start must be 16-aligned');
    if (from < 0xC00) {
      const h = await this.header();
      yield from ? h.subarray(from) : h;
    }
    const ctr = new AesCtr(this.aesKey);
    for (let i = 0; i < this.sections.length; i++) {
      const s = this.sections[i];
      const start = this.starts[i];
      if (start + s.size <= from) continue;
      const inner = Math.max(0, from - start);
      let pos = start + inner;
      let written = inner;
      const stream = !inner ? s.chunks() : s.seekable ? s.chunks(inner) : skipBytes(s.chunks(), inner);
      for await (const block of rechunk(stream, this.chunkBytes, this.signal)) {
        yield await ctr.apply(s.ctrUpper, pos, block);
        pos += block.length;
        written += block.length;
      }
      if (written !== s.size) throw new PackError(`NCA section size mismatch (${written} != ${s.size})`);
    }
  }

  async toBytes() {
    const parts = [];
    for await (const c of this.chunks()) parts.push(c);
    return concat(parts);
  }
}

/** Decrypts an NCA header (3 KiB, AES-XTS). */
export function decryptNcaHeader(data, headerKey) {
  return aesXts(headerKey, data.subarray(0, 0xC00), { decrypt: true });
}

export function buildCnmt(titleId, version, records, metaType = CNMT_APPLICATION, applicationId = null) {
  let ext;
  if (metaType === CNMT_APPLICATION) {
    ext = new Uint8Array(16);
    new Fields(ext).u64(0, BigInt(titleId) + 0x800n);
  } else {
    ext = new Uint8Array(24);
    new Fields(ext).u64(0, applicationId);
  }
  const head = new Uint8Array(0x20);
  new Fields(head).u64(0, titleId).u32(8, version).u8(12, metaType).u8(13, 0).u16(14, ext.length).u16(16, records.length)
    .u16(18, 0).u8(20, 0).u32(24, 0);
  const recs = records.map(([digest, size, ctype]) => {
    const r = new Uint8Array(0x38);
    r.set(digest, 0);
    r.set(digest.subarray(0, 16), 0x20);
    const dv = view(r);
    dv.setUint32(0x30, size % 0x100000000, true);
    dv.setUint16(0x34, Math.floor(size / 0x100000000), true);
    r[0x36] = ctype;
    r[0x37] = 0;
    return r;
  });
  return concat([head, ext, ...recs, new Uint8Array(0x20)]);
}

// ---- base metadata and reading a base NSP --------------------------------------------------------------------------------

/** The base metadata object of a full pack (tools/build_full_nsp.py base_metadata); insertion order matters for JSON. */
function baseMetadata(titleId, romfs, programNca, dataDir) {
  const plan = romfs.plan;
  const files = {};
  for (const [path, off, size] of romfs.fileRanges) {
    files[path] = { offset: off, size, hashes: base64Encode(concat(romfs.chunkHashes.get(path) ?? [])) };
  }
  return {
    format: BASE_META_FORMAT,
    kind: 'masseffect-nx base RomFS',
    title_id: hex16(titleId),
    program_nca: programNca,
    data_dir: dataDir,
    romfs: {
      section_size: plan.size, l6_offset: plan.offsets[IVFC_LEVELS - 1], image_size: romfs.size,
      master_hash: toHex(plan.master), chunk: PATCH_CHUNK,
    },
    files,
  };
}

/** JSON exactly as Python's json.dump writes it (separators ", " and ": ", non-ASCII escaped). */
export function pythonJson(value) {
  if (value === null || value === undefined) return 'null';
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  if (typeof value === 'number') {
    if (!Number.isInteger(value)) throw new Error('pythonJson: only integers are supported');
    return String(value);
  }
  if (typeof value === 'bigint') return value.toString();
  if (typeof value === 'string') {
    return JSON.stringify(value).replace(/[\u007f-￿]/g, (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`);
  }
  if (Array.isArray(value)) return `[${value.map(pythonJson).join(', ')}]`;
  return `{${Object.entries(value).map(([k, v]) => `${pythonJson(k)}: ${pythonJson(v)}`).join(', ')}}`;
}

/** Parses and checks a <base>.nsp.basemeta.json text. */
export function parseBaseMetadata(text) {
  let meta;
  try {
    meta = JSON.parse(text);
  } catch (e) {
    throw new PackError(`the base metadata is not valid JSON (${e.message})`);
  }
  if (meta?.format !== BASE_META_FORMAT || meta?.romfs?.chunk !== PATCH_CHUNK || typeof meta?.files !== 'object' || !/^[0-9a-f]{16}$/.test(meta?.title_id ?? '')) {
    throw new PackError('unsupported base metadata format');
  }
  return meta;
}

/** Random access over one file or the parts of a split NSP (00, 01, ...): [{size, read(offset, length)}]. */
export class PartsReader {
  /** parts: Blob/File objects in order, or objects { size, async read(offset, length) -> Uint8Array }. */
  constructor(parts) {
    if (!parts.length) throw new PackError('no base NSP file');
    this.parts = parts.map((p) => (typeof p.read === 'function' ? p : {
      size: p.size,
      read: async (offset, length) => new Uint8Array(await p.slice(offset, offset + length).arrayBuffer()),
    }));
    this.size = this.parts.reduce((a, p) => a + p.size, 0);
  }

  async read(offset, length) {
    const out = new Uint8Array(length);
    let got = 0;
    for (const p of this.parts) {
      if (offset >= p.size) { offset -= p.size; continue; }
      const n = Math.min(length - got, p.size - offset);
      out.set(await p.read(offset, n), got);
      got += n;
      offset = 0;
      if (got === length) break;
    }
    if (got !== length) throw new PackError('unexpected end of the base NSP');
    return out;
  }
}

class NcaReader {
  static async open(src, offset, keys) {
    const r = new NcaReader();
    r.src = src;
    r.offset = offset;
    r.h = decryptNcaHeader(await src.read(offset, 0xC00), keys.header_key);
    if (dec.decode(r.h.subarray(0x200, 0x204)) !== 'NCA3') throw new PackError('base NCA header does not decrypt (wrong header_key?)');
    r.contentType = r.h[0x205];
    const dv = view(r.h);
    r.size = Number(dv.getBigUint64(0x208, true));
    r.titleId = dv.getBigUint64(0x210, true);
    if (Math.max(r.h[0x206], r.h[0x220]) > 1 || r.h[0x207] !== 0 || r.h.subarray(0x230, 0x240).some((b) => b)) {
      throw new PackError('base NCA uses another key generation or a title key (not made by this packer)');
    }
    r.ctr = new AesCtr(aesEcb(keys.key_area_key_application_00, r.h.slice(0x300, 0x340), true).slice(0x20, 0x30));
    return r;
  }

  section(index) {
    const dv = view(this.h);
    const start = dv.getUint32(0x240 + 0x10 * index, true) * MEDIA_UNIT;
    const end = dv.getUint32(0x244 + 0x10 * index, true) * MEDIA_UNIT;
    return [start, end, this.h.subarray(0x400 + 0x200 * index, 0x600 + 0x200 * index)];
  }

  async readSection(index, offset, length) {
    const [start, , fs] = this.section(index);
    if (fs[4] !== 3) throw new PackError('base RomFS section is not plain AES-CTR (a patch cannot be a base)');
    const aligned = offset - (offset % 16);
    const dv = view(fs);
    const upper = (BigInt(dv.getUint32(0x144, true)) << 32n) | BigInt(dv.getUint32(0x140, true));
    const raw = await this.src.read(this.offset + start + aligned, offset - aligned + length);
    const padded = raw.length % 16 ? concat([raw, new Uint8Array(16 - (raw.length % 16))]) : raw;
    const plain = await this.ctr.apply(upper, start + aligned, padded);
    return plain.subarray(offset - aligned, offset - aligned + length);
  }
}

async function readPfs0Entries(src) {
  const head = await src.read(0, 0x10);
  if (dec.decode(head.subarray(0, 4)) !== 'PFS0') throw new PackError('the base is not an NSP (no PFS0 header)');
  const dv = view(head);
  const count = dv.getUint32(4, true), strtab = dv.getUint32(8, true);
  if (count > 1024 || strtab > 0x100000) throw new PackError('the base is not an NSP (bad PFS0 header)');
  const table = await src.read(0x10, 0x18 * count + strtab);
  const tv = view(table);
  const names = table.subarray(0x18 * count);
  const dataOff = 0x10 + 0x18 * count + strtab;
  const out = [];
  for (let i = 0; i < count; i++) {
    const off = Number(tv.getBigUint64(0x18 * i, true));
    const size = Number(tv.getBigUint64(0x18 * i + 8, true));
    const nameOff = tv.getUint32(0x18 * i + 16, true);
    const end = names.indexOf(0, nameOff);
    out.push([dec.decode(names.subarray(nameOff, end < 0 ? names.length : end)), dataOff + off, size]);
  }
  return out;
}

/** Title ID and Program NCA name of a base NSP (reads only the container header and the NCA headers). */
export async function inspectBaseNsp(src, keys) {
  for (const [name, off] of await readPfs0Entries(src)) {
    if (name.endsWith('.nca') && !name.endsWith('.cnmt.nca')) {
      const nca = await NcaReader.open(src, off, keys);
      if (nca.contentType === CONTENT_PROGRAM) return { titleId: hex16(nca.titleId), programNca: name.slice(0, -4) };
    }
  }
  throw new PackError('the base NSP has no Program NCA');
}

/** data_dir= of a package marker (masseffect-nx-package.txt), or null (tools/build_full_nsp.py marker_data_dir). */
export function markerDataDir(bytes) {
  for (const line of new TextDecoder('utf-8').decode(bytes).split(/\r\n|\n|\r/)) {
    if (line.startsWith('data_dir=')) {
      const d = line.slice('data_dir='.length).trim();
      return validDataDir(d) ? d : null;
    }
  }
  return null;
}

/** '01a5eec700010000' from a BigInt, a number or a hex string (with or without 0x). */
export function titleIdHex(id) {
  const v = typeof id === 'string' ? BigInt(id.startsWith('0x') ? id : `0x${id}`) : BigInt(id);
  return v.toString(16).padStart(16, '0');
}

/**
 * An update of the title `expected` against a base of title baseTitleId (hex strings, BigInts or numbers). The base's
 * own title ID always wins; a base of another known edition (known: { titleIdHex: label }) is refused with a
 * PackError (the program of one edition over the game data of the other), any other mismatch returns a warning text.
 * Returns null when they match (tools/build_full_nsp.py check_update_title).
 */
export function checkUpdateTitle(baseTitleId, expected, known = {}) {
  const base = titleIdHex(baseTitleId), want = titleIdHex(expected);
  if (base === want) return null;
  const label = (id) => (known[id] ? `the ${known[id]} edition (title ${id})` : `title ${id}`);
  if (known[base]) {
    throw new PackError(`the base is ${label(base)}, but this update is for ${label(want)}; choose the base of the same edition`);
  }
  return `warning: the base has title ${base}, not ${label(want)}; the update keeps the base's title ID`;
}

/**
 * The base metadata of an existing full NSP (tools/build_full_nsp.py metadata_from_nsp): decrypts its Program NCA's
 * RomFS tables and hashes its files in 64 KiB chunks (one read of the RomFS section). src: a PartsReader.
 */
export async function metadataFromNsp(src, keys, { progress, signal, log, chunkBytes = CHUNK } = {}) {
  let program = null, programName = null;
  for (const [name, off] of await readPfs0Entries(src)) {
    if (name.endsWith('.nca') && !name.endsWith('.cnmt.nca')) {
      const nca = await NcaReader.open(src, off, keys);
      if (nca.contentType === CONTENT_PROGRAM) { program = nca; programName = name.slice(0, -4); }
    }
  }
  if (!program) throw new PackError('the base NSP has no Program NCA');
  const [start, end, fs] = program.section(1);
  if (fs[2] !== 0 || fs[3] !== 3 || dec.decode(fs.subarray(8, 12)) !== 'IVFC') throw new PackError('the base Program NCA has no RomFS section');
  const fdv = view(fs);
  const l6 = Number(fdv.getBigUint64(0x18 + 0x18 * 5, true));
  const imageSize = Number(fdv.getBigUint64(0x20 + 0x18 * 5, true));
  const hv = view(await program.readSection(1, l6, 0x50));
  const hdr = Array.from({ length: 10 }, (_, i) => Number(hv.getBigUint64(i * 8, true)));
  const [, , , dtOff, dtSize, , , ftOff, ftSize, dataOff] = hdr;
  const dirs = await program.readSection(1, l6 + dtOff, dtSize);
  const files = await program.readSection(1, l6 + ftOff, ftSize);
  const dd = view(dirs), fd = view(files);
  const found = [];
  const walk = (doff, prefix, depth) => {
    if (depth > 64) throw new PackError('the base RomFS is too deep (damaged?)');
    const child0 = dd.getUint32(doff + 8, true);
    let file = dd.getUint32(doff + 12, true);
    while (file !== ROMFS_EMPTY) {
      const sibling = fd.getUint32(file + 4, true);
      const foff = Number(fd.getBigUint64(file + 8, true));
      const fsize = Number(fd.getBigUint64(file + 16, true));
      const nlen = fd.getUint32(file + 28, true);
      found.push([prefix + dec.decode(files.subarray(file + 0x20, file + 0x20 + nlen)), dataOff + foff, fsize]);
      file = sibling;
    }
    let child = child0;
    while (child !== ROMFS_EMPTY) {
      const sibling = dd.getUint32(child + 4, true);
      const nlen = dd.getUint32(child + 20, true);
      walk(child, `${prefix}${dec.decode(dirs.subarray(child + 0x18, child + 0x18 + nlen))}/`, depth + 1);
      child = sibling;
    }
  };
  walk(0, '', 0);
  found.sort((a, b) => a[1] - b[1]);
  log?.(`Base: title ${hex16(program.titleId)}, ${found.length} files; hashing its RomFS`);
  const ranges = found.filter(([, , size]) => size).map(([p, off, size]) => [p, off, off + align(size, 0x10)]);
  const hasher = new RangeChunkHasher(ranges, PATCH_CHUNK);
  if (ranges.length) {
    let pos = ranges[0][1];
    const endPos = ranges[ranges.length - 1][2];
    hasher.pos = pos;
    const first = pos;
    while (pos < endPos) {
      throwIfAborted(signal);
      const n = Math.min(chunkBytes, endPos - pos);
      await hasher.update(await program.readSection(1, l6 + pos, n));
      pos += n;
      progress?.(pos - first, endPos - first);
    }
  }
  let dataDir = null;
  const marker = found.find(([p, , size]) => p === MARKER_NAME && size > 0 && size <= 0x1000);
  if (marker) dataDir = markerDataDir(await program.readSection(1, l6 + marker[1], marker[2]));
  const out = {};
  for (const [p, off, size] of found) out[p] = { offset: off, size, hashes: base64Encode(concat(hasher.result.get(p) ?? [])) };
  return {
    format: BASE_META_FORMAT,
    kind: 'masseffect-nx base RomFS (from the NSP)',
    title_id: hex16(program.titleId),
    program_nca: programName,
    data_dir: dataDir,
    romfs: {
      section_size: end - start, l6_offset: l6, image_size: imageSize, master_hash: toHex(fs.subarray(0xC8, 0xE8)), chunk: PATCH_CHUNK,
    },
    files: out,
  };
}

// ---- patch RomFS (BKTR: indirect storage + AES-CTR-EX); see tools/build_full_nsp.py for the format notes ---------------

export function bucketTree(entries, entrySize, endOffset) {
  const perSet = Math.floor((BUCKET_NODE - 0x10) / entrySize);
  const sets = [];
  for (let i = 0; i < entries.length; i += perSet) sets.push(entries.slice(i, i + perSet));
  if (sets.length > (BUCKET_NODE - 0x10) / 8) throw new PackError('patch table too large (more than one L1 node of entry sets)');
  const out = new Uint8Array(BUCKET_NODE * (1 + sets.length));
  const f = new Fields(out).i32(0, 0).i32(4, sets.length).i64(8, endOffset);
  sets.forEach((s, i) => f.i64(0x10 + 8 * i, s[0][0]));
  sets.forEach((s, i) => {
    const base = BUCKET_NODE * (i + 1);
    const setEnd = i + 1 < sets.length ? sets[i + 1][0][0] : endOffset;
    f.i32(base, i).i32(base + 4, s.length).i64(base + 8, setEnd);
    s.forEach(([, e], k) => out.set(e, base + 0x10 + entrySize * k));
  });
  const header = new Uint8Array(16);
  header.set(enc.encode('BKTR'), 0);
  new Fields(header).u32(4, 1).i32(8, entries.length).i32(12, 0);
  return [header, out];
}

function planPatchRuns(romfs, baseMeta) {
  const plan = romfs.plan;
  const l6 = plan.offsets[IVFC_LEVELS - 1], total = plan.size;
  const baseL6 = baseMeta.romfs.l6_offset;
  const baseFiles = baseMeta.files;
  const runs = [];
  let patch = 0;
  let mapped = 0;
  const add = (virt, length, storage, phys) => {
    if (length <= 0) return;
    if (runs.length) {
      const last = runs[runs.length - 1];
      if (last[2] === storage && last[0] + last[1] === virt && last[3] + last[1] === phys) { last[1] += length; return; }
    }
    runs.push([virt, length, storage, phys]);
  };
  const addPatch = (virt, length) => { add(virt, length, 1, patch); patch += Math.max(length, 0); };
  let pos = 0;
  for (const [path, off, size] of romfs.fileRanges) {
    if (!size) continue;
    const vs = l6 + off, ve = l6 + off + align(size, 0x10);
    addPatch(pos, vs - pos);
    const newHashes = romfs.chunkHashes.get(path);
    const base = Object.prototype.hasOwnProperty.call(baseFiles, path) ? baseFiles[path] : null;
    const baseHashes = base ? base64Decode(base.hashes) : new Uint8Array(0);
    const baseSpan = base ? align(base.size, 0x10) : 0;
    newHashes.forEach((digest, k) => {
      const cs = vs + k * PATCH_CHUNK, ce = Math.min(vs + (k + 1) * PATCH_CHUNK, ve);
      const baseLen = base ? Math.min(PATCH_CHUNK, baseSpan - k * PATCH_CHUNK) : 0;
      if (base && baseLen === ce - cs && equalBytes(baseHashes.subarray(32 * k, 32 * k + 32), digest)) {
        add(cs, ce - cs, 0, baseL6 + base.offset + k * PATCH_CHUNK);
        mapped += ce - cs;
      } else {
        addPatch(cs, ce - cs);
      }
    });
    pos = ve;
  }
  addPatch(pos, total - pos);
  if (runs.reduce((a, r) => a + r[1], 0) !== total || runs.some(([v, , , p]) => v % 16 || p % 16)) throw new Error('patch runs are inconsistent');
  return { runs, patchSize: patch, mapped };
}

/** The BKTR RomFS section of an update. runs: [[virtual, length, storage, physical]]; dataStream: () => virtual bytes. */
function patchSection(plan, runs, patchSize, mapped, generation, virtualStream) {
  const indirectOffset = align(patchSize, BUCKET_NODE);
  const [indHeader, indTable] = bucketTree(runs.map(([v, , st, p]) => {
    const e = new Uint8Array(0x14);
    new Fields(e).i64(0, v).i64(8, p).i32(16, st);
    return [v, e];
  }), 0x14, plan.size);
  const aesOffset = indirectOffset + indTable.length;
  const aesEntry = new Uint8Array(0x10);
  new Fields(aesEntry).i64(0, 0).u8(8, 0).i32(12, generation);
  const [aesHeader, aesTable] = bucketTree([[0, aesEntry]], 0x10, aesOffset);
  const size = aesOffset + aesTable.length;
  const fs = plan.fsHeader();
  fs[4] = 4; // AesCtrEx
  new Fields(fs).i64(0x100, indirectOffset).i64(0x108, indTable.length).bytes(0x110, indHeader)
    .i64(0x120, aesOffset).i64(0x128, aesTable.length).bytes(0x130, aesHeader);
  const patchRuns = runs.filter(([, , st]) => st === 1).map(([v, n]) => [v, n]);
  const chunks = async function* () {
    if (patchRuns.length) yield* selectRanges(virtualStream(), patchRuns);
    if (indirectOffset > patchSize) yield new Uint8Array(indirectOffset - patchSize);
    yield indTable;
    yield aesTable;
  };
  const section = new Section(fs, size, chunks, generation);
  section.stats = { runs: runs.length, patchBytes: patchSize, baseBytes: mapped, virtual: plan.size };
  return section;
}

async function* selectRanges(stream, ranges) {
  let pos = 0;
  let i = 0;
  for await (const block of stream) {
    const blockStart = pos, blockEnd = pos + block.length;
    while (i < ranges.length && ranges[i][0] < blockEnd) {
      const [start, length] = ranges[i];
      const lo = Math.max(start, blockStart), hi = Math.min(start + length, blockEnd);
      if (lo < hi) yield block.subarray(lo - blockStart, hi - blockStart);
      if (start + length <= blockEnd) i++;
      else break;
    }
    pos = blockEnd;
  }
}

/** Program-only update: the new RomFS section is the base's, mapped 1:1 (no game data needed, no patch data). */
function programOnlyPatchSection(baseMeta, generation) {
  const r = baseMeta.romfs;
  const plan = IvfcPlan.forImage(r.image_size, fromHex(r.master_hash));
  if (plan.size !== r.section_size || plan.offsets[IVFC_LEVELS - 1] !== r.l6_offset) {
    throw new PackError('the base metadata does not describe a RomFS section made by this packer');
  }
  return patchSection(plan, [[0, plan.size, 0, 0]], 0, plan.size, generation, null);
}

// ---- input checks ------------------------------------------------------------------------------------------------------

/**
 * The game ignores a masseffect.toml that does not parse, silently (the Python packer checks it with tomllib). This
 * catches the failure seen in practice, a key assigned twice in the same table; it is not a full TOML parser.
 */
export function checkToml(bytes) {
  const text = dec.decode(bytes);
  const seen = new Set();
  let table = '';
  let inMultiline = null;
  text.split(/\r?\n/).forEach((line, i) => {
    if (inMultiline) {
      if (line.includes(inMultiline)) inMultiline = null;
      return;
    }
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) return;
    const header = /^\[\[?\s*([^\]]+?)\s*\]\]?/.exec(trimmed);
    if (header) {
      table = trimmed.startsWith('[[') ? `${header[1]}#${i}` : header[1];
      if (!trimmed.startsWith('[[') && seen.has(`[${table}]`)) throw new PackError(`masseffect.toml line ${i + 1}: table [${table}] defined twice; the game would ignore all settings`);
      seen.add(`[${table}]`);
      return;
    }
    const m = /^([A-Za-z0-9_.-]+|"[^"]*")\s*=\s*(.*)$/.exec(trimmed);
    if (!m) return;
    const key = `${table}\u0000${m[1]}`;
    if (seen.has(key)) throw new PackError(`masseffect.toml line ${i + 1}: ${m[1]} is set twice; the game would ignore all settings`);
    seen.add(key);
    for (const q of ['"""', "'''"]) {
      const at = m[2].indexOf(q);
      if (at >= 0 && m[2].indexOf(q, at + 3) < 0) inMultiline = q;
    }
  });
}

export function validDataDir(d) {
  return typeof d === 'string' && d.startsWith('sdmc:/') && d.length > 6 && d.length <= 700 && !d.includes('..') &&
    !d.includes('//') && !d.slice(6).includes(':') && !d.endsWith('/');
}

// ---- the whole package -----------------------------------------------------------------------------------------------------

/**
 * Builds a full NSP or an update NSP and streams it into `sink`.
 *
 * options:
 *   keys        { header_key, key_area_key_application_00 } (parseProdKeys)
 *   nro         Uint8Array: the program
 *   entries     [{ path, size, chunks() }]: the RomFS files (masseffect.toml, the shader package and index,
 *               masseffect_prewarm_list.bin when the release has one, game_root/..., masseffect/0000000000000000/...);
 *               the marker file is added here. Each chunks() must be
 *               readable twice. Not used for a program-only update.
 *   titleId     BigInt or hex string (default 01a5eec700010000); for updates the base's title ID is used, and a
 *               given titleId is only checked against it (checkUpdateTitle, with knownTitleIds { hex: label })
 *   dataDir     writable SD folder (default: for updates the base's, else DEFAULT_DATA_DIR)
 *   name, displayVersion, emulatorCompatible, createdUtc (marker timestamp, default now)
 *   version     full: title version; update: update number N (title version N * 0x10000)
 *   update      null, or { baseMeta, programOnly }: a patch against the base described by baseMeta
 *   signer      { modulus, sign(message) } (default: a fresh WebCrypto RSA-2048 key)
 *   aesKeyFor   (contentType) => Uint8Array(16) (default: random keys)
 *   sink        { write(bytes), writeAt(position, bytes) } (js/nsp_sink.js)
 *   onProgress  (phase 'hash' | 'write' | 'usb', done, total, info); 'usb' only with a serving sink, info = { rate }
 *   chunkBytes  read/encrypt block size (default 4 MiB; a multiple of 16 KiB)
 *   log, signal
 * Returns { titleId, patchId, version, files: [[name, size]], size, baseMeta (full packs), stats }.
 */
export async function buildNsp(options) {
  const { keys, sink, signal } = options;
  const chunkBytes = options.chunkBytes ?? CHUNK;
  if (!Number.isInteger(chunkBytes) || chunkBytes <= 0 || chunkBytes % IVFC_BLOCK) throw new Error('chunkBytes must be a multiple of 16 KiB');
  const log = options.log ?? (() => {});
  const progress = options.onProgress ?? (() => {});
  if (!keys?.header_key || !keys?.key_area_key_application_00) throw new PackError('the console keys are missing');
  const nro = new Nro(options.nro);
  if (!nro.assets.nacp || !nro.assets.icon) throw new PackError('the NRO has no NACP or icon');
  const update = options.update ?? null;
  const dataDir = options.dataDir ?? (update?.baseMeta?.data_dir || DEFAULT_DATA_DIR);
  if (!validDataDir(dataDir)) throw new PackError('the data folder must look like sdmc:/switch/<folder>');

  let titleId, version, baseMeta = null;
  if (update) {
    baseMeta = update.baseMeta;
    if (!baseMeta) throw new PackError('an update needs the base (its NSP or its .basemeta.json)');
    const n = Number(options.version);
    if (!Number.isInteger(n) || n < 1 || n > 0xFFFF) throw new PackError('the update number must be 1 or more (the title version becomes N * 0x10000)');
    titleId = BigInt(`0x${baseMeta.title_id}`);
    if (options.titleId != null) {
      const warning = checkUpdateTitle(titleId, options.titleId, options.knownTitleIds ?? {});
      if (warning) log(warning, 'warn');
    }
    if (!update.programOnly && baseMeta.data_dir && baseMeta.data_dir !== dataDir) {
      log(`warning: the base was packed with data_dir=${baseMeta.data_dir}, this update uses ${dataDir} (saves and caches move to the new folder)`, 'warn');
    }
    version = n * 0x10000;
  } else {
    titleId = BigInt(`0x${titleIdHex(options.titleId ?? DEFAULT_TITLE_ID)}`);
    version = Number(options.version ?? 0);
  }
  if (titleId & 0xFFFn || titleId < 0x0100000000000000n || titleId > 0x0FFFFFFFFFFFF000n) {
    throw new PackError('the title ID must be 01xxxxxxxxxxx000 .. 0fffffffffffffff with the last three digits 000');
  }
  const patchId = titleId + 0x800n;
  const aesKeyFor = options.aesKeyFor ?? (() => randomBytes(16));

  // Program NCA, ExeFS
  const signer = options.signer ?? await rsaPssSigner();
  const npdm = buildNpdm(npdmConfig(titleId, { emulatorCompatible: !!options.emulatorCompatible }), signer.modulus);
  if (options.emulatorCompatible) log('NPDM: emulator-compatible (pre-19.0.0 DebugFlags layout; for yuzu/Eden, not for the console)');
  const nso = nsoFromNro(nro);
  const exefs = await pfs0Section(pfs0Bytes([['main', nso], ['main.npdm', npdm]]), EXEFS_HASH_BLOCK);

  // Program NCA, RomFS
  let romfs = null, romfsSec, hasDlc = false;
  if (update?.programOnly) {
    romfsSec = programOnlyPatchSection(baseMeta, Number(options.version));
    log(`Patch RomFS: the base's RomFS section unchanged (${formatSize(romfsSec.stats.virtual)}); only the program is new`);
    log(`note: the settings and ${PREWARM_LIST_NAME} stay the base's; if this program changed the pipeline list version or the shader package, put the new list into the SD data folder or make a full update`);
  } else {
    const created = options.createdUtc ?? new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');
    const marker = `format=1\ndata_dir=${dataDir}\ntitle_id=${hex16(titleId)}\nversion=${version}\n` +
      `created_utc=${created}\nnro_sha256=${toHex(sha256(nro.data))}\n`;
    romfs = new Romfs();
    const entries = options.entries ?? [];
    for (const name of [TOML_NAME, SHADERS_NAME, `${SHADERS_NAME}.idx`]) {
      if (!entries.some((e) => e.path === name)) throw new PackError(`${name} is missing from the package`);
    }
    if (!entries.some((e) => e.path === 'game_root/default.xex')) throw new PackError('game_root/default.xex is missing: the NSP needs the full game');
    if (!entries.some((e) => e.path === PREWARM_LIST_NAME)) {
      log(`warning: no ${PREWARM_LIST_NAME}: packed without the shipped prewarm list (a cold start compiles more pipelines during play)`, 'warn');
    }
    for (const e of entries) {
      if (e.path === MARKER_NAME) throw new PackError(`${MARKER_NAME} is reserved`);
      romfs.add(e.path, e);
      if (e.path.startsWith(DLC_PREFIX)) hasDlc = true;
    }
    const toml = entries.find((e) => e.path === TOML_NAME);
    const tomlParts = [];
    for await (const c of toml.chunks()) tomlParts.push(c);
    checkToml(concat(tomlParts));
    romfs.add(MARKER_NAME, sourceFromBytes(enc.encode(marker)));
    log(`RomFS: ${romfs.count} files, ${formatSize(romfs.dataBytes)}${hasDlc ? ' (with DLC)' : ''}`);
    log('Pass 1/2: hashing the RomFS');
    romfsSec = await romfsSection(romfs, { progress: (d, t) => progress('hash', d, t), signal, chunkBytes });
    if (update) {
      const { runs, patchSize, mapped } = planPatchRuns(romfs, baseMeta);
      const plan = romfs.plan;
      romfsSec = patchSection(plan, runs, patchSize, mapped, Number(options.version), () => plan.chunks(() => romfs.chunks()));
      const st = romfsSec.stats;
      log(`Patch RomFS: ${st.runs} ranges; ${formatSize(st.baseBytes)} of files read from the base, ${formatSize(st.patchBytes)} stored in the update`);
    }
  }
  const program = new NcaBuilder(keys, titleId, CONTENT_PROGRAM, [exefs, romfsSec], { aesKey: aesKeyFor(CONTENT_PROGRAM), signer, signal, chunkBytes });

  // Control NCA
  const controlRomfs = new Romfs();
  const nacp = patchNacp(nro.assets.nacp, options.name ?? null, options.displayVersion ?? null, titleId);
  controlRomfs.add('control.nacp', sourceFromBytes(nacp));
  controlRomfs.add('icon_AmericanEnglish.dat', sourceFromBytes(nro.assets.icon));
  const control = await new NcaBuilder(keys, titleId, CONTENT_CONTROL, [await romfsSection(controlRomfs)],
    { aesKey: aesKeyFor(CONTENT_CONTROL) }).toBytes();

  // Meta NCA
  const [metaId, cnmtName, metaType] = update
    ? [patchId, `Patch_${hex16(patchId)}.cnmt`, CNMT_PATCH]
    : [titleId, `Application_${hex16(titleId)}.cnmt`, CNMT_APPLICATION];
  const makeMeta = async (programHash, programSize, controlHash) => {
    const cnmt = buildCnmt(metaId, version, [[programHash, programSize, CNMT_PROGRAM], [controlHash, control.length, CNMT_CONTROL]],
      metaType, titleId);
    return new NcaBuilder(keys, metaId, CONTENT_META, [await pfs0Section(pfs0Bytes([[cnmtName, cnmt]]), META_HASH_BLOCK)],
      { aesKey: aesKeyFor(CONTENT_META) }).toBytes();
  };

  const { files, total } = await writeNsp(sink, program, control, makeMeta, { progress, signal, log });
  log(`NSP done: ${update ? 'update' : 'title'} ${hex16(update ? patchId : titleId)} version ${version}, program ${files[0][0]}, control ${files[1][0]}, meta ${files[2][0]}`);
  const result = {
    titleId: hex16(titleId), patchId: update ? hex16(patchId) : null, version, files, size: total, dlc: hasDlc,
    stats: romfsSec.stats ?? null, baseMeta: null,
  };
  if (!update) result.baseMeta = baseMetadata(titleId, romfs, files[0][0].slice(0, -4), dataDir);
  return result;
}

async function writeNsp(sink, program, control, makeMeta, { progress, signal, log }) {
  // A serving sink (USB install) gets nothing written: the header must be final before the console reads it, so pass 2
  // only hashes, and the NSP is then served by range (NspImage regenerates the Program NCA).
  const serving = typeof sink.serve === 'function';
  const metaSize = (await makeMeta(new Uint8Array(32), program.size, new Uint8Array(32))).length;
  const zeros = '0'.repeat(32);
  const names = [[`${zeros}.nca`, program.size], [`${zeros}.nca`, control.length], [`${zeros}.cnmt.nca`, metaSize]];
  const headerSize = pfs0Header(names).length;
  const total = headerSize + names.reduce((a, [, s]) => a + s, 0);
  log(`NSP: ${formatSize(total)}`);
  if (!serving) await sink.write(new Uint8Array(headerSize));
  log(serving
    ? 'Pass 2/3: encrypting and hashing the Program NCA (its SHA-256 is its name in the NSP header, which the console reads first; nothing is written)'
    : 'Pass 2/2: encrypting and writing the Program NCA');
  const digest = new Sha256();
  let written = 0;
  for await (const block of program.chunks()) {
    throwIfAborted(signal);
    if (!serving) await sink.write(block);
    digest.update(block);
    written += block.length;
    progress('write', written, program.size);
  }
  if (written !== program.size) throw new PackError('Program NCA size mismatch');
  const programHash = digest.digest();
  const controlHash = sha256(control);
  if (!serving) await sink.write(control);
  const meta = await makeMeta(programHash, program.size, controlHash);
  if (meta.length !== metaSize) throw new PackError('Meta NCA size changed');
  const metaHash = sha256(meta);
  if (!serving) await sink.write(meta);
  const files = [
    [`${toHex(programHash.subarray(0, 16))}.nca`, program.size],
    [`${toHex(controlHash.subarray(0, 16))}.nca`, control.length],
    [`${toHex(metaHash.subarray(0, 16))}.cnmt.nca`, meta.length],
  ];
  const header = pfs0Header(files);
  if (header.length !== headerSize) throw new PackError('NSP header size changed');
  if (serving) {
    log('Pass 3/3: serving the NSP to the console (the Program NCA is read and encrypted again as the console asks for it)');
    const image = new NspImage(header, program, programHash, control, meta, { signal, log });
    await sink.serve(image, { signal, log, progress: (done, all, info) => progress('usb', done, all, info) });
  } else {
    await sink.writeAt(0, header);
  }
  return { files, total };
}

/**
 * A finished NSP that is never stored: read(offset, length) returns any range. The header, the Control and the Meta
 * NCA are in memory; the Program NCA (the 8 GB) is made again on demand from a cursor over NcaBuilder.chunks(from):
 * sequential reads (what Sphaira does for an NCA) continue the cursor, a recent range is answered from a small window
 * of the last blocks, and anything else reopens the cursor at the requested offset (no data is read before it).
 * While the Program NCA is produced in one sequential run from its start, its SHA-256 is checked against the hash
 * from pass 2: a source that changed between the passes fails before its last block reaches the console.
 */
export class NspImage {
  constructor(header, program, programHash, control, meta, { signal = null, log = () => {}, windowBytes = 16 << 20, skipAheadBytes = 64 << 20 } = {}) {
    this.header = header;
    this.program = program;
    this.programHash = programHash;
    this.control = control;
    this.meta = meta;
    this.signal = signal;
    this.log = log;
    this.windowBytes = windowBytes;
    this.skipAheadBytes = skipAheadBytes;
    this.programStart = header.length;
    this.controlStart = this.programStart + program.size;
    this.metaStart = this.controlStart + control.length;
    this.size = this.metaStart + meta.length;
    this.stats = { reopens: 0, producedBytes: 0, verified: false };
    this.cursor = null; // { it, pos }
    this.window = []; // [{ start, bytes }] of the Program NCA, in order
    this.hash = new Sha256();
    this.hashedTo = 0;
    this.coverage = []; // merged [start, end) ranges of the NSP read so far
  }

  /** Bytes [offset, offset + length) of the NSP (shorter at the end of the file). */
  async read(offset, length) {
    throwIfAborted(this.signal);
    if (offset < 0 || offset > this.size) throw new RangeError(`read past the end of the NSP (${offset})`);
    const end = Math.min(this.size, offset + length);
    const out = new Uint8Array(end - offset);
    const pieces = [[0, this.header], [this.controlStart, this.control], [this.metaStart, this.meta]];
    for (const [start, bytes] of pieces) {
      const lo = Math.max(offset, start), hi = Math.min(end, start + bytes.length);
      if (lo < hi) out.set(bytes.subarray(lo - start, hi - start), lo - offset);
    }
    const lo = Math.max(offset, this.programStart), hi = Math.min(end, this.controlStart);
    if (lo < hi) await this.#program(lo - this.programStart, hi - this.programStart, out, lo - offset);
    this.#cover(offset, end);
    return out;
  }

  /** Whether every byte of the NSP has been read at least once (the console installed all of it). */
  complete() {
    return this.coverage.length === 1 && this.coverage[0][0] === 0 && this.coverage[0][1] === this.size;
  }

  #cover(lo, hi) {
    if (lo >= hi) return;
    const next = [];
    for (const [a, b] of this.coverage) {
      if (b < lo || a > hi) next.push([a, b]);
      else { lo = Math.min(lo, a); hi = Math.max(hi, b); }
    }
    next.push([lo, hi]);
    next.sort((x, y) => x[0] - y[0]);
    this.coverage = next;
  }

  async #program(from, to, out, outOffset) {
    const windowStart = this.window.length ? this.window[0].start : this.cursor?.pos;
    if (!this.cursor || from < windowStart || from > this.#windowEnd() + this.skipAheadBytes) this.#reopen(from);
    while (this.#windowEnd() < to) {
      const { value, done } = await this.cursor.it.next();
      if (done) throw new PackError('the Program NCA ended early (a source changed size?)');
      throwIfAborted(this.signal);
      const start = this.cursor.pos;
      this.cursor.pos += value.length;
      this.stats.producedBytes += value.length;
      this.#hashBlock(start, value);
      this.window.push({ start, bytes: value });
      // Keep the window small, but never drop a block the current request still needs.
      while (this.window.length > 1 && this.#windowEnd() - this.window[0].start - this.window[0].bytes.length >= this.windowBytes &&
        this.window[0].start + this.window[0].bytes.length <= from) this.window.shift();
    }
    for (const { start, bytes } of this.window) {
      const lo = Math.max(from, start), hi = Math.min(to, start + bytes.length);
      if (lo < hi) out.set(bytes.subarray(lo - start, hi - start), outOffset + (lo - from));
    }
  }

  #windowEnd() {
    return this.cursor.pos; // the window always ends where the cursor is
  }

  #reopen(from) {
    const start = from - (from % 0x4000); // an IVFC block: whole blocks from the sources, AES-CTR 16-aligned
    if (this.cursor) {
      this.stats.reopens++;
      Promise.resolve(this.cursor.it.return?.()).catch(() => {});
      this.log(`USB: the console asked for Program NCA offset ${from} out of order; regenerating from ${start}`);
    }
    this.window = [];
    this.cursor = { it: this.program.chunks(start)[Symbol.asyncIterator](), pos: start };
  }

  #hashBlock(start, bytes) {
    // Only the continuation of what was hashed so far (re-made blocks after a reopen backwards add their new tail; a
    // reopen past it ends the check).
    if (start > this.hashedTo || start + bytes.length <= this.hashedTo) return;
    this.hash.update(bytes.subarray(this.hashedTo - start));
    this.hashedTo = start + bytes.length;
    if (this.hashedTo === this.program.size) {
      if (!equalBytes(this.hash.digest(), this.programHash)) {
        throw new PackError('the data changed between the hashing pass and the install (a file on the disc or in the folder was modified or became unreadable); the console would reject the package');
      }
      this.stats.verified = true;
      this.log('USB: Program NCA verified (same SHA-256 as pass 2)');
    }
  }
}

function formatSize(n) {
  const units = ['B', 'KiB', 'MiB', 'GiB'];
  let u = 0;
  while (n >= 1024 && u < units.length - 1) { n /= 1024; u++; }
  return u ? `${n.toFixed(1)} ${units[u]}` : `${n} B`;
}

// ---- estimates for the page ---------------------------------------------------------------------------------------------

/**
 * Estimated NSP size for the page: the payload (game files, shaders, toml, DLC) plus the RomFS tables, the IVFC
 * hash levels (32 bytes per 16 KiB, ~0.2 %), the program (NSO ~ the NRO) and the small Control and Meta NCAs.
 */
export function estimateNspBytes({ payloadBytes, fileCount, nroBytes }) {
  const tables = fileCount * 96 + 4096;
  const ivfc = Math.ceil((payloadBytes + tables) / 0x4000) * 32 + 6 * 0x4000;
  const alignment = fileCount * 8;
  return payloadBytes + tables + ivfc + alignment + nroBytes + 0x20000 + 0x10000;
}

/** Estimated size of a program-only update: the program plus the two BKTR tables and the small NCAs. */
export function estimateProgramUpdateBytes(nroBytes) {
  return nroBytes + 0x10000 + 0x40000;
}

/** The next update number for a base: one more than the last one recorded in its metadata (or 1). */
export function nextUpdateVersion(baseMeta, remembered = 0) {
  const last = Math.max(Number(baseMeta?.last_update_version) || 0, Number(remembered) || 0);
  return Math.min(last + 1, 0xFFFF);
}

/** The base metadata with the update number recorded (saved next to the base so the next update counts on). */
export function withLastUpdateVersion(baseMeta, n) {
  return { ...baseMeta, last_update_version: n };
}

