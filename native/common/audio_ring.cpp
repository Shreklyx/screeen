#include "audio_ring.h"

#include <opus.h>

#include <algorithm>
#include <climits>
#include <cmath>

namespace ss {

namespace {
const int kRate = 48000;
const int kRing = kRate; // one second
const int kMinTarget = 240, kMaxTarget = 4800; // 5 … 100 ms
const float K = 0.70710678f;
float soft(float x) { return x > 0.9f ? 0.9f + 0.1f * std::tanh((x - 0.9f) / 0.1f) : x < -0.9f ? -0.9f + 0.1f * std::tanh((x + 0.9f) / 0.1f) : x; }
}  // namespace

AudioRing::AudioRing() : read_(LLONG_MIN), end_(LLONG_MIN) {}

AudioRing::~AudioRing() {
  for (OpusDecoder* d : decoders_) if (d) opus_decoder_destroy(d);
}

void AudioRing::configure(int ch) {
  for (OpusDecoder* d : decoders_) if (d) opus_decoder_destroy(d);
  decoders_.assign(size_t((ch + 1) / 2), nullptr);
  channels_ = ch;
  ring_.assign(size_t(ch), std::vector<float>(kRing, 0.0f));
  read_ = end_ = LLONG_MIN;
  target_ = 480; // 10 ms to start with
}

void AudioRing::push(int sub, const uint8_t* data, size_t n, double tsUs) {
  int ch = std::max(1, std::min(8, sub >> 4));
  int pair = sub & 15;
  if (pair * 2 >= ch) return;
  std::lock_guard<std::mutex> l(mu_);
  if (ch != channels_) configure(ch);
  int pc = std::min(2, ch - pair * 2);
  OpusDecoder*& dec = decoders_[size_t(pair)];
  if (!dec) {
    int err = 0;
    dec = opus_decoder_create(kRate, pc, &err);
    if (!dec) return;
  }
  static thread_local std::vector<float> pcm(5760 * 2);
  int frames = opus_decode_float(dec, data, opus_int32(n), pcm.data(), 5760, 0);
  if (frames <= 0) return;
  long long at = llround(tsUs * kRate / 1e6);
  if (read_ == LLONG_MIN) read_ = at - target_;
  if (at + frames <= read_) return; // too late, already played
  if (at - read_ > kRing - 2048) read_ = at - target_; // far ahead (clock jump): start over
  for (int c = 0; c < pc; c++) {
    std::vector<float>& r = ring_[size_t(pair * 2 + c)];
    for (int i = 0; i < frames; i++) {
      long long a = at + i;
      if (a >= read_) r[size_t(((a % kRing) + kRing) % kRing)] = pcm[size_t(i * pc + c)];
    }
  }
  if (at + frames > end_) end_ = at + frames;
}

void AudioRing::pull(float* out, int outs, int n) {
  std::fill(out, out + size_t(n) * size_t(outs), 0.0f);
  std::lock_guard<std::mutex> l(mu_);
  if (read_ == LLONG_MIN || !channels_) return;
  long long buffered = end_ - read_;
  if (buffered < n) {
    // ran dry: wait a little longer next time
    target_ = std::min(kMaxTarget, target_ + 240);
    calm_ = 0;
    if (buffered <= 0) read_ = end_ - target_;
    return;
  }
  if (buffered > target_ + 2400) read_ = end_ - target_; // the buffer grew (clock drift): catch up
  calm_ += n;
  if (calm_ > kRate * 5 && target_ > kMinTarget) {
    target_ -= 48;
    calm_ = 0;
  }
  float v = volume_;
  int ch = channels_;
  for (int i = 0; i < n; i++) {
    size_t idx = size_t((((read_ + i) % kRing) + kRing) % kRing);
    auto s = [&](int c) { return c < ch ? ring_[size_t(c)][idx] : 0.0f; };
    float* o = out + size_t(i) * size_t(outs);
    if (ch == 1) {
      for (int k = 0; k < std::min(outs, 2); k++) o[k] = s(0);
    } else if (ch <= 2 || outs >= ch) {
      for (int k = 0; k < std::min(outs, ch); k++) o[k] = s(k);
    } else if (outs >= 6) {
      // 7.1 → 5.1: sides folded into the surrounds
      o[0] = s(0);
      o[1] = s(1);
      o[2] = s(2);
      o[3] = s(3);
      o[4] = soft(K * (s(4) + s(6)));
      o[5] = soft(K * (s(5) + s(7)));
    } else {
      o[0] = soft(s(0) + K * s(2) + K * s(4) + K * s(6));
      if (outs > 1) o[1] = soft(s(1) + K * s(2) + K * s(5) + K * s(7));
    }
    for (int k = 0; k < outs; k++) o[k] *= v;
    for (int c = 0; c < ch; c++) ring_[size_t(c)][idx] = 0; // never play a sample twice
  }
  read_ += n;
}

void AudioRing::setVolume(float v) { volume_ = std::max(0.0f, std::min(1.0f, v)); }

void AudioRing::reset() {
  std::lock_guard<std::mutex> l(mu_);
  configure(std::max(1, channels_));
}

}  // namespace ss
