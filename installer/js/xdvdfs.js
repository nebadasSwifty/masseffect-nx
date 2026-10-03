// Read-only XDVDFS (Xbox 360 game disc file system) reader for Blob / File objects.
//
// Nothing is ever loaded as a whole: the volume descriptor, each directory table and each file are read
// with Blob.slice(), so a 7 GB (or dual-layer) disc image costs only the memory of the chunk being read.
//
// Layout (all little-endian):
//   volume descriptor at <base> + 0x10000: "MICROSOFT*XBOX*MEDIA", u32 root dir sector, u32 root dir size,
//   FILETIME, padding, the same magic again at +0x7EC.
//   <base> is where the game partition starts inside the image: 0 for a plain XISO, or one of the offsets
//   the xiso tools probe for images that keep the video partition in front (XGD2, XGD3, XGD1).
//   directory table: entries of  u16 left, u16 right, u32 sector, u32 size, u8 attributes, u8 nameLength, name
//   (left/right are offsets in 4-byte units inside the table, 0 = no child, entries are 4-byte aligned and the
//   table is padded with 0xFF).  Sector = 2048 bytes, relative to <base>.

export const SECTOR = 2048;
export const VOLUME_DESCRIPTOR_OFFSET = 0x10000;
export const MAGIC = 'MICROSOFT*XBOX*MEDIA';
// Same order as extract-xiso: plain, XGD2 (0x0FD90000), XGD3 (0x02080000), XGD1 (0x18300000).
export const PARTITION_OFFSETS = [0, 0x0fd90000, 0x02080000, 0x18300000];

const ATTR_DIRECTORY = 0x10;
const MAX_DIR_BYTES = 64 * 1024 * 1024;
const MAX_ENTRIES = 1_000_000;

export class XdvdfsError extends Error {
  constructor(message) {
    super(message);
    this.name = 'XdvdfsError';
  }
}

async function readBytes(blob, offset, length) {
  if (offset < 0 || offset + length > blob.size) throw new XdvdfsError('read past the end of the image (truncated file?)');
  return new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
}

function ascii(bytes, start, end) {
  let s = '';
  for (let i = start; i < end; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

/** Looks for the volume descriptor at every known partition offset. Returns the offset or -1. */
export async function findPartition(blob) {
  for (const base of PARTITION_OFFSETS) {
    const at = base + VOLUME_DESCRIPTOR_OFFSET;
    if (at + 0x800 > blob.size) continue;
    const head = await readBytes(blob, at, 20);
    if (ascii(head, 0, 20) !== MAGIC) continue;
    const tail = await readBytes(blob, at + 0x7ec, 20);
    if (ascii(tail, 0, 20) === MAGIC) return base;
  }
  return -1;
}

function safeName(name) {
  return name.length > 0 && name !== '.' && name !== '..' && !/[\\/\0:*?"<>|]/.test(name);
}

/** Parses one directory table into entries {name, sector, size, isDir}. */
function parseTable(table, dirPath) {
  const out = [];
  const seen = new Set();
  const stack = [0];
  const view = new DataView(table.buffer, table.byteOffset, table.byteLength);
  while (stack.length) {
    const unit = stack.pop();
    const at = unit * 4;
    if (seen.has(unit)) throw new XdvdfsError(`directory ${dirPath || '/'}: loop in the entry tree`);
    seen.add(unit);
    if (at + 14 > table.length) throw new XdvdfsError(`directory ${dirPath || '/'}: entry outside the table`);
    const left = view.getUint16(at, true);
    const right = view.getUint16(at + 2, true);
    if (left === 0xffff && right === 0xffff) continue; // padding: an empty directory
    const sector = view.getUint32(at + 4, true);
    const size = view.getUint32(at + 8, true);
    const attributes = table[at + 12];
    const nameLength = table[at + 13];
    if (at + 14 + nameLength > table.length) throw new XdvdfsError(`directory ${dirPath || '/'}: name outside the table`);
    const name = ascii(table, at + 14, at + 14 + nameLength);
    if (!safeName(name)) throw new XdvdfsError(`directory ${dirPath || '/'}: unsafe file name ${JSON.stringify(name)}`);
    out.push({ name, sector, size, isDir: (attributes & ATTR_DIRECTORY) !== 0 });
    if (out.length > MAX_ENTRIES) throw new XdvdfsError('too many entries');
    if (right !== 0) stack.push(right);
    if (left !== 0) stack.push(left);
  }
  return out;
}

/**
 * Opens an image. Returns {base, files, partitionOffset}: `files` is a sorted array of
 * {path, size, offset, blob} where `offset` is the absolute byte position in the image and `blob` is the lazy
 * Blob slice of the file's bytes. Directories are not listed (empty ones are dropped).
 */
export async function openXdvdfs(blob) {
  const base = await findPartition(blob);
  if (base < 0) throw new XdvdfsError('not an Xbox 360 disc image (no XDVDFS volume descriptor found)');
  const vd = await readBytes(blob, base + VOLUME_DESCRIPTOR_OFFSET, 32);
  const dv = new DataView(vd.buffer);
  const rootSector = dv.getUint32(20, true);
  const rootSize = dv.getUint32(24, true);
  const files = [];
  const pending = [{ path: '', sector: rootSector, size: rootSize }];
  let visitedDirs = 0;
  while (pending.length) {
    const dir = pending.pop();
    if (++visitedDirs > 100000) throw new XdvdfsError('too many directories');
    if (dir.size === 0) continue;
    if (dir.size > MAX_DIR_BYTES) throw new XdvdfsError(`directory ${dir.path || '/'} is implausibly large`);
    const table = await readBytes(blob, base + dir.sector * SECTOR, dir.size);
    for (const e of parseTable(table, dir.path)) {
      const path = dir.path ? `${dir.path}/${e.name}` : e.name;
      if (e.isDir) {
        pending.push({ path, sector: e.sector, size: e.size });
      } else {
        const offset = base + e.sector * SECTOR;
        if (offset + e.size > blob.size) {
          throw new XdvdfsError(`${path} lies beyond the end of the image (incomplete or truncated dump)`);
        }
        files.push({ path, size: e.size, offset, blob: blob.slice(offset, offset + e.size) });
      }
    }
  }
  files.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  return { base, partitionOffset: base, files };
}
