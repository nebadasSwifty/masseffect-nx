import test from 'node:test';
import assert from 'node:assert/strict';
import { parseProdKeys, KeyFileError, forgetKeys, romfsHash, romfsTableCount, estimateNspBytes, NSP_BROWSER_READY } from '../js/nsp.js';

const hex = (n, byte) => byte.repeat(n);

test('parseProdKeys keeps only the two keys the NSP needs', () => {
  const text = [
    '; comment',
    `master_key_00 = ${hex(16, 'ab')}`,
    `HEADER_KEY = ${hex(16, '01')}${hex(16, '02')}`,
    `key_area_key_application_00=${hex(16, 'cd')}\r`,
    `titlekek_00 = ${hex(16, 'ef')}`,
  ].join('\n');
  const keys = parseProdKeys(text);
  assert.deepEqual(Object.keys(keys).sort(), ['header_key', 'key_area_key_application_00']);
  assert.equal(keys.header_key.length, 32);
  assert.equal(keys.header_key[0], 1);
  assert.equal(keys.header_key[31], 2);
  assert.equal(keys.key_area_key_application_00[15], 0xcd);
  forgetKeys(keys);
  assert.ok(keys.header_key.every((b) => b === 0));
});

test('parseProdKeys names what is missing, never the values', () => {
  const secret = hex(16, '7f');
  assert.throws(() => parseProdKeys(`header_key = ${secret}`), (e) => {
    assert.ok(e instanceof KeyFileError);
    assert.deepEqual(e.missing, ['header_key', 'key_area_key_application_00']);
    assert.ok(!e.message.includes(secret));
    return true;
  });
  // equal halves = not a real header key
  assert.throws(() => parseProdKeys(`header_key = ${hex(32, '11')}\nkey_area_key_application_00 = ${hex(16, '22')}`),
    (e) => e.missing.includes('header_key'));
  assert.throws(() => parseProdKeys(''), KeyFileError);
});

test('RomFS hash and table size match tools/build_full_nsp.py', () => {
  const enc = (s) => new TextEncoder().encode(s);
  // Vectors computed with build_full_nsp.romfs_hash.
  assert.equal(romfsHash(0, enc('')), 123456789);
  assert.equal(romfsHash(0, enc('game_root')), 1284110737);
  assert.equal(romfsHash(0x18, enc('default.xex')), 3304091053);
  assert.equal(romfsHash(0x1234, enc('BIOA_PRO10.xxx')), 1219144704);
  assert.equal(romfsHash(0x7fffffff, enc('Ж')), 983443868);
  assert.deepEqual([0, 2, 3, 18, 19, 20, 100].map(romfsTableCount), [3, 3, 3, 19, 19, 23, 101]);
});

test('estimateNspBytes is close to a real packed size', () => {
  // tools/build_full_nsp.py on the English edition + both DLC: 3,110 files, 8,614,845,467 bytes of payload,
  // Program NCA 8,693,450,752 bytes (NRO 61,476,051 bytes).
  const est = estimateNspBytes({ payloadBytes: 8614845467, fileCount: 3110, nroBytes: 61476051 });
  assert.ok(est > 8693450752 && est < 8693450752 * 1.002, String(est));
  assert.equal(NSP_BROWSER_READY, true);
});
