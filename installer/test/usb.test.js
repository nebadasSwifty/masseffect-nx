// The USB install (js/crc32c.js, js/usb_install.js, UsbNspSink in js/nsp_sink.js, NspImage in js/nsp.js): CRC-32C,
// Sphaira's packets, and whole installs against a fake console (test/usb_fake_console.js) over NSPs made from the
// synthetic fixtures, compared byte for byte with the same NSP written to memory.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { crc32c, crc32cBitwise } from '../js/crc32c.js';
import {
  encodePacket, decodePacket, openResultArgs, serveFiles, webUsbSupport, UsbProtocolError, FLAG_STREAM, CMD_OPEN,
  WebUsbTransport, findPermittedSwitch, requestSwitch,
} from '../js/usb_install.js';
import { buildNsp, sourceFromBytes, PackError } from '../js/nsp.js';
import { MemoryNspSink, UsbNspSink, openNspSink } from '../js/nsp_sink.js';
import { deterministicPssSigner } from '../js/nsp_crypto.js';
import { stagesFor } from '../js/pipeline.js';
import { prng, makeInput, makeNro, entriesFromDir, makeKeys, makeRsa } from './nsp_fixtures.js';
import { usbLink, fakeSphairaInstall } from './usb_fake_console.js';

const enc = (s) => new TextEncoder().encode(s);
const hex = (s) => Uint8Array.from(Buffer.from(s, 'hex'));

test('CRC-32C: published check values, the bitwise reference, continuation, speed', (t) => {
  assert.equal(crc32c(enc('123456789')), 0xE3069283);
  assert.equal(crc32c(new Uint8Array(0)), 0);
  // RFC 3720 (iSCSI) appendix B.4.
  assert.equal(crc32c(new Uint8Array(32)), 0x8A9136AA);
  assert.equal(crc32c(new Uint8Array(32).fill(0xFF)), 0x62A8AB43);
  assert.equal(crc32c(Uint8Array.from({ length: 32 }, (_, i) => i)), 0x46DD794E);
  assert.equal(crc32c(Uint8Array.from({ length: 32 }, (_, i) => 31 - i)), 0x113FDB5C);
  const rand = prng(5);
  const big = rand(5000);
  for (let start = 0; start < 9; start++) {
    for (const len of [0, 1, 3, 7, 8, 9, 15, 16, 17, 63, 64, 65, 1000, 4991 - start]) {
      const piece = big.subarray(start, start + len);
      assert.equal(crc32c(piece), crc32cBitwise(piece), `start ${start}, length ${len}`);
    }
  }
  assert.equal(crc32c(big.subarray(777), crc32c(big.subarray(0, 777))), crc32c(big));

  const buf = rand(16 << 20);
  crc32c(buf); // warm up
  const t0 = performance.now();
  for (let i = 0; i < 4; i++) crc32c(buf);
  const mbs = (4 * buf.length) / 1e6 / ((performance.now() - t0) / 1000);
  t.diagnostic(`crc32c: ${mbs.toFixed(0)} MB/s`);
  assert.ok(mbs > 50, `crc32c too slow for USB: ${mbs.toFixed(0)} MB/s`);
});

