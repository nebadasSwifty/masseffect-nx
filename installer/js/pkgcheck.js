// Structural check of Mass Effect 1 (Xbox 360) UE3 packages: a port of tools/check_packages.py.
//
// Only the package summary (the first bytes of the file) and, for compressed packages, the 16-byte header and the
// block table of every chunk are read, each with blob.slice(), so this works on lazy Blobs of an ISO entry or of a
// folder file and never reads a whole package. The problem texts are the same as the Python tool's (the tests
// compare both on the same files). Format notes: see the docstring of tools/check_packages.py.

export const TAG = 0x9e2a83c1;
export const EXPECTED_VERSION = 0x005c0187;
export const PACKAGE_EXTENSIONS = ['.xxx', '.upk', '.sfm', '.u'];
const HEAD_BYTES = 0x10000;   // what the Python tool reads; parsing never needs more
const FIRST_HEAD_BYTES = 0x1000; // tried first; a summary that does not fit is read again with HEAD_BYTES

export class PackageBad extends Error {}

export function isPackagePath(path) {
  const lower = path.toLowerCase();
  return PACKAGE_EXTENSIONS.some((e) => lower.endsWith(e));
}

const hex = (n, width = 0) => n.toString(16).toUpperCase().padStart(width, '0');

function u32(head, off) {
  if (off + 4 > head.length) throw new PackageBad(`summary cut at 0x${hex(off)}`);
  return ((head[off] << 24) | (head[off + 1] << 16) | (head[off + 2] << 8) | head[off + 3]) >>> 0;
}

/** Parses the package summary from the first bytes of a package. Throws PackageBad on a malformed header. */
export function parseSummary(head) {
  if (head.length < 4 || u32(head, 0) !== TAG) {
    const first = Array.from(head.subarray(0, 4), (b) => b.toString(16).padStart(2, '0')).join('');
    throw new PackageBad(`no package tag (first bytes ${first})`);
  }
  const s = { version: u32(head, 4), headerSize: u32(head, 8) };
  const flen = head.length >= 16 ? u32(head, 12) | 0 : null;
  if (flen === null || flen < -256 || flen > 256) throw new PackageBad(`bad folder name length ${flen === null ? 'None' : flen}`);
  let pos = 16 + (flen >= 0 ? flen : -2 * flen);
  s.packageFlags = u32(head, pos);
  [s.nameCount, s.nameOffset, s.exportCount, s.exportOffset, s.importCount, s.importOffset] =
    [0, 1, 2, 3, 4, 5].map((i) => u32(head, pos + 4 + 4 * i));
  s.guid = Array.from(head.subarray(pos + 28, pos + 44), (b) => b.toString(16).padStart(2, '0')).join('');
  pos += 28 + 16;
  const gens = u32(head, pos);
  if (gens > 1000) throw new PackageBad(`generation count ${gens}`);
  pos += 4 + 12 * gens;
  s.engineVersion = u32(head, pos);
  s.cookerVersion = u32(head, pos + 4);
  pos += 8 + 28;
  s.compression = u32(head, pos);
  const count = u32(head, pos + 4);
  if (![0, 1, 2, 4].includes(s.compression)) throw new PackageBad(`compression flags ${s.compression} at 0x${hex(pos)}`);
  if (count > 4096 || (count && s.compression === 0) || (s.compression && !count)) {
    throw new PackageBad(`chunk count ${count} with compression ${s.compression}`);
  }
  pos += 8;
  if (pos + 16 * count > head.length) throw new PackageBad('chunk table cut');
  s.chunks = [];
  for (let i = 0; i < count; i++) {
    const at = pos + 16 * i;
    s.chunks.push([u32(head, at), u32(head, at + 4), u32(head, at + 8), u32(head, at + 12)]);
  }
  s.tableEnd = pos + 16 * count;
  return s;
}

async function read(blob, offset, length) {
  const end = Math.min(blob.size, offset + length);
  if (end <= offset) return new Uint8Array(0);
  return new Uint8Array(await blob.slice(offset, end).arrayBuffer());
}

