// js/stfs.js on synthetic packages (test/stfs_builder.js); no game data. When python3 is available, the same packages
// also go through tools/stfs_extract.py and both outputs are compared byte for byte.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, rmSync, readdirSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  openStfs, verifyStfs, parseHeader, StfsGeometry, StfsError, aggregateData, headerFileBytes, packageFolderName,
  HEADER_FILE_SIZE,
} from '../js/stfs.js';
import { buildStfs, sampleFiles, pattern } from './stfs_builder.js';

const here = dirname(fileURLToPath(import.meta.url));
const extractor = join(here, '../../tools/stfs_extract.py');
const havePython = spawnSync('python3', ['--version']).status === 0;
const FILE_NAME = 'A1B2C3D4E5F60718293A4B5C6D7E8F9012345678AB'; // 42 characters, like a console's file name

const asFile = (bytes, name = FILE_NAME) => new File([bytes], name);
const bytesOf = async (blob) => new Uint8Array(await blob.arrayBuffer());

test('block math matches tools/stfs_extract.py (read-only and read-write volumes)', () => {
  // [index, offset, L0, L1, L2] computed by stfs_extract.py for header size 0xAD0E.
  const ro = [[0, 49152, 0, 171, 29071], [169, 741376, 0, 171, 29071], [170, 753664, 172, 171, 29071],
    [340, 1454080, 343, 171, 29071], [28899, 119115776, 28900, 171, 29071], [28900, 119132160, 29073, 29072, 29071],
    [57800, 238206976, 58144, 58143, 29071], [97551, 401985536, 97988, 87214, 29071],
    [4913001, 20242784256, 4942072, 4942071, 29071]];
  const rw = [[0, 53248, 0, 172, 29242], [169, 745472, 0, 172, 29242], [170, 765952, 174, 172, 29242],
    [340, 1470464, 346, 172, 29242], [28899, 119816192, 29070, 172, 29242], [28900, 119844864, 29246, 29244, 29242],
    [57800, 239620096, 58488, 58486, 29242], [97551, 404357120, 98566, 87728, 29242],
    [4913001, 20361871360, 4971144, 4971142, 29242]];
  for (const [readOnly, table] of [[true, ro], [false, rw]]) {
    const g = new StfsGeometry({ headerSize: 0xad0e, volume: { readOnly, rootActiveIndex: false, totalBlocks: 0 } });
    for (const [i, off, l0, l1, l2] of table) {
      assert.equal(g.blockToOffset(i), off, `offset of ${i}`);
      assert.deepEqual([g.hashBlockNumber(i, 0), g.hashBlockNumber(i, 1), g.hashBlockNumber(i, 2)], [l0, l1, l2], `hash blocks of ${i}`);
    }
  }
});

test('header fields', () => {
  const { bytes } = buildStfs({
    files: [{ path: 'a.bin', data: pattern(10, 1) }],
    names: { en: 'Bring Down the Synthetic [ENPLES]', pl: 'Polska nazwa' },
    licenses: [{ id: 0xffffffffffffffffn, bits: 1, flags: 0 }, { id: 0xffffffffffffffffn, bits: 4, flags: 1 }],
    contentId: Uint8Array.from({ length: 20 }, (_, i) => i * 11),
  });
  const m = parseHeader(bytes.subarray(0, 0xa000), bytes.length);
  assert.equal(m.magic, 'LIVE');
  assert.equal(m.titleIdHex, '4D5307E8');
  assert.equal(m.contentTypeHex, '00000002');
  assert.equal(m.contentTypeName, 'MarketplaceContent');
  assert.equal(m.metaVersion, 2);
  assert.equal(m.displayName, 'Bring Down the Synthetic [ENPLES]');
  assert.equal(m.displayNames.pl, 'Polska nazwa');
  assert.equal(m.descriptions.en, 'Synthetic description');
  assert.equal(m.titleName, 'Mass Effect');
  assert.equal(m.contentId, '000B16212C37424D58636E79848F9AA5B0BBC6D1');
  assert.equal(m.licenses.length, 2);
  assert.equal(m.licenseMask, 4);
  assert.equal(m.volume.readOnly, true);
  assert.equal(m.volume.fileTableBlockCount, 1);
});

