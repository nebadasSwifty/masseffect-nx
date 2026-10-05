// Pure logic: which disc files are copied, how the disc root is found in a folder pick, which edition it is.

export function normalise(path) {
  return path.replace(/\\/g, '/').replace(/^\/+/, '');
}

/**
 * A folder pick may be the disc root itself or its parent. Finds the shallowest default.xex and re-roots the
 * file list there. files: [{path, ...}] -> {files (re-rooted), prefix} or null when there is no default.xex.
 */
export function findDiscRoot(files, xexName) {
  const want = xexName.toLowerCase();
  let best = null;
  for (const f of files) {
    const p = normalise(f.path);
    const parts = p.split('/');
    if (parts[parts.length - 1].toLowerCase() !== want) continue;
    if (best === null || parts.length < best.length) best = parts;
  }
  if (!best) return null;
  const prefix = best.slice(0, -1).join('/');
  const cut = prefix ? prefix.length + 1 : 0;
  const lower = prefix.toLowerCase();
  const out = [];
  for (const f of files) {
    const p = normalise(f.path);
    if (prefix && !p.toLowerCase().startsWith(lower + '/')) continue;
    out.push({ ...f, path: p.slice(cut) });
  }
  return { files: out, prefix };
}

/** Splits the disc files into those copied to game_root and those left out (with the reason). */
export function planGameFiles(files, disc) {
  const copy = [];
  const skipped = [];
  for (const f of files) {
    const lower = f.path.toLowerCase();
    const rule = disc.skip.find((s) => lower.startsWith(s.prefix.toLowerCase()));
    if (rule) skipped.push({ file: f, why: rule.why });
    else copy.push(f);
  }
  const sum = (list, get) => list.reduce((a, x) => a + get(x), 0);
  return {
    copy,
    skipped,
    copyBytes: sum(copy, (f) => f.size),
    skippedBytes: sum(skipped, (s) => s.file.size),
  };
}

export function scanCandidates(files, disc) {
  return files.filter((f) => disc.scanExtensions.some((e) => f.path.toLowerCase().endsWith(e)));
}

export function findEdition(editions, sha256Hex) {
  const h = sha256Hex.toLowerCase();
  return editions.find((e) => {
    if (Array.isArray(e.xexSha256)) {
      return e.xexSha256.some((x) => x.toLowerCase() === h);
    }
    return typeof e.xexSha256 === 'string' && e.xexSha256.toLowerCase() === h;
  }) ?? null;
}

export function parseXexHeader(bytes) {
  if (!bytes || bytes.length < 0x20) {
    throw new Error('File is too small to be a valid XEX2 executable');
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = view.getUint32(0, false);
  if (magic !== 0x58455832) { // 'XEX2'
    throw new Error('Not a valid Xbox 360 executable (missing XEX2 magic)');
  }
  const securityOffset = view.getUint32(0x10, false);
  const optionalCount = view.getUint32(0x14, false);
  if (bytes.length < 0x18 + optionalCount * 8) {
    throw new Error('Truncated XEX2 optional headers table');
  }
  const optional = new Map();
  for (let i = 0; i < optionalCount; i++) {
    const key = view.getUint32(0x18 + i * 8, false);
    const val = view.getUint32(0x18 + i * 8 + 4, false);
    optional.set(key, val);
  }

  let titleId = null;
  let mediaId = null;
  let version = null;
  if (optional.has(0x00040006)) {
    const execInfoOff = optional.get(0x00040006);
    if (bytes.length < execInfoOff + 16) {
      throw new Error('Truncated XEX2 execution info');
    }
    mediaId = view.getUint32(execInfoOff, false);
    version = view.getUint32(execInfoOff + 4, false);
    titleId = view.getUint32(execInfoOff + 12, false);
  }

  let entryPoint = optional.get(0x00010100) ?? null;
  let imageSize = null;
  if (securityOffset + 8 <= bytes.length) {
    imageSize = view.getUint32(securityOffset + 4, false);
  }

  return { titleId, mediaId, version, entryPoint, imageSize };
}

export function matchEditionHeader(editions, header) {
  if (!header || header.titleId == null) return null;
  return editions.find((e) => {
    if (!e.header) return false;
    return (
      e.header.titleId === header.titleId &&
      e.header.mediaId === header.mediaId &&
      e.header.version === header.version &&
      e.header.entryPoint === header.entryPoint &&
      (!e.header.imageSize || e.header.imageSize === header.imageSize)
    );
  }) ?? null;
}

export function identifyEdition(editions, sha256Hex, bytes = null) {
  const exact = findEdition(editions, sha256Hex);
  if (exact) {
    return { edition: exact, matchType: 'exact', header: null, error: null };
  }
  if (!bytes) {
    return { edition: null, matchType: 'unsupported', header: null, error: null };
  }
  let header = null;
  try {
    header = parseXexHeader(bytes);
  } catch (err) {
    return { edition: null, matchType: 'invalid', header: null, error: err.message };
  }
  const fallback = matchEditionHeader(editions, header);
  if (fallback) {
    return { edition: fallback, matchType: 'unverified_header', header, error: null };
  }
  return { edition: null, matchType: 'foreign_header', header, error: null };
}

export async function sha256Hex(bytes) {
  if (!globalThis.crypto?.subtle) throw new Error('This page needs a secure context (https:// or http://localhost) to hash default.xex.');
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
}

export function formatBytes(n) {
  if (n < 1024) return `${n} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let v = n / 1024, i = 0;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return `${v.toFixed(v >= 100 ? 0 : v >= 10 ? 1 : 2)} ${units[i]}`;
}

export function formatDuration(seconds) {
  if (!isFinite(seconds) || seconds < 0) return '';
  const s = Math.round(seconds);
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ${String(s % 60).padStart(2, '0')} s`;
  return `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, '0')} min`;
}
