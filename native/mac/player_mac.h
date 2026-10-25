// The viewer's picture and sound on macOS: VideoToolbox decodes each frame the moment it arrives
// (hardware, real-time) and the image (an IOSurface) becomes the view's layer contents at once;
// the sound goes through the shared AudioRing to Core Audio with a small output buffer.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#ifdef __OBJC__
@class CALayer;
#else
typedef struct objc_object CALayer;
#endif

namespace ss {

class MacPlayer {
 public:
  struct Stats {
    int width = 0, height = 0;
    double fps = 0;
    double latencyMs = -1;
    bool hardware = true;
    bool metal = false; // drawn with Metal (its shader compiled), not handed to Core Animation
  };
  explicit MacPlayer(CALayer* layer);
  ~MacPlayer();
  std::function<double()> clockOffset; // the engine's; read in stats()
  std::function<void()> needKeyframe;
  /** An encoded frame (H.264 Annex B) – any thread. */
  void push(const uint8_t* data, size_t n, bool key, double captureMs);
  /** No more calls to clockOffset / needKeyframe after this returns (before the engine goes). */
  void detach();
  Stats stats() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

class MacAudioOut {
 public:
  MacAudioOut();
  ~MacAudioOut();
  void push(int sub, const uint8_t* data, size_t n, double tsUs);
  void setVolume(float v);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
