// Install over USB to Sphaira's "USB install" screen, with WebUSB (desktop Chrome, Edge, Opera). Sphaira's own
// protocol (sphaira/include/usb/usb_api.hpp, tools/usb_install.py and tools/webusb in github.com/ITotalJustice/sphaira;
// this is our own implementation of it, tools/usb_install.py of mass-effect-recomp is the host version we verified on
// a console). The console is the USB device 057E:3000 (vendor-specific interface 0, one bulk IN and one bulk OUT
// endpoint) and drives everything; the page only answers:
//
//   packet   6 little-endian u32: magic 'SPH0' (0x53504830), arg2, arg3, arg4, arg5, CRC-32C of the first 20 bytes
//   console  hello (arg2 = 0)                         page  result(OK, length of the name table) + the name table
//   console  command: 0 quit                          page  result(OK), done
//                     1 open, arg3 = file index       page  result(OK, size >> 32 & 0xFFFF | flags << 16, size & 0xFFFFFFFF)
//   console  range: arg2:arg3 offset, arg4 length     page  result(OK, length sent, CRC-32C of the data) + the data
//            offset 0 and length 0 closes the file    page  result(OK), back to the commands
//
// The name table is "name\n" per file; Sphaira picks the installer by the extension (.nsp). Flags 0: Sphaira may read
// any range (it reads the PFS0 header, then the Meta and Control NCA at the end, then the Program NCA in order) and
// checks every NCA's SHA-256 against its name. FLAG_STREAM (1) would promise strictly increasing offsets and make
// Sphaira skip those checks; not used (docs/full-nsp.md section 6, "Install over USB").
import { crc32c } from './crc32c.js?v=0.3.3';

export const USB_VENDOR_ID = 0x057E;
export const USB_PRODUCT_ID = 0x3000;
export const MAGIC = 0x53504830;
export const PACKET_SIZE = 24;
export const CMD_QUIT = 0, CMD_OPEN = 1;
export const RESULT_OK = 0, RESULT_ERROR = 1;
export const FLAG_NONE = 0, FLAG_STREAM = 1;
export const USB_FILTERS = [{ vendorId: USB_VENDOR_ID, productId: USB_PRODUCT_ID }];

/** A malformed packet or an unexpected request from the console. */
export class UsbProtocolError extends Error {
  constructor(message) { super(message); this.name = 'UsbProtocolError'; }
}

/** One 24-byte packet: magic, the four arguments (u32 each), CRC-32C of the first 20 bytes. */
export function encodePacket(arg2 = 0, arg3 = 0, arg4 = 0, arg5 = 0) {
  const out = new Uint8Array(PACKET_SIZE);
  const dv = new DataView(out.buffer);
  [MAGIC, arg2, arg3, arg4, arg5].forEach((v, i) => dv.setUint32(i * 4, v >>> 0, true));
  dv.setUint32(20, crc32c(out.subarray(0, 20)), true);
  return out;
}

/** Parses and checks a packet from the console: { arg2, arg3, arg4, arg5 }. */
export function decodePacket(bytes) {
  if (!bytes || bytes.length !== PACKET_SIZE) throw new UsbProtocolError(`bad packet length from the console (${bytes?.length ?? 0} bytes, expected ${PACKET_SIZE})`);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, PACKET_SIZE);
  if (dv.getUint32(0, true) !== MAGIC) throw new UsbProtocolError('bad packet from the console (no SPH0 magic): is this Sphaira\'s USB install screen?');
  if (dv.getUint32(20, true) !== crc32c(bytes.subarray(0, 20))) throw new UsbProtocolError('bad packet from the console (CRC-32C mismatch)');
  return { arg2: dv.getUint32(4, true), arg3: dv.getUint32(8, true), arg4: dv.getUint32(12, true), arg5: dv.getUint32(16, true) };
}

