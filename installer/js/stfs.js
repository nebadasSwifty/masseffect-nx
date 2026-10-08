// Read-only reader of Xbox 360 STFS content packages (CON / LIVE / PIRS) for Blob / File objects.
//
// A port of tools/stfs_extract.py (itself a port of the block math in
// sdk/src/filesystem/devices/stfs_container_device.cpp, Xenia's StfsContainerDevice). Same rules, same results: the
// tests and docs/dlc.md compare the two byte for byte.
//
// Nothing is loaded as a whole. The header, the file table and the hash tables that a fragmented file needs are read
// with Blob.slice(); every file is returned as a lazy Blob made of slices of the package (new Blob([slice, ...])), so
// the shader scan workers and the zip writer read it exactly like a disc file.
//
// Layout (header fields big-endian unless noted):
//   0x000 magic "CON " / "LIVE" / "PIRS"          0x22C 16 license entries {u64 id, u32 bits, u32 flags}
//   0x32C content id (20 bytes)                    0x340 header size       0x344 content type   0x348 metadata version
//   0x34C content size (u64)                       0x354 media id, version, base version, title id
//   0x379 STFS volume descriptor (0x24 bytes): u8 size, u8 reserved, u8 flags (bit 0 read-only, bit 1 root active
//         index), u16 LE file table block count, u24 LE file table block, 20-byte top hash, u32 total, u32 free blocks
//   0x3A9 volume type (0 = STFS, 1 = SVOD)        0x411 display names (UTF-16BE, 9 x 0x100), 0x541A 3 more (v2)
// Data blocks are 4 KB, starting at the header size rounded up to 4 KB. Every 0xAA data blocks are preceded by a
// level-0 hash table (one per volume if read-only, two otherwise); level-1 and level-2 tables sit in front of every
// 0xAA^2 / 0xAA^3 blocks. A hash entry is {20-byte SHA-1 of the block, u32 info}; the low 24 bits of info are the
// next block of the chain (0xFFFFFF = end).
// File table entries (0x40 bytes): name (0x28), u8 flags (0x80 dir, 0x40 contiguous, 0x3F name length),
// u24 LE valid blocks, u24 LE allocated blocks, u24 LE start block, u16 BE parent (0xFFFF = root), u32 BE length.

export const BLOCK = 0x1000;
const PER_LEVEL = [0xaa, 0xaa * 0xaa, 0xaa * 0xaa * 0xaa];
export const END_OF_CHAIN = 0xffffff;
const HEADER_READ = 0xa000;

export const MAGICS = { 'CON ': 'CON', LIVE: 'LIVE', PIRS: 'PIRS' };
export const CONTENT_TYPES = {
  0x00000001: 'SavedGame',
  0x00000002: 'MarketplaceContent',
  0x00000003: 'Publisher',
  0x00001000: 'IPTV',
  0x00002000: 'InstalledGame',
  0x00004000: 'GamesOnDemand',
  0x000b0000: 'GameTitle',
  0x000d0000: 'ArcadeTitle',
  0x00080000: 'GameDemo',
  0x00090000: 'Video',
  0x000f0000: 'AvatarItem',
};
// XLanguage order of the 9 v1 display-name slots, then the 3 slots of metadata v2.
const LANGS_V1 = ['en', 'ja', 'de', 'fr', 'es', 'it', 'ko', 'zh', 'pt'];
const LANGS_V2 = ['pl', 'ru', 'sv'];

export const OFF = {
  licenses: 0x22c,
  contentId: 0x32c,
  headerSize: 0x340,
  contentType: 0x344,
  metaVersion: 0x348,
  contentSize: 0x34c,
  execInfo: 0x354,
  profileId: 0x371,
  volume: 0x379,
  dataFileCount: 0x39d,
  volumeType: 0x3a9,
  displayName: 0x411,
  description: 0xd11,
  publisher: 0x1611,
  titleName: 0x1691,
  displayNameEx: 0x541a,
  descriptionEx: 0x941a,
};

// XCONTENT_AGGREGATE_DATA as ContentManager::WriteContentHeaderFile stores it, followed by the u32 license mask.
export const AGGREGATE_SIZE = 0x148;
export const HEADER_FILE_SIZE = AGGREGATE_SIZE + 4;

export class StfsError extends Error {
  constructor(message) {
    super(message);
    this.name = 'StfsError';
  }
}

const hex = (n, w = 8) => n.toString(16).toUpperCase().padStart(w, '0');
const utf16be = new TextDecoder('utf-16be');

