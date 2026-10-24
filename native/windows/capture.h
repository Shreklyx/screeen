// Screen capture on Windows: DXGI Desktop Duplication hands over a new desktop image the moment
// Windows composes it; the image stays on the GPU – copied, the mouse pointer drawn in, scaled to
// the sent size with one draw – ready for the encoder (BGRA texture).
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <memory>
#include <string>
#include <vector>

#include "gpu.h"

namespace ss {

struct Monitor {
  std::wstring name; // "1. képernyő (2560×1440)"
  RECT rect;         // desktop coordinates, physical pixels
  bool primary;
};
std::vector<Monitor> listMonitors();

class ScreenCapture {
 public:
  ScreenCapture();
  ~ScreenCapture();
  /** "" or why not: display-not-found, d3d11, access-denied, busy, duplication-failed, rotated-display, shaders. */
  std::string open(const RECT& monitor, int maxW, int maxH);
  void close();
  /** Waits up to timeoutMs: 1 a new image (or pointer move), 0 nothing new, -1 lost (open again), -2 error. */
  int acquire(unsigned timeoutMs, double& captureMs);
  /** Draws the last image (scaled, with the pointer) into frame(). */
  void render();
  ID3D11Device* device() const;
  ID3D11DeviceContext* context() const;
  ID3D11Texture2D* frame() const;
  int width() const;
  int height() const;
  int sourceWidth() const;
  int sourceHeight() const;
  bool nvidia() const;
  LUID adapterLuid() const;
  std::string adapterName() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