test('packets: the bytes Sphaira expects (vectors made with Python struct + a bitwise CRC-32C)', () => {
  assert.deepEqual(encodePacket(0), hex('304850530000000000000000000000000000000078dca5b9'));
  assert.deepEqual(encodePacket(0, 5, 0x12345678, 0), hex('30485053000000000500000078563412000000009f355907'));
  // An open result for a 0x2_0BADF00D-byte file with FLAG_STREAM: size bits 32-47 in arg3, flags above them.
  assert.deepEqual(encodePacket(2, ...openResultArgs(0x20BADF00D, FLAG_STREAM)), hex('3048505302000000020001000df0ad0b00000000fab74cdc'));
  assert.deepEqual(openResultArgs(8_700_000_000), [2, 8_700_000_000 - 2 * 2 ** 32]);
  assert.throws(() => openResultArgs(2 ** 48), RangeError);

  assert.deepEqual(decodePacket(encodePacket(1, 2, 3, 4)), { arg2: 1, arg3: 2, arg4: 3, arg5: 4 });
  assert.deepEqual(decodePacket(encodePacket(0xFFFFFFFF, 0x80000000, 0, 7)), { arg2: 0xFFFFFFFF, arg3: 0x80000000, arg4: 0, arg5: 7 });
  const bad = encodePacket(1, 2, 3, 4);
  bad[8] ^= 1;
  assert.throws(() => decodePacket(bad), /CRC-32C/);
  const magic = encodePacket(1);
  magic[0] = 0;
  assert.throws(() => decodePacket(magic), /magic/);
  assert.throws(() => decodePacket(new Uint8Array(23)), UsbProtocolError);
});

test('WebUSB detection', () => {
  assert.deepEqual(webUsbSupport({ isSecureContext: true, navigator: {} }), { ok: false, reason: 'no-webusb' });
  assert.deepEqual(webUsbSupport({ isSecureContext: false, navigator: { usb: { requestDevice() {} } } }), { ok: false, reason: 'insecure' });
  assert.deepEqual(webUsbSupport({ isSecureContext: true, navigator: { usb: { requestDevice() {} } } }), { ok: true, reason: null });
});

test('serveFiles: plain files, several files, the console asks for each by index', async () => {
  const rand = prng(9);
  const a = rand(100000), b = rand(33);
  const files = [a, b].map((bytes, i) => ({ name: `f${i}.nsp`, size: bytes.length, read: async (o, n) => bytes.slice(o, o + n) }));
  const { host, console: con } = usbLink();
  const progress = [];
  const served = serveFiles(host, files, { onProgress: (sent, total) => progress.push([sent, total]) });
  // A minimal console: hello, open 1, read it whole, close; open 0, read in pieces, close; quit.
  const result = async () => decodePacket(await con.recv());
  con.send(encodePacket(0));
  assert.equal((await result()).arg3, 'f0.nsp\nf1.nsp\n'.length);
  assert.equal(new TextDecoder().decode(await con.recv()), 'f0.nsp\nf1.nsp\n');
  const readAll = async (index, size, step) => {
    con.send(encodePacket(CMD_OPEN, index));
    const r = await result();
    assert.equal(r.arg4, size);
    const out = new Uint8Array(size);
    for (let o = 0; o < size; o += step) {
      con.send(encodePacket(0, o, step, 0));
      const h = await result();
      const d = await con.recv();
      assert.equal(h.arg3, d.length);
      assert.equal(h.arg4, crc32c(d));
      out.set(d, o);
    }
    con.send(encodePacket(0, 0, 0, 0));
    await result();
    return out;
  };
  assert.deepEqual(await readAll(1, 33, 64), b);
  assert.deepEqual(await readAll(0, 100000, 4096), a);
  con.send(encodePacket(0));
  await result();
  const r = await served;
  assert.equal(r.quit, true);
  assert.deepEqual(r.files.map((f) => f.sent), [100000, 33]);
  assert.deepEqual(progress.at(-1), [100000, 100000]);
});

test('serveFiles: an unknown command or file index is answered with an error', async () => {
  const { host, console: con } = usbLink();
  const served = serveFiles(host, [{ name: 'x.nsp', size: 1, read: async () => new Uint8Array(1) }]);
  con.send(encodePacket(0));
  await con.recv(); await con.recv();
  con.send(encodePacket(CMD_OPEN, 5));
  assert.equal(decodePacket(await con.recv()).arg2, 1);
  await assert.rejects(served, /file 5 of 1/);
});

// ---- NSP installs against the fake console ----------------------------------------------------------------------

