// The website's WebAssembly (public/ss-web.wasm, from native/web): X25519 as RFC 7748 and WebCrypto
// have it, and the Opus decoder giving back what was encoded.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';

vm.runInThisContext(fs.readFileSync(new URL('../public/ss-web.js', import.meta.url), 'utf8'));
const wasm = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(new URL('../public/ss-web.wasm', import.meta.url))), {});
const ss = globalThis.ScreenShareWasm.wrap(wasm.exports);

const hex = (s) => Uint8Array.from(Buffer.from(s, 'hex'));
const toHex = (b) => Buffer.from(b).toString('hex');

test('X25519: the RFC 7748 vectors', () => {
  const alice = hex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a');
  const bob = hex('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb');
  const alicePub = ss.x25519(alice);
  const bobPub = ss.x25519(bob);
  assert.equal(toHex(alicePub), '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
  assert.equal(toHex(bobPub), 'de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f');
  const shared = '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742';
  assert.equal(toHex(ss.x25519(alice, bobPub)), shared);
  assert.equal(toHex(ss.x25519(bob, alicePub)), shared);
});

test('X25519: agrees with WebCrypto (one side each)', async () => {
  const subtle = webcrypto.subtle;
  for (let i = 0; i < 8; i++) {
    const kp = await subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
    const theirPub = new Uint8Array(await subtle.exportKey('raw', kp.publicKey));
    const mine = webcrypto.getRandomValues(new Uint8Array(32));
    const myPub = ss.x25519(mine);
    const fromWasm = ss.x25519(mine, theirPub);
    const other = await subtle.importKey('raw', myPub, { name: 'X25519' }, false, []);
    const fromWebCrypto = new Uint8Array(await subtle.deriveBits({ name: 'X25519', public: other }, kp.privateKey, 256));
    assert.equal(toHex(fromWasm), toHex(fromWebCrypto));
  }
});

test('Opus: 2.5 ms stereo packets decode to the tones they were made of', () => {
  const { packets } = JSON.parse(fs.readFileSync(new URL('./data/opus-stereo-2.5ms.json', import.meta.url), 'utf8'));
  const dec = ss.opusDecoder(2);
  const left = [];
  const right = [];
  for (const p of packets) {
    const pcm = dec.decode(Uint8Array.from(Buffer.from(p, 'base64')));
    assert.ok(pcm && pcm.length === 240, 'a packet is 120 stereo samples');
    for (let i = 0; i < pcm.length; i += 2) {
      left.push(pcm[i]);
      right.push(pcm[i + 1]);
    }
  }
  dec.close();
  // the tone's strength at its frequency vs at another one (after the codec's start-up)
  const power = (x, hz) => {
    let re = 0;
    let im = 0;
    for (let i = 2400; i < x.length; i++) {
      re += x[i] * Math.cos((2 * Math.PI * hz * i) / 48000);
      im += x[i] * Math.sin((2 * Math.PI * hz * i) / 48000);
    }
    return Math.hypot(re, im) / (x.length - 2400);
  };
  assert.ok(power(left, 1000) > 0.2, `left 1 kHz: ${power(left, 1000)}`);
  assert.ok(power(right, 440) > 0.2, `right 440 Hz: ${power(right, 440)}`);
  assert.ok(power(left, 440) < 0.02 && power(right, 1000) < 0.02, 'the channels stay apart');
});

test('Opus: a broken packet is an error, not a crash', () => {
  const dec = ss.opusDecoder(2);
  assert.equal(dec.decode(new Uint8Array([0xff, 0xff, 0xff])), null);
  dec.close();
});
