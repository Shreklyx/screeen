// The viewers' sound, shared by the native apps: Opus packets are decoded and written into a ring by
// their timestamps (each channel pair arrives separately) and played with a small buffer that grows
// only when the network makes it run dry (5–100 ms) and slowly shrinks again – the same as the
// browsers' worklet (public/direct-audio.js). Fewer speakers get a downmix.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct OpusDecoder;

namespace ss {

class AudioRing {
 public:
  AudioRing();
  ~AudioRing();
  /** One Opus packet: sub = pair | channels << 4 (as sent), ts in µs – any thread. */
  void push(int sub, const uint8_t* data, size_t n, double tsUs);
  /** Fills out (outs channels, n frames, interleaved float at 48 kHz) – the device's thread. */
  void pull(float* out, int outs, int n);
  void setVolume(float v);
  void reset();

 private:
  void configure(int channels);
  std::mutex mu_;
  int channels_ = 0;
  std::vector<std::vector<float>> ring_;
  std::vector<OpusDecoder*> decoders_;
  long long read_, end_;
  int target_ = 480;
  long long calm_ = 0;
  std::atomic<float> volume_{1.0f};
};

}  // namespace ss
