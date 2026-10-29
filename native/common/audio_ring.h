// The viewers' sound, shared by the native apps: Opus packets are decoded and written into a ring by
// their timestamps (each channel pair arrives separately) and played with a small buffer that grows
// only when the network makes it run dry (5–100 ms) and slowly shrinks again; what it holds beyond
// that is played past, so no delay stays – the same as the browsers' worklet (public/direct-audio.js).
// Fewer speakers get a downmix.
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
  /** Sound already decoded (interleaved, the pair's channels) – what push does next; tests. */
  void write(int sub, const float* pcm, int frames, double tsUs);
  /** Fills out (outs channels, n frames, interleaved float at 48 kHz) – the device's thread. */
  void pull(float* out, int outs, int n);
  void setVolume(float v);
  void reset();

 private:
  void configure(int channels);
  void place(int pair, int pc, const float* pcm, int frames, double tsUs);
  void restart(long long at);
  std::mutex mu_;
  int channels_ = 0;
  std::vector<std::vector<float>> ring_;
  std::vector<OpusDecoder*> decoders_;
  long long read_, end_;
  int target_ = 480;
  long long calm_ = 0;   // samples played since it last ran dry
  long long since_ = -1; // samples since it ran dry (-1: long enough ago)
  long long dryLen_ = 0; // for how long it was dry
  long long dryEnd_ = 0; // where the sound ended then
  int dryTarget_ = 480;  // the wait then
  int behind_ = 0;       // packets far too late in a row
  long long low_ = 0, lowFor_ = 0; // the lowest the buffer got in this window
  std::atomic<float> volume_{1.0f};
};

}  // namespace ss
