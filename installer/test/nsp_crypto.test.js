// js/nsp_crypto.js against Node's OpenSSL (AES-ECB, AES-XTS, AES-CTR, SHA-256, RSA-PSS) and FIPS-197's example.
import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import {
  Aes128, aesEcb, aesXts, AesCtr, Sha256, sha256, sha256Many, pssEncode, deterministicPssSigner, rsaPssSigner,
  base64Encode, base64Decode, toHex, fromHex,
} from '../js/nsp_crypto.js';

const rnd = (n) => new Uint8Array(crypto.randomBytes(n));
const nodeCipher = (name, key, iv, data, decrypt = false) => {
  const c = (decrypt ? crypto.createDecipheriv : crypto.createCipheriv)(name, key, iv);
  c.setAutoPadding(false);
  return new Uint8Array(Buffer.concat([c.update(data), c.final()]));
};

test('AES-128 block: FIPS-197 appendix C.1', () => {
  const aes = new Aes128(fromHex('000102030405060708090a0b0c0d0e0f'));
  const ct = aes.encryptBlock(fromHex('00112233445566778899aabbccddeeff'));
  assert.equal(toHex(ct), '69c4e0d86a7b0430d8cdb78070b4c55a');
  assert.equal(toHex(aes.decryptBlock(ct)), '00112233445566778899aabbccddeeff');
});

test('AES-ECB matches OpenSSL', () => {
  for (let i = 0; i < 20; i++) {
    const key = rnd(16), data = rnd(16 * (1 + i));
    const ct = aesEcb(key, data);
    assert.deepEqual(ct, nodeCipher('aes-128-ecb', key, null, data));
    assert.deepEqual(aesEcb(key, ct, true), data);
  }
});

test('AES-XTS with big-endian sector tweaks matches OpenSSL per sector', () => {
  for (let i = 0; i < 5; i++) {
    const key = rnd(32);
    if (toHex(key.subarray(0, 16)) === toHex(key.subarray(16))) continue;
    const data = rnd(0xC00);
    const ours = aesXts(key, data);
    for (let s = 0; s < 6; s++) {
      const tweak = new Uint8Array(16);
      tweak[15] = s;
      assert.deepEqual(ours.subarray(s * 0x200, (s + 1) * 0x200), nodeCipher('aes-128-xts', key, tweak, data.subarray(s * 0x200, (s + 1) * 0x200)));
    }
    assert.deepEqual(aesXts(key, ours, { decrypt: true }), data);
  }
});

test('AES-CTR (NCA counter: upper IV, offset / 16) matches OpenSSL', async () => {
  const key = rnd(16), data = rnd(100000);
  const upper = 0x0000000500000003n;
  const offset = 0xC00 + 0x4000;
  const iv = Buffer.alloc(16);
  iv.writeBigUInt64BE(upper, 0);
  iv.writeBigUInt64BE(BigInt(offset / 16), 8);
  const ours = await new AesCtr(key).apply(upper, offset, data);
  assert.deepEqual(ours, nodeCipher('aes-128-ctr', key, iv, data));
});

test('SHA-256: incremental, one-shot and batched match OpenSSL', async () => {
  const data = rnd(300000);
  const h = new Sha256();
  for (let p = 0; p < data.length;) { const n = Math.min(data.length - p, 1 + ((p * 7919) % 70000)); h.update(data.subarray(p, p + n)); p += n; }
  const want = new Uint8Array(crypto.createHash('sha256').update(data).digest());
  assert.deepEqual(h.digest(), want);
  for (const n of [0, 1, 55, 56, 63, 64, 65, 1000]) assert.deepEqual(sha256(data.subarray(0, n)), new Uint8Array(crypto.createHash('sha256').update(data.subarray(0, n)).digest()));
  const pieces = [data.subarray(0, 0x4000), data.subarray(0x4000, 0x8000)];
  assert.deepEqual((await sha256Many(pieces)).map(toHex), pieces.map((p) => toHex(sha256(p))));
});

test('RSA-PSS: the deterministic signer and the WebCrypto signer both verify in OpenSSL', async () => {
  const { privateKey, publicKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });
  const jwk = privateKey.export({ format: 'jwk' });
  const big = (s) => BigInt(`0x${Buffer.from(s, 'base64url').toString('hex')}`);
  const message = rnd(0x200);
  const salt = rnd(32);
  const det = deterministicPssSigner({ n: big(jwk.n), d: big(jwk.d) }, salt);
  const sig = await det.sign(message);
  const verify = (key, s) => crypto.verify('sha256', message, { key, padding: crypto.constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 }, s);
  assert.ok(verify(publicKey, sig));
  assert.deepEqual(await det.sign(message), sig, 'deterministic');
  assert.equal(pssEncode(message, salt).length, 256);
  const web = await rsaPssSigner();
  const pub = crypto.createPublicKey({ key: { kty: 'RSA', n: Buffer.from(web.modulus).toString('base64url'), e: 'AQAB' }, format: 'jwk' });
  assert.ok(verify(pub, await web.sign(message)));
});

test('base64 and hex round trips match Node', () => {
  for (const n of [0, 1, 2, 3, 4, 31, 32, 33, 1000]) {
    const d = rnd(n);
    assert.equal(base64Encode(d), Buffer.from(d).toString('base64'));
    assert.deepEqual(base64Decode(base64Encode(d)), d);
    assert.deepEqual(fromHex(toHex(d)), d);
  }
});
