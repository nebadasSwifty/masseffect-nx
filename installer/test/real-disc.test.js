// Optional: runs only when you point it at your own copy of the game. Read-only.
//   MASSEFFECT_TEST_ISO=/path/game.iso  MASSEFFECT_TEST_DISC=/path/extracted-disc  node --test test/
//   MASSEFFECT_TEST_RU_DISC1=/path/Disc1.iso  MASSEFFECT_TEST_RU_DISC2=/path/Disc2.iso  (RU two-disc merge)
import test, { after } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, statSync, readFileSync, openSync, readSync, closeSync } from 'node:fs';
import { join, relative } from 'node:path';
import { createHash } from 'node:crypto';
import { openXdvdfs } from '../js/xdvdfs.js';
import { mergeDiscFiles, auditPackages, knownBadFor } from '../js/source.js';
import { CONFIG } from '../config.js';

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

const ru1 = process.env.MASSEFFECT_TEST_RU_DISC1;
const ru2 = process.env.MASSEFFECT_TEST_RU_DISC2;
test('RU two-disc merge takes Feros WAR00 from Disc 1 and Ilos LOS00 from Disc 2, result passes the package check', { skip: !ru1 || !ru2 }, async () => {
  const images = [new FileBlob(ru1), new FileBlob(ru2)];
  after(() => images.forEach((i) => closeSync(i.fd)));
  const discs = [];
  for (const [i, image] of images.entries()) discs.push({ name: `Disc${i + 1}.iso`, files: (await openXdvdfs(image)).files });
  const { files, decisions, checks } = await mergeDiscFiles(discs);
  const from = (path) => decisions.find((d) => d.startsWith(`${path}: `))?.split(': ')[1].split(' ')[0];
  assert.equal(from('Layer0/Maps/BIOA_WAR00.xxx'), 'Disc1.iso');
  assert.equal(from('Layer0/Maps/BIOA_LOS00.xxx'), 'Disc2.iso');
  const audit = await auditPackages(files, { cache: checks, known: knownBadFor(CONFIG.disc.packageCheck, 'rus-rev0') });
  assert.deepEqual(audit.bad, []);
  assert.deepEqual(audit.duplicates, []);
  // GlobalTlk_ES.xxx is junk on both RU discs and unused: a note, not a finding.
  assert.deepEqual(audit.known.map((k) => k.path), ['Layer0/MEInit/GlobalTlk_ES.xxx']);
  // Feros and Ilos are different packages, and every Feros/Ilos map of the result passed.
  const guid = (p) => checks.get(files.find((f) => f.path === p).blob)?.info.guid;
  assert.notEqual(guid('Layer0/Maps/BIOA_LOS00.xxx'), guid('Layer0/Maps/BIOA_WAR00.xxx'));
  const maps = files.filter((f) => /\/Maps\/BIOA_(WAR|LOS)[^/]*\.xxx$/i.test(f.path));
  assert.ok(maps.length > 20, String(maps.length));
  for (const f of maps) assert.equal(audit.results.get(f.path)?.status, 'ok', f.path);
});
