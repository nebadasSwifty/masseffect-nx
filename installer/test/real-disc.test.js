// Optional: runs only when you point it at your own copy of the game. Read-only.
//   MASSEFFECT_TEST_ISO=/path/game.iso  MASSEFFECT_TEST_DISC=/path/extracted-disc  node --test test/
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, statSync, readFileSync, openSync, readSync, closeSync } from 'node:fs';
import { join, relative } from 'node:path';
import { createHash } from 'node:crypto';
import { openXdvdfs } from '../js/xdvdfs.js';

// Node's fs.openAsBlob reports sizes modulo 2^32 for files over 4 GiB, so use a tiny Blob-like adapter
// (what the parser needs: size, slice(a, b), arrayBuffer()).
class FileBlob {
  constructor(path, start = 0, end = statSync(path).size, fd = openSync(path, 'r')) {
    this.path = path; this.start = start; this.end = end; this.fd = fd;
  }
  get size() { return this.end - this.start; }
  slice(a, b = this.size) { return new FileBlob(this.path, this.start + a, this.start + b, this.fd); }
  async arrayBuffer() {
    const buf = Buffer.alloc(this.size);
    assert.equal(readSync(this.fd, buf, 0, buf.length, this.start), buf.length);
    return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length);
  }
}

const iso = process.env.MASSEFFECT_TEST_ISO;
const disc = process.env.MASSEFFECT_TEST_DISC;

function walk(dir, out = []) {
  for (const name of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, name.name);
    if (name.isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}

test('XDVDFS listing and file bytes of a real image match the extracted disc', { skip: !iso || !disc }, async () => {
  const image = new FileBlob(iso);
  after(() => closeSync(image.fd));
  const xiso = await openXdvdfs(image);
  const onDisk = new Map(walk(disc).map((p) => [relative(disc, p).split('\\').join('/'), statSync(p).size]));
  assert.equal(xiso.files.length, onDisk.size);
  for (const f of xiso.files) assert.equal(onDisk.get(f.path), f.size, f.path);
  const xex = xiso.files.find((f) => f.path === 'default.xex');
  const fromIso = createHash('sha256').update(new Uint8Array(await xex.blob.arrayBuffer())).digest('hex');
  assert.equal(fromIso, createHash('sha256').update(readFileSync(join(disc, 'default.xex'))).digest('hex'));
  // a few files from the end of the image (tests offsets beyond 4 GiB)
  for (const f of xiso.files.filter((x) => x.offset > 2 ** 32).slice(0, 3)) {
    const a = new Uint8Array(await f.blob.slice(0, Math.min(f.size, 1 << 20)).arrayBuffer());
    const b = readFileSync(join(disc, f.path)).subarray(0, a.length);
    assert.deepEqual(a, new Uint8Array(b), f.path);
  }
});