/** The answer to "open": the size split in 16 + 32 bits, the flags in the top 16 bits of arg3. */
export function openResultArgs(size, flags = FLAG_NONE) {
  if (!Number.isSafeInteger(size) || size < 0 || size >= 2 ** 48) throw new RangeError('file size out of range for the protocol (48 bits)');
  return [(Math.floor(size / 2 ** 32) & 0xFFFF) | ((flags & 0xFFFF) << 16), size >>> 0];
}

/**
 * Serves files to the console until it quits. transport: { read(length) -> Uint8Array, write(bytes) } (WebUsbTransport,
 * or a fake in the tests). files: [{ name, size, read(offset, length) -> Uint8Array }].
 * onProgress(sentBytes, totalBytes, { rate, file }) is called after every range (sentBytes counts what was sent, also
 * ranges sent twice; the caller clamps). onWaiting() is called once before the console's hello is awaited.
 * Returns { files: [{ name, sent, ranges, seconds }], quit: true } when the console quits.
 */
export async function serveFiles(transport, files, { flags = FLAG_NONE, onProgress = () => {}, onWaiting = () => {}, log = () => {}, signal = null, now = () => Date.now() } = {}) {
  const names = new TextEncoder().encode(files.map((f) => `${f.name}\n`).join(''));
  const stats = files.map((f) => ({ name: f.name, sent: 0, ranges: 0, seconds: 0 }));
  const readPacket = async () => decodePacket(await transport.read(PACKET_SIZE));
  const result = (code, a3 = 0, a4 = 0) => transport.write(encodePacket(code, a3, a4));
  const check = () => { if (signal?.aborted) throw signal.reason ?? new DOMException('Aborted', 'AbortError'); };

  onWaiting();
  log('USB: waiting for the console (Sphaira, Install -> USB)');
  // The console says hello first; anything else before it (a stale packet of an earlier session) is skipped.
  for (let tries = 0; ; tries++) {
    check();
    try {
      await readPacket();
      break;
    } catch (e) {
      if (!(e instanceof UsbProtocolError) || tries >= 8) throw e;
      log(`USB: ${e.message}; waiting for the next hello`, 'warn');
    }
  }
  await result(RESULT_OK, names.length);
  await transport.write(names);
  log(`USB: connected; offered ${files.map((f) => f.name).join(', ')}`);

  for (;;) {
    check();
    const { arg2: cmd, arg3: index } = await readPacket();
    if (cmd === CMD_QUIT) {
      await result(RESULT_OK);
      log('USB: the console finished');
      return { files: stats, quit: true };
    }
    if (cmd !== CMD_OPEN || index >= files.length) {
      await result(RESULT_ERROR);
      throw new UsbProtocolError(cmd !== CMD_OPEN ? `unknown command ${cmd} from the console` : `the console asked for file ${index} of ${files.length}`);
    }
    const file = files[index];
    const st = stats[index];
    await result(RESULT_OK, ...openResultArgs(file.size, flags));
    log(`USB: the console opened ${file.name} (${file.size} bytes)`);
    const t0 = now();
    let last = -1;
    for (;;) {
      check();
      const p = await readPacket();
      const offset = p.arg2 * 2 ** 32 + p.arg3;
      const length = p.arg4;
      if (offset === 0 && length === 0) {
        await result(RESULT_OK);
        st.seconds = (now() - t0) / 1000;
        log(`USB: ${file.name}: ${st.sent} bytes in ${st.ranges} ranges, ${((now() - t0) / 1000).toFixed(0)} s`);
        break;
      }
      if ((flags & FLAG_STREAM) && offset < last) {
        await result(RESULT_ERROR);
        continue;
      }
      let data;
      try {
        data = offset >= file.size ? new Uint8Array(0) : await file.read(offset, Math.min(length, file.size - offset));
      } catch (e) {
        // Tell the console, so it stops with an error instead of waiting for its transfer timeout.
        try { await result(RESULT_ERROR); } catch { /* the link may be gone too */ }
        throw e;
      }
      await result(RESULT_OK, data.length, crc32c(data));
      if (data.length) await transport.write(data);
      last = offset + data.length;
      st.sent += data.length;
      st.ranges++;
      const secs = (now() - t0) / 1000;
      onProgress(st.sent, file.size, { rate: secs > 0 ? st.sent / secs : 0, file: file.name, offset, length: data.length });
    }
  }
}

