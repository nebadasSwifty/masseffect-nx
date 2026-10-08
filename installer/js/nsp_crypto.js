// Cryptography for the NSP writer (js/nsp.js). Our own code, written from the standards; no third-party code.
//
//   AES-128 block cipher (FIPS-197), ECB and XTS (IEEE 1619) on top of it: WebCrypto has neither. Used only for the
//     NCA headers (3 KiB each, AES-XTS with Nintendo's big-endian sector tweak) and the key area (64 bytes, AES-ECB).
//   SHA-256 (FIPS 180-4), incremental: WebCrypto's digest() is one-shot, the NCA hash covers gigabytes.
//   AES-CTR over gigabytes: WebCrypto (native speed).
//   RSA-PSS/SHA-256 (RFC 8017): WebCrypto for real packs; a deterministic signer (fixed salt, BigInt) for tests.
//
// Keys passed to these functions are never logged or copied anywhere else.

const subtle = globalThis.crypto?.subtle;

// ---- AES-128 ---------------------------------------------------------------------------------------------------------

const SBOX = new Uint8Array(256);
const INV_SBOX = new Uint8Array(256);
(function initSbox() {
  // Multiplicative inverse in GF(2^8) via log tables (generator 3), then the affine transform.
  const exp = new Uint8Array(256);
  const log = new Uint8Array(256);
  let x = 1;
  for (let i = 0; i < 255; i++) {
    exp[i] = x;
    log[x] = i;
    x ^= (x << 1) ^ ((x & 0x80) ? 0x11b : 0); // x * 3
    x &= 0xff;
  }
  for (let i = 0; i < 256; i++) {
    const inv = i === 0 ? 0 : exp[(255 - log[i]) % 255];
    let s = inv;
    let r = inv;
    for (let k = 0; k < 4; k++) {
      r = ((r << 1) | (r >> 7)) & 0xff;
      s ^= r;
    }
    s ^= 0x63;
    SBOX[i] = s;
    INV_SBOX[s] = i;
  }
})();

const xtime = (b) => ((b << 1) ^ ((b & 0x80) ? 0x1b : 0)) & 0xff;
function gmul(a, b) {
  let p = 0;
  for (let i = 0; i < 8; i++) {
    if (b & 1) p ^= a;
    a = xtime(a);
    b >>= 1;
  }
  return p;
}

/** AES-128 with an expanded key: encryptBlock / decryptBlock on 16-byte arrays (in place into `out`). */
export class Aes128 {
  constructor(key) {
    if (!(key instanceof Uint8Array) || key.length !== 16) throw new Error('AES-128 needs a 16-byte key');
    const w = new Uint8Array(176);
    w.set(key);
    let rcon = 1;
    for (let i = 16; i < 176; i += 4) {
      let t0 = w[i - 4], t1 = w[i - 3], t2 = w[i - 2], t3 = w[i - 1];
      if (i % 16 === 0) {
        const tmp = t0;
        t0 = SBOX[t1] ^ rcon; t1 = SBOX[t2]; t2 = SBOX[t3]; t3 = SBOX[tmp];
        rcon = xtime(rcon);
      }
      w[i] = w[i - 16] ^ t0; w[i + 1] = w[i - 15] ^ t1; w[i + 2] = w[i - 14] ^ t2; w[i + 3] = w[i - 13] ^ t3;
    }
    this.w = w;
  }

