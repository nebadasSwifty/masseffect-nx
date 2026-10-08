// Synthetic inputs for the NSP tests (the same shapes as tests/tools/test_full_nsp.py): a fake NRO with NACP and icon,
// an installer output folder, throwaway keys. Nothing here is real game data or real key material.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const enc = new TextEncoder();

/** Deterministic pseudo-random bytes (xorshift32), so failures reproduce. */
export function prng(seed) {
  let x = (seed >>> 0) || 1;
  return (n) => {
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) {
      x ^= x << 13; x >>>= 0;
      x ^= x >>> 17;
      x ^= x << 5; x >>>= 0;
      out[i] = x & 0xff;
    }
    return out;
  };
}

/** A fake NRO: text (NRO header at 0x10), ro, data segments, then an ASET section with an icon and a NACP. */
export function makeNro(rand, name = 'Mass Effect') {
  const text = rand(0x3000);
  text.set([0x20, 0, 0, 0x14, 0, 0, 0, 0, ...enc.encode('HOMEBREW')], 0);
  const ro = rand(0x2000);
  const data = rand(0x1000);
  const segs = [[0, text.length], [0x3000, ro.length], [0x5000, data.length]];
  const header = new Uint8Array(0x70);
  const dv = new DataView(header.buffer);
  header.set(enc.encode('NRO0'), 0);
  dv.setUint32(8, 0x6000, true);
  segs.forEach(([off, n], i) => { dv.setUint32(0x10 + 8 * i, off, true); dv.setUint32(0x14 + 8 * i, n, true); });
  dv.setUint32(0x28, 0x4000, true);
  for (let i = 0; i < 32; i++) header[0x30 + i] = i;
  text.set(header, 0x10);
  const icon = new Uint8Array([0xff, 0xd8, 0xff, 0xe0, ...rand(1000)]);
  const nacp = new Uint8Array(0x4000);
  nacp.set(enc.encode(name), 0);
  nacp[0x3025] = 1;
  nacp.fill(0x11, 0x3080, 0x3090);
  const aset = new Uint8Array(0x38);
  const av = new DataView(aset.buffer);
  aset.set(enc.encode('ASET'), 0);
  av.setBigUint64(8, 0x38n, true); av.setBigUint64(16, BigInt(icon.length), true);
  av.setBigUint64(24, BigInt(0x38 + icon.length), true); av.setBigUint64(32, BigInt(nacp.length), true);
  const parts = [text, ro, data, aset, icon, nacp];
  const out = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

export function writeFile(file, data) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, data);
}

/** An installer output folder like the zip's masseffect-nx/ (with DLC and a few junk files that must be skipped). */
export function makeInput(root, rand, extra = {}) {
  const files = {
    'masseffect.toml': enc.encode('dlc_enable = true\n'),
    'masseffect_shaders.mesp': rand(70000),
    'masseffect_shaders.mesp.idx': rand(333),
    'masseffect_prewarm_list.bin': new Uint8Array([...enc.encode('NFPL'), 5, 0, 0, 0, 4, 0, 0, 0, 3, 0, 0, 0, ...rand(12)]),
    'game_root/default.xex': new Uint8Array([...enc.encode('XEX2'), ...rand(5000)]),
    'game_root/Layer0/BIOGame/CookedXenon/Startup_INT.xxx': rand(0x4000 * 3 + 17),
    'game_root/Layer0/BIOGame/CookedXenon/BIOA_PRO10.xxx': rand(40000),
    'game_root/Layer0/BIOGame/Config/abcd': enc.encode('four'),
    'game_root/Layer1/Movies/empty.bik': new Uint8Array(0),
    'game_root/Layer1/a': enc.encode('x'),
    'game_root/Layer1/Ünïcode ✓.txt': enc.encode('names are UTF-8'),
    'masseffect/0000000000000000/4D5307E8/00000002/BDTS/AutoLoad.ini': enc.encode('[Packages]\n'),
    'masseffect/0000000000000000/4D5307E8/Headers/00000002/BDTS.header': rand(0x971A),
    ...extra,
  };
  for (const [p, d] of Object.entries(files)) writeFile(path.join(root, p), d);
  // Not packed: saves, caches, macOS junk.
  writeFile(path.join(root, 'game_root/._default.xex'), enc.encode('junk'));
  writeFile(path.join(root, 'game_root/.DS_Store'), enc.encode('junk'));
  writeFile(path.join(root, 'cache/pipeline.bin'), enc.encode('cache'));
  writeFile(path.join(root, 'masseffect-nx.nro'), makeNro(rand));
  return files;
}

const skipName = (n) => n.startsWith('._') || ['.DS_Store', 'Thumbs.db', 'desktop.ini'].includes(n);

function fileSource(file) {
  const size = fs.statSync(file).size;
  return {
    size,
    async *chunks() {
      const fd = fs.openSync(file, 'r');
      try {
        let pos = 0;
        while (pos < size) {
          const n = Math.min(1 << 20, size - pos);
          const buf = new Uint8Array(n);
          fs.readSync(fd, buf, 0, n, pos);
          pos += n;
          yield buf;
        }
      } finally {
        fs.closeSync(fd);
      }
    },
  };
}

function collectTree(base, prefix, out) {
  const walk = (dir, rel) => {
    for (const ent of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
      if (skipName(ent.name)) continue;
      const host = path.join(dir, ent.name);
      if (ent.isDirectory()) walk(host, `${rel}${ent.name}/`);
      else out.push({ path: `${prefix}${rel}${ent.name}`, ...fileSource(host) });
    }
  };
  walk(base, '');
}

/** The RomFS entries of an installer output folder, as tools/build_full_nsp.py build_romfs takes them. */
export function entriesFromDir(root, { includeDlc = true } = {}) {
  const out = [];
  for (const name of ['masseffect.toml', 'masseffect_shaders.mesp', 'masseffect_shaders.mesp.idx']) {
    out.push({ path: name, ...fileSource(path.join(root, name)) });
  }
  // Optional, like build_romfs: the shipped pipeline prewarm list.
  const list = path.join(root, 'masseffect_prewarm_list.bin');
  if (fs.existsSync(list)) out.push({ path: 'masseffect_prewarm_list.bin', ...fileSource(list) });
  collectTree(path.join(root, 'game_root'), 'game_root/', out);
  const dlc = path.join(root, 'masseffect', '0000000000000000');
  if (includeDlc && fs.existsSync(dlc)) collectTree(dlc, 'masseffect/0000000000000000/', out);
  return out;
}

/** Throwaway console keys (random) and their prod.keys text. */
export function makeKeys() {
  const keys = { header_key: new Uint8Array(crypto.randomBytes(32)), key_area_key_application_00: new Uint8Array(crypto.randomBytes(16)) };
  const hex = (b) => Buffer.from(b).toString('hex');
  const text = `; throwaway test keys\nmaster_key_00 = ${hex(crypto.randomBytes(16))}\nheader_key = ${hex(keys.header_key)}\n` +
    `key_area_key_application_00 = ${hex(keys.key_area_key_application_00)}\n`;
  return { keys, text };
}

/** A throwaway RSA-2048 key: PEM (for the Python packer) and the BigInt numbers (for the JS deterministic signer). */
export function makeRsa() {
  const { privateKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 2048, publicExponent: 65537 });
  const jwk = privateKey.export({ format: 'jwk' });
  const big = (s) => BigInt(`0x${Buffer.from(s, 'base64url').toString('hex')}`);
  return { pem: privateKey.export({ format: 'pem', type: 'pkcs8' }), n: big(jwk.n), d: big(jwk.d) };
}
