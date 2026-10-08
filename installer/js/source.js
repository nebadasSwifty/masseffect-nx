// Turns what the user picked (a disc image, a folder from <input webkitdirectory>, a File System Access directory
// handle or a drop) into one list of files: [{path, size, blob}], where `blob` is a lazy Blob/File (nothing is read).
import { openXdvdfs } from './xdvdfs.js';
import { findDiscRoot, normalise, sha256Hex, findEdition, identifyEdition } from './plan.js';
import { checkPackage, checkPackages, isPackagePath, isMapPackage, duplicateIndex } from './pkgcheck.js';

export async function sourceFromIso(input, { onProgress } = {}) {
  const filesList = Array.isArray(input) ? input : (input instanceof FileList ? Array.from(input) : [input]);
  if (filesList.length === 1) {
    const file = filesList[0];
    const xiso = await openXdvdfs(file);
    return {
      kind: 'iso',
      label: file.name,
      discCount: 1,
      files: xiso.files.map(({ path, size, blob }) => ({ path, size, blob })),
      detail: xiso.partitionOffset ? `game partition at 0x${xiso.partitionOffset.toString(16).toUpperCase()}` : 'plain XDVDFS image',
      log: [],
    };
  }

  // Multiple ISO files (e.g. Disc 1 + Disc 2): sort so Disc1 comes before Disc2
  filesList.sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' }));

  const discs = [];
  for (const file of filesList) {
    const xiso = await openXdvdfs(file);
    discs.push({ name: file.name, files: xiso.files });
  }
  const { files, replacedByEarlier, replacedByLater, decisions, checks } = await mergeDiscFiles(discs, { onProgress });
  const discNames = discs.map((d) => d.name);
  return {
    kind: 'iso',
    label: discNames.join(' + '),
    discCount: discs.length,
    files,
    detail: `${filesList.length} discs merged (${discNames.join(', ')})` +
      (replacedByEarlier ? `, ${replacedByEarlier} duplicate files of a later disc skipped` : '') +
      (replacedByLater ? `, ${replacedByLater} taken from a later disc` : ''),
    log: [`Merging ${discNames.join(' + ')} (best copy per file, earliest disc on a tie):`, ...decisions.map((d) => `  ${d}`)],
    checks,
  };
}

/**
 * Structural check of the packages of a source (after the disc root is known). scope: 'all' (every package) or 'maps'
 * (Maps/*.xxx only). Returns {checked, bad: [{path, problems}], duplicates: [[path, ...]], log: [string]}.
 * Nothing is blocked here; the page shows a warning when bad or duplicate packages are found.
 */
export async function auditPackages(files, { scope = 'all', cache, onProgress } = {}) {
  const filter = scope === 'maps' ? isMapPackage : () => true;
  const r = await checkPackages(files, { filter, cache, onProgress });
  const log = [`Package check (${scope === 'maps' ? 'Maps/*.xxx' : 'all packages'}): ${r.checked} checked, ` +
    `${r.bad.length} bad, ${r.duplicates.length} same-GUID group(s).`];
  for (const b of r.bad) log.push(`  BAD ${b.path}: ${b.problems.join('; ')}`);
  for (const g of r.duplicates) log.push(`  SAME GUID under different names: ${g.join(', ')}`);
  return { ...r, log };
}

/*
 * Mass Effect's two discs (the RU "RUSSOUND+RUS_TEXT" repack) carry the same file list, but each disc fills the files
 * of the other disc's areas with placeholders: Disc 1 has junk instead of the ending maps (BIOA_END*) and of
 * UplinkSEQ02.bik, and its BIOA_LOS00.xxx (Ilos) is a complete, valid copy of the Feros map BIOA_WAR00 (same package
 * GUID); Disc 2 has junk instead of the Feros maps (BIOA_WAR*), and its BIOA_WAR00.xxx is the Ilos map cut to Disc 1's
 * size (its chunk table points past the end of the file). A tag check alone passes both fakes.
 *
 * Rule: for a path present on several discs, every copy gets a rank and the highest rank wins; on a tie the earliest
 * disc wins (Disc 1 is the primary disc of the game). Packages (js/pkgcheck.js, the port of tools/check_packages.py):
 *   3  passes the structural check and its GUID is not used by another file name in the same game root of that disc
 *   2  passes the structural check, but another name on that disc has the same GUID (a copy of another map)
 *   1  has the package tag but fails the structural check (cut or damaged)
 *   0  no package tag (junk)
 * Bink movies: 3 with the "BIK" tag, 0 without. Other files: all equal (earliest disc).
 */
