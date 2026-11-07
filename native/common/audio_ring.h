// The viewers' sound, shared by the native apps: lossless (FLAC) packets are decoded and written into a ring by
// their timestamps (each channel pair arrives separately) and played with a small buffer that grows
// only when the network makes it run dry (5–100 ms) and slowly shrinks again; what it holds beyond
// that is played past, so no delay stays – the same as the browsers' worklet (public/direct-audio.js).
// Fewer speakers get a downmix.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "spatial.h"

namespace ss {

class AudioRing {
 public:
  /** In a packet's sub (pair | channels << 4): the pair's sound is lossless (FLAC, see lossless.h), with
   *  the frame before it again. */
  static const int kLossless = 8;
  AudioRing();
  ~AudioRing();
  /** One sound packet (FLAC, see kLossless): sub = pair | channels << 4 (as sent), ts in µs – any thread. */
  void push(int sub, const uint8_t* data, size_t n, double tsUs);
  /** Sound already decoded (interleaved, the pair's channels) – what push does next; tests. */
  void write(int sub, const float* pcm, int frames, double tsUs);
  /** Fills out (outs channels, n frames, interleaved float at 48 kHz) – the device's thread. */
  void pull(float* out, int outs, int n);
  void setVolume(float v);
  /** How many channels the sound has (as sent; 0 before the first packet). */
  int sourceChannels() {
    std::lock_guard<std::mutex> l(mu_);
    return channels_;
  }
  /** Spatial sound for headphones (spatial.h): the channels as speakers around the listener, to the
   *  device's first two channels. */
  void setSpatial(bool on) { spatialOn_ = on; }
  bool spatial() const { return spatialOn_; }
  /** Where the head looks (head tracking; radians, right positive): the speakers stay put. */
  void setYaw(float radians) { yaw_ = radians; }
  void reset();

 private:
  void configure(int channels);
  void place(int pair, int pc, const float* pcm, int frames, double tsUs);
  void restart(long long at);
  std::mutex mu_;
  int channels_ = 0;
  std::vector<std::vector<float>> ring_;
  long long read_, end_;
  int target_ = 240;
  int lastPacket_ = 120; // frames a packet brings (the least wait: a device period and one of these)
  long long calm_ = 0;   // samples played since it last ran dry
  long long since_ = -1; // samples since it ran dry (-1: long enough ago)
  long long dryLen_ = 0; // for how long it was dry
  long long dryEnd_ = 0; // where the sound ended then
  int dryTarget_ = 240;  // the wait then
  int behind_ = 0;       // packets far too late in a row
  long long low_ = 0, lowFor_ = 0; // the lowest the buffer got in this window
  std::atomic<float> volume_{1.0f};
  std::atomic<bool> spatialOn_{false};
  std::atomic<float> yaw_{0.0f};
  spatial::Renderer spatial_;
  std::vector<float> spatialIn_, spatialOut_;
};

}  // namespace ss