let ctx;
function fixture() {
  if (ctx) return ctx;
  const rand = prng(77);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nsp-usb-'));
  // A larger file than the other fixtures: many 16 KiB blocks, several Program NCA windows.
  makeInput(dir, rand, { 'game_root/Layer1/big.bin': rand(600000) });
  const { keys } = makeKeys();
  const rsa = makeRsa();
  const salt = new Uint8Array(crypto.randomBytes(32));
  const aes = { 0: new Uint8Array(crypto.randomBytes(16)), 1: new Uint8Array(crypto.randomBytes(16)), 2: new Uint8Array(crypto.randomBytes(16)) };
  ctx = {
    dir, keys, nro: fs.readFileSync(path.join(dir, 'masseffect-nx.nro')),
    options: { keys, signer: deterministicPssSigner(rsa, salt), aesKeyFor: (type) => aes[type], createdUtc: '2026-10-09T00:00:00Z', dataDir: 'sdmc:/switch/masseffect-nx' },
  };
  return ctx;
}

/** The same NSP in memory (the reference). */
async function reference(c, extra = {}) {
  const sink = new MemoryNspSink();
  const result = await buildNsp({ ...c.options, nro: c.nro, entries: entriesFromDir(c.dir), sink, chunkBytes: 0x4000, ...extra });
  return { bytes: sink.bytes(), result };
}

/** Installs through a UsbNspSink to the fake console; returns what the console assembled. */
async function install(c, { entries = entriesFromDir(c.dir), consoleOptions = {}, imageOptions = null, extra = {} } = {}) {
  const { host, console: con } = usbLink();
  const sink = await openNspSink('usb', 'masseffect-nx.nsp', { connect: async () => host });
  if (imageOptions) {
    // A small window and skip-ahead limit (the defaults are 16 and 64 MiB, more than the fixture's whole NCA).
    const serve = sink.serve.bind(sink);
    sink.serve = (image, o) => serve(Object.assign(image, imageOptions), o);
  }
  const phases = new Set();
  const consoleRun = fakeSphairaInstall(con, consoleOptions);
  const built = buildNsp({
    ...c.options, nro: c.nro, entries, sink, chunkBytes: 0x4000, onProgress: (p) => phases.add(p), ...extra,
  });
  const [seen, result] = await Promise.all([consoleRun, built]);
  return { seen, result, sink, phases, sinkResult: await sink.close() };
}

test('USB install: the console gets exactly the NSP the file output writes; NCA hashes match their names', async () => {
  const c = fixture();
  const want = await reference(c);
  const got = await install(c);
  assert.deepEqual(got.seen.names, ['masseffect-nx.nsp']);
  assert.equal(got.seen.flags, 0, 'no FLAG_STREAM: Sphaira checks the NCA hashes');
  assert.equal(got.seen.size, want.bytes.length);
  assert.ok(Buffer.from(got.seen.bytes).equals(Buffer.from(want.bytes)), 'same bytes as the memory sink');
  assert.deepEqual(got.result.files, want.result.files);
  assert.equal(got.sinkResult, 'installed');
  assert.equal(got.sink.stats.verified, true, 'the Program NCA was checked against the pass 2 hash');
  assert.equal(got.sink.stats.reopens, 0, 'Sphaira\'s order needs no regeneration');
  assert.deepEqual([...got.phases].sort(), ['hash', 'usb', 'write']);
  assert.ok(got.result.baseMeta, 'the base metadata is still returned (for later updates)');
  // Sphaira's order: header pieces, then the Meta NCA (last in the file), the Control NCA, the Program NCA.
  const [program, control, meta] = got.seen.entries;
  const firstAt = (e) => got.seen.reads.findIndex(([o]) => o >= e.offset && o < e.offset + e.size);
  assert.ok(firstAt(meta) < firstAt(control) && firstAt(control) < firstAt(program));
});

