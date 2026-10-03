// Test double for dxc_web.mjs: same interface (FS + ccall('compile')), but runs the native DXC with the exact options
// of shaders/tools/compile_spirv_one.sh. Lets the Node tests exercise the real translator/packer WebAssembly and the
// installer's worker logic without the (unbuilt) DXC WebAssembly module.
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

export default async function createDxcModule(options = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'dxc-shim-'));
  const files = new Map();
  const real = (p) => join(dir, p.replace(/\//g, '_'));
  const dxc = process.env.DXC || 'dxc';
  return {
    FS: {
      writeFile: (p, d) => writeFileSync(real(p), d),
      readFile: (p) => new Uint8Array(readFileSync(real(p))),
      unlink: (p) => rmSync(real(p), { force: true }),
    },
    ccall(name, ret, types, [input, output, vertex]) {
      const args = ['-spirv', '-T', vertex ? 'vs_6_6' : 'ps_6_6', '-E', 'main', '-HV', '2021', '-fspv-target-env=vulkan1.2',
        '-fvk-use-dx-layout', '-Werror=parameter-usage'];
      if (vertex) args.push('-fvk-invert-y');
      args.push('-Fo', real(output), real(input));
      const r = spawnSync(dxc, args, { encoding: 'utf8' });
      if (r.status !== 0 && options.printErr) options.printErr((r.stderr || '').split('\n')[0]);
      return r.status === 0 && existsSync(real(output)) ? 0 : 4;
    },
  };
}