for (const variant of [
  { label: 'read-only', readOnly: true },
  { label: 'read-write, root active index set', readOnly: false, rootActiveIndex: true },
  { label: 'read-write, root active index clear', readOnly: false, rootActiveIndex: false },
]) {
  test(`every file reads back intact (${variant.label})`, async () => {
    const files = sampleFiles();
    const { bytes, totalBlocks } = buildStfs({ files, readOnly: variant.readOnly, rootActiveIndex: variant.rootActiveIndex });
    assert.ok(totalBlocks > 340, 'the sample spans three hash-table groups');
    const pkg = await openStfs(asFile(bytes), { expect: { titleId: 0x4d5307e8, contentType: 2 } });
    assert.equal(pkg.folderName, FILE_NAME);
    assert.equal(pkg.files.length, files.length);
    assert.equal(pkg.meta.volume.fileTableBlockCount, 2, 'two file-table blocks (more than 64 entries)');
    const byPath = new Map(pkg.files.map((f) => [f.path, f]));
    for (const f of files) {
      const got = byPath.get(f.path);
      assert.ok(got, `${f.path} listed`);
      assert.equal(got.size, f.data.length);
      assert.equal(got.blob.size, f.data.length);
      assert.deepEqual(await bytesOf(got.blob), f.data, `${f.path} bytes`);
    }
    assert.ok(pkg.entries.some((e) => e.isDir && e.path === 'Content/Packages/Small'));
    const big = byPath.get('Content/Maps/SYN01/SYN01_big.xxx');
    assert.equal(big.contiguous, true);
    assert.equal(big.runs.length, 2, 'a contiguous file is split only where a hash table sits');
    assert.equal(byPath.get('Content/Packages/frag.xxx').contiguous, false);
    assert.ok(byPath.get('Content/Packages/frag.xxx').runs.length > 10);
    assert.equal(pkg.payloadBytes, files.reduce((a, f) => a + f.data.length, 0));
    const v = await verifyStfs(pkg);
    assert.equal(v.bytes, pkg.payloadBytes);
  });
}

test('a file table spread over non-adjacent blocks is followed through the hash chain', async () => {
  const files = sampleFiles().slice(0, 40).concat(Array.from({ length: 40 }, (_, i) => ({ path: `X/f${i}`, data: pattern(5, i) })));
  const { bytes } = buildStfs({ files, fileTableBlocks: [3, 0] });
  const pkg = await openStfs(asFile(bytes));
  assert.equal(pkg.files.length, files.length);
  for (const f of files) assert.deepEqual(await bytesOf(pkg.files.find((x) => x.path === f.path).blob), f.data);
});

test('verification finds a damaged block', async () => {
  const files = sampleFiles();
  const { bytes } = buildStfs({ files });
  const good = await openStfs(asFile(bytes));
  const frag = good.files.find((f) => f.path === 'Content/Packages/frag.xxx');
  const damaged = bytes.slice();
  damaged[frag.runs[3][0] + 100] ^= 0x55;
  const pkg = await openStfs(asFile(damaged));
  await assert.rejects(verifyStfs(pkg), (e) => e instanceof StfsError && /frag\.xxx: SHA-1 mismatch in block \d+/.test(e.message));
});

test('a contiguous flag that disagrees with the hash chain is caught by verification', async () => {
  const { bytes } = buildStfs({ files: [{ path: 'f.bin', data: pattern(5 * 4096, 1), fragmented: true }] });
  const pkg = await openStfs(asFile(bytes));
  const e = pkg.files[0];
  // pretend the file table said "contiguous": the reader would take blocks start, start+1, ... instead of the chain
  e.contiguous = true;
  e.runs = await pkg.reader.fileRuns(e);
  await assert.rejects(verifyStfs(pkg), StfsError);
});

