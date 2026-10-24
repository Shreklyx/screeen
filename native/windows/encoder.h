// H.264 encoders for the sharer: NVENC (NVIDIA, straight from the captured texture) or a Media
// Foundation encoder (Intel Quick Sync / AMD on their own GPU, or Windows' software encoder).
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ss {

/** One encoded frame: H.264 Annex B. */
using EncodedFrame = std::function<void(const uint8_t* data, size_t n, bool key, double captureMs)>;

class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;
  /** input: the BGRA texture (w×h, on dev) that encode() reads. "" or why not. */
  virtual std::string open(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* input, int w, int h, int fps, int bps) = 0;
  /** Encodes the input texture as it is now; finished frames go to out (usually this one, at once). */
  virtual bool encode(bool key, double captureMs, const EncodedFrame& out) = 0;
  virtual void setBitrate(int bps) = 0;
  virtual std::string name() const = 0;
  virtual bool hardware() const = 0;
};

std::unique_ptr<VideoEncoder> createNvencEncoder();
/** hardware: a GPU encoder on the adapter with that LUID; otherwise Windows' software encoder. */
std::unique_ptr<VideoEncoder> createMfEncoder(LUID adapter, bool hardware);

}  // namespace ss
