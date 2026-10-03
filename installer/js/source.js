// Turns what the user picked (a disc image, a folder from <input webkitdirectory>, a File System Access directory
// handle or a drop) into one list of files: [{path, size, blob}], where `blob` is a lazy Blob/File (nothing is read).
import { openXdvdfs } from './xdvdfs.js';
import { findDiscRoot, normalise, sha256Hex, findEdition } from './plan.js';

export async function sourceFromIso(file) {
  const xiso = await openXdvdfs(file);
  return {
    kind: 'iso',
    label: file.name,
    files: xiso.files.map(({ path, size, blob }) => ({ path, size, blob })),
    detail: xiso.partitionOffset ? `game partition at 0x${xiso.partitionOffset.toString(16).toUpperCase()}` : 'plain XDVDFS image',
  };
}

export function sourceFromFileList(list) {
  const files = [];
  let label = '';
  for (const f of list) {
    const rel = f.webkitRelativePath || f.name;
    if (!label) label = rel.split('/')[0];
    files.push({ path: normalise(rel), size: f.size, blob: f });
  }
  return { kind: 'folder', label, files, detail: 'folder' };
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
  return { kind: 'folder', label: handle.name, files, detail: 'folder' };
}

/** Reads a drop. Returns {kind:'iso', file} for a single file, or a folder source. */
export async function sourceFromDataTransfer(dt, onCount) {
  const items = [...(dt.items ?? [])].filter((i) => i.kind === 'file');
  if (items.length === 0 && dt.files?.length) return { file: dt.files[0] };
  const entries = items.map((i) => i.webkitGetAsEntry?.()).filter(Boolean);
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
  return { source: { kind: 'folder', label: entries[0]?.name ?? 'folder', files, detail: 'folder' } };
}

/**
 * Finds the disc root, reads default.xex and identifies the edition.
 * Returns {files, xex, sha256, edition|null}; throws a user-readable Error when there is no default.xex.
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
  return { files: root.files, prefix: root.prefix, xex, sha256, edition: findEdition(config.editions, sha256) };
}
