// js/nsp.js against tools/build_full_nsp.py: the same synthetic input, throwaway keys, the same AES keys and a fixed
// PSS salt on both sides, then the NSPs (and the base metadata files) must be identical byte for byte. Full packs,
// FAT32 split output, updates from a .basemeta.json and from the base NSP, program-only updates.
// Skipped when python3 with the 'cryptography' package is not available.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import {
  buildNsp, parseBaseMetadata, metadataFromNsp, PartsReader, pythonJson, parseProdKeys, decryptNcaHeader,
  CONTENT_PROGRAM, nextUpdateVersion, withLastUpdateVersion, patchNacp,
} from '../js/nsp.js';
import { CONFIG } from '../config.js';
import { MemoryNspSink, SplitNspSink } from '../js/nsp_sink.js';
import { deterministicPssSigner, aesEcb, AesCtr, toHex } from '../js/nsp_crypto.js';
import { prng, makeInput, makeNro, entriesFromDir, makeKeys, makeRsa, writeFile } from './nsp_fixtures.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const oracle = path.join(here, 'fixtures', 'nsp_oracle.py');
const python = spawnSync('python3', ['-c', 'import cryptography'], { encoding: 'utf8' });
const skip = python.status === 0 ? false : 'python3 with the cryptography package is not available';

const DATA_DIR = 'sdmc:/switch/me-test';
const CREATED = '2026-10-08T00:00:00Z';

function runOracle(mode, cfg, dir) {
  const cfgPath = path.join(dir, `oracle-${Math.random().toString(16).slice(2)}.json`);
  fs.writeFileSync(cfgPath, JSON.stringify(cfg));
  const r = spawnSync('python3', [oracle, mode, cfgPath], { encoding: 'utf8' });
  assert.equal(r.status, 0, `python packer failed:\n${r.stdout}\n${r.stderr}`);
}

function partsReaderOf(file) {
  const size = fs.statSync(file).size;
  return new PartsReader([{
    size,
    read: async (offset, length) => {
      const buf = new Uint8Array(length);
      const fd = fs.openSync(file, 'r');
      try { fs.readSync(fd, buf, 0, length, offset); } finally { fs.closeSync(fd); }
      return buf;
    },
  }]);
}

let ctx;
function setup() {
  if (ctx) return ctx;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nsp-js-'));
  const rand = prng(20261008);
  const base = path.join(dir, 'base', 'masseffect-nx');
  const big = rand(5 * 0x10000 + 1234);
  makeInput(base, rand, { 'game_root/Layer1/big.bin': big, 'game_root/Layer1/removed.bin': rand(3000) });
  // Version 1 input: one 64 KiB chunk of big.bin changed, the toml and the prewarm list changed, a file added, one
  // removed, a new program.
  const next = path.join(dir, 'new', 'masseffect-nx');
  fs.cpSync(base, next, { recursive: true });
  const changed = big.slice();
  changed.set(rand(100), 2 * 0x10000 + 100);
  writeFile(path.join(next, 'game_root/Layer1/big.bin'), changed);
  writeFile(path.join(next, 'masseffect.toml'), new TextEncoder().encode('dlc_enable = true\nnew_setting = 1\n'));
  writeFile(path.join(next, 'game_root/Layer1/added.bin'), rand(777));
  writeFile(path.join(next, 'masseffect_prewarm_list.bin'), new Uint8Array([...new TextEncoder().encode('NFPL'), 5, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, ...rand(16)]));
  fs.rmSync(path.join(next, 'game_root/Layer1/removed.bin'));
  writeFile(path.join(next, 'masseffect-nx.nro'), makeNro(rand));
  const newNro = path.join(dir, 'program-only.nro');
  writeFile(newNro, makeNro(rand, 'Mass Effect 2.0'));

  const { keys, text } = makeKeys();
  const keysPath = path.join(dir, 'prod.keys');
  fs.writeFileSync(keysPath, text);
  const rsa = makeRsa();
  const pemPath = path.join(dir, 'sign.pem');
  fs.writeFileSync(pemPath, rsa.pem);
  const salt = new Uint8Array(crypto.randomBytes(32));
  const aes = { 0: new Uint8Array(crypto.randomBytes(16)), 1: new Uint8Array(crypto.randomBytes(16)), 2: new Uint8Array(crypto.randomBytes(16)) };
  ctx = { dir, base, next, newNro, keys, keysPath, rsa, pemPath, salt, aes };
  return ctx;
}

