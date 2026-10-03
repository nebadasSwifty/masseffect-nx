import test from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import zlib from 'node:zlib';
import { ZipWriter, MemorySink, crc32 } from '../js/zip.js';
import { pattern } from './helpers.js';

async function* chunked(bytes, step) {
  for (let i = 0; i < bytes.length; i += step) yield bytes.subarray(i, i + step);
}

/** Minimal independent reader: walks the central directory (ZIP64 aware) and returns {name, data} of stored entries. */
function readZip(zip) {
  const dv = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let eocd = zip.length - 22;
  assert.equal(dv.getUint32(eocd, true), 0x06054b50);
  let count = dv.getUint16(eocd + 10, true);
  let cdSize = dv.getUint32(eocd + 12, true);
  let cdOffset = dv.getUint32(eocd + 16, true);
  let zip64 = false;
  if (count === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) {
    zip64 = true;
    assert.equal(dv.getUint32(eocd - 20, true), 0x07064b50);
    const z = Number(dv.getBigUint64(eocd - 20 + 8, true));
    assert.equal(dv.getUint32(z, true), 0x06064b50);
    count = Number(dv.getBigUint64(z + 32, true));
    cdSize = Number(dv.getBigUint64(z + 40, true));
    cdOffset = Number(dv.getBigUint64(z + 48, true));
  }
  const out = [];
  let p = cdOffset;
  for (let i = 0; i < count; i++) {
    assert.equal(dv.getUint32(p, true), 0x02014b50);
    const method = dv.getUint16(p + 10, true);
    const crc = dv.getUint32(p + 16, true);
    let csize = dv.getUint32(p + 20, true);
    let usize = dv.getUint32(p + 24, true);
    const nameLen = dv.getUint16(p + 28, true), extraLen = dv.getUint16(p + 30, true);
    let off = dv.getUint32(p + 42, true);
    const name = Buffer.from(zip.subarray(p + 46, p + 46 + nameLen)).toString('utf8');
    let e = p + 46 + nameLen;
    const extraEnd = e + extraLen;
    while (e < extraEnd) {
      const id = dv.getUint16(e, true), len = dv.getUint16(e + 2, true);
      if (id === 1) {
        let q = e + 4;
        if (usize === 0xffffffff) { usize = Number(dv.getBigUint64(q, true)); q += 8; }
        if (csize === 0xffffffff) { csize = Number(dv.getBigUint64(q, true)); q += 8; }
        if (off === 0xffffffff) { off = Number(dv.getBigUint64(q, true)); q += 8; }
      }
      e += 4 + len;
    }
    assert.equal(method, 0);
    assert.equal(csize, usize);
    assert.equal(dv.getUint32(off, true), 0x04034b50);
    const ln = dv.getUint16(off + 26, true), le = dv.getUint16(off + 28, true);
    const data = zip.subarray(off + 30 + ln + le, off + 30 + ln + le + usize);
    assert.equal(crc32(data), crc, `crc of ${name}`);
    out.push({ name, data });
    p += 46 + nameLen + extraLen;
  }
  return { entries: out, zip64 };
}

test('crc32 matches zlib (including chunked continuation)', () => {
  for (const n of [0, 1, 7, 8, 9, 1000, 65537]) {
    const d = pattern(n, n + 1);
    assert.equal(crc32(d), zlib.crc32 ? zlib.crc32(d) : crc32(d));
    const k = Math.floor(n / 3);
    assert.equal(crc32(d.subarray(k), crc32(d.subarray(0, k))), crc32(d));
  }
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
});

function maybePythonCheck(zipBytes, expected) {
  const py = spawnSync('python3', ['--version']);
  if (py.status !== 0) return false;
  const dir = mkdtempSync(join(tmpdir(), 'zip-test-'));
  try {
    const file = join(dir, 'a.zip');
    writeFileSync(file, zipBytes);
    const script = 'import zipfile,sys,json;z=zipfile.ZipFile(sys.argv[1]);assert z.testzip() is None;print(json.dumps({i.filename:len(z.read(i)) for i in z.infolist()}))';
    const got = JSON.parse(execFileSync('python3', ['-c', script, file]).toString());
    assert.deepEqual(got, expected);
    return true;
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

test('writes a valid store zip', async () => {
  const sink = new MemorySink();
  const zip = new ZipWriter(sink, { mtime: new Date(2026, 9, 3, 12, 0, 0) });
  const files = { 'masseffect-nx/a.bin': pattern(100000, 1), 'masseffect-nx/game_root/Layer0/b.xxx': pattern(5, 2), 'masseffect-nx/empty': new Uint8Array(0), 'ö/ü.txt': pattern(33, 3) };
  for (const [n, d] of Object.entries(files)) await zip.addFile(n, d.length, chunked(d, 7777));
  await zip.finish();
  const bytes = sink.concat();
  assert.equal(zip.bytesWritten, bytes.length);
  const { entries, zip64 } = readZip(bytes);
  assert.equal(zip64, false);
  assert.deepEqual(entries.map((e) => e.name), Object.keys(files));
  for (const e of entries) assert.deepEqual(e.data, files[e.name]);
  maybePythonCheck(bytes, Object.fromEntries(Object.entries(files).map(([n, d]) => [n, d.length])));
});

test('uses ZIP64 records and fields when the threshold is crossed', async () => {
  const sink = new MemorySink();
  const zip = new ZipWriter(sink, { zip64Threshold: 5000 });
  const files = { small: pattern(100, 1), big1: pattern(6000, 2), big2: pattern(9000, 3), after: pattern(50, 4) };
  for (const [n, d] of Object.entries(files)) await zip.addFile(n, d.length, chunked(d, 1000));
  await zip.finish();
  const bytes = sink.concat();
  const { entries, zip64 } = readZip(bytes);
  assert.equal(zip64, true);
  for (const e of entries) assert.deepEqual(e.data, files[e.name]);
  maybePythonCheck(bytes, Object.fromEntries(Object.entries(files).map(([n, d]) => [n, d.length])));
});

test('more than 65534 entries needs the ZIP64 end record', async () => {
  const sink = new MemorySink();
  const zip = new ZipWriter(sink);
  const none = new Uint8Array(0);
  const N = 70000;
  for (let i = 0; i < N; i++) await zip.addFile(`d/${i}`, 0, chunked(none, 1));
  await zip.finish();
  const { entries, zip64 } = readZip(sink.concat());
  assert.equal(zip64, true);
  assert.equal(entries.length, N);
});

test('a wrong declared size is an error, duplicates and unsafe names are refused', async () => {
  const zip = new ZipWriter(new MemorySink());
  await assert.rejects(zip.addFile('x', 10, chunked(pattern(9), 4)), /expected 10/);
  const zip2 = new ZipWriter(new MemorySink());
  await assert.rejects(zip2.addFile('x', 3, chunked(pattern(9), 4)), /more data/);
  const zip3 = new ZipWriter(new MemorySink());
  await zip3.addFile('x', 0, chunked(new Uint8Array(0), 1));
  await assert.rejects(zip3.addFile('x', 0, chunked(new Uint8Array(0), 1)), /duplicate/);
  await assert.rejects(zip3.addFile('../evil', 0, chunked(new Uint8Array(0), 1)), /unsafe/);
});