  encryptBlock(input, out = new Uint8Array(16)) {
    const s = Uint8Array.from(input.subarray(0, 16));
    const w = this.w;
    for (let i = 0; i < 16; i++) s[i] ^= w[i];
    const t = new Uint8Array(16);
    for (let round = 1; round <= 10; round++) {
      // SubBytes + ShiftRows (state is column-major: s[c*4 + r])
      for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) t[c * 4 + r] = SBOX[s[((c + r) % 4) * 4 + r]];
      if (round < 10) {
        for (let c = 0; c < 4; c++) {
          const a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
          const all = a0 ^ a1 ^ a2 ^ a3;
          s[c * 4] = a0 ^ all ^ xtime(a0 ^ a1);
          s[c * 4 + 1] = a1 ^ all ^ xtime(a1 ^ a2);
          s[c * 4 + 2] = a2 ^ all ^ xtime(a2 ^ a3);
          s[c * 4 + 3] = a3 ^ all ^ xtime(a3 ^ a0);
        }
      } else {
        s.set(t);
      }
      for (let i = 0; i < 16; i++) s[i] ^= w[round * 16 + i];
    }
    out.set(s);
    return out;
  }

  decryptBlock(input, out = new Uint8Array(16)) {
    const s = Uint8Array.from(input.subarray(0, 16));
    const w = this.w;
    for (let i = 0; i < 16; i++) s[i] ^= w[160 + i];
    const t = new Uint8Array(16);
    for (let round = 9; round >= 0; round--) {
      // InvShiftRows + InvSubBytes
      for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = INV_SBOX[s[c * 4 + r]];
      for (let i = 0; i < 16; i++) t[i] ^= w[round * 16 + i];
      if (round > 0) {
        for (let c = 0; c < 4; c++) {
          const a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
          s[c * 4] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
          s[c * 4 + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
          s[c * 4 + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
          s[c * 4 + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
        }
      } else {
        s.set(t);
      }
    }
    out.set(s);
    return out;
  }
}

/** AES-128-ECB (no padding): data length must be a multiple of 16. */
export function aesEcb(key, data, decrypt = false) {
  if (data.length % 16) throw new Error('AES-ECB data must be a multiple of 16 bytes');
  const aes = new Aes128(key);
  const out = new Uint8Array(data.length);
  for (let i = 0; i < data.length; i += 16) {
    const block = data.subarray(i, i + 16);
    out.set(decrypt ? aes.decryptBlock(block) : aes.encryptBlock(block), i);
  }
  return out;
}

/**
 * AES-128-XTS over `sectorSize`-byte sectors, the tweak of sector n being n as a 16-byte big-endian number (Nintendo's
 * convention for NCA headers; the IEEE standard uses little-endian). key: 32 bytes (data key, tweak key).
 */
export function aesXts(key, data, { sectorSize = 0x200, firstSector = 0, decrypt = false } = {}) {
  if (key.length !== 32) throw new Error('AES-XTS needs a 32-byte key');
  if (data.length % sectorSize || sectorSize % 16) throw new Error('AES-XTS data must be whole sectors');
  const k1 = new Aes128(key.subarray(0, 16));
  const k2 = new Aes128(key.subarray(16, 32));
  const out = new Uint8Array(data.length);
  const block = new Uint8Array(16);
  for (let s = 0; s < data.length / sectorSize; s++) {
    const tweakIn = new Uint8Array(16);
    let n = BigInt(firstSector + s);
    for (let i = 15; i >= 0 && n > 0n; i--) { tweakIn[i] = Number(n & 0xffn); n >>= 8n; }
    const tweak = k2.encryptBlock(tweakIn);
    for (let off = s * sectorSize; off < (s + 1) * sectorSize; off += 16) {
      for (let i = 0; i < 16; i++) block[i] = data[off + i] ^ tweak[i];
      const r = decrypt ? k1.decryptBlock(block) : k1.encryptBlock(block);
      for (let i = 0; i < 16; i++) out[off + i] = r[i] ^ tweak[i];
      // tweak *= alpha in GF(2^128), little-endian byte order
      let carry = 0;
      for (let i = 0; i < 16; i++) {
        const next = tweak[i] >> 7;
        tweak[i] = ((tweak[i] << 1) | carry) & 0xff;
        carry = next;
      }
      if (carry) tweak[0] ^= 0x87;
    }
  }
  return out;
}

// ---- SHA-256 ---------------------------------------------------------------------------------------------------------

const K = new Int32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

/** Incremental SHA-256: update(bytes)... digest() -> Uint8Array(32). */
export class Sha256 {
  constructor() {
    this.h = new Int32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    this.w = new Int32Array(64);
    this.buf = new Uint8Array(64);
    this.bufLen = 0;
    this.total = 0;
  }

  #blocks(p, off, end) {
    const w = this.w;
    const h = this.h;
    let h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], h5 = h[5], h6 = h[6], h7 = h[7];
    for (; off < end; off += 64) {
      for (let i = 0; i < 16; i++) {
        const j = off + i * 4;
        w[i] = (p[j] << 24) | (p[j + 1] << 16) | (p[j + 2] << 8) | p[j + 3];
      }
      for (let i = 16; i < 64; i++) {
        const a = w[i - 15], b = w[i - 2];
        const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
        const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, hh = h7;
      for (let i = 0; i < 64; i++) {
        const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
        const ch = (e & f) ^ (~e & g);
        const t1 = (hh + S1 + ch + K[i] + w[i]) | 0;
        const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) | 0;
        hh = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
      h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + hh) | 0;
    }
    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4; h[5] = h5; h[6] = h6; h[7] = h7;
  }

  update(data) {
    let off = 0;
    const len = data.length;
    this.total += len;
    if (this.bufLen) {
      const take = Math.min(64 - this.bufLen, len);
      this.buf.set(data.subarray(0, take), this.bufLen);
      this.bufLen += take;
      off = take;
      if (this.bufLen < 64) return this;
      this.#blocks(this.buf, 0, 64);
      this.bufLen = 0;
    }
    const full = off + ((len - off) & ~63);
    if (full > off) this.#blocks(data, off, full);
    if (full < len) {
      this.buf.set(data.subarray(full), 0);
      this.bufLen = len - full;
    }
    return this;
  }

  digest() {
    const bits = this.total * 8;
    const pad = new Uint8Array(((this.bufLen + 9 + 63) & ~63) - this.bufLen);
    pad[0] = 0x80;
    const dv = new DataView(pad.buffer);
    dv.setUint32(pad.length - 8, Math.floor(bits / 0x100000000));
    dv.setUint32(pad.length - 4, bits >>> 0);
    const total = this.total;
    this.update(pad);
    this.total = total;
    const out = new Uint8Array(32);
    const odv = new DataView(out.buffer);
    for (let i = 0; i < 8; i++) odv.setUint32(i * 4, this.h[i]);
    return out;
  }
}

