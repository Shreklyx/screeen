// Screen Share for the website – WebAssembly from the apps' own C++ code: the lossless sound (FLAC, our
// own encoder and decoder – native/common/lossless.cpp, the same as the apps'), the spatial sound for
// headphones (native/common/spatial.cpp, the apps' head model) and our own cryptography
// (native/core/crypto_core.cpp, the apps' too: X25519, HKDF, ChaCha20-Poly1305 and AES-128-GCM, HMAC – the
// server's scrypt).
// No library. public/ss-web.js loads it (the server too: server/own-crypto.js); build: native/web/build.sh.
// (C headers: there is no C++ library for wasm32-wasi here, and none is needed)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_core.h"
#include "lossless.h"
#include "spatial.h"

// (no C++ library here: placement new by hand, for the renderer in memory from malloc)
inline void* operator new(size_t, void* p) noexcept { return p; }

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

EXPORT(ss_malloc) void* ss_malloc(size_t n) { return malloc(n); }
EXPORT(ss_free) void ss_free(void* p) { free(p); }

/** A FLAC frame (ours) → interleaved float samples: the frames (its channels into *channels), 0 when bad. */
EXPORT(ss_flac_decode) int ss_flac_decode(const uint8_t* data, int len, float* pcm, int maxFrames, int* channels) {
  return len > 0 ? ss::lossless::decode(data, size_t(len), pcm, maxFrames, channels) : 0;
}

/** Interleaved float samples (48 kHz) → one FLAC frame: its bytes, 0 when it does not fit. */
EXPORT(ss_flac_encode) int ss_flac_encode(const float* pcm, int channels, int frames, uint8_t* out, int max, uint32_t number) {
  return int(ss::lossless::encode(pcm, channels, frames, out, max > 0 ? size_t(max) : 0, number));
}

/** Spatial sound for headphones (native/common/spatial.h – the apps' own): a renderer for `channels`. */
EXPORT(ss_spatial_create) ss::spatial::Renderer* ss_spatial_create(int channels) {
  void* m = malloc(sizeof(ss::spatial::Renderer));
  if (!m) return nullptr;
  ss::spatial::Renderer* r = new (m) ss::spatial::Renderer();
  r->configure(channels);
  return r;
}
EXPORT(ss_spatial_yaw) void ss_spatial_yaw(ss::spatial::Renderer* r, float radians) { r->setYaw(radians); }
/** frames of its channels (interleaved) → stereo (interleaved). */
EXPORT(ss_spatial_process) void ss_spatial_process(ss::spatial::Renderer* r, const float* in, int frames, float* out) { r->process(in, frames, out); }
EXPORT(ss_spatial_destroy) void ss_spatial_destroy(ss::spatial::Renderer* r) { free(r); }

/* ---- our own cryptography (native/core/crypto_core.h – the apps' code; portable: WebAssembly has no AES
   instructions, so AES runs bitsliced on its SIMD, eight blocks at once – in constant time) */

namespace sc = ss::crypto;

/** X25519 (RFC 7748): out = scalar · point; 0 for a low-order point. */
EXPORT(ss_x25519) int ss_x25519(uint8_t* out, const uint8_t* scalar, const uint8_t* point) { return sc::x25519(out, scalar, point) ? 1 : 0; }
EXPORT(ss_sha256) void ss_sha256(const uint8_t* data, int n, uint8_t* out) { sc::sha256(data, size_t(n), out); }
EXPORT(ss_sha1) void ss_sha1(const uint8_t* data, int n, uint8_t* out) { sc::sha1(data, size_t(n), out); }
EXPORT(ss_hmac_sha256) void ss_hmac_sha256(const uint8_t* key, int keyLen, const uint8_t* data, int n, uint8_t* out) {
  sc::hmacSha256(key, size_t(keyLen), data, size_t(n), out);
}
EXPORT(ss_hmac_sha1) void ss_hmac_sha1(const uint8_t* key, int keyLen, const uint8_t* data, int n, uint8_t* out) {
  sc::hmacSha1(key, size_t(keyLen), data, size_t(n), out);
}
EXPORT(ss_hkdf_sha256) int ss_hkdf_sha256(const uint8_t* salt, int saltLen, const uint8_t* ikm, int ikmLen, const uint8_t* info, int infoLen, uint8_t* out, int outLen) {
  return sc::hkdfSha256(salt, size_t(saltLen), ikm, size_t(ikmLen), info, size_t(infoLen), out, size_t(outLen)) ? 1 : 0;
}
EXPORT(ss_scrypt) int ss_scrypt(const uint8_t* pass, int passLen, const uint8_t* salt, int saltLen, uint32_t N, uint32_t r, uint32_t p, uint8_t* out, int outLen) {
  return sc::scrypt(pass, size_t(passLen), salt, size_t(saltLen), N, r, p, out, size_t(outLen)) ? 1 : 0;
}
/** AES-128-GCM: a key's state (ss_gcm_free wipes it), then seal / open in place (iv: 12 bytes, tag: 16). */
EXPORT(ss_gcm_new) sc::Gcm* ss_gcm_new(const uint8_t* key) {
  sc::Gcm* g = static_cast<sc::Gcm*>(malloc(sizeof(sc::Gcm)));
  if (g) sc::gcmInit(*g, key);
  return g;
}
EXPORT(ss_gcm_seal) void ss_gcm_seal(const sc::Gcm* g, const uint8_t* iv, const uint8_t* aad, int aadLen, uint8_t* data, int n, uint8_t* tag) {
  sc::gcmSeal(*g, iv, aad, size_t(aadLen), data, size_t(n), tag);
}
EXPORT(ss_gcm_open) int ss_gcm_open(const sc::Gcm* g, const uint8_t* iv, const uint8_t* aad, int aadLen, uint8_t* data, int n, const uint8_t* tag) {
  return sc::gcmOpen(*g, iv, aad, size_t(aadLen), data, size_t(n), tag) ? 1 : 0;
}
/** ChaCha20-Poly1305 (RFC 8439): a 32-byte key, a 12-byte nonce; seal / open in place. */
EXPORT(ss_chacha_seal) void ss_chacha_seal(const uint8_t* key, const uint8_t* iv, const uint8_t* aad, int aadLen, uint8_t* data, int n, uint8_t* tag) {
  sc::chachaSeal(key, iv, aad, size_t(aadLen), data, size_t(n), tag);
}
EXPORT(ss_chacha_open) int ss_chacha_open(const uint8_t* key, const uint8_t* iv, const uint8_t* aad, int aadLen, uint8_t* data, int n, const uint8_t* tag) {
  return sc::chachaOpen(key, iv, aad, size_t(aadLen), data, size_t(n), tag) ? 1 : 0;
}
EXPORT(ss_gcm_free) void ss_gcm_free(sc::Gcm* g) {
  if (!g) return;
  sc::wipe(g, sizeof *g);
  free(g);
}