// ---- WebUSB ---------------------------------------------------------------------------------------------------------------

/**
 * Whether this page can talk to the console: { ok, reason } with reason 'no-webusb' (Firefox, Safari, mobile browsers:
 * no navigator.usb) or 'insecure' (WebUSB needs https:// or localhost).
 */
export function webUsbSupport(g = globalThis) {
  if (g.isSecureContext === false) return { ok: false, reason: 'insecure' };
  if (!g.navigator?.usb || typeof g.navigator.usb.requestDevice !== 'function') return { ok: false, reason: 'no-webusb' };
  return { ok: true, reason: null };
}

/** A console this site was already allowed to use and that is connected now (no prompt), or null. */
export async function findPermittedSwitch(usb = globalThis.navigator?.usb) {
  const list = await usb.getDevices();
  return list.find((d) => d.vendorId === USB_VENDOR_ID && d.productId === USB_PRODUCT_ID) ?? null;
}

/** The browser's device chooser (needs a click: user activation). Null when the user closes it without a choice. */
export async function requestSwitch(usb = globalThis.navigator?.usb) {
  try {
    return await usb.requestDevice({ filters: USB_FILTERS });
  } catch (e) {
    if (e?.name === 'NotFoundError') return null; // closed, or no matching device
    throw e;
  }
}

/** Bulk transfers to and from an opened USBDevice (interface 0). Closing it (also on abort) fails pending transfers. */
export class WebUsbTransport {
  constructor(device, epIn, epOut) {
    this.device = device; this.epIn = epIn; this.epOut = epOut; this.closed = false;
  }

  static async open(device, { signal = null } = {}) {
    await device.open();
    if (device.configuration === null) await device.selectConfiguration(1);
    try {
      await device.claimInterface(0);
    } catch (e) {
      try { await device.close(); } catch { /* ignore */ }
      throw new Error(`could not claim the console's USB interface (${e?.message ?? e}). Another program may be using it (close tools/usb_install.py, DBI or NS-USBloader hosts); on Windows the device may need the WinUSB driver (see the page's notes).`);
    }
    const alt = device.configuration.interfaces.find((i) => i.interfaceNumber === 0)?.alternate ?? device.configuration.interfaces[0].alternates[0];
    const epIn = alt.endpoints.find((e) => e.direction === 'in' && e.type === 'bulk')?.endpointNumber;
    const epOut = alt.endpoints.find((e) => e.direction === 'out' && e.type === 'bulk')?.endpointNumber;
    if (epIn === undefined || epOut === undefined) {
      await device.close();
      throw new Error('the console\'s USB interface has no bulk endpoints (is Sphaira\'s USB install screen open?)');
    }
    const t = new WebUsbTransport(device, epIn, epOut);
    if (signal) {
      const onAbort = () => { t.close(); };
      signal.addEventListener('abort', onAbort, { once: true });
      t.detach = () => signal.removeEventListener('abort', onAbort);
    }
    return t;
  }

  async read(length) {
    const r = await this.device.transferIn(this.epIn, length);
    if (r.status !== 'ok') throw new Error(`USB read failed (${r.status})`);
    return r.data ? new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength) : new Uint8Array(0);
  }

  async write(bytes) {
    const r = await this.device.transferOut(this.epOut, bytes);
    if (r.status !== 'ok') throw new Error(`USB write failed (${r.status})`);
    if (r.bytesWritten !== bytes.length) throw new Error(`USB write short (${r.bytesWritten} of ${bytes.length} bytes)`);
  }

  async close() {
    if (this.closed) return;
    this.closed = true;
    this.detach?.();
    try { await this.device.releaseInterface(0); } catch { /* gone or never claimed */ }
    try { await this.device.close(); } catch { /* already closed */ }
  }
}