/** Reads and parses the summary (small read first, the Python tool's 64 KiB only when needed). */
async function readSummary(blob) {
  let head = await read(blob, 0, FIRST_HEAD_BYTES);
  try {
    return parseSummary(head);
  } catch (e) {
    if (!(e instanceof PackageBad) || head.length >= Math.min(blob.size, HEAD_BYTES)) throw e;
  }
  head = await read(blob, 0, HEAD_BYTES);
  return parseSummary(head);
}

/** Returns the package GUID (32 hex digits) or null when the summary does not parse. One small read. */
export async function readPackageGuid(blob) {
  try {
    return (await readSummary(blob)).guid;
  } catch (e) {
    if (e instanceof PackageBad) return null;
    throw e;
  }
}

/**
 * Checks one package. blob: a Blob-like object (size, slice(a, b), arrayBuffer()).
 * Returns {status: 'ok' | 'bad', problems: [string], info: {size, compression?, chunks?, guid?, needs?, streamSize?}}.
 */
export async function checkPackage(blob) {
  const size = blob.size;
  const problems = [];
  const info = { size };
  let s;
  try {
    s = await readSummary(blob);
  } catch (e) {
    if (e instanceof PackageBad) return { status: 'bad', problems: [e.message], info };
    throw e;
  }
  info.compression = s.compression;
  info.chunks = s.chunks.length;
  info.guid = s.guid;
  if (s.version !== EXPECTED_VERSION) {
    problems.push(`version 0x${hex(s.version, 8)} (expected 0x${hex(EXPECTED_VERSION, 8)})`);
  }
  const chunks = s.chunks;
  let streamSize = size;
  if (chunks.length) {
    streamSize = Math.max(...chunks.map(([uo, us]) => uo + us));
    if (chunks[0][0] > s.headerSize) problems.push(`first chunk uncompressed offset ${chunks[0][0]} implausible`);
    let prevUend = chunks[0][0];
    let need = 0;
    for (let i = 0; i < chunks.length; i++) {
      const [uo, us, co, cs] = chunks[i];
      if (uo !== prevUend) problems.push(`chunk ${i} uncompressed offset ${uo}, expected ${prevUend}`);
      prevUend = uo + us;
      need = Math.max(need, co + cs);
      if (co + cs > size) {
        problems.push(`chunk ${i} needs bytes ${co}..${co + cs}, file has ${size}`);
        continue;
      }
      const ch = await read(blob, co, 16);
      const [tag, rawBlock, csz, usz] = ch.length === 16 ? [0, 4, 8, 12].map((o) => u32(ch, o)) : [0, 0, 0, 0];
      if (tag !== TAG) {
        problems.push(`chunk ${i} at ${co}: no tag (${hex(tag, 8)})`);
        continue;
      }
      if (usz !== us || rawBlock === 0) {
        problems.push(`chunk ${i} header sizes ${usz}/${rawBlock} disagree with table ${us}`);
        continue;
      }
      const block = rawBlock === TAG ? 0x20000 : rawBlock; // ME1 Xbox writes the tag again where UE3 stores BlockSize
      const nblocks = Math.ceil(usz / block);
      const bt = await read(blob, co + 16, 8 * nblocks);
      if (bt.length !== 8 * nblocks) {
        problems.push(`chunk ${i} block table cut`);
        continue;
      }
      let sumC = 0;
      let sumU = 0;
      for (let b = 0; b < nblocks; b++) {
        sumC += u32(bt, 8 * b);
        sumU += u32(bt, 8 * b + 4);
      }
      if (sumU !== usz || sumC !== csz) problems.push(`chunk ${i} block sums ${sumC}/${sumU} != ${csz}/${usz}`);
      const blocksEnd = co + 16 + 8 * nblocks + sumC;
      if (blocksEnd > co + cs || blocksEnd > size) {
        problems.push(`chunk ${i} blocks end at ${blocksEnd} (chunk end ${co + cs}, file ${size})`);
      }
    }
    info.needs = need;
    info.streamSize = streamSize;
  }
  for (const [name, off, cnt] of [['name', s.nameOffset, s.nameCount], ['export', s.exportOffset, s.exportCount],
    ['import', s.importOffset, s.importCount]]) {
    if (cnt && !(off > 0 && off < streamSize)) problems.push(`${name} table offset ${off} outside package (${streamSize})`);
  }
  if (!(s.headerSize > 0 && s.headerSize <= streamSize)) problems.push(`header size ${s.headerSize} outside package (${streamSize})`);
  if (!chunks.length && s.headerSize > size) problems.push('file cut inside the header');
  return { status: problems.length ? 'bad' : 'ok', problems, info };
}