export function sha256(data) {
  return new Sha256().update(data).digest();
}

/**
 * SHA-256 of many independent pieces at once: WebCrypto's native digest where available (each call is async, so the
 * pieces of one chunk run concurrently), the JS implementation otherwise. Returns [Uint8Array(32)].
 */
export async function sha256Many(pieces) {
  if (subtle && pieces.length > 0) {
    const out = await Promise.all(pieces.map((p) => subtle.digest('SHA-256', p)));
    return out.map((b) => new Uint8Array(b));
  }
  return pieces.map((p) => sha256(p));
}

// ---- AES-CTR (WebCrypto) ---------------------------------------------------------------------------------------------

/**
 * AES-128-CTR with a 16-byte counter block = upper (8 bytes, big-endian) || (byteOffset / 16) (8 bytes, big-endian),
 * the NCA convention. `byteOffset` must be 16-aligned. Encryption and decryption are the same operation.
 */
export class AesCtr {
  constructor(key) {
    if (!subtle) throw new Error('This browser has no WebCrypto (crypto.subtle); open the page over https://');
    this.keyPromise = subtle.importKey('raw', key, { name: 'AES-CTR' }, false, ['encrypt']);
  }

  async apply(upper, byteOffset, data) {
    if (byteOffset % 16) throw new Error('AES-CTR offset must be 16-aligned');
    if (!data.length) return new Uint8Array(0);
    const counter = new Uint8Array(16);
    const dv = new DataView(counter.buffer);
    dv.setBigUint64(0, BigInt.asUintN(64, BigInt(upper)));
    dv.setBigUint64(8, BigInt(byteOffset / 16));
    const key = await this.keyPromise;
    return new Uint8Array(await subtle.encrypt({ name: 'AES-CTR', counter, length: 64 }, key, data));
  }
}

// ---- RSA-PSS ---------------------------------------------------------------------------------------------------------

function bigFromBytes(bytes) {
  let n = 0n;
  for (const b of bytes) n = (n << 8n) | BigInt(b);
  return n;
}

function bytesFromBig(n, size) {
  const out = new Uint8Array(size);
  for (let i = size - 1; i >= 0; i--) { out[i] = Number(n & 0xffn); n >>= 8n; }
  return out;
}

function modPow(base, exp, mod) {
  let result = 1n;
  base %= mod;
  while (exp > 0n) {
    if (exp & 1n) result = (result * base) % mod;
    base = (base * base) % mod;
    exp >>= 1n;
  }
  return result;
}

