// Screen Share for the website – WebAssembly from the apps' own C++ code: the lossless sound (FLAC, our
// own encoder and decoder – native/common/lossless.cpp, the same as the apps') and the spatial sound for
// headphones (native/common/spatial.cpp, the apps' head model). No library: the key exchange and the
// encryption are the browser's own (WebCrypto). public/ss-web.js loads it; build: native/web/build.sh.
// (C headers: there is no C++ library for wasm32-wasi here, and none is needed)
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
