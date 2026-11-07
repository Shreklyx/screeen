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
