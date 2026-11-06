// The sharer's lossless sound (FLAC, see lossless.h), a channel pair at a time as Opus went: each pair's
// frame with the pair's frame before it again – a lost packet's sound comes in the next one – while both
// fit in one packet of the direct transport. What goes out: [this frame's length (2 bytes)][this frame]
// [the one before]; its pair is marked lossless (AudioRing::kLossless).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lossless.h"

namespace ss {

class FlacSender {
 public:
  static const int kLosslessPair = 8;     // (AudioRing::kLossless)
  static const size_t kMaxPayload = 1100; // one packet of the direct transport (its room: 1138 bytes)

  explicit FlacSender(int channels) : channels_(std::max(1, std::min(lossless::kMaxChannels, channels))), last_(size_t((channels_ + 1) / 2)) {}

  /** `frames` frames of every channel (interleaved floats): fn(pair | kLosslessPair, data, n) for each pair. */
  template <typename Fn>
  void send(const float* pcm, int frames, Fn fn) {
    if (frames < 1 || frames > lossless::kMaxFrames) return;
    float pairPcm[lossless::kMaxFrames * 2];
    for (int p = 0; p < int(last_.size()); p++) {
      int ch = std::min(2, channels_ - p * 2);
      for (int i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) pairPcm[i * ch + c] = pcm[i * channels_ + p * 2 + c];
      size_t room = lossless::maxPacket(ch, frames);
      out_.resize(2 + room + kMaxPayload);
      size_t n = lossless::encode(pairPcm, ch, frames, out_.data() + 2, room, number_);
      if (!n) continue;
      out_[0] = uint8_t(n);
      out_[1] = uint8_t(n >> 8);
      size_t total = 2 + n;
      std::vector<uint8_t>& prev = last_[size_t(p)];
      if (!prev.empty() && total + prev.size() <= kMaxPayload) {
        std::memcpy(out_.data() + total, prev.data(), prev.size());
        total += prev.size();
      }
      prev.assign(out_.data() + 2, out_.data() + 2 + n);
      fn(p | kLosslessPair, out_.data(), total);
    }
    number_++;
  }

 private:
  int channels_;
  std::vector<std::vector<uint8_t>> last_; // each pair's frame before
  std::vector<uint8_t> out_;
  uint32_t number_ = 0;
};

}  // namespace ss
