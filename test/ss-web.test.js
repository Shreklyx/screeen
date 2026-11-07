// The website's WebAssembly (public/ss-web.wasm, from native/web): our lossless sound (FLAC) – the apps'
// own code.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

vm.runInThisContext(fs.readFileSync(new URL('../public/ss-web.js', import.meta.url), 'utf8'));
const wasm = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(new URL('../public/ss-web.wasm', import.meta.url))), {});
const ss = globalThis.ScreenShareWasm.wrap(wasm.exports);

test('FLAC (our own, the apps’ code): encodes and decodes bit for bit (24-bit), 1, 2 and 8 channels; a damaged frame is refused', () => {
  for (const channels of [1, 2, 8]) {
    const frames = 120;
    const pcm = new Float32Array(frames * channels);
    for (let i = 0; i < frames; i++)
      for (let c = 0; c < channels; c++) pcm[i * channels + c] = Math.round(Math.sin((i + c * 7) * 0.05) * 0.6 * 8388608) / 8388608;
    const frame = ss.flac.encode(pcm, channels, frames, 3);
    assert.ok(frame && frame.length < frames * channels * 3, `${channels} channels: smaller than raw 24-bit`);
    assert.equal(frame[0], 0xff, 'a FLAC frame (its sync code)');
    const back = ss.flac.decode(frame);
    assert.equal(back.channels, channels);
    assert.deepEqual(back.pcm, pcm);
  }
  // a damaged frame: refused (its checksums)
  const frame = ss.flac.encode(new Float32Array(240).fill(0.25), 2, 120);
  frame[frame.length - 3] ^= 0x40;
  assert.equal(ss.flac.decode(frame), null);
});

/* ---- our own cryptography (native/core/crypto_core.cpp – the apps' code) as the website and the server
   run it: against the standards' vectors and against Node's own (OpenSSL) ---- */

const c = ss.crypto;
const hex = (b) => Buffer.from(b).toString('hex');
const unhex = (s) => new Uint8Array(Buffer.from(s, 'hex'));

test('our crypto in WebAssembly: SHA-256, SHA-1, HMAC, HKDF, scrypt – the standards’ vectors and OpenSSL’s answers', async () => {
  const nodeCrypto = await import('node:crypto');
  assert.equal(hex(c.sha256('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(hex(c.sha1('abc')), 'a9993e364706816aba3e25717850c26c9cd0d89d');
  assert.equal(hex(c.hmacSha256('Jefe', 'what do ya want for nothing?')), '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843');
  assert.equal(hex(c.hmacSha1('Jefe', 'what do ya want for nothing?')), 'effcdf6ae5eb2fa2d27416d5f184df9c259a7c79');
  assert.equal(
    hex(c.hkdfSha256(unhex('000102030405060708090a0b0c'), unhex('0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b'), unhex('f0f1f2f3f4f5f6f7f8f9'), 42)),
    '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865',
  );
  assert.equal(hex(c.scrypt('', '', { N: 16, r: 1, p: 1 }, 64)), '77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906');
  for (let n = 0; n < 300; n += 7) {
    const data = nodeCrypto.randomBytes(n), key = nodeCrypto.randomBytes(n % 130);
    assert.equal(hex(c.sha256(data)), nodeCrypto.createHash('sha256').update(data).digest('hex'));
    assert.equal(hex(c.sha1(data)), nodeCrypto.createHash('sha1').update(data).digest('hex'));
    assert.equal(hex(c.hmacSha256(key, data)), nodeCrypto.createHmac('sha256', key).update(data).digest('hex'));
    assert.equal(hex(c.hmacSha1(key, data)), nodeCrypto.createHmac('sha1', key).update(data).digest('hex'));
  }
  const salt = nodeCrypto.randomBytes(16);
  assert.equal(hex(c.scrypt('jelszó', salt)), nodeCrypto.scryptSync('jelszó', salt, 32).toString('hex'), 'scrypt: as the server makes its hashes');
  assert.equal(c.scrypt('x', salt, { N: 1000 }), null, 'an N that is no power of two');
});

test('our crypto in WebAssembly: X25519 (RFC 7748) and with OpenSSL; AES-128-GCM as OpenSSL, a changed byte refused', async () => {
  const nodeCrypto = await import('node:crypto');
  assert.equal(
    hex(c.x25519(unhex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a'), unhex('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f'))),
    '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742',
  );
  assert.equal(c.x25519(nodeCrypto.randomBytes(32), new Uint8Array(32)), null, 'a low-order point: refused');
  const theirs = nodeCrypto.generateKeyPairSync('x25519');
  const spki = (raw) => nodeCrypto.createPublicKey({ key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), Buffer.from(raw)]), format: 'der', type: 'spki' });
  const mine = nodeCrypto.randomBytes(32);
  const base = new Uint8Array(32);
  base[0] = 9;
  const shared = c.x25519(mine, theirs.publicKey.export({ format: 'der', type: 'spki' }).subarray(-32));
  assert.equal(hex(shared), nodeCrypto.diffieHellman({ privateKey: theirs.privateKey, publicKey: spki(c.x25519(mine, base)) }).toString('hex'));
  for (let n = 0; n < 1500; n += n < 140 ? 1 : 101) {
    const key = nodeCrypto.randomBytes(16), iv = nodeCrypto.randomBytes(12), aad = nodeCrypto.randomBytes(n % 33), pt = nodeCrypto.randomBytes(n);
    const g = c.gcm(key);
    const sealed = g.seal(iv, aad, pt);
    const e = nodeCrypto.createCipheriv('aes-128-gcm', key, iv);
    e.setAAD(aad);
    assert.equal(hex(sealed), hex(Buffer.concat([e.update(pt), e.final(), e.getAuthTag()])), `${n} bytes`);
    assert.equal(hex(g.open(iv, aad, sealed)), hex(pt));
    sealed[(n * 7) % sealed.length] ^= 4;
    assert.equal(g.open(iv, aad, sealed), null, 'changed: refused');
    g.close();
  }
});
