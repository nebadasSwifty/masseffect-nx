// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78), the checksum of Sphaira's USB protocol: every packet and every
// data block carries one. Slicing-by-8 tables (8 KiB): ~0.5-1 GB/s in V8 on a desktop CPU, far above what the USB link
// carries (test/usb.test.js prints the measured rate). crc32cBitwise is the slow reference the tests compare against.

const POLY = 0x82F63B78;

const TABLES = (() => {
  const t = new Uint32Array(8 * 256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ POLY : c >>> 1;
    t[n] = c >>> 0;
  }
  for (let n = 0; n < 256; n++) {
    let c = t[n];
    for (let s = 1; s < 8; s++) {
      c = t[c & 0xff] ^ (c >>> 8);
      t[s * 256 + n] = c >>> 0;
    }
  }
  return t;
})();

/**
 * CRC-32C of `data` (a Uint8Array). `crc` continues an earlier result (pass the previous return value), so a block can
 * be checked in pieces: crc32c(b, crc32c(a)) === crc32c(a + b).
 */
export function crc32c(data, crc = 0) {
  const t = TABLES;
  let c = ~crc >>> 0;
  let i = 0;
  const n = data.length;
  // Byte steps up to a 4-byte boundary of the underlying buffer, then 8 bytes per step through two aligned u32 reads.
  while (i < n && ((data.byteOffset + i) & 3)) c = t[(c ^ data[i++]) & 0xff] ^ (c >>> 8);
  const words = (n - i) >>> 3;
  if (words) {
    const u32 = new Uint32Array(data.buffer, data.byteOffset + i, words * 2);
    for (let w = 0; w < u32.length; w += 2) {
      // Little-endian hosts only (every browser and Node platform in use); checked once below.
      const lo = u32[w] ^ c;
      const hi = u32[w + 1];
      c = t[1792 + (lo & 0xff)] ^ t[1536 + ((lo >>> 8) & 0xff)] ^ t[1280 + ((lo >>> 16) & 0xff)] ^ t[1024 + (lo >>> 24)] ^
        t[768 + (hi & 0xff)] ^ t[512 + ((hi >>> 8) & 0xff)] ^ t[256 + ((hi >>> 16) & 0xff)] ^ t[hi >>> 24];
    }
    i += words * 8;
  }
  while (i < n) c = t[(c ^ data[i++]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

if (new Uint8Array(new Uint32Array([1]).buffer)[0] !== 1) throw new Error('crc32c.js needs a little-endian host');

/** Bit-by-bit reference (slow; for tests). */
export function crc32cBitwise(data, crc = 0) {
  let c = ~crc >>> 0;
  for (const b of data) {
    c ^= b;
    for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ POLY : c >>> 1;
  }
  return ~c >>> 0;
}