const baseName = (path) => path.slice(path.lastIndexOf('/') + 1).toLowerCase();

/** The game root of a package path (its folder's parent, e.g. "Layer0" for "Layer0/Maps/X.xxx"), as the Python tool groups. */
export function guidGroupKey(path) {
  const parts = path.replace(/\\/g, '/').split('/');
  return parts.slice(0, Math.max(0, parts.length - 2)).join('/').toLowerCase();
}

/**
 * entries: [{path, guid}] (guid may be null). Returns the groups of paths that share one package GUID under different
 * file names inside one game root: [[path, ...], ...]. A placeholder copy of another map looks like this (the RU
 * two-disc repack ships Feros' BIOA_WAR00 data as BIOA_LOS00 on Disc 1); the retail EN disc has none.
 */
export function guidDuplicates(entries) {
  const by = new Map();
  for (const { path, guid } of entries) {
    if (!guid) continue;
    const key = `${guidGroupKey(path)}\n${guid}`;
    if (!by.has(key)) by.set(key, []);
    by.get(key).push(path);
  }
  return [...by.values()].filter((v) => new Set(v.map(baseName)).size > 1);
}

/** path -> [other paths with the same GUID and another name] for every path in a duplicate group. */
export function duplicateIndex(entries) {
  const index = new Map();
  for (const group of guidDuplicates(entries)) {
    for (const p of group) index.set(p, group.filter((q) => baseName(q) !== baseName(p)));
  }
  return index;
}

/** Maps/*.xxx files of a file list (the packages where a broken copy makes a location unreachable). */
export function isMapPackage(path) {
  return /(^|\/)maps\/[^/]+\.xxx$/i.test(path);
}

/**
 * Checks the packages of a file list. files: [{path, blob}]. options: {filter(path) -> bool (default: all packages),
 * onProgress(done, total), concurrency (default 8), cache: Map/WeakMap blob -> result}.
 * Returns {checked, bad: [{path, problems}], duplicates: [[path, ...]], results: Map path -> result}.
 */
export async function checkPackages(files, options = {}) {
  const { filter = () => true, onProgress, concurrency = 8, cache } = options;
  const list = files.filter((f) => isPackagePath(f.path) && filter(f.path));
  const results = new Map();
  let next = 0;
  let done = 0;
  const worker = async () => {
    while (next < list.length) {
      const f = list[next++];
      let r = cache?.get(f.blob);
      if (!r) {
        r = await checkPackage(f.blob);
        cache?.set(f.blob, r);
      }
      results.set(f.path, r);
      done++;
      if (onProgress && (done % 50 === 0 || done === list.length)) onProgress(done, list.length);
    }
  };
  await Promise.all(Array.from({ length: Math.min(concurrency, list.length) }, worker));
  const bad = list.filter((f) => results.get(f.path).status === 'bad')
    .map((f) => ({ path: f.path, problems: results.get(f.path).problems }));
  // Same-GUID groups among the packages that pass (a cut copy is already reported as bad; its GUID must not make
  // the intact package of the other name look suspect). tools/check_packages.py groups all packages.
  const duplicates = guidDuplicates(list.map((f) => {
    const r = results.get(f.path);
    return { path: f.path, guid: r.status === 'ok' ? r.info.guid : null };
  }));
  return { checked: list.length, bad, duplicates, results };
}