/** Packs with Python (deterministic) and returns the output path. */
function pyPack(c, argv, output) {
  runOracle('pack', {
    argv: [...argv, '--keys', c.keysPath, '--output', output],
    aes_keys: Object.fromEntries(Object.entries(c.aes).map(([k, v]) => [k, toHex(v)])),
    rsa_pem: c.pemPath,
    salt: toHex(c.salt),
  }, c.dir);
  return output;
}

/** Packs with JS into memory with the same deterministic keys. */
async function jsPack(c, options) {
  const sink = options.sink ?? new MemoryNspSink();
  const result = await buildNsp({
    keys: parseProdKeys(fs.readFileSync(c.keysPath, 'utf8')),
    signer: deterministicPssSigner({ n: c.rsa.n, d: c.rsa.d }, c.salt),
    aesKeyFor: (type) => c.aes[type],
    createdUtc: CREATED,
    dataDir: DATA_DIR,
    sink,
    ...options,
  });
  return { result, bytes: sink instanceof MemoryNspSink ? sink.bytes() : null, sink };
}

function assertSameBytes(actual, expected, what) {
  assert.equal(actual.length, expected.length, `${what}: size`);
  const a = Buffer.from(actual.buffer, actual.byteOffset, actual.byteLength);
  if (!a.equals(expected)) {
    let i = 0;
    while (a[i] === expected[i]) i++;
    assert.fail(`${what}: first difference at byte ${i} (0x${i.toString(16)})`);
  }
}

test('full NSP: byte-identical to tools/build_full_nsp.py, base metadata too', { skip }, async () => {
  const c = setup();
  const out = pyPack(c, ['--input', c.base, '--data-dir', DATA_DIR, '--created-utc', CREATED], path.join(c.dir, 'base.nsp'));
  const { result, bytes } = await jsPack(c, { nro: fs.readFileSync(path.join(c.base, 'masseffect-nx.nro')), entries: entriesFromDir(c.base) });
  assertSameBytes(bytes, fs.readFileSync(out), 'full NSP');
  assert.equal(pythonJson(result.baseMeta), fs.readFileSync(`${out}.basemeta.json`, 'utf8'));
  assert.equal(result.titleId, '01a5eec700010000');
  assert.equal(result.dlc, true);
});

test('full NSP options (title ID, name, display version, emulator NPDM, no DLC): byte-identical', { skip }, async () => {
  const c = setup();
  const out = pyPack(c, ['--input', c.base, '--data-dir', 'sdmc:/switch/other', '--created-utc', CREATED, '--title-id', '01a5eec700000000',
    '--name', 'Масс Эффект', '--display-version', '1.2', '--emulator-compatible', '--no-dlc', '--version', '3'], path.join(c.dir, 'opts.nsp'));
  const { bytes } = await jsPack(c, {
    nro: fs.readFileSync(path.join(c.base, 'masseffect-nx.nro')), entries: entriesFromDir(c.base, { includeDlc: false }),
    dataDir: 'sdmc:/switch/other', titleId: 0x01A5EEC700000000n, name: 'Масс Эффект', displayVersion: '1.2', emulatorCompatible: true, version: 3,
  });
  assertSameBytes(bytes, fs.readFileSync(out), 'NSP with options');
});

