'use strict';

// The website's WebAssembly (ss-web.wasm, built from the apps' own C++ code – native/web): the lossless
// sound (FLAC, our own encoder and decoder – the apps' code) and the spatial sound's head model.
//
// ScreenShareWasm.load() → Promise<{ flac, module, bytes }>

(() => {
  let loading = null;

  function wrap(x) {
    x._initialize?.();
    const bytes = () => new Uint8Array(x.memory.buffer); // (anew each time: the memory may grow)

    return {
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
    };
  }

  async function instantiate(url) {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`ss-web.wasm: HTTP ${res.status}`);
    const bytes = await res.arrayBuffer();
    const module = await WebAssembly.compile(bytes);
    const instance = await WebAssembly.instantiate(module, {});
    // (the module: the sound's worklet makes its own – or from the bytes, where a browser cannot send it there)
    return Object.assign(wrap(instance.exports), { module, bytes });
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