const BINK_MAGIC = [0x42, 0x49, 0x4b];           // "BIK"

function fileType(path) {
  if (isPackagePath(path)) return 'package';
  if (path.toLowerCase().endsWith('.bik')) return 'bink';
  return null;
}

async function binkValid(entry) {
  if (entry.size < BINK_MAGIC.length) return false;
  const head = new Uint8Array(await entry.blob.slice(0, BINK_MAGIC.length).arrayBuffer());
  return BINK_MAGIC.every((byte, i) => head[i] === byte);
}

const baseName = (path) => path.slice(path.lastIndexOf('/') + 1);

/** Ranks one disc copy. Returns {rank, note} (note: a short text for the log). */
async function rankCopy(entry, disc) {
  const type = fileType(entry.path);
  if (type === 'bink') return (await binkValid(entry)) ? { rank: 3, note: 'Bink ok' } : { rank: 0, note: 'no Bink tag' };
  if (type !== 'package') return { rank: 0, note: '' };
  const r = await disc.check(entry);
  if (r.status !== 'ok') {
    const hasTag = !r.problems[0]?.startsWith('no package tag');
    return { rank: hasTag ? 1 : 0, note: `bad: ${r.problems[0]}${r.problems.length > 1 ? ` (+${r.problems.length - 1} more)` : ''}` };
  }
  const twins = (await disc.duplicates()).get(entry.path);
  if (twins?.length) return { rank: 2, note: `ok, but same package GUID as ${twins.map(baseName).join(', ')}` };
  return { rank: 3, note: 'ok' };
}

/**
 * discs: [{name, files: [{path, size, blob}]}] in disc order.
 * Returns {files, replacedByEarlier, replacedByLater, decisions: [string], checks: Map blob -> pkgcheck result}.
 */
export async function mergeDiscFiles(discs, { onProgress } = {}) {
  const checks = new Map(); // blob -> checkPackage result, shared with the later whole-source check
  const contexts = discs.map((d) => {
    const files = d.files.map((f) => ({ path: normalise(f.path), size: f.size, blob: f.blob }));
    let dupes = null;
    return {
      name: d.name,
      files,
      byKey: new Map(files.map((f) => [f.path.toLowerCase(), f])),
      async check(entry) {
        let r = checks.get(entry.blob);
        if (!r) checks.set(entry.blob, (r = await checkPackage(entry.blob)));
        return r;
      },
      // Same-GUID groups among the valid packages of this whole disc, so a copy of another map is seen even when
      // that other map exists on this disc only. A cut copy does not count: Disc 2's cut BIOA_WAR00 still carries
      // the GUID of the real Ilos map next to it, which must not make that real map suspect.
      async duplicates() {
        if (!dupes) {
          const entries = [];
          for (const f of files) {
            if (!isPackagePath(f.path)) continue;
            const r = await this.check(f);
            entries.push({ path: f.path, guid: r.status === 'ok' ? r.info.guid : null });
          }
          dupes = duplicateIndex(entries);
        }
        return dupes;
      },
    };
  });

  const order = [];
  const seen = new Set();
  for (const c of contexts) for (const f of c.files) {
    const key = f.path.toLowerCase();
    if (!seen.has(key)) { seen.add(key); order.push(key); }
  }

  const files = [];
  const decisions = [];
  let replacedByEarlier = 0;
  let replacedByLater = 0;
  let otherDuplicates = 0;
  let done = 0;
  for (const key of order) {
    const copies = contexts.map((c, i) => ({ disc: c, index: i, entry: c.byKey.get(key) })).filter((x) => x.entry);
    if (onProgress && ++done % 200 === 0) onProgress(done, order.length);
    if (copies.length === 1) { files.push(copies[0].entry); continue; }
    if (!fileType(copies[0].entry.path)) {
      files.push(copies[0].entry);
      replacedByEarlier += copies.length - 1;
      otherDuplicates++;
      continue;
    }
    for (const c of copies) Object.assign(c, await rankCopy(c.entry, c.disc));
    let best = copies[0];
    for (const c of copies) if (c.rank > best.rank) best = c;
    files.push(best.entry);
    if (best === copies[0]) replacedByEarlier += copies.length - 1;
    else replacedByLater++;
    const others = copies.filter((c) => c !== best).map((c) => `${c.disc.name} (${c.note})`).join(', ');
    decisions.push(`${best.entry.path}: ${best.disc.name} (${best.note}) over ${others}`);
  }
  if (otherDuplicates) decisions.push(`${otherDuplicates} other files on several discs: the earliest disc's copy`);
  return { files, replacedByEarlier, replacedByLater, decisions, checks };
}

