// The sharer's pipeline on macOS: ScreenCaptureKit hands over each new image of the display (scaled
// and converted to YUV on the GPU), VideoToolbox encodes that surface on the media engine (low-latency
// rate control, no B-frames, keyframes on request); the system sound (macOS 13+) as Opus beside.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"

namespace ss {

struct MacDisplay {
  uint32_t id;
  std::string name; // "1. képernyő (2560×1440)"
  int width, height;
};
std::vector<MacDisplay> listDisplays();

class MacHost {
 public:
  struct Options {
    uint32_t display = 0; // 0: the main display
    int maxWidth = 3840, maxHeight = 2160;
    int fps = 60;
    bool audio = true;
  };
  struct Info {
    std::string encoder = "VideoToolbox";
    bool hardware = true;
    int width = 0, height = 0;
    double fps = 0, encodeMs = 0;
    std::string error;
    std::string audioLayout;
    int audioBuffers = 0, audioPackets = 0; // what ScreenCaptureKit gave / what went out
  };

  explicit MacHost(Engine& engine);
  ~MacHost();
  void start(const Options& options);
  void stop();
  void setStreaming(bool on);
  void keyframe();
  void setBitrate(double bps);
  Info info() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace ss
