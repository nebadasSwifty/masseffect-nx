// A stand-in for the console in the USB install tests: an in-memory USB link (each write is one transfer, like a bulk
// transfer of a whole buffer) and the client side of Sphaira's protocol, reading an NSP in the order Sphaira's
// installer does without FLAG_STREAM (sphaira/source/yati/yati.cpp InstallInternal + container/nsp.cpp): the PFS0
// header (16 bytes, the file table, the string table), then the Meta NCA, then the NCAs it lists sorted by content type
// descending (Control before Program), each from its start in reads of 0x4000 bytes first and then `readSize`. Every
// NCA's SHA-256 is checked against its name, as Sphaira does by default.
import crypto from 'node:crypto';
import { encodePacket, decodePacket, CMD_OPEN, CMD_QUIT, RESULT_OK } from '../js/usb_install.js';
import { crc32c } from '../js/crc32c.js';

class Channel {
  constructor() { this.queue = []; this.waiters = []; this.closed = null; }
  push(bytes) {
    const copy = Uint8Array.from(bytes);
    const w = this.waiters.shift();
    if (w) w.resolve(copy); else this.queue.push(copy);
  }
  pull() {
    if (this.queue.length) return Promise.resolve(this.queue.shift());
    if (this.closed) return Promise.reject(this.closed);
    return new Promise((resolve, reject) => this.waiters.push({ resolve, reject }));
  }
  close(err) {
    this.closed = err;
    for (const w of this.waiters.splice(0)) w.reject(err);
  }
}

/** { host: transport for serveFiles/UsbNspSink, console: { send, recv } }. Closing the host fails pending reads. */
export function usbLink() {
  const toHost = new Channel(), toConsole = new Channel();
  const host = {
    closed: false,
    async read(n) {
      const m = await toHost.pull();
      if (m.length > n) throw new Error(`fake link: a ${m.length}-byte transfer into a ${n}-byte read`);
      return m;
    },
    async write(bytes) { if (this.closed) throw new Error('fake link: closed'); toConsole.push(bytes); },
    async close() { this.closed = true; toHost.close(new Error('fake link: host closed')); toConsole.close(new Error('fake link: host closed')); },
  };
  const console = { send: (b) => toHost.push(b), recv: () => toConsole.pull() };
  return { host, console };
}

const toHex = (b) => Buffer.from(b).toString('hex');

/** The console's side. Returns what it saw: names, size, flags, the bytes it assembled, the reads, the NCA list. */
export async function fakeSphairaInstall(con, { readSize = 0x8000, firstRead = 0x4000, midReads = [], stopAfterHeader = false } = {}) {
  const result = async () => {
    const p = decodePacket(await con.recv());
    if (p.arg2 !== RESULT_OK) throw new Error(`console: the host answered with error ${p.arg2}`);
    return p;
  };
  con.send(encodePacket(RESULT_OK)); // hello
  const hello = await result();
  const names = new TextDecoder().decode(await con.recv()).split('\n').filter(Boolean);
  if (new TextEncoder().encode(names.map((n) => `${n}\n`).join('')).length !== hello.arg3) throw new Error('console: name table length mismatch');
  con.send(encodePacket(CMD_OPEN, 0));
  const opened = await result();
  const size = (opened.arg3 & 0xFFFF) * 2 ** 32 + opened.arg4;
  const flags = opened.arg3 >>> 16;
  const bytes = new Uint8Array(size);
  const reads = [];
  const read = async (offset, length) => {
    con.send(encodePacket(Math.floor(offset / 2 ** 32), offset >>> 0, length, 0));
    const r = await result();
    const data = await con.recv();
    if (data.length !== r.arg3) throw new Error(`console: got ${data.length} bytes, the result said ${r.arg3}`);
    if (crc32c(data) !== r.arg4) throw new Error(`console: CRC-32C mismatch at ${offset}`);
    bytes.set(data, offset);
    reads.push([offset, data.length]);
    return data;
  };

  const head = await read(0, 16);
  const dv = new DataView(head.buffer, head.byteOffset, 16);
  if (dv.getUint32(0, true) !== 0x30534650) throw new Error('console: not a PFS0');
  const count = dv.getUint32(4, true), strtab = dv.getUint32(8, true);
  const table = await read(16, count * 24);
  const strings = await read(16 + count * 24, strtab);
  const dataOffset = 16 + count * 24 + strtab;
  const tv = new DataView(table.buffer, table.byteOffset, table.length);
  const entries = [];
  for (let i = 0; i < count; i++) {
    const nameOff = tv.getUint32(i * 24 + 16, true);
    const end = strings.indexOf(0, nameOff);
    entries.push({
      name: new TextDecoder().decode(strings.subarray(nameOff, end)),
      offset: dataOffset + Number(tv.getBigUint64(i * 24, true)),
      size: Number(tv.getBigUint64(i * 24 + 8, true)),
    });
  }
  const closeAndQuit = async () => {
    con.send(encodePacket(0, 0, 0, 0)); // close the file
    await result();
    con.send(encodePacket(CMD_QUIT));
    await result();
  };
  if (stopAfterHeader) {
    await closeAndQuit();
    return { names, size, flags, bytes, reads, entries };
  }

  const readNca = async (e, hook = null) => {
    const h = crypto.createHash('sha256');
    let pos = 0;
    while (pos < e.size) {
      const n = Math.min(pos === 0 ? firstRead : readSize, e.size - pos);
      h.update(await read(e.offset + pos, n));
      pos += n;
      if (hook) await hook(pos);
    }
    const digest = h.digest();
    if (!e.name.startsWith(toHex(digest.subarray(0, 16)))) throw new Error(`console: ${e.name}: SHA-256 does not match the name`);
  };
  const metaEntry = entries.find((e) => e.name.endsWith('.cnmt.nca'));
  await readNca(metaEntry);
  const others = entries.filter((e) => e !== metaEntry);
  // Our NSPs list Program, Control: Sphaira installs Control (type 3) first, then Program (type 1).
  for (const e of [...others].reverse()) {
    const pending = e === others[0] ? [...midReads] : [];
    await readNca(e, async (pos) => {
      // Out-of-order reads in the middle of the Program NCA (offsets relative to it): exercises the server's window
      // and its reopen path.
      while (pending.length && pos >= pending[0].after * e.size) {
        const { offset, length } = pending.shift();
        const o = Math.max(0, Math.min(e.size - 1, offset < 0 ? pos + offset : offset));
        await read(e.offset + o, Math.min(length, e.size - o));
      }
    });
  }
  await closeAndQuit();
  return { names, size, flags, bytes, reads, entries };
}