test('small read blocks (64 KiB chunks spanning blocks): still byte-identical, full and update', { skip }, async () => {
  const c = setup();
  const baseNsp = pyPack(c, ['--input', c.base, '--data-dir', DATA_DIR, '--created-utc', CREATED], path.join(c.dir, 'b0.nsp'));
  const full = await jsPack(c, { nro: fs.readFileSync(path.join(c.base, 'masseffect-nx.nro')), entries: entriesFromDir(c.base), chunkBytes: 0x4000 });
  assertSameBytes(full.bytes, fs.readFileSync(baseNsp), 'full NSP, 16 KiB blocks');
  assert.equal(pythonJson(full.result.baseMeta), fs.readFileSync(`${baseNsp}.basemeta.json`, 'utf8'));
  const out = pyPack(c, ['--update', '--version', '1', '--input', c.next, '--base', baseNsp, '--data-dir', DATA_DIR, '--created-utc', CREATED],
    path.join(c.dir, 'u0.nsp'));
  const meta = await metadataFromNsp(partsReaderOf(baseNsp), c.keys, { chunkBytes: 0x4000 });
  const upd = await jsPack(c, { nro: fs.readFileSync(path.join(c.next, 'masseffect-nx.nro')), entries: entriesFromDir(c.next), update: { baseMeta: meta }, version: 1, chunkBytes: 0xC000 });
  assertSameBytes(upd.bytes, fs.readFileSync(out), 'update NSP, 48 KiB blocks');
});

test('FAT32 split output: the same parts as --split', { skip }, async () => {
  const c = setup();
  const out = pyPack(c, ['--input', c.base, '--data-dir', DATA_DIR, '--created-utc', CREATED], path.join(c.dir, 'whole.nsp'));
  // Python's --split uses 0xFFFF0000-byte parts; a small part size exercises the same code paths on a small NSP.
  const partSize = 0x10000;
  const parts = new Map();
  const sink = new SplitNspSink(async (index) => {
    const part = { data: new Uint8Array(0), closed: false };
    parts.set(index, part);
    return {
      async write({ position, data }) {
        if (part.closed) throw new Error('write to a closed part');
        if (position + data.length > part.data.length) { const n = new Uint8Array(position + data.length); n.set(part.data); part.data = n; }
        part.data.set(data, position);
      },
      async close() { part.closed = true; },
    };
  }, { partSize });
  await jsPack(c, { nro: fs.readFileSync(path.join(c.base, 'masseffect-nx.nro')), entries: entriesFromDir(c.base), sink });
  await sink.close();
  const whole = fs.readFileSync(out);
  assert.equal(parts.size, Math.ceil(whole.length / partSize));
  for (const [i, p] of parts) {
    assert.ok(p.closed);
    assertSameBytes(p.data, whole.subarray(i * partSize, (i + 1) * partSize), `part ${i}`);
  }
});

test('update from the base metadata file: byte-identical BKTR patch', { skip }, async () => {
  const c = setup();
  const baseNsp = pyPack(c, ['--input', c.base, '--data-dir', DATA_DIR, '--created-utc', CREATED], path.join(c.dir, 'b1.nsp'));
  const out = pyPack(c, ['--update', '--version', '1', '--input', c.next, '--base', `${baseNsp}.basemeta.json`, '--data-dir', DATA_DIR,
    '--created-utc', CREATED, '--display-version', '1.1'], path.join(c.dir, 'u1.nsp'));
  const baseMeta = parseBaseMetadata(fs.readFileSync(`${baseNsp}.basemeta.json`, 'utf8'));
  const { result, bytes } = await jsPack(c, {
    nro: fs.readFileSync(path.join(c.next, 'masseffect-nx.nro')), entries: entriesFromDir(c.next),
    update: { baseMeta }, version: 1, displayVersion: '1.1',
  });
  assertSameBytes(bytes, fs.readFileSync(out), 'update NSP');
  assert.equal(result.patchId, '01a5eec700010800');
  assert.equal(result.version, 0x10000);
  assert.ok(result.stats.baseBytes > 5 * 0x10000, 'most of big.bin comes from the base');
  assert.ok(result.stats.patchBytes < bytes.length);
  assert.equal(result.baseMeta, null);
});

