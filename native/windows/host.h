// The sharer's pipeline on Windows: a thread captures the chosen screen, and while a viewer is
// connected encodes each new image (NVENC on an NVIDIA card, the GPU's own Media Foundation encoder
// on Intel / AMD, software as the last resort) and hands it to the engine; the system sound beside.
#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "engine.h"

namespace ss {

class HostPipeline {
 public:
  struct Options {
    RECT monitor{};
    int maxWidth = 3840, maxHeight = 2160; // the picture is never larger (keeps the aspect)
    int fps = 60;
    bool audio = true;
  };
  struct Info {
    std::string encoder; // "NVENC", "Intel Quick Sync", "szoftveres"
    bool hardware = false;
    int width = 0, height = 0;
    double fps = 0, encodeMs = 0;
    std::string error;       // why the picture cannot go (empty: all right)
    std::string audioLayout; // "7.1", "5.1", "sztereó"; empty without sound
  };

  explicit HostPipeline(Engine& engine);
  ~HostPipeline();
  void start(const Options& options);
  void stop();
  /** The engine says whether anybody watches (encode only then). */
  void setStreaming(bool on);
  void keyframe();
  void setBitrate(double bps);
  Info info() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