test('USB install: out-of-order reads (back into the window, back past it, far ahead) are served correctly', async () => {
  const c = fixture();
  const want = await reference(c);
  const got = await install(c, {
    consoleOptions: {
      readSize: 0x6000,
      midReads: [
        { after: 0.3, offset: -0x3000, length: 0x1000 }, // inside the window
        { after: 0.5, offset: 0x100, length: 0x9000 }, // far back: reopen
        { after: 0.6, offset: 0x7FFF0, length: 0x20 }, // ahead of the cursor
      ],
    },
    imageOptions: { windowBytes: 0x8000, skipAheadBytes: 0x10000 },
  });
  assert.ok(Buffer.from(got.seen.bytes).equals(Buffer.from(want.bytes)));
  assert.equal(got.sinkResult, 'installed');
  assert.ok(got.sink.stats.reopens >= 1, `reopens: ${got.sink.stats.reopens}`);
  assert.equal(got.sink.stats.verified, true, 'the hash check survives a reopen backwards');
});

test('NspImage: random ranges equal the reference, with seekable and non-seekable sources and a tiny window', async () => {
  const c = fixture();
  const want = (await reference(c)).bytes;
  for (const seekable of [false, true]) {
    const entries = entriesFromDir(c.dir);
    const used = seekable ? await Promise.all(entries.map(async (e) => {
      const parts = [];
      for await (const b of e.chunks()) parts.push(b);
      return { path: e.path, ...sourceFromBytes(new Uint8Array(Buffer.concat(parts))) };
    })) : entries;
    const rand = prng(seekable ? 3 : 4);
    let image;
    const sink = { kind: 'test', serve: async (img) => { image = img; img.windowBytes = 0x8000; img.skipAheadBytes = 0x10000; } };
    await buildNsp({ ...c.options, nro: c.nro, entries: used, sink, chunkBytes: 0x4000 });
    assert.equal(image.size, want.length);
    for (let i = 0; i < 300; i++) {
      const r = rand(8);
      const offset = new DataView(r.buffer).getUint32(0, true) % want.length;
      const length = 1 + (new DataView(r.buffer).getUint32(4, true) % 0x12000);
      const got = await image.read(offset, length);
      assert.ok(Buffer.from(got).equals(Buffer.from(want.subarray(offset, offset + length))), `${seekable ? 'seekable' : 'plain'} sources: ${offset}+${length}`);
    }
    assert.ok(image.stats.reopens > 10);
  }
});

test('USB install: a source that changes after the hashing pass is caught before the last block; the console gets an error', async () => {
  const c = fixture();
  const entries = entriesFromDir(c.dir).map((e) => {
    if (e.path !== 'game_root/Layer1/big.bin') return e;
    let reads = 0;
    return {
      ...e,
      chunks: async function* () {
        reads++;
        for await (const b of e.chunks()) {
          const out = b.slice();
          if (reads === 3) out[0] ^= 1; // the third read (the install) differs
          yield out;
        }
      },
    };
  });
  const { host, console: con } = usbLink();
  const sink = new UsbNspSink(async () => host, 'masseffect-nx.nsp');
  const consoleRun = fakeSphairaInstall(con).then(() => null, (e) => e);
  await assert.rejects(buildNsp({ ...c.options, nro: c.nro, entries, sink, chunkBytes: 0x4000 }), (e) => e instanceof PackError && /changed between/.test(e.message));
  const consoleError = await consoleRun;
  assert.match(String(consoleError?.message), /host answered with error|closed/);
  assert.equal(await sink.close(), 'not-installed');
});

test('USB install: a console that stops early (cancelled, no space) is an error, not success', async () => {
  const c = fixture();
  await assert.rejects(install(c, { consoleOptions: { stopAfterHeader: true } }), /stopped before it had read the whole package/);
});

test('USB install of a program-only update (non-seekable patch section) gives the update NSP', async () => {
  const c = fixture();
  const base = await reference(c);
  const meta = base.result.baseMeta;
  const nro2 = makeNro(prng(8), 'Mass Effect');
  const want = await reference(c, { nro: nro2, entries: undefined, update: { baseMeta: meta, programOnly: true }, version: 1 });
  const got = await install({ ...c, nro: nro2 }, { entries: undefined, extra: { update: { baseMeta: meta, programOnly: true }, version: 1 } });
  assert.ok(Buffer.from(got.seen.bytes).equals(Buffer.from(want.bytes)));
  assert.equal(got.sinkResult, 'installed');
});