/** EMSA-PSS-ENCODE (RFC 8017 9.1.1) with SHA-256 and MGF1-SHA-256, for a 2048-bit modulus (emBits 2047). */
export function pssEncode(message, salt, emBits = 2047) {
  const emLen = Math.ceil(emBits / 8);
  const mHash = sha256(message);
  const mPrime = new Uint8Array(8 + 32 + salt.length);
  mPrime.set(mHash, 8);
  mPrime.set(salt, 40);
  const h = sha256(mPrime);
  const dbLen = emLen - 32 - 1;
  const db = new Uint8Array(dbLen);
  db[dbLen - salt.length - 1] = 1;
  db.set(salt, dbLen - salt.length);
  const mask = new Uint8Array(dbLen);
  for (let counter = 0, pos = 0; pos < dbLen; counter++) {
    const c = new Uint8Array(36);
    c.set(h);
    new DataView(c.buffer).setUint32(32, counter);
    const part = sha256(c);
    mask.set(part.subarray(0, Math.min(32, dbLen - pos)), pos);
    pos += 32;
  }
  for (let i = 0; i < dbLen; i++) db[i] ^= mask[i];
  db[0] &= 0xff >> (8 * emLen - emBits);
  const em = new Uint8Array(emLen);
  em.set(db);
  em.set(h, dbLen);
  em[emLen - 1] = 0xbc;
  return em;
}

/**
 * A deterministic RSA-PSS signer for tests: { modulus: Uint8Array(256), sign(message) -> Uint8Array(256) }, from the
 * private key numbers (BigInt n, d) and a fixed salt. Real packs use rsaPssSigner().
 */
export function deterministicPssSigner({ n, d }, salt) {
  const modulus = bytesFromBig(n, 256);
  return {
    modulus,
    async sign(message) {
      const em = pssEncode(message, salt, 2047);
      return bytesFromBig(modPow(bigFromBytes(em), d, n), 256);
    },
  };
}

function b64urlToBytes(s) {
  const b64 = s.replace(/-/g, '+').replace(/_/g, '/');
  return base64Decode(b64 + '='.repeat((4 - (b64.length % 4)) % 4));
}

/** A fresh RSA-2048 key in WebCrypto (non-extractable private part): { modulus, sign(message) } (PSS, salt 32). */
export async function rsaPssSigner() {
  if (!subtle) throw new Error('This browser has no WebCrypto (crypto.subtle)');
  const pair = await subtle.generateKey(
    { name: 'RSA-PSS', modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: 'SHA-256' },
    false, ['sign', 'verify']);
  const jwk = await subtle.exportKey('jwk', pair.publicKey);
  const n = b64urlToBytes(jwk.n);
  const modulus = new Uint8Array(256);
  modulus.set(n.subarray(Math.max(0, n.length - 256)), 256 - Math.min(256, n.length));
  return {
    modulus,
    async sign(message) {
      return new Uint8Array(await subtle.sign({ name: 'RSA-PSS', saltLength: 32 }, pair.privateKey, message));
    },
  };
}

/** Random bytes (WebCrypto). */
export function randomBytes(n) {
  const out = new Uint8Array(n);
  globalThis.crypto.getRandomValues(out);
  return out;
}

// ---- base64 (standard alphabet, padded; Python's base64.b64encode) --------------------------------------------------

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const B64_INV = new Int16Array(128).fill(-1);
for (let i = 0; i < 64; i++) B64_INV[B64.charCodeAt(i)] = i;

export function base64Encode(bytes) {
  const parts = [];
  let s = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i], b = bytes[i + 1], c = bytes[i + 2];
    const n = (a << 16) | ((b ?? 0) << 8) | (c ?? 0);
    s += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + (b === undefined ? '=' : B64[(n >> 6) & 63]) + (c === undefined ? '=' : B64[n & 63]);
    if (s.length >= 8192) { parts.push(s); s = ''; }
  }
  parts.push(s);
  return parts.join('');
}

export function base64Decode(text) {
  const clean = String(text).replace(/[\s]/g, '');
  if (clean.length % 4) throw new Error('bad base64');
  const pad = clean.endsWith('==') ? 2 : clean.endsWith('=') ? 1 : 0;
  const out = new Uint8Array((clean.length / 4) * 3 - pad);
  let o = 0;
  for (let i = 0; i < clean.length; i += 4) {
    const v = [0, 1, 2, 3].map((k) => (clean[i + k] === '=' ? 0 : B64_INV[clean.charCodeAt(i + k)] ?? -1));
    if (v.some((x) => x < 0)) throw new Error('bad base64');
    const n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    if (o < out.length) out[o++] = (n >> 16) & 0xff;
    if (o < out.length) out[o++] = (n >> 8) & 0xff;
    if (o < out.length) out[o++] = n & 0xff;
  }
  return out;
}

export function toHex(bytes) {
  let s = '';
  for (const b of bytes) s += b.toString(16).padStart(2, '0');
  return s;
}

export function fromHex(hex) {
  if (hex.length % 2 || !/^[0-9a-fA-F]*$/.test(hex)) throw new Error('bad hex');
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}
