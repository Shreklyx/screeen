// The viewer's picture on Windows: Media Foundation's H.264 decoder in low-latency mode (on the GPU
// through DXVA when it can), and a Direct3D 11 flip-model swap chain that shows every decoded frame
// the moment it is ready – no frame queue, tearing allowed, at most one frame of latency.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace ss {

class VideoPlayer {
 public:
  struct Stats {
    int width = 0, height = 0;
    double fps = 0;         // frames shown per second
    double latencyMs = -1;  // capture on the sharer → on this screen (median of the last second)
    bool hardware = false;  // decoded on the GPU
  };

  /** window: a child window the picture fills (letterboxed). */
  explicit VideoPlayer(HWND window);
  ~VideoPlayer();
  VideoPlayer(const VideoPlayer&) = delete;
  VideoPlayer& operator=(const VideoPlayer&) = delete;

  /** The sharer's clock minus ours (from the engine), for the latency. */
  std::function<double()> clockOffset;
  /** The decoder cannot go on without a keyframe. */
  std::function<void()> needKeyframe;

  /** An encoded frame (H.264 Annex B) – any thread. */
  void push(const uint8_t* data, size_t n, bool key, double captureMs);
  /** The window changed size. */
  void resized();
  /** Forgets the stream (a new connection starts with a keyframe). */
  void reset();
  Stats stats() const;
  /** A picture is on the screen (the window can be shown). */
  bool hasPicture() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