test('stagesFor: the USB target adds the nsp_usb stage', () => {
  assert.deepEqual(stagesFor('nsp', { kind: 'full', target: 'usb' }), ['download', 'scan', 'translate', 'pack', 'nsp_hash', 'nsp_write', 'nsp_usb']);
  assert.deepEqual(stagesFor('nsp', { kind: 'update', programOnly: true, target: 'usb' }), ['download', 'nsp_base', 'nsp_write', 'nsp_usb']);
});

/** A USBDevice stand-in (the WebUSB calls WebUsbTransport makes) wired to the fake console's link. */
function fakeUsbDevice(link, { claimFails = false } = {}) {
  const calls = [];
  const dataView = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
  return {
    calls, vendorId: 0x057E, productId: 0x3000, productName: 'Nintendo Switch', configuration: null, opened: false,
    async open() { calls.push('open'); this.opened = true; },
    async selectConfiguration(n) {
      calls.push(`config ${n}`);
      this.configuration = { interfaces: [{ interfaceNumber: 0, alternate: { endpoints: [{ direction: 'in', type: 'bulk', endpointNumber: 1 }, { direction: 'out', type: 'bulk', endpointNumber: 1 }] } }] };
    },
    async claimInterface(n) { calls.push(`claim ${n}`); if (claimFails) throw new Error('Unable to claim interface.'); },
    async releaseInterface(n) { calls.push(`release ${n}`); },
    async close() { calls.push('close'); this.opened = false; await link.host.close(); },
    async transferIn(ep, n) { assert.equal(ep, 1); return { status: 'ok', data: dataView(await link.host.read(n)) }; },
    async transferOut(ep, bytes) { assert.equal(ep, 1); await link.host.write(bytes); return { status: 'ok', bytesWritten: bytes.length }; },
  };
}

test('WebUsbTransport over a fake USBDevice: configuration, interface 0, bulk endpoints, a full install, release and close', async () => {
  const c = fixture();
  const want = await reference(c);
  const link = usbLink();
  const device = fakeUsbDevice(link);
  const usb = {
    getDevices: async () => [{ vendorId: 0x1234, productId: 1 }, device],
    requestDevice: async (o) => { assert.deepEqual(o.filters, [{ vendorId: 0x057E, productId: 0x3000 }]); throw new DOMException('No device selected.', 'NotFoundError'); },
  };
  assert.equal(await findPermittedSwitch(usb), device);
  assert.equal(await requestSwitch(usb), null, 'a closed chooser is not an error');
  const sink = new UsbNspSink(async ({ signal }) => WebUsbTransport.open(await findPermittedSwitch(usb), { signal }), 'masseffect-nx.nsp');
  const [seen] = await Promise.all([
    fakeSphairaInstall(link.console),
    buildNsp({ ...c.options, nro: c.nro, entries: entriesFromDir(c.dir), sink, chunkBytes: 0x4000 }),
  ]);
  assert.ok(Buffer.from(seen.bytes).equals(Buffer.from(want.bytes)));
  assert.deepEqual(device.calls, ['open', 'config 1', 'claim 0', 'release 0', 'close']);
  assert.equal(await sink.close(), 'installed');
});

test('WebUsbTransport: a busy interface gives an explanation; cancelling closes the device', async () => {
  await assert.rejects(WebUsbTransport.open(fakeUsbDevice(usbLink(), { claimFails: true })), /Another program may be using it.*WinUSB/s);
  const link = usbLink();
  const device = fakeUsbDevice(link);
  const ac = new AbortController();
  const transport = await WebUsbTransport.open(device, { signal: ac.signal });
  const served = serveFiles(transport, [{ name: 'x.nsp', size: 1, read: async () => new Uint8Array(1) }], { signal: ac.signal });
  ac.abort();
  await assert.rejects(served);
  assert.ok(device.calls.includes('close'));
});
