'use strict';

// The website's WebAssembly (ss-web.wasm, built from the apps' own C/C++ code – native/web): the lossless
// sound (FLAC, our own encoder and decoder – the apps' code), the direct mode's X25519 key exchange where
// WebCrypto has none, and an Opus decoder where the browser has no AudioDecoder.
//
// ScreenShareWasm.load() → Promise<{ x25519(scalar, point?), opusDecoder(channels), flac }>

(() => {
  let loading = null;

  function wrap(x) {
    x._initialize?.();
    const bytes = () => new Uint8Array(x.memory.buffer); // (anew each time: the memory may grow)
    const scratch = x.ss_malloc(96); // X25519: the result, the scalar, the point

    return {
      /** X25519 (RFC 7748): scalar · point – without a point, the base point (a public key). */
      x25519(scalar, point = null) {
        bytes().set(scalar.subarray(0, 32), scratch + 32);
        if (point) bytes().set(point.subarray(0, 32), scratch + 64);
        const ok = x.ss_x25519(scratch, scratch + 32, point ? scratch + 64 : 0);
        const out = bytes().slice(scratch, scratch + 32);
        bytes().fill(0, scratch, scratch + 96); // the private scalar does not stay in memory
        if (!ok) throw new Error('x25519');
        return out;
      },

      /** FLAC (ours, 48 kHz 24-bit): decode(frame) → { channels, pcm (interleaved) } or null; encode(pcm,
       *  channels, frames, number) → the frame's bytes (null when it does not fit). */
      flac: (() => {
        const maxFrames = 512;
        const pcm = x.ss_malloc(maxFrames * 8 * 4);
        const io = x.ss_malloc(64 * 1024);
        const chOut = x.ss_malloc(4);
        return {
          decode(frame) {
            if (frame.length > 64 * 1024) return null;
            bytes().set(frame, io);
            const n = x.ss_flac_decode(io, frame.length, pcm, maxFrames, chOut);
            if (n <= 0) return null;
            const channels = new Int32Array(x.memory.buffer, chOut, 1)[0];
            return { channels, pcm: new Float32Array(x.memory.buffer, pcm, n * channels).slice() };
          },
          encode(samples, channels, frames, number = 0) {
            if (frames > maxFrames || samples.length < frames * channels) return null;
            new Float32Array(x.memory.buffer, pcm, frames * channels).set(samples.subarray(0, frames * channels));
            const n = x.ss_flac_encode(pcm, channels, frames, io, 64 * 1024, number >>> 0);
            return n > 0 ? bytes().slice(io, io + n) : null;
          },
        };
      })(),

      /** An Opus decoder (48 kHz, 1 or 2 channels): decode(packet) → interleaved Float32Array. */
      opusDecoder(channels) {
        const state = x.ss_opus_create(channels);
        if (!state) throw new Error('opus');
        const maxFrames = 5760; // 120 ms, Opus' longest packet
        const pcm = x.ss_malloc(maxFrames * channels * 4);
        let input = 0;
        let size = 0;
        return {
          channels,
          decode(packet) {
            if (packet.length > size) {
              if (input) x.ss_free(input);
              size = Math.max(4096, packet.length);
              input = x.ss_malloc(size);
            }
            bytes().set(packet, input);
            const n = x.ss_opus_decode(state, input, packet.length, pcm, maxFrames);
            return n < 0 ? null : new Float32Array(x.memory.buffer, pcm, n * channels).slice();
          },
          close() {
            x.ss_opus_destroy(state);
            x.ss_free(pcm);
            if (input) x.ss_free(input);
          },
        };
      },
    };
  }

  async function instantiate(url) {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`ss-web.wasm: HTTP ${res.status}`);
    const { instance } = await WebAssembly.instantiate(await res.arrayBuffer(), {});
    return wrap(instance.exports);
  }

  globalThis.ScreenShareWasm = {
    load(url = '/ss-web.wasm') {
      loading ||= instantiate(url).catch((err) => {
        loading = null;
        throw err;
      });
      return loading;
    },
    wrap, // (the tests)
  };
})();
