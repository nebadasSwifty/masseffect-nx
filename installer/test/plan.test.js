import test from 'node:test';
import assert from 'node:assert/strict';
import { CONFIG } from '../config.js';
import { findDiscRoot, planGameFiles, scanCandidates, findEdition, formatBytes, formatDuration, parseXexHeader, matchEditionHeader, identifyEdition } from '../js/plan.js';

const f = (path, size = 10) => ({ path, size });

function makeMockXex({ magic = 0x58455832, titleId = 0x4D5307E8, mediaId = 0x572BA75D, version = 5, entryPoint = 0x82812A00, imageSize = 16515072 } = {}) {
  const buf = new Uint8Array(0x200);
  const view = new DataView(buf.buffer);
  view.setUint32(0x00, magic, false);
  view.setUint32(0x10, 0x100, false); // security offset
  view.setUint32(0x14, 2, false); // 2 optional headers

  // opt 0: execution info at 0x80
  view.setUint32(0x18, 0x00040006, false);
  view.setUint32(0x1C, 0x80, false);

  // opt 1: entry point
  view.setUint32(0x20, 0x00010100, false);
  view.setUint32(0x24, entryPoint, false);

  // execution info at 0x80: mediaId, version, base_version, titleId
  view.setUint32(0x80, mediaId, false);
  view.setUint32(0x84, version, false);
  view.setUint32(0x88, 0, false);
  view.setUint32(0x8C, titleId, false);

  // security info at 0x100: imageSize at 0x100 + 4
  view.setUint32(0x104, imageSize, false);
  return buf;
}

test('findDiscRoot re-roots a folder pick at the shallowest default.xex', () => {
  const picked = [f('Mass Effect/Default.XEX'), f('Mass Effect/Layer0/a.xxx'), f('Mass Effect/sub/deep/default.xex'), f('Other/readme.txt')];
  const r = findDiscRoot(picked, 'default.xex');
  assert.equal(r.prefix, 'Mass Effect');
  assert.deepEqual(r.files.map((x) => x.path), ['Default.XEX', 'Layer0/a.xxx', 'sub/deep/default.xex']);
  assert.equal(findDiscRoot([f('a/b.txt')], 'default.xex'), null);
  assert.equal(findDiscRoot([f('default.xex'), f('x')], 'default.xex').prefix, '');
});

test('the copy plan leaves out exactly the configured paths (case-insensitive) and nothing else', () => {
  const files = [f('default.xex'), f('Layer0/MEInit/Coalesced.ini'), f('Layer1/Splash.bmp'), f('Layer0/Movies/x.bik'), f('Layer1/ISACT/y.isb'),
    f('$SystemUpdate/system.manifest', 5), f('$systemupdate/su20076000_00000000', 7), f('FillerFiles/a.jnk', 3), f('nxeart', 100)];
  const plan = planGameFiles(files, CONFIG.disc);
  assert.deepEqual(plan.copy.map((x) => x.path), ['default.xex', 'Layer0/MEInit/Coalesced.ini', 'Layer1/Splash.bmp', 'Layer0/Movies/x.bik', 'Layer1/ISACT/y.isb']);
  assert.equal(plan.skipped.length, 4);
  assert.equal(plan.skippedBytes, 115);
});

test('scan candidates are the Unreal packages', () => {
  const c = scanCandidates([f('Layer0/MEInit/Core.xxx'), f('Layer0/MEInit/Coalesced.ini'), f('Layer1/ISACT/a.isb'), f('Layer0/Maps/B.XXX')], CONFIG.disc);
  assert.deepEqual(c.map((x) => x.path), ['Layer0/MEInit/Core.xxx', 'Layer0/Maps/B.XXX']);
});

test('edition lookup by default.xex hash', () => {
  const e0 = CONFIG.editions[0];
  assert.equal(findEdition(CONFIG.editions, e0.xexSha256.toUpperCase()), e0);

  const e1 = CONFIG.editions.find((e) => e.id === 'rus-rev0');
  assert.ok(e1);
  for (const h of e1.xexSha256) {
    assert.equal(findEdition(CONFIG.editions, h.toUpperCase()), e1);
    assert.equal(findEdition(CONFIG.editions, h.toLowerCase()), e1);
  }

  assert.equal(findEdition(CONFIG.editions, '00'.repeat(32)), null);
});