test('packages of another game, another content type, SVOD and non-packages are rejected', async () => {
  const files = [{ path: 'a.bin', data: pattern(100, 1) }];
  const expect = { titleId: 0x4d5307e8, contentType: 2 };
  await assert.rejects(openStfs(asFile(buildStfs({ files, titleId: 0x4d5307d5 }).bytes), { expect }),
    (e) => e instanceof StfsError && /another game \(title 4D5307D5, expected 4D5307E8\)/.test(e.message));
  await assert.rejects(openStfs(asFile(buildStfs({ files, contentType: 0x000b0000 }).bytes), { expect }),
    (e) => e instanceof StfsError && /not downloadable content \(content type 000B0000 GameTitle/.test(e.message));
  await assert.rejects(openStfs(asFile(buildStfs({ files, contentType: 1 }).bytes), { expect }), /SavedGame/);
  await assert.rejects(openStfs(asFile(buildStfs({ files, volumeType: 1 }).bytes), { expect }), /SVOD/);
  await assert.rejects(openStfs(asFile(new Uint8Array(0x4000))), /not an Xbox 360 content package/);
  await assert.rejects(openStfs(asFile(new Uint8Array(16))), /too small/);
  for (const magic of ['CON ', 'PIRS']) {
    const pkg = await openStfs(asFile(buildStfs({ files, magic }).bytes), { expect });
    assert.equal(pkg.meta.magic, magic.trim());
  }
});

test('truncated packages and unsafe names are rejected', async () => {
  const { bytes } = buildStfs({ files: sampleFiles() });
  await assert.rejects(openStfs(asFile(bytes.subarray(0, bytes.length - 3 * 4096))), StfsError);
  const evil = buildStfs({ files: [{ path: 'Content/../../x.bin', data: pattern(10, 1) }] });
  await assert.rejects(openStfs(asFile(evil.bytes)), /unsafe file name/);
});

test('the .header file: aggregate data and license mask', async () => {
  const { bytes } = buildStfs({ files: [{ path: 'a.bin', data: pattern(10, 1) }], names: { en: 'Pinnacle Synthetic [ENPLES]' } });
  const pkg = await openStfs(asFile(bytes));
  const h = pkg.header;
  assert.equal(h.length, HEADER_FILE_SIZE);
  assert.equal(h.length, 332);
  const dv = new DataView(h.buffer);
  assert.equal(dv.getUint32(0, false), 1, 'device id');
  assert.equal(dv.getUint32(4, false), 2, 'content type');
  assert.equal(new TextDecoder('utf-16be').decode(h.subarray(8, 8 + 27 * 2)), 'Pinnacle Synthetic [ENPLES]');
  assert.equal(new TextDecoder().decode(h.subarray(0x108, 0x108 + 42)), FILE_NAME);
  assert.equal(dv.getBigUint64(0x138, false), 0n, 'xuid 0 (shared content)');
  assert.equal(dv.getUint32(0x140, false), 0x4d5307e8, 'title id');
  assert.equal(dv.getUint32(0x148, true), 0xffffffff, 'license mask, little-endian');
  assert.deepEqual(headerFileBytes(pkg.meta, FILE_NAME, 5).subarray(0x148), Uint8Array.of(5, 0, 0, 0));
  // no display name: the file name is used
  const noName = { ...pkg.meta, displayNames: { en: '' } };
  assert.equal(new TextDecoder('utf-16be').decode(aggregateData(noName, 'ABC').subarray(8, 14)), 'ABC');
});

test('folder name: the package file name, or the content id when the name is unusable', () => {
  const meta = { contentId: 'C823528729EC4E1A8A21D5EBB89F5B8245E986A4' };
  assert.equal(packageFolderName('A275890E35D31622A6AB8D43C53F932A0342DEF64D', meta), 'A275890E35D31622A6AB8D43C53F932A0342DEF64D');
  assert.equal(packageFolderName('A275890E35D31622A6AB8D43C53F932A0342DEF64D (1)', meta), meta.contentId);
  assert.equal(packageFolderName('dlc ü.bin', meta), meta.contentId);
  assert.equal(packageFolderName('x'.repeat(43), meta), meta.contentId);
  assert.equal(packageFolderName('', meta), meta.contentId);
});

test('same output as tools/stfs_extract.py, byte for byte', { skip: havePython ? false : 'python3 not found' }, async () => {
  const tmp = mkdtempSync(join(tmpdir(), 'stfs-'));
  try {
    for (const [i, opts] of [{ readOnly: true }, { readOnly: false, rootActiveIndex: true }].entries()) {
      const name = `${FILE_NAME.slice(0, 41)}${i}`;
      const { bytes } = buildStfs({ files: sampleFiles(), ...opts });
      const pkgPath = join(tmp, name);
      writeFileSync(pkgPath, bytes);
      const root = join(tmp, `out${i}`);
      const r = spawnSync('python3', ['-I', extractor, 'extract', '--verify', '--content-root', root, pkgPath], { encoding: 'utf8' });
      assert.equal(r.status, 0, r.stderr);
      const pkg = await openStfs(asFile(bytes, name));
      const titleDir = join(root, '0000000000000000', '4D5307E8');
      assert.deepEqual(readFileSync(join(titleDir, 'Headers', '00000002', `${name}.header`)), Buffer.from(pkg.header));
      const dest = join(titleDir, '00000002', name);
      const pyFiles = [];
      const walk = (d, pre) => {
        for (const n of readdirSync(d)) {
          const p = join(d, n);
          if (statSync(p).isDirectory()) walk(p, `${pre}${n}/`); else pyFiles.push(`${pre}${n}`);
        }
      };
      walk(dest, '');
      assert.deepEqual(pyFiles.sort(), pkg.files.map((f) => f.path).sort());
      for (const f of pkg.files) assert.deepEqual(readFileSync(join(dest, f.path)), Buffer.from(await bytesOf(f.blob)), f.path);
    }
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
});