function u16str(bytes, start, length) {
  if (start + length > bytes.length) return '';
  const s = utf16be.decode(bytes.subarray(start, start + length));
  const nul = s.indexOf('\0');
  return nul < 0 ? s : s.slice(0, nul);
}

const u24le = (b, at) => b[at] | (b[at + 1] << 8) | (b[at + 2] << 16);

async function readBytes(blob, offset, length) {
  if (offset < 0 || offset + length > blob.size) throw new StfsError('read past the end of the package (truncated file?)');
  return new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
}

function safeName(name) {
  return name.length > 0 && name !== '.' && name !== '..' && !/[\\/\0:*?"<>|]/.test(name);
}

/** Parses the package header (the first 0xA000 bytes or less). Throws StfsError when it is not an STFS package. */
export function parseHeader(h, fileSize = h.length) {
  if (h.length < 0x3b0) throw new StfsError('too small to be an Xbox 360 content package');
  const magicText = String.fromCharCode(h[0], h[1], h[2], h[3]);
  const magic = MAGICS[magicText];
  if (!magic) throw new StfsError('not an Xbox 360 content package (no CON/LIVE/PIRS header)');
  const dv = new DataView(h.buffer, h.byteOffset, h.byteLength);
  const meta = {
    magic,
    fileSize,
    headerSize: dv.getUint32(OFF.headerSize, false),
    contentType: dv.getUint32(OFF.contentType, false),
    metaVersion: dv.getUint32(OFF.metaVersion, false),
    contentSize: Number(dv.getBigUint64(OFF.contentSize, false)),
    mediaId: dv.getUint32(OFF.execInfo, false),
    version: dv.getUint32(OFF.execInfo + 4, false),
    baseVersion: dv.getUint32(OFF.execInfo + 8, false),
    titleId: dv.getUint32(OFF.execInfo + 12, false),
    contentId: Array.from(h.subarray(OFF.contentId, OFF.contentId + 0x14), (b) => hex(b, 2)).join(''),
    profileId: dv.getBigUint64(OFF.profileId, false).toString(16).toUpperCase().padStart(16, '0'),
    volumeType: dv.getUint32(OFF.volumeType, false),
    dataFileCount: dv.getUint32(OFF.dataFileCount, false),
    licenses: [],
    displayNames: {},
    descriptions: {},
  };
  for (let i = 0; i < 0x10; i++) {
    const at = OFF.licenses + i * 0x10;
    const id = dv.getBigUint64(at, false);
    const bits = dv.getUint32(at + 8, false);
    const flags = dv.getUint32(at + 12, false);
    if (id || bits || flags) meta.licenses.push({ id: id.toString(16).toUpperCase().padStart(16, '0'), bits, flags });
  }
  LANGS_V1.forEach((lang, i) => {
    meta.displayNames[lang] = u16str(h, OFF.displayName + i * 0x100, 0x100);
    meta.descriptions[lang] = u16str(h, OFF.description + i * 0x100, 0x100);
  });
  if (meta.metaVersion >= 2) {
    LANGS_V2.forEach((lang, i) => {
      meta.displayNames[lang] = u16str(h, OFF.displayNameEx + i * 0x100, 0x100);
      meta.descriptions[lang] = u16str(h, OFF.descriptionEx + i * 0x100, 0x100);
    });
  }
  meta.publisher = u16str(h, OFF.publisher, 0x80);
  meta.titleName = u16str(h, OFF.titleName, 0x80);
  meta.displayName = meta.displayNames.en || '';
  // Same rule as ContentManager::InstallContent (and stfs_extract.py license_mask()).
  meta.licenseMask = meta.licenses.reduce((m, l) => (l.flags ? (m | l.bits) >>> 0 : m), 0);

  const vd = OFF.volume;
  meta.volume = {
    flags: h[vd + 2],
    readOnly: (h[vd + 2] & 1) !== 0,
    rootActiveIndex: (h[vd + 2] & 2) !== 0,
    fileTableBlockCount: h[vd + 3] | (h[vd + 4] << 8),
    fileTableBlock: u24le(h, vd + 5),
    totalBlocks: dv.getUint32(vd + 0x1c, false),
    freeBlocks: dv.getUint32(vd + 0x20, false),
  };
  meta.contentTypeName = CONTENT_TYPES[meta.contentType] ?? 'unknown';
  meta.titleIdHex = hex(meta.titleId);
  meta.contentTypeHex = hex(meta.contentType);
  return meta;
}

/** The block math of StfsContainerDevice (BlockToOffsetSTFS, BlockToHashBlockNumberSTFS) for one package header. */
export class StfsGeometry {
  constructor(meta) {
    this.readOnly = meta.volume.readOnly;
    this.rootActiveIndex = meta.volume.rootActiveIndex;
    this.totalBlocks = meta.volume.totalBlocks;
    this.bpht = this.readOnly ? 1 : 2; // hash tables per hash block level
    this.step0 = PER_LEVEL[0] + this.bpht;
    this.step1 = PER_LEVEL[1] + (PER_LEVEL[0] + 1) * this.bpht;
    this.base = Math.ceil(meta.headerSize / BLOCK) * BLOCK;
  }

  /** Byte offset of data block `index` in the package. */
  blockToOffset(index) {
    let base = PER_LEVEL[0];
    let block = index;
    for (let i = 0; i < 3; i++) {
      block += Math.floor((index + base) / base) * this.bpht;
      if (index < base) break;
      base *= PER_LEVEL[0];
    }
    return this.base + block * BLOCK;
  }

  /** Block number (relative to the data area) of the hash table at `level` that covers data block `index`. */
  hashBlockNumber(index, level) {
    if (level === 0) {
      if (index < PER_LEVEL[0]) return 0;
      let block = Math.floor(index / PER_LEVEL[0]) * this.step0;
      block += (Math.floor(index / PER_LEVEL[1]) + 1) * this.bpht;
      if (index < PER_LEVEL[1]) return block;
      return block + this.bpht;
    }
    if (level === 1) {
      if (index < PER_LEVEL[1]) return this.step0;
      return Math.floor(index / PER_LEVEL[1]) * this.step1 + this.bpht;
    }
    return this.step1;
  }
}

class Reader {
  constructor(blob, meta) {
    this.blob = blob;
    this.meta = meta;
    this.geo = new StfsGeometry(meta);
    this.tables = new Map();
  }

  async table(offset) {
    let t = this.tables.get(offset);
    if (!t) {
      t = await readBytes(this.blob, offset, BLOCK);
      this.tables.set(offset, t);
    }
    return t;
  }

  /** {sha1: Uint8Array(20), info: u32} of the level-0 hash entry of a data block (stfs_extract.py block_hash()). */
  async blockHash(index) {
    const g = this.geo;
    let sec = g.rootActiveIndex ? BLOCK : 0;
    const lv0 = g.base + g.hashBlockNumber(index, 0) * BLOCK;
    const info = (t, rec) => new DataView(t.buffer, t.byteOffset).getUint32(rec * 0x18 + 0x14, false);
    if (!g.readOnly) {
      if (g.totalBlocks > PER_LEVEL[0]) {
        const lv1 = g.base + g.hashBlockNumber(index, 1) * BLOCK;
        if (g.totalBlocks > PER_LEVEL[1]) {
          const lv2 = g.base + g.hashBlockNumber(index, 2) * BLOCK;
          const t2 = await this.table(lv2 + sec);
          sec = info(t2, Math.floor(index / PER_LEVEL[1]) % PER_LEVEL[0]) & 0x40000000 ? BLOCK : 0;
        }
        const t1 = await this.table(lv1 + sec);
        sec = info(t1, Math.floor(index / PER_LEVEL[0]) % PER_LEVEL[0]) & 0x40000000 ? BLOCK : 0;
      }
    } else {
      sec = 0;
    }
    const t0 = await this.table(lv0 + sec);
    const rec = index % PER_LEVEL[0];
    return { sha1: t0.subarray(rec * 0x18, rec * 0x18 + 0x14), info: info(t0, rec) };
  }

  async nextBlock(index) {
    return (await this.blockHash(index)).info & 0xffffff;
  }

  async readTree() {
    if (this.meta.volumeType !== 0) throw new StfsError('SVOD packages are not supported (only STFS)');
    const entries = [];
    let block = this.meta.volume.fileTableBlock;
    for (let n = 0; n < this.meta.volume.fileTableBlockCount; n++) {
      const data = await readBytes(this.blob, this.geo.blockToOffset(block), BLOCK);
      const dv = new DataView(data.buffer);
      let done = false;
      for (let m = 0; m < 0x40; m++) {
        const at = m * 0x40;
        if (data[at] === 0) { done = true; break; }
        const flags = data[at + 0x28];
        let name = '';
        for (let i = 0; i < (flags & 0x3f); i++) name += String.fromCharCode(data[at + i]); // latin-1
        entries.push({
          name,
          isDir: (flags & 0x80) !== 0,
          contiguous: (flags & 0x40) !== 0,
          validBlocks: u24le(data, at + 0x29),
          allocBlocks: u24le(data, at + 0x2c),
          start: u24le(data, at + 0x2f),
          parent: dv.getUint16(at + 0x32, false),
          size: dv.getUint32(at + 0x34, false),
        });
      }
      if (done) break;
      block = await this.nextBlock(block);
      if (block === END_OF_CHAIN) break;
    }
    for (const e of entries) {
      if (!safeName(e.name)) throw new StfsError(`unsafe file name ${JSON.stringify(e.name)} in the package`);
      const parts = [e.name];
      let p = e.parent;
      for (let guard = 0; p !== 0xffff && guard < 64; guard++) {
        const parent = entries[p];
        if (!parent) throw new StfsError(`${e.name}: parent entry ${p} does not exist`);
        parts.push(parent.name);
        p = parent.parent;
      }
      e.path = parts.reverse().join('/');
    }
    return entries;
  }

  /**
   * The data blocks of a file as stfs_extract.py iter_file_blocks() walks them: contiguous files block by block
   * (unless `followChain`), fragmented ones along the hash chain. Calls visit(block, offset, size) for each.
   */
  async walkFile(e, followChain, visit) {
    let remaining = e.size;
    let block = e.start;
    while (remaining && block !== END_OF_CHAIN) {
      const size = Math.min(BLOCK, remaining);
      const off = this.geo.blockToOffset(block);
      if (off + size > this.blob.size) throw new StfsError(`${e.path} lies beyond the end of the package (truncated file?)`);
      await visit(block, off, size);
      remaining -= size;
      block = e.contiguous && !followChain ? block + 1 : await this.nextBlock(block);
    }
    if (remaining) throw new StfsError(`${e.path}: chain ended with ${remaining} bytes missing`);
  }

  /** Byte ranges [[offset, length], ...] of a file in the package, adjacent blocks merged. */
  async fileRuns(e) {
    const runs = [];
    await this.walkFile(e, false, (block, off, size) => {
      const last = runs[runs.length - 1];
      if (last && last[0] + last[1] === off) last[1] += size;
      else runs.push([off, size]);
    });
    return runs;
  }
}

/**
 * The 0x148-byte XCONTENT_AGGREGATE_DATA (big-endian) that starts a .header file: stfs_extract.py aggregate_data().
 * u32 device id, u32 content type, display name (UTF-16BE, 128 chars), file name (ASCII, 42 chars), padding,
 * u64 xuid (0 = shared) at 0x138, u32 title id at 0x140.
 */
export function aggregateData(meta, fileName, deviceId = 1) {
  const b = new Uint8Array(AGGREGATE_SIZE);
  const dv = new DataView(b.buffer);
  dv.setUint32(0, deviceId, false);
  dv.setUint32(4, meta.contentType, false);
  const name = Array.from(meta.displayNames?.en || fileName).slice(0, 127).join('');
  let p = 8;
  for (let i = 0; i < name.length; i++, p += 2) dv.setUint16(p, name.charCodeAt(i), false);
  for (let i = 0; i < Math.min(fileName.length, 42); i++) {
    const c = fileName.charCodeAt(i);
    if (c > 0x7f) throw new StfsError(`package file name ${JSON.stringify(fileName)} is not ASCII`);
    b[0x108 + i] = c;
  }
  dv.setBigUint64(0x138, 0n, false);
  dv.setUint32(0x140, meta.titleId, false);
  return b;
}

/** The whole .header file: aggregate data + the license mask as a little-endian u32 (what ContentManager writes). */
export function headerFileBytes(meta, fileName, licenseMask = 0xffffffff) {
  const b = new Uint8Array(HEADER_FILE_SIZE);
  b.set(aggregateData(meta, fileName), 0);
  new DataView(b.buffer).setUint32(AGGREGATE_SIZE, licenseMask >>> 0, true);
  return b;
}

/**
 * The folder name of a package under <title>/<content type>/: the package's own file name (what stfs_extract.py and
 * the console use) when it is a plain ASCII name of at most 42 characters, else the content id (40 hex digits).
 */
export function packageFolderName(fileName, meta) {
  const base = String(fileName ?? '').split(/[\\/]/).pop();
  if (/^[A-Za-z0-9_.-]{1,42}$/.test(base) && base !== '.' && base !== '..') return base;
  return meta.contentId;
}

/**
 * Opens a package. options: {fileName (default blob.name), expect: {titleId, contentType}, licenseMask}.
 * Returns {meta, folderName, entries, files, header}: `entries` are every file-table entry
 * {path, size, isDir, contiguous, start, runs, blob}; `files` only the files; `header` the .header file bytes.
 * Throws StfsError with a user-readable message when the package is not what `expect` asks for.
 */
export async function openStfs(blob, options = {}) {
  if (blob.size < 0x3b0) throw new StfsError('too small to be an Xbox 360 content package');
  const head = await readBytes(blob, 0, Math.min(HEADER_READ, blob.size));
  const meta = parseHeader(head, blob.size);
  const want = options.expect;
  if (want?.titleId != null && meta.titleId !== want.titleId) {
    throw new StfsError(`belongs to another game (title ${meta.titleIdHex}, expected ${hex(want.titleId)})`);
  }
  if (want?.contentType != null && meta.contentType !== want.contentType) {
    throw new StfsError(`is not downloadable content (content type ${meta.contentTypeHex} ${meta.contentTypeName}, expected ${hex(want.contentType)} ${CONTENT_TYPES[want.contentType] ?? ''})`.trim());
  }
  if (meta.volumeType !== 0) throw new StfsError('is an SVOD package; only STFS packages are supported');
  const reader = new Reader(blob, meta);
  const entries = await reader.readTree();
  for (const e of entries) {
    if (e.isDir) { e.runs = []; e.blob = null; continue; }
    e.runs = await reader.fileRuns(e);
    e.blob = new Blob(e.runs.map(([off, len]) => blob.slice(off, off + len)));
  }
  const files = entries.filter((e) => !e.isDir);
  const fileName = options.fileName ?? blob.name ?? meta.contentId;
  const folderName = packageFolderName(fileName, meta);
  return {
    meta,
    folderName,
    entries,
    files,
    payloadBytes: files.reduce((a, f) => a + f.size, 0),
    header: headerFileBytes(meta, folderName, options.licenseMask ?? 0xffffffff),
    reader,
  };
}

async function sha1(bytes) {
  return new Uint8Array(await globalThis.crypto.subtle.digest('SHA-1', bytes));
}

/**
 * Checks the SHA-1 of every data block of every file against the level-0 hash tables (stfs_extract.py --verify) and
 * that every file's hash chain gives the same blocks as its lazy Blob. onProgress(doneBytes, totalBytes).
 * Throws StfsError on the first mismatch.
 */
export async function verifyStfs(pkg, { onProgress, signal } = {}) {
  const { reader, files } = pkg;
  const blob = reader.blob;
  const total = files.reduce((a, f) => a + f.size, 0);
  let done = 0;
  // Sequential reads through a window: most files are contiguous, so one slice serves many blocks.
  const WINDOW = 4 * 1024 * 1024;
  let winStart = -1, win = null;
  const blockAt = async (off) => {
    if (!win || off < winStart || off + BLOCK > winStart + win.length) {
      winStart = off;
      win = await readBytes(blob, off, Math.min(WINDOW, blob.size - off));
      if (win.length < BLOCK) throw new StfsError('read past the end of the package (truncated file?)');
    }
    return win.subarray(off - winStart, off - winStart + BLOCK);
  };
  for (const f of files) {
    if (signal?.aborted) throw Object.assign(new Error('Cancelled'), { name: 'AbortError' });
    const runs = [];
    const pending = [];
    const check = async () => {
      const results = await Promise.all(pending.map(async (p) => ({ p, got: await sha1(p.data) })));
      for (const { p, got } of results) {
        if (!got.every((v, i) => v === p.want[i])) throw new StfsError(`${f.path}: SHA-1 mismatch in block ${p.block}`);
      }
      pending.length = 0;
    };
    await reader.walkFile(f, true, async (block, off, size) => {
      const { sha1: want } = await reader.blockHash(block);
      pending.push({ block, want: want.slice(), data: (await blockAt(off)).slice() });
      if (pending.length >= 256) await check();
      const last = runs[runs.length - 1];
      if (last && last[0] + last[1] === off) last[1] += size;
      else runs.push([off, size]);
      done += size;
    });
    await check();
    if (runs.length !== f.runs.length || runs.some((r, i) => r[0] !== f.runs[i][0] || r[1] !== f.runs[i][1])) {
      throw new StfsError(`${f.path}: the hash chain does not match the contiguous layout of the file table`);
    }
    onProgress?.(done, total);
  }
  return { files: files.length, bytes: total };
}