test('update from the base NSP: same metadata as Python, byte-identical patch', { skip }, async () => {
  const c = setup();
  const baseNsp = pyPack(c, ['--input', c.base, '--data-dir', DATA_DIR, '--created-utc', CREATED], path.join(c.dir, 'b2.nsp'));
  runOracle('basemeta', { base: baseNsp, keys: c.keysPath, output: path.join(c.dir, 'b2.fromnsp.json') }, c.dir);
  const steps = [];
  const meta = await metadataFromNsp(partsReaderOf(baseNsp), c.keys, { progress: (d, t) => steps.push([d, t]) });
  assert.equal(pythonJson(meta), fs.readFileSync(path.join(c.dir, 'b2.fromnsp.json'), 'utf8'));
  assert.ok(steps.length > 0 && steps.at(-1)[0] === steps.at(-1)[1]);
  const out = pyPack(c, ['--update', '--version', '2', '--input', c.next, '--base', baseNsp, '--data-dir', DATA_DIR, '--created-utc', CREATED],
    path.join(c.dir, 'u2.nsp'));
  const { bytes } = await jsPack(c, { nro: fs.readFileSync(path.join(c.next, 'masseffect-nx.nro')), entries: entriesFromDir(c.next), update: { baseMeta: meta }, version: 2 });
  assertSameBytes(bytes, fs.readFileSync(out), 'update NSP (base from the NSP)');
  // The wrong keys are reported, never printed.
  const wrong = { header_key: new Uint8Array(32).map((_, i) => i), key_area_key_application_00: new Uint8Array(16) };
  await assert.rejects(metadataFromNsp(partsReaderOf(baseNsp), wrong), /wrong header_key/);
});

test('program-only update (no game data): byte-identical, from the metadata file or the base NSP', { skip }, async () => {
  const c = setup();
  const baseNsp = pyPack(c, ['--input', c.base, '--data-dir', DATA_DIR, '--created-utc', CREATED], path.join(c.dir, 'b3.nsp'));
  const out = pyPack(c, ['--update', '--program-only', '--version', '3', '--nro', c.newNro, '--base', `${baseNsp}.basemeta.json`],
    path.join(c.dir, 'u3.nsp'));
  const meta = parseBaseMetadata(fs.readFileSync(`${baseNsp}.basemeta.json`, 'utf8'));
  const { result, bytes } = await jsPack(c, { nro: fs.readFileSync(c.newNro), update: { baseMeta: meta, programOnly: true }, version: 3 });
  assertSameBytes(bytes, fs.readFileSync(out), 'program-only update');
  assert.equal(result.stats.patchBytes, 0);
  const fromNsp = await metadataFromNsp(partsReaderOf(baseNsp), c.keys);
  const again = await jsPack(c, { nro: fs.readFileSync(c.newNro), update: { baseMeta: fromNsp, programOnly: true }, version: 3 });
  assertSameBytes(again.bytes, fs.readFileSync(out), 'program-only update (base from the NSP)');
  // Update numbering helpers.
  assert.equal(nextUpdateVersion(meta), 1);
  assert.equal(nextUpdateVersion(withLastUpdateVersion(meta, 3)), 4);
  assert.equal(nextUpdateVersion(meta, 7), 8);
  await assert.rejects(jsPack(c, { nro: fs.readFileSync(c.newNro), update: { baseMeta: meta, programOnly: true }, version: 0 }), /update number/);
});

