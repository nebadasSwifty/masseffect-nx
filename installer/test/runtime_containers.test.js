import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { CONFIG } from '../config.js';
import { checkToolchain, wasmUrls } from '../js/pipeline.js';

const here = dirname(fileURLToPath(import.meta.url));

test('runtime_containers.json exists, is valid JSON, and contains valid containers', () => {
  const jsonPath = join(here, '../wasm/runtime_containers.json');
  assert.ok(existsSync(jsonPath), 'runtime_containers.json must exist in installer/wasm/');

  const data = JSON.parse(readFileSync(jsonPath, 'utf8'));
  const entries = Object.entries(data);
  assert.ok(entries.length >= 280, `expected at least 280 runtime containers, got ${entries.length}`);

  // Critical UI and Scaleform shaders must be present
  const keys = Object.keys(data);
  const critical = [
    'ps_706f57115bb33790.bin', // D3D immediate mode UI
    'ps_7ca16a6f1e83b4ed.bin', // Scaleform UI
    'ps_259080e95e4e6ee0.bin', // Scaleform UI
    'ps_6c85c130eb1e02fe.bin',
    'ps_859a5a7e2388806c.bin',
    'ps_1c5217e55088b28a.bin',
    'ps_7247403865c742de.bin',
    'vs_a8e96f33705777a3.bin',
    'vs_3764e952b138617e.bin',
    'vs_b3ec65f15de773dd.bin',
    'vs_27d6b834855f7771.bin',
    'vs_a620a3a39a79d16f.bin',
    'vs_c503a9add0b6641d.bin',
  ];

  for (const name of critical) {
    assert.ok(keys.includes(name), `Missing critical runtime shader container: ${name}`);
  }

  // Validate container header format (0x102A11xx 2008 layout)
  for (const [name, b64] of entries) {
    const buf = Buffer.from(b64, 'base64');
    assert.ok(buf.length >= 24, `${name} container too small`);
    const sig = buf.readUInt32BE(0);
    assert.equal(sig & 0xFFFFFF00, 0x102A1100, `${name} invalid 2008 container signature: 0x${sig.toString(16)}`);
    const isVertex = (sig & 1) === 1;
    assert.equal(name.startsWith('vs_'), isVertex, `${name} stage flag mismatch with file prefix`);
  }
});

test('checkToolchain includes runtime_containers.json and wasmUrls exposes it', async () => {
  const urls = wasmUrls(CONFIG, 'https://example.com/installer/');
  assert.equal(urls.runtimeContainers, 'https://example.com/installer/wasm/runtime_containers.json');

  const files = new Set([
    'wasm/scan.mjs', 'wasm/hlsl.mjs', 'wasm/dxc_web.mjs', 'wasm/pack.mjs', 'wasm/shader_common.h',
    'wasm/scan.wasm', 'wasm/hlsl.wasm', 'wasm/dxc_web.wasm', 'wasm/pack.wasm', 'wasm/runtime_containers.json',
  ]);

  const fakeFetch = async (url) => {
    const rel = new URL(url).pathname.replace(/^\/installer\//, '');
    return { ok: files.has(rel) };
  };

  const missing = await checkToolchain(CONFIG, 'https://example.com/installer/', fakeFetch);
  assert.deepEqual(missing, []);

  // When runtime_containers.json is missing, checkToolchain must report it
  files.delete('wasm/runtime_containers.json');
  const missing2 = await checkToolchain(CONFIG, 'https://example.com/installer/', fakeFetch);
  assert.deepEqual(missing2, ['wasm/runtime_containers.json']);
});
