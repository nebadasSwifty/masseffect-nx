import test from 'node:test';
import assert from 'node:assert/strict';
import { CONFIG } from '../config.js';
import { findDiscRoot, planGameFiles, scanCandidates, findEdition, formatBytes, formatDuration } from '../js/plan.js';

const f = (path, size = 10) => ({ path, size });

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
  const e = CONFIG.editions[0];
  assert.equal(findEdition(CONFIG.editions, e.xexSha256.toUpperCase()), e);
  assert.equal(findEdition(CONFIG.editions, '00'.repeat(32)), null);
});

test('the config is self-consistent', () => {
  assert.equal(CONFIG.files.shadersIndex, `${CONFIG.files.shaders}.idx`);
  for (const e of CONFIG.editions) assert.match(e.xexSha256, /^[0-9a-f]{64}$/);
  assert.ok(CONFIG.wasm.extraFiles.every((n) => /\.wasm$/.test(n)));
});

test('formatting helpers', () => {
  assert.equal(formatBytes(1536), '1.50 KB');
  assert.equal(formatBytes(7 * 2 ** 30), '7.00 GB');
  assert.equal(formatDuration(125), '2 min 05 s');
});
