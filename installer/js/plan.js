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
  return editions.find((e) => e.xexSha256.toLowerCase() === h) ?? null;
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