test('edition identification with header fallback and error handling', () => {
  const eEng = CONFIG.editions[0];
  const eRus = CONFIG.editions.find((e) => e.id === 'rus-rev0');

  // 1. Known hash returns exact match
  const resExact = identifyEdition(CONFIG.editions, eRus.xexSha256[0]);
  assert.equal(resExact.matchType, 'exact');
  assert.equal(resExact.edition, eRus);

  // 2. Unknown hash with matching Russian header returns unverified variant
  const rusBytes = makeMockXex({
    titleId: eRus.header.titleId,
    mediaId: eRus.header.mediaId,
    version: eRus.header.version,
    entryPoint: eRus.header.entryPoint,
    imageSize: eRus.header.imageSize,
  });
  const resUnverified = identifyEdition(CONFIG.editions, 'ab'.repeat(32), rusBytes);
  assert.equal(resUnverified.matchType, 'unverified_header');
  assert.equal(resUnverified.edition, eRus);
  assert.equal(resUnverified.header.mediaId, 0x572BA75D);

  // 3. Unknown hash with matching English header
  const engBytes = makeMockXex({
    titleId: eEng.header.titleId,
    mediaId: eEng.header.mediaId,
    version: eEng.header.version,
    entryPoint: eEng.header.entryPoint,
    imageSize: eEng.header.imageSize,
  });
  const resEngUnverified = identifyEdition(CONFIG.editions, 'cd'.repeat(32), engBytes);
  assert.equal(resEngUnverified.matchType, 'unverified_header');
  assert.equal(resEngUnverified.edition, eEng);

  // 4. Unknown hash with a foreign header must refuse
  const foreignBytes = makeMockXex({ titleId: 0x12345678, mediaId: 0x99999999 });
  const resForeign = identifyEdition(CONFIG.editions, 'fe'.repeat(32), foreignBytes);
  assert.equal(resForeign.matchType, 'foreign_header');
  assert.equal(resForeign.edition, null);

  // 5. Truncated or invalid XEX gives a clear error
  const invalidMagic = makeMockXex({ magic: 0x11223344 });
  const resInvalid = identifyEdition(CONFIG.editions, 'ef'.repeat(32), invalidMagic);
  assert.equal(resInvalid.matchType, 'invalid');
  assert.equal(resInvalid.edition, null);
  assert.match(resInvalid.error, /XEX2/);

  const truncated = new Uint8Array(10);
  const resTrunc = identifyEdition(CONFIG.editions, 'ef'.repeat(32), truncated);
  assert.equal(resTrunc.matchType, 'invalid');
  assert.equal(resTrunc.edition, null);
  assert.match(resTrunc.error, /too small/);
});

test('the config is self-consistent', () => {
  assert.equal(CONFIG.files.shadersIndex, `${CONFIG.files.shaders}.idx`);
  for (const e of CONFIG.editions) {
    const hashes = Array.isArray(e.xexSha256) ? e.xexSha256 : [e.xexSha256];
    for (const h of hashes) {
      assert.match(h, /^[0-9a-f]{64}$/);
    }
    assert.ok(e.header);
    assert.equal(typeof e.header.titleId, 'number');
    assert.equal(typeof e.header.mediaId, 'number');
    assert.equal(typeof e.header.version, 'number');
    assert.equal(typeof e.header.entryPoint, 'number');
  }
  assert.ok(CONFIG.wasm.extraFiles.every((n) => /\.wasm$/.test(n)));
});

test('formatting helpers', () => {
  assert.equal(formatBytes(1536), '1.50 KB');
  assert.equal(formatBytes(7 * 2 ** 30), '7.00 GB');
  assert.equal(formatDuration(125), '2 min 05 s');
});
