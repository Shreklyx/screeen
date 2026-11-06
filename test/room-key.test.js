// The room's own key (public/room-key.js): the same values as the apps' core (native/tests/core_test.cpp),
// computed apart with Python's hmac – the website and the apps prove the key to each other the same way.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

// as the page loads it: a plain script that puts RoomKey on the global object
const context = { crypto: globalThis.crypto, TextEncoder, Uint8Array, String, btoa };
context.globalThis = context;
vm.runInNewContext(fs.readFileSync(new URL('../public/room-key.js', import.meta.url), 'utf8'), context);
const { base32, unbase32, parseRoomKey, e2eProof, e2eProofOk } = context.RoomKey;

const key = Uint8Array.from({ length: 16 }, (_, i) => i);
const pub = 'QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIzNDU=';

test('the key in a link: base32 both ways, from the link or code.key', () => {
  assert.equal(base32(key), 'aaaqeayeaudaocajbifqydiob4');
  assert.deepEqual(unbase32('aaaqeayeaudaocajbifqydiob4'), key);
  assert.deepEqual(parseRoomKey('https://mozizas.hu/#abc123.aaaqeayeaudaocajbifqydiob4'), key);
  assert.deepEqual(parseRoomKey('abc123.AAAQEAYEAUDAOCAJBIFQYDIOB4'), key, '(typed in capitals)');
  assert.equal(parseRoomKey('https://mozizas.hu/#abc123'), null);
  assert.equal(parseRoomKey('abc123.aaaq'), null);
});

test('the proofs: the same as the apps', async () => {
  assert.equal(await e2eProof(key, 'host', '0011223344556677', pub), 'UK0D35ordS81pSx+UUzQHQ==');
  assert.equal(await e2eProof(key, 'viewer', '0011223344556677', pub), 'svWv4TCw/ObLMyKUqLi+xg==');
  assert.equal(await e2eProofOk(key, 'host', '0011223344556677', pub, 'UK0D35ordS81pSx+UUzQHQ=='), true);
  assert.equal(await e2eProofOk(key, 'viewer', '0011223344556677', pub, 'UK0D35ordS81pSx+UUzQHQ=='), false, 'a sharer proof is not a viewer proof');
  const other = key.slice();
  other[0] ^= 1;
  assert.equal(await e2eProofOk(other, 'host', '0011223344556677', pub, 'UK0D35ordS81pSx+UUzQHQ=='), false, 'another key');
  assert.equal(await e2eProofOk(key, 'host', '0011223344556678', pub, 'UK0D35ordS81pSx+UUzQHQ=='), false, 'another session');
  assert.equal(await e2eProofOk(key, 'host', '0011223344556677', pub, undefined), false, 'none');
});
