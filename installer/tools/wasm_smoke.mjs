// Deployment gate: run the actual WASM compiler and check the SPIR-V output.
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import assert from 'node:assert/strict';
const dir = resolve(process.argv[2]);
for (const [file, factory] of [['scan', 'createScanModule'], ['hlsl', 'createHlslModule'], ['pack', 'createPackModule'], ['dxc_web', 'createDxcModule']]) {
  const mod = await import(pathToFileURL(resolve(dir, `${file}.mjs`)));
  const m = await (mod.default ?? mod[factory])({ print() {}, printErr: console.error });
  assert.ok(m.FS, `${file}: missing FS`);
  if (file !== 'dxc_web') {
    assert.equal(typeof m.callMain, 'function', `${file}: missing callMain`);
    continue;
  }
  for (const vertex of [0, 1]) {
    m.FS.writeFile('/smoke.hlsl', vertex
      ? 'float4 main(uint id : SV_VertexID) : SV_Position { return float4(float(id), 0, 0, 1); }'
      : 'float4 main() : SV_Target0 { return float4(1, 0, 0, 1); }');
    assert.equal(m.ccall('compile', 'number', ['string', 'string', 'number'], ['/smoke.hlsl', '/smoke.spv', vertex]), 0);
    const bytes = m.FS.readFile('/smoke.spv');
    assert.ok(bytes.length > 20);
    assert.equal(new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(0, true), 0x07230203);
  }
}
console.log('All WASM modules load; DXC compiles vertex and pixel shaders to SPIR-V.');
