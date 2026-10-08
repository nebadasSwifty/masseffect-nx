import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { checkPackage, checkPackages, guidDuplicates } from '../js/pkgcheck.js';
import { mergeDiscFiles, auditPackages } from '../js/source.js';
import { buildPackage, chunkOffset } from './pkg_builder.js';

const here = dirname(fileURLToPath(import.meta.url));
const pythonTool = join(here, '..', '..', 'tools', 'check_packages.py');

/** A Blob that counts the bytes read through it (to prove that only headers and block tables are read). */
class CountingBlob {
  constructor(bytes, start = 0, end = bytes.length, stats = { read: 0, calls: 0 }) {
    Object.assign(this, { bytes, start, end, stats });
  }
  get size() { return this.end - this.start; }
  slice(a, b = this.size) {
    return new CountingBlob(this.bytes, this.start + a, this.start + Math.min(b, this.size), this.stats);
  }
  async arrayBuffer() {
    this.stats.read += this.size;
    this.stats.calls++;
    return this.bytes.slice(this.start, this.end).buffer;
  }
}

const entry = (path, bytes) => ({ path, size: bytes.length, blob: new Blob([bytes]) });
const cut = (bytes, n) => bytes.slice(0, bytes.length - n);
const pick = (files, path) => files.find((f) => f.path === path);
async function bytesOf(e) { return new Uint8Array(await e.blob.arrayBuffer()); }

function corrupt(bytes, at, value) {
  const b = bytes.slice();
  new DataView(b.buffer).setUint32(at, value);
  return b;
}

// Synthetic packages, valid and broken in the ways check_packages.py knows.
function cases() {
  const good = buildPackage({ guid: 1, chunks: [3, 2, 1] });
  const c1 = chunkOffset(good, 1);
  return {
    'good.xxx': good,
    'uncompressed.upk': buildPackage({ guid: 2, chunks: [] }),
    'manygens.xxx': buildPackage({ guid: 3, chunks: [2], gens: 7 }),
    'blocksize.xxx': buildPackage({ guid: 4, chunks: [2], blockSize: 0x20000 }),
    'truncated_last_chunk.xxx': cut(good, 40),
    'truncated_heavily.xxx': good.slice(0, chunkOffset(good, 1) + 10),
    'block_table_cut.xxx': good.slice(0, chunkOffset(good, 2) + 20),
    'no_tag.xxx': new Uint8Array([0x49, 0x51, 0xf2, 0x7d, 1, 2, 3, 4, 5, 6]),
    'tiny.xxx': new Uint8Array([0x9e, 0x2a]),
    'summary_cut.xxx': good.slice(0, 40),
    'chunk_table_cut.xxx': good.slice(0, chunkOffset(good, 0) - 20),
    'version.xxx': corrupt(good, 4, 0x005c0188),
    'folder_len.xxx': corrupt(good, 12, 9999),
    'chunk_no_tag.xxx': corrupt(good, c1, 0x11223344),
    'chunk_usize.xxx': corrupt(good, c1 + 12, 5),
    'block_sum.xxx': corrupt(good, c1 + 16, 999),
    'compression.xxx': corrupt(buildPackage({ guid: 5, chunks: [1] }), 16 + 5 + 4 + 24 + 16 + 4 + 12 + 8 + 28, 3),
  };
}

test('valid synthetic packages pass, truncated or damaged ones are rejected', async () => {
  const all = cases();
  for (const name of ['good.xxx', 'uncompressed.upk', 'manygens.xxx', 'blocksize.xxx']) {
    const r = await checkPackage(new Blob([all[name]]));
    assert.equal(r.status, 'ok', `${name}: ${r.problems}`);
    assert.equal(r.info.guid.length, 32);
  }
  for (const [name, bytes] of Object.entries(all)) {
    if (['good.xxx', 'uncompressed.upk', 'manygens.xxx', 'blocksize.xxx'].includes(name)) continue;
    const r = await checkPackage(new Blob([bytes]));
    assert.equal(r.status, 'bad', name);
    assert.ok(r.problems.length > 0, name);
  }
  const t = await checkPackage(new Blob([all['truncated_last_chunk.xxx']]));
  assert.match(t.problems[0], /^chunk 2 needs bytes \d+\.\.\d+, file has \d+$/);
});

