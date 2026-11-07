'use strict';

// The website's WebAssembly (ss-web.wasm, built from the apps' own C++ code – native/web): the lossless
// sound (FLAC, our own encoder and decoder – the apps' code), the spatial sound's head model, and our own
// cryptography (the apps' native/core/crypto_core.cpp: X25519, HKDF, AES-128-GCM, HMAC – and scrypt, which
// the server uses through the same file).
//
// ScreenShareWasm.load() → Promise<{ flac, crypto, module, bytes }>

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

      crypto: ourCrypto(x, bytes),
    };
  }

  /** Our own cryptography – synchronous: no waiting on promises between the packets. Byte arrays in and
   *  out (a string: its UTF-8). */
  function ourCrypto(x, bytes) {
    const utf8 = (v) => (typeof v === 'string' ? new TextEncoder().encode(v) : v);
    // the module's memory for the arguments: one area, grown for a bigger packet (a whole frame, over a
    // WebSocket) – and wiped after each call that held a secret
    let area = 0;
    let size = 0;
    const room = (n) => {
      if (n > size) {
        if (area) x.ss_free(area);
        size = Math.max(n, 2 * size, 64 * 1024);
        area = x.ss_malloc(size);
        if (!area) {
          size = 0;
          throw new Error('ss-web.wasm: no memory');
        }
      }
      return area;
    };
    const wipe = (n) => bytes().fill(0, area, area + n);
    const mac = (fn, length) => (key, data) => {
      key = utf8(key);
      data = utf8(data);
      const at = room(key.length + data.length + length);
      const m = bytes();
      m.set(key, at);
      m.set(data, at + key.length);
      fn(at, key.length, at + key.length, data.length, at + key.length + data.length);
      const out = bytes().slice(at + key.length + data.length, at + key.length + data.length + length);
      wipe(key.length + data.length + length);
      return out;
    };
    const hash = (fn, length) => (data) => {
      data = utf8(data);
      const at = room(data.length + length);
      bytes().set(data, at);
      fn(at, data.length, at + data.length);
      return bytes().slice(at + data.length, at + data.length + length);
    };
    return {
      /** X25519 (RFC 7748): scalar · point (32 bytes each) → 32 bytes, or null for a low-order point. */
      x25519(scalar, point) {
        const at = room(96);
        const m = bytes();
        m.set(scalar.subarray(0, 32), at + 32);
        m.set(point.subarray(0, 32), at + 64);
        const ok = x.ss_x25519(at, at + 32, at + 64);
        const out = bytes().slice(at, at + 32);
        wipe(96);
        return ok ? out : null;
      },
      sha256: hash((d, n, o) => x.ss_sha256(d, n, o), 32),
      sha1: hash((d, n, o) => x.ss_sha1(d, n, o), 20),
      hmacSha256: mac((k, kn, d, n, o) => x.ss_hmac_sha256(k, kn, d, n, o), 32),
      hmacSha1: mac((k, kn, d, n, o) => x.ss_hmac_sha1(k, kn, d, n, o), 20),
      /** HKDF-SHA256 (RFC 5869). */
      hkdfSha256(salt, ikm, info, length) {
        salt = utf8(salt ?? new Uint8Array(0));
        info = utf8(info ?? new Uint8Array(0));
        const total = salt.length + ikm.length + info.length + length;
        const at = room(total);
        const m = bytes();
        m.set(salt, at);
        m.set(ikm, at + salt.length);
        m.set(info, at + salt.length + ikm.length);
        const out = at + salt.length + ikm.length + info.length;
        if (!x.ss_hkdf_sha256(at, salt.length, at + salt.length, ikm.length, at + salt.length + ikm.length, info.length, out, length)) throw new Error('hkdf: too long');
        const result = bytes().slice(out, out + length);
        wipe(total);
        return result;
      },
      /** scrypt (RFC 7914) – null when the parameters are out of range or there is no memory for them. */
      scrypt(pass, salt, { N = 16384, r = 8, p = 1 } = {}, length = 32) {
        pass = utf8(pass);
        salt = utf8(salt);
        const total = pass.length + salt.length + length;
        const at = room(total);
        const m = bytes();
        m.set(pass, at);
        m.set(salt, at + pass.length);
        const out = at + pass.length + salt.length;
        const ok = x.ss_scrypt(at, pass.length, at + pass.length, salt.length, N, r, p, out, length);
        const result = ok ? bytes().slice(out, out + length) : null;
        wipe(total);
        return result;
      },
      /** AES-128-GCM with a key (16 bytes): seal(iv, aad, plain[, out, at]) → the ciphertext and its tag
       *  (in out at `at` when given); open(iv, aad, sealed) → the plaintext, or null when it does not
       *  open. close() wipes the key. */
      gcm(key) {
        const keyAt = room(16);
        bytes().set(key.subarray(0, 16), keyAt);
        let state = x.ss_gcm_new(keyAt);
        wipe(16);
        if (!state) throw new Error('ss-web.wasm: no memory');
        return {
          seal(iv, aad, plain, out = new Uint8Array(plain.length + 16), atOut = 0) {
            const at = room(12 + aad.length + plain.length + 16);
            const data = at + 12 + aad.length;
            const m = bytes();
            m.set(iv, at);
            m.set(aad, at + 12);
            m.set(plain, data);
            x.ss_gcm_seal(state, at, at + 12, aad.length, data, plain.length, data + plain.length);
            out.set(bytes().subarray(data, data + plain.length + 16), atOut);
            return out;
          },
          open(iv, aad, sealed) {
            if (sealed.length < 16) return null;
            const n = sealed.length - 16;
            const at = room(12 + aad.length + sealed.length);
            const data = at + 12 + aad.length;
            const m = bytes();
            m.set(iv, at);
            m.set(aad, at + 12);
            m.set(sealed, data);
            if (!x.ss_gcm_open(state, at, at + 12, aad.length, data, n, data + n)) return null;
            return bytes().slice(data, data + n);
          },
          close() {
            if (state) x.ss_gcm_free(state);
            state = 0;
          },
        };
      },
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
