// Spatial sound for headphones – our own: the shared sound's channels as speakers around the listener
// (stereo at ±30°, 5.1 and 7.1 where their speakers stand), heard through the two ears of a model head:
// each ear's delay (Woodworth's formula for a sphere), the head's shadow on the far ear and the near
// ear's lift (Brown & Duda's one-pole/one-zero filter), and a little room around them (a few early
// echoes) so that the sound is out in front, not inside the head. With head tracking (AirPods on a Mac)
// the speakers stay where they are while the head turns. Plain C++ (no library): the apps and the
// website's WebAssembly share it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {
namespace spatial {

const int kMaxChannels = 8;

class Renderer {
 public:
  Renderer();
  /** The input's channels (1–8: mono, stereo, 3.0, quad, 5.0, 5.1, 6.1, 7.1 as Windows orders them). */
  void configure(int channels);
  int channels() const { return channels_; }
  /** Where the head looks (radians, right is positive): the speakers stay put, so they turn the other way. */
  void setYaw(float radians) { yaw_ = radians; }
  /** frames of `channels` interleaved → stereo interleaved (out may not be in). */
  void process(const float* in, int frames, float* out);
  void reset();

 private:
  struct Ear {
    float b0 = 1, b1 = 0, a1 = 0; // the head's shadow (or lift) on this ear
    float x1 = 0, y1 = 0;
    float delay = 0;              // samples, now (it glides to its new value)
  };
  void aim(int c, float azimuth, Ear& left, Ear& right);

  int channels_ = 2;
  float azimuth_[kMaxChannels] = {};
  bool lfe_[kMaxChannels] = {};
  float yaw_ = 0, aimedYaw_ = 1e9f;
  Ear ears_[kMaxChannels][2];
  float targets_[kMaxChannels][2] = {}; // where each ear's delay glides to
  // each channel's recent samples (the ears' delays are read from them)
  static const int kHistory = 64;
  float history_[kMaxChannels][kHistory] = {};
  int pos_ = 0;
  // the room: a few early echoes of what the speakers play, apart for the two ears
  static const int kRoom = 4096;
  float room_[kRoom] = {};
  int roomPos_ = 0;
};

}  // namespace spatial
}  // namespace ss