test('the check reads only the summary and the chunk block tables', async () => {
  const pkg = buildPackage({ guid: 9, chunks: [8, 8, 8, 8] });
  const blob = new CountingBlob(pkg);
  const r = await checkPackage(blob);
  assert.equal(r.status, 'ok');
  assert.equal(blob.stats.calls, 1 + 2 * 4);           // summary + (chunk header, block table) per chunk
  assert.ok(blob.stats.read <= 0x1000 + 4 * (16 + 64), `${blob.stats.read} bytes read`);
});

test('JS checker matches tools/check_packages.py on synthetic packages', { skip: spawnSync('python3', ['--version']).status !== 0 }, async () => {
  const dir = mkdtempSync(join(tmpdir(), 'pkgcheck-'));
  try {
    const all = cases();
    // a same-GUID pair under different names in one game root
    all['BIOA_LOS00.xxx'] = buildPackage({ guid: 1, chunks: [3, 2, 1], seed: 5 });
    for (const [name, bytes] of Object.entries(all)) writeFileSync(join(dir, name), bytes);
    const py = spawnSync('python3', [pythonTool, '--json', dir], { encoding: 'utf8' });
    const lines = py.stdout.trim().split('\n');
    const json = lines.filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
    assert.equal(json.length, Object.keys(all).length);
    for (const o of json) {
      const name = o.path.slice(dir.length + 1);
      const r = await checkPackage(new Blob([all[name]]));
      assert.equal(r.status, o.status, name);
      assert.deepEqual(r.problems, o.problems, name);
      assert.equal(r.info.guid ?? null, o.guid ?? null, name);
      assert.equal(r.info.chunks ?? null, o.chunks ?? null, name);
      assert.equal(r.info.compression ?? null, o.compression ?? null, name);
      assert.equal(r.info.needs ?? null, o.needs ?? null, name);
      assert.equal(r.info.streamSize ?? null, o.stream_size ?? null, name);
    }
    // duplicate groups: Python prints a DUP line with the paths
    const pyDup = lines.filter((l) => l.startsWith('DUP')).length;
    const jsDup = guidDuplicates(await Promise.all(Object.entries(all).map(async ([name, bytes]) => (
      { path: `${dir}/${name}`, guid: (await checkPackage(new Blob([bytes]))).info.guid ?? null }))));
    assert.equal(jsDup.length, pyDup);
    assert.equal(py.status, 1);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// The RU two-disc repack: Disc 1 has the real Feros BIOA_WAR00 and a valid Feros copy as BIOA_LOS00 (same GUID);
// Disc 2 has the real Ilos BIOA_LOS00 and the Ilos map cut to Disc 1's size as BIOA_WAR00.
function ruDiscs() {
  const feros = buildPackage({ guid: 'aa'.repeat(16), chunks: [4, 3] });
  const ferosCopy = buildPackage({ guid: 'aa'.repeat(16), chunks: [4, 3], seed: 9 });
  const ilos = buildPackage({ guid: 'bb'.repeat(16), chunks: [5, 5] });
  const ilosCut = ilos.slice(0, feros.length);
  const other1 = buildPackage({ guid: 'cc'.repeat(16), chunks: [1] });
  const other2 = buildPackage({ guid: 'cc'.repeat(16), chunks: [1], seed: 3 });
  return {
    feros, ferosCopy, ilos, ilosCut,
    discs: [
      { name: 'Disc1.iso', files: [entry('Layer0/Maps/BIOA_WAR00.xxx', feros), entry('Layer0/Maps/BIOA_LOS00.xxx', ferosCopy),
        entry('Layer0/Maps/BIOA_NOR10.xxx', other1)] },
      { name: 'Disc2.iso', files: [entry('Layer0/Maps/BIOA_WAR00.xxx', ilosCut), entry('Layer0/Maps/BIOA_LOS00.xxx', ilos),
        entry('Layer0/Maps/BIOA_NOR10.xxx', other2)] },
    ],
  };
}

test('RU merge: Disc 1 fake BIOA_LOS00 (Feros copy, same GUID as WAR00) loses to the real Ilos map of Disc 2', async () => {
  const { feros, ilos, discs } = ruDiscs();
  assert.notEqual(ilos.length, feros.length);
  const { files, decisions, replacedByLater } = await mergeDiscFiles(discs);
  assert.deepEqual(await bytesOf(pick(files, 'Layer0/Maps/BIOA_LOS00.xxx')), ilos);
  assert.deepEqual(await bytesOf(pick(files, 'Layer0/Maps/BIOA_WAR00.xxx')), feros);
  assert.equal(replacedByLater, 1);
  const los = decisions.find((d) => d.startsWith('Layer0/Maps/BIOA_LOS00.xxx'));
  assert.match(los, /^Layer0\/Maps\/BIOA_LOS00\.xxx: Disc2\.iso \(ok\) over Disc1\.iso \(ok, but same package GUID as BIOA_WAR00\.xxx\)$/);
  const war = decisions.find((d) => d.startsWith('Layer0/Maps/BIOA_WAR00.xxx'));
  assert.match(war, /Disc1\.iso \(ok, but same package GUID as BIOA_LOS00\.xxx\) over Disc2\.iso \(bad: chunk 1 needs bytes/);
  // both pass and are unique: the earliest disc wins
  assert.match(decisions.find((d) => d.startsWith('Layer0/Maps/BIOA_NOR10.xxx')), /: Disc1\.iso \(ok\) over Disc2\.iso \(ok\)$/);
  // the merged set is clean
  const audit = await auditPackages(files);
  assert.equal(audit.bad.length, 0);
  assert.equal(audit.duplicates.length, 0);
});

test('merge: a package with a truncated chunk is rejected in favour of a later valid copy', async () => {
  const good = buildPackage({ guid: 7, chunks: [2, 2] });
  const { files, decisions } = await mergeDiscFiles([
    { name: 'Disc1.iso', files: [entry('Layer1/Maps/BIOA_STA00.xxx', cut(good, 100))] },
    { name: 'Disc2.iso', files: [entry('Layer1/Maps/BIOA_STA00.xxx', good)] },
  ]);
  assert.deepEqual(await bytesOf(pick(files, 'Layer1/Maps/BIOA_STA00.xxx')), good);
  assert.match(decisions[0], /Disc2\.iso \(ok\) over Disc1\.iso \(bad: chunk 1 needs bytes/);
});

test('single source audit: the fake LOS00 of Disc 1 and the cut WAR00 of Disc 2 are reported', async () => {
  const { discs } = ruDiscs();
  const d1 = await auditPackages(discs[0].files, { scope: 'maps' });
  assert.equal(d1.bad.length, 0);
  assert.deepEqual(d1.duplicates, [['Layer0/Maps/BIOA_WAR00.xxx', 'Layer0/Maps/BIOA_LOS00.xxx']]);
  assert.match(d1.log.join('\n'), /SAME GUID under different names: Layer0\/Maps\/BIOA_WAR00\.xxx, Layer0\/Maps\/BIOA_LOS00\.xxx/);
  const d2 = await auditPackages(discs[1].files, { scope: 'maps' });
  assert.deepEqual(d2.bad.map((b) => b.path), ['Layer0/Maps/BIOA_WAR00.xxx']);
  // scope 'maps' leaves other packages alone; the cache is filled and reused
  const cache = new Map();
  const files = [...discs[1].files, entry('Layer0/Packages/Junk.upk', new Uint8Array(8))];
  assert.equal((await checkPackages(files, { filter: (p) => p.includes('/Maps/'), cache })).checked, 3);
  assert.equal(cache.size, 3);
  assert.equal((await checkPackages(files, { cache })).bad.length, 2);
});

// Optional: MASSEFFECT_TEST_PACKAGES=/path/game_root (an extracted disc or game_root) compares every package with the
// Python tool. Read-only.
const realDir = process.env.MASSEFFECT_TEST_PACKAGES;
test('JS checker matches tools/check_packages.py on real packages', { skip: !realDir }, async () => {
  const { openSync, readSync, statSync, closeSync } = await import('node:fs');
  const py = spawnSync('python3', [pythonTool, '--json', realDir], { encoding: 'utf8', maxBuffer: 1 << 28 });
  const json = py.stdout.trim().split('\n').filter((l) => l.startsWith('{')).map((l) => JSON.parse(l));
  assert.ok(json.length > 0);
  for (const o of json) {
    const fd = openSync(o.path, 'r');
    const blob = {
      size: statSync(o.path).size,
      slice(a, b) {
        return { async arrayBuffer() { const buf = Buffer.alloc(b - a); readSync(fd, buf, 0, buf.length, a); return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length); } };
      },
    };
    const r = await checkPackage(blob);
    closeSync(fd);
    assert.deepEqual([r.status, r.problems, r.info.guid ?? null], [o.status, o.problems, o.guid ?? null], o.path);
  }
});