export function sourceFromFileList(list) {
  const files = [];
  let label = '';
  for (const f of list) {
    const rel = f.webkitRelativePath || f.name;
    if (!label) label = rel.split('/')[0];
    files.push({ path: normalise(rel), size: f.size, blob: f });
  }
  return { kind: 'folder', label, files, detail: 'folder', log: [] };
}

export async function sourceFromDirectoryHandle(handle, onCount) {
  const files = [];
  const walk = async (dir, prefix) => {
    for await (const [name, entry] of dir.entries()) {
      if (entry.kind === 'directory') await walk(entry, `${prefix}${name}/`);
      else {
        const f = await entry.getFile();
        files.push({ path: `${prefix}${name}`, size: f.size, blob: f });
        if (onCount && files.length % 200 === 0) onCount(files.length);
      }
    }
  };
  await walk(handle, `${handle.name}/`);
  return { kind: 'folder', label: handle.name, files, detail: 'folder', log: [] };
}

/** Reads a drop. Returns {kind:'iso', file|files} for single/multi ISO, or a folder source. */
export async function sourceFromDataTransfer(dt, onCount) {
  const items = [...(dt.items ?? [])].filter((i) => i.kind === 'file');
  if (items.length === 0 && dt.files?.length) {
    const fl = [...dt.files];
    if (fl.every((f) => /\.(iso|xiso)$/i.test(f.name))) return { files: fl };
    return { file: fl[0] };
  }
  const entries = items.map((i) => i.webkitGetAsEntry?.()).filter(Boolean);
  if (entries.length > 0 && entries.every((e) => e.isFile && /\.(iso|xiso)$/i.test(e.name))) {
    const files = await Promise.all(entries.map((e) => new Promise((res, rej) => e.file(res, rej))));
    return { files: files.length > 1 ? files : undefined, file: files.length === 1 ? files[0] : undefined };
  }
  if (entries.length === 1 && entries[0].isFile) {
    return { file: await new Promise((res, rej) => entries[0].file(res, rej)) };
  }
  const files = [];
  const readAll = (reader) => new Promise((res, rej) => {
    const all = [];
    const next = () => reader.readEntries((batch) => (batch.length ? (all.push(...batch), next()) : res(all)), rej);
    next();
  });
  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const f = await new Promise((res, rej) => entry.file(res, rej));
      files.push({ path: `${prefix}${entry.name}`, size: f.size, blob: f });
      if (onCount && files.length % 200 === 0) onCount(files.length);
    } else {
      for (const child of await readAll(entry.createReader())) await walk(child, `${prefix}${entry.name}/`);
    }
  };
  for (const e of entries) await walk(e, '');
  return { source: { kind: 'folder', label: entries[0]?.name ?? 'folder', files, detail: 'folder', log: [] } };
}

/**
 * Finds the disc root, reads default.xex and identifies the edition.
 * Returns {files, xex, sha256, edition|null, matchType, header, error}; throws a user-readable Error when there is no default.xex.
 */
export async function inspectDisc(source, config) {
  const root = findDiscRoot(source.files, config.disc.xex);
  if (!root) {
    throw new Error(source.kind === 'iso'
      ? `${config.disc.xex} was not found in the disc image. Is it a Mass Effect disc?`
      : `${config.disc.xex} was not found in the folder. Pick the folder that contains ${config.disc.xex} (or its parent).`);
  }
  const xex = root.files.find((f) => f.path.toLowerCase() === config.disc.xex.toLowerCase());
  const bytes = new Uint8Array(await xex.blob.arrayBuffer());
  const sha256 = await sha256Hex(bytes);
  const identified = identifyEdition(config.editions, sha256, bytes);
  return {
    files: root.files,
    prefix: root.prefix,
    xex,
    sha256,
    edition: identified.edition,
    matchType: identified.matchType,
    header: identified.header,
    error: identified.error,
  };
}