test('per-edition packs (--edition en / ru): byte-identical, title ID and data folder of the edition', { skip }, async () => {
  const c = setup();
  const editions = { en: CONFIG.editions.find((e) => e.id === 'usa-eur-en-es-pl-rev1').nsp, ru: CONFIG.editions.find((e) => e.id === 'rus-rev0').nsp };
  for (const [name, ed] of Object.entries(editions)) {
    const out = pyPack(c, ['--input', c.base, '--edition', name, '--created-utc', CREATED], path.join(c.dir, `ed-${name}.nsp`));
    const { result, bytes } = await jsPack(c, { nro: fs.readFileSync(path.join(c.base, 'masseffect-nx.nro')), entries: entriesFromDir(c.base), titleId: ed.titleId, dataDir: ed.dataDir });
    assertSameBytes(bytes, fs.readFileSync(out), `${name} NSP`);
    assert.equal(result.titleId, ed.titleId);
    assert.equal(result.baseMeta.data_dir, ed.dataDir);
    assert.equal(pythonJson(result.baseMeta), fs.readFileSync(`${out}.basemeta.json`, 'utf8'));
    // The base read back from the NSP knows its data folder (the marker), in both packers.
    runOracle('basemeta', { base: out, keys: c.keysPath, output: path.join(c.dir, `ed-${name}.fromnsp.json`) }, c.dir);
    const meta = await metadataFromNsp(partsReaderOf(out), c.keys);
    assert.equal(meta.data_dir, ed.dataDir);
    assert.equal(pythonJson(meta), fs.readFileSync(path.join(c.dir, `ed-${name}.fromnsp.json`), 'utf8'));
    // An update without --data-dir keeps the base's folder: Python and JS agree.
    const upd = pyPack(c, ['--update', '--edition', name, '--version', '1', '--input', c.next, '--base', out, '--created-utc', CREATED],
      path.join(c.dir, `ed-${name}-u1.nsp`));
    const u = await jsPack(c, { nro: fs.readFileSync(path.join(c.next, 'masseffect-nx.nro')), entries: entriesFromDir(c.next), update: { baseMeta: meta }, version: 1, dataDir: undefined, titleId: ed.titleId });
    assertSameBytes(u.bytes, fs.readFileSync(upd), `${name} update`);
  }
});

test('NACP ID fields set by the NRO follow the title ID: byte-identical', { skip }, async () => {
  const c = setup();
  const nro = makeNro(prng(99));
  // nacptool --titleid style: PresenceGroupId, AddOnContentBaseId, SaveDataOwnerId, LocalCommunicationId[0..7], SeedForPseudoDeviceId.
  const asset = new DataView(nro.buffer, nro.byteOffset).getUint32(0x18, true);
  const nacpOff = asset + Number(new DataView(nro.buffer, nro.byteOffset).getBigUint64(asset + 24, true));
  const nv = new DataView(nro.buffer, nro.byteOffset + nacpOff, 0x4000);
  const old = 0x0100000000ABC000n;
  for (const off of [0x3038, 0x3078, 0x30B0, 0x30B8, 0x30C0, 0x30C8, 0x30D0, 0x30D8, 0x30E0, 0x30E8, 0x30F8]) nv.setBigUint64(off, old, true);
  nv.setBigUint64(0x3070, old + 0x1000n, true);
  const nroPath = path.join(c.dir, 'ids.nro');
  writeFile(nroPath, nro);
  const out = pyPack(c, ['--input', c.base, '--nro', nroPath, '--edition', 'en', '--created-utc', CREATED], path.join(c.dir, 'ids.nsp'));
  const { bytes } = await jsPack(c, { nro, entries: entriesFromDir(c.base), titleId: '01a5eec700020000', dataDir: 'sdmc:/switch/masseffect-nx-en' });
  assertSameBytes(bytes, fs.readFileSync(out), 'NSP with NACP IDs');
  const patched = patchNacp(new Uint8Array(nro.buffer, nro.byteOffset + nacpOff, 0x4000), null, null, 0x01A5EEC700020000n);
  const pv = new DataView(patched.buffer, patched.byteOffset);
  assert.equal(pv.getBigUint64(0x3078, true), 0x01A5EEC700020000n);
  assert.equal(pv.getBigUint64(0x30E8, true), 0x01A5EEC700020000n);
  assert.equal(pv.getBigUint64(0x3070, true), 0x01A5EEC700021000n);
  // Zero fields (what the port's NROs carry) stay zero.
  assert.equal(new DataView(patchNacp(new Uint8Array(0x4000), null, null, 0x01A5EEC700020000n).buffer).getBigUint64(0x3078, true), 0n);
});

