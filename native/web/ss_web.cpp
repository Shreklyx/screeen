// Screen Share for the website – WebAssembly from the apps' own C/C++ code: the lossless sound (FLAC, our
// own encoder and decoder – native/common/lossless.cpp, the same as the apps'), and for what a browser
// may lack: the X25519 key exchange of the direct mode (BearSSL, as in native/core) and an Opus decoder
// (libopus, as in the apps). public/ss-web.js loads it; build: native/web/build.sh.
// (C headers: there is no C++ library for wasm32-wasi here, and none is needed)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "bearssl.h"
#include "opus.h"
}

#include "lossless.h"

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

EXPORT(ss_malloc) void* ss_malloc(size_t n) { return malloc(n); }
EXPORT(ss_free) void ss_free(void* p) { free(p); }

/** X25519 (RFC 7748): out = scalar · point; point null: the base point. 1 when it worked (as
 *  native/core/crypto.cpp: BearSSL takes the scalar big-endian, and a low-order point fails). */
EXPORT(ss_x25519) int ss_x25519(uint8_t* out, const uint8_t* scalar, const uint8_t* point) {
  static const uint8_t base[32] = {9};
  uint8_t g[32], be[32];
  memcpy(g, point ? point : base, 32);
  for (int i = 0; i < 32; i++) be[i] = scalar[31 - i];
  uint32_t ok = br_ec_c25519_m31.mul(g, 32, be, 32, BR_EC_curve25519);
  memset(be, 0, sizeof be);
  uint8_t any = 0;
  for (int i = 0; i < 32; i++) any |= g[i];
  if (!ok || !any) return 0;
  memcpy(out, g, 32);
  return 1;
}

/** An Opus decoder at 48 kHz, 1 or 2 channels (null when it cannot be made). */
EXPORT(ss_opus_create) OpusDecoder* ss_opus_create(int channels) {
  int err = 0;
  OpusDecoder* d = opus_decoder_create(48000, channels, &err);
  return err == OPUS_OK ? d : nullptr;
}

/** One packet → interleaved float samples; the number of samples per channel, or < 0 on an error. */
EXPORT(ss_opus_decode) int ss_opus_decode(OpusDecoder* d, const uint8_t* data, int len, float* pcm, int maxFrames) {
  return opus_decode_float(d, data, len, pcm, maxFrames, 0);
}

EXPORT(ss_opus_destroy) void ss_opus_destroy(OpusDecoder* d) { opus_decoder_destroy(d); }

/** A FLAC frame (ours) → interleaved float samples: the frames (its channels into *channels), 0 when bad. */
EXPORT(ss_flac_decode) int ss_flac_decode(const uint8_t* data, int len, float* pcm, int maxFrames, int* channels) {
  return len > 0 ? ss::lossless::decode(data, size_t(len), pcm, maxFrames, channels) : 0;
}

/** Interleaved float samples (48 kHz) → one FLAC frame: its bytes, 0 when it does not fit. */
EXPORT(ss_flac_encode) int ss_flac_encode(const float* pcm, int channels, int frames, uint8_t* out, int max, uint32_t number) {
  return int(ss::lossless::encode(pcm, channels, frames, out, max > 0 ? size_t(max) : 0, number));
}

#ifdef SS_WEB_TEST
// tests only (test/ss-web.test.js): an encoder to make packets with
EXPORT(ss_opus_encoder) OpusEncoder* ss_opus_encoder(int channels) {
  int err = 0;
  OpusEncoder* e = opus_encoder_create(48000, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  return err == OPUS_OK ? e : nullptr;
}
EXPORT(ss_opus_encode) int ss_opus_encode(OpusEncoder* e, const float* pcm, int frames, uint8_t* out, int max) {
  return opus_encode_float(e, pcm, frames, out, max);
}
#endif
