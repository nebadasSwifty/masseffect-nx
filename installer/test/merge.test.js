import test from 'node:test';
import assert from 'node:assert/strict';
import { mergeDiscFiles } from '../js/source.js';

const pkg = new Uint8Array([0x9e, 0x2a, 0x83, 0xc1, 1, 2, 3, 4]);
const junk = new Uint8Array([0x49, 0x51, 0xf2, 0x7d, 9, 9, 9, 9]);
const bik = new Uint8Array([0x42, 0x49, 0x4b, 0x69, 0, 0, 0, 0]);
const e = (path, bytes, tag) => ({ path, size: bytes.length, blob: new Blob([bytes]), tag });

async function pick(files, path) {
  return files.find((f) => f.path === path);
}

test('a placeholder on a later disc does not replace a real package', async () => {
  const { files, replacedByEarlier } = await mergeDiscFiles([
    { name: 'Disc1.iso', files: [e('Layer0/Maps/BIOA_WAR00.xxx', pkg, 'd1')] },
    { name: 'Disc2.iso', files: [e('Layer0/Maps/BIOA_WAR00.xxx', junk, 'd2')] },
  ]);
  assert.equal(replacedByEarlier, 1);
  assert.equal((await pick(files, 'Layer0/Maps/BIOA_WAR00.xxx')).blob.size, pkg.length);
  const head = new Uint8Array(await (await pick(files, 'Layer0/Maps/BIOA_WAR00.xxx')).blob.slice(0, 4).arrayBuffer());
  assert.deepEqual([...head], [0x9e, 0x2a, 0x83, 0xc1]);
});

test('a real package on a later disc replaces a placeholder of an earlier disc', async () => {
  const { files } = await mergeDiscFiles([
    { name: 'Disc1.iso', files: [e('Layer0/Maps/BIOA_LOS10.xxx', junk)] },
    { name: 'Disc2.iso', files: [e('Layer0/Maps/BIOA_LOS10.xxx', pkg)] },
  ]);
  const head = new Uint8Array(await (await pick(files, 'Layer0/Maps/BIOA_LOS10.xxx')).blob.slice(0, 4).arrayBuffer());
  assert.equal(head[0], 0x9e);
});

test('movies: the Bink copy wins over a placeholder', async () => {
  const { files } = await mergeDiscFiles([
    { name: 'Disc1.iso', files: [e('Layer1/Movies/UplinkSEQ02.bik', pkg)] },
    { name: 'Disc2.iso', files: [e('Layer1/Movies/UplinkSEQ02.bik', bik)] },
  ]);
  const head = new Uint8Array(await (await pick(files, 'Layer1/Movies/UplinkSEQ02.bik')).blob.slice(0, 3).arrayBuffer());
  assert.deepEqual([...head], [0x42, 0x49, 0x4b]);
});

test('both valid or unknown type: the first disc wins', async () => {
  const pkg2 = new Uint8Array([0x9e, 0x2a, 0x83, 0xc1, 7, 7]);
  const { files } = await mergeDiscFiles([
    { name: 'Disc1.iso', files: [e('Layer1/Maps/BIOA_UNC10.xxx', pkg), e('a/readme.txt', junk)] },
    { name: 'Disc2.iso', files: [e('Layer1/Maps/BIOA_UNC10.xxx', pkg2), e('a/readme.txt', bik)] },
  ]);
  assert.equal((await pick(files, 'Layer1/Maps/BIOA_UNC10.xxx')).size, pkg.length);
  assert.equal((await pick(files, 'a/readme.txt')).blob.size, junk.length);
});

test('a valid-looking package on a later disc does not replace the first disc copy (BIOA_WAR00 case)', async () => {
  const ilosCut = new Uint8Array([0x9e, 0x2a, 0x83, 0xc1, 4, 4, 4, 4]);
  const { files } = await mergeDiscFiles([
    { name: 'Disc1.iso', files: [e('Layer0/Maps/BIOA_WAR00.xxx', pkg)] },
    { name: 'Disc2.iso', files: [e('Layer0/Maps/BIOA_WAR00.xxx', ilosCut)] },
  ]);
  const got = new Uint8Array(await (await pick(files, 'Layer0/Maps/BIOA_WAR00.xxx')).blob.arrayBuffer());
  assert.deepEqual([...got], [...pkg]);
});