test('real packs: WebCrypto RSA key and random AES keys; the header signature verifies against the NPDM key', async () => {
  const rand = prng(7);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nsp-js-rnd-'));
  makeInput(dir, rand);
  const { keys } = makeKeys();
  const sink = new MemoryNspSink();
  const phases = new Set();
  await buildNsp({ keys, nro: fs.readFileSync(path.join(dir, 'masseffect-nx.nro')), entries: entriesFromDir(dir), sink, onProgress: (p) => phases.add(p) });
  assert.deepEqual([...phases].sort(), ['hash', 'write']);
  const nsp = sink.bytes();
  const dv = new DataView(nsp.buffer);
  const count = dv.getUint32(4, true), strtab = dv.getUint32(8, true);
  const dataOff = 0x10 + 0x18 * count + strtab;
  // The first entry is the Program NCA.
  const ncaOff = dataOff + Number(dv.getBigUint64(0x10, true));
  const h = decryptNcaHeader(nsp.subarray(ncaOff, ncaOff + 0xC00), keys.header_key);
  assert.equal(new TextDecoder().decode(h.subarray(0x200, 0x204)), 'NCA3');
  assert.equal(h[0x205], CONTENT_PROGRAM);
  const ctrKey = aesEcb(keys.key_area_key_application_00, h.slice(0x300, 0x340), true).slice(0x20, 0x30);
  const hv = new DataView(h.buffer);
  const start = hv.getUint32(0x240, true) * 0x200, end = hv.getUint32(0x244, true) * 0x200;
  const exefs = await new AesCtr(ctrKey).apply(0n, start, nsp.subarray(ncaOff + start, ncaOff + end));
  const fsh = new DataView(h.buffer, 0x400, 0x200);
  const pfs = exefs.subarray(Number(fsh.getBigUint64(0x40, true)));
  const pv = new DataView(pfs.buffer, pfs.byteOffset);
  const n = pv.getUint32(4, true), st = pv.getUint32(8, true);
  const names = pfs.subarray(0x10 + 0x18 * n, 0x10 + 0x18 * n + st);
  let npdm = null;
  for (let i = 0; i < n; i++) {
    const nameOff = pv.getUint32(0x10 + 0x18 * i + 16, true);
    const name = new TextDecoder().decode(names.subarray(nameOff, names.indexOf(0, nameOff)));
    const off = 0x10 + 0x18 * n + st + Number(pv.getBigUint64(0x10 + 0x18 * i, true));
    if (name === 'main.npdm') npdm = pfs.subarray(off, off + Number(pv.getBigUint64(0x18 + 0x18 * i, true)));
  }
  assert.ok(npdm);
  const acid = new DataView(npdm.buffer, npdm.byteOffset).getUint32(0x78, true);
  const modulus = npdm.subarray(acid + 0x100, acid + 0x200);
  const publicKey = crypto.createPublicKey({ key: { kty: 'RSA', n: Buffer.from(modulus).toString('base64url'), e: 'AQAB' }, format: 'jwk' });
  const ok = crypto.verify('sha256', h.subarray(0x200, 0x400), { key: publicKey, padding: crypto.constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 },
    h.subarray(0x100, 0x200));
  assert.ok(ok, 'Program NCA header signature verifies with the ACID modulus');
});
