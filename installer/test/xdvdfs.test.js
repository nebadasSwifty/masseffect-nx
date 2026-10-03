import test from 'node:test';
import assert from 'node:assert/strict';
import { openXdvdfs, findPartition, XdvdfsError, PARTITION_OFFSETS } from '../js/xdvdfs.js';
import { buildXiso, pattern } from './helpers.js';

async function bytesOf(file) {
  return new Uint8Array(await file.blob.arrayBuffer());
}

const tree = () => ({
  'default.xex': pattern(5000, 1),
  Layer0: {
    MEInit: { 'Coalesced.ini': pattern(3000, 2), 'Core.xxx': pattern(2048, 3), 'Empty.bin': new Uint8Array(0) },
    Maps: { 'BIOA_STA00.xxx': pattern(9000, 4), 'a.xxx': pattern(1, 5) },
  },
  Layer1: { 'Splash.bmp': pattern(4097, 6), EmptyDir: {} },
  FillerFiles: Object.fromEntries(Array.from({ length: 40 }, (_, i) => [`f${String(i).padStart(2, '0')}.jnk`, pattern(10 + i, 10 + i)])),
});

test('lists and reads every file of a plain image', async () => {
  const img = buildXiso(tree());
  const xiso = await openXdvdfs(new Blob([img]));
  assert.equal(xiso.partitionOffset, 0);
  const paths = xiso.files.map((f) => f.path);
  assert.ok(paths.includes('default.xex'));
  assert.ok(paths.includes('Layer0/MEInit/Coalesced.ini'));
  assert.ok(paths.includes('Layer0/MEInit/Empty.bin'));
  assert.ok(!paths.some((p) => p.includes('EmptyDir')));
  assert.equal(xiso.files.length, 1 + 3 + 2 + 1 + 40);
  assert.deepEqual(paths, [...paths].sort());
  const t = tree();
  const get = (p) => xiso.files.find((f) => f.path === p);
  assert.deepEqual(await bytesOf(get('default.xex')), t['default.xex']);
  assert.deepEqual(await bytesOf(get('Layer0/Maps/BIOA_STA00.xxx')), t.Layer0.Maps['BIOA_STA00.xxx']);
  assert.deepEqual(await bytesOf(get('Layer1/Splash.bmp')), t.Layer1['Splash.bmp']);
  assert.equal(get('Layer0/MEInit/Empty.bin').size, 0);
  assert.deepEqual(await bytesOf(get('FillerFiles/f17.jnk')), t.FillerFiles['f17.jnk']);
});

for (const base of PARTITION_OFFSETS.slice(1)) {
  test(`finds the game partition at 0x${base.toString(16)}`, async () => {
    // sparse: only allocate what is needed (the offsets reach 400 MB, so build the tail and prepend zeros lazily)
    const small = { 'default.xex': pattern(3000, 9), Layer0: { 'x.xxx': pattern(5000, 8) } };
    const part = buildXiso(small, 0);
    const blob = new Blob([new Uint8Array(base), part]); // Blob parts are not copied into one big buffer
    assert.equal(await findPartition(blob), base);
    const xiso = await openXdvdfs(blob);
    assert.equal(xiso.partitionOffset, base);
    assert.deepEqual(await bytesOf(xiso.files.find((f) => f.path === 'Layer0/x.xxx')), small.Layer0['x.xxx']);
    assert.equal(xiso.files.find((f) => f.path === 'default.xex').offset % 2048, 0);
  });
}

test('rejects a file that is not a disc image', async () => {
  await assert.rejects(openXdvdfs(new Blob([pattern(300000, 3)])), XdvdfsError);
  await assert.rejects(openXdvdfs(new Blob([new Uint8Array(10)])), /not an Xbox 360 disc image/);
});

test('rejects a truncated image', async () => {
  const img = buildXiso({ 'big.bin': pattern(100000, 1) });
  await assert.rejects(openXdvdfs(new Blob([img.subarray(0, img.length - 50000)])), /beyond the end|past the end/);
});

test('rejects a directory tree that loops', async () => {
  const img = buildXiso({ a: pattern(10), b: pattern(10), c: pattern(10) });
  // find the root table through the volume descriptor and point the root entry's right link back to itself
  const dv = new DataView(img.buffer);
  const rootSector = dv.getUint32(0x10000 + 20, true);
  dv.setUint16(rootSector * 2048 + 2, 0, true); // right = 0 means none; make left point to the root (unit 0)... use a cycle:
  // entries are in pre-order: root at 0, left child at unit (14+1+3 &~3)/4 = 4 (name length 1 -> 16 bytes).
  dv.setUint16(rootSector * 2048 + 16 + 2, 0, true);
  dv.setUint16(rootSector * 2048, 4, true);
  dv.setUint16(rootSector * 2048 + 16, 4, true); // child's left points to itself
  await assert.rejects(openXdvdfs(new Blob([img])), /loop|outside/);
});

test('refuses unsafe names (zip-slip)', async () => {
  const img = buildXiso({ 'ok.bin': pattern(5), '..': pattern(5) });
  await assert.rejects(openXdvdfs(new Blob([img])), /unsafe file name/);
});
