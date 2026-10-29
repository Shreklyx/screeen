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
const int kFade = 96;                          // 2 ms: playing past what is too much is crossfaded
const int kWindow = kRate / 2;                 // the buffer's lowest level is looked at every half second
const float K = 0.70710678f;
size_t wrap(long long a) { return size_t(((a % kRing) + kRing) % kRing); }
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
  calm_ = 0;
  since_ = -1;
  dryLen_ = dryEnd_ = 0;
  dryTarget_ = target_;
  behind_ = 0;
  low_ = LLONG_MAX;
  lowFor_ = 0;
}

void AudioRing::restart(long long at) {
  for (std::vector<float>& r : ring_) std::fill(r.begin(), r.end(), 0.0f);
  read_ = at - target_;
  end_ = at;
  low_ = LLONG_MAX;
  lowFor_ = 0;
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
  place(pair, pc, pcm.data(), frames, tsUs);
}

void AudioRing::write(int sub, const float* pcm, int frames, double tsUs) {
  int ch = std::max(1, std::min(8, sub >> 4));
  int pair = sub & 15;
  if (pair * 2 >= ch || frames <= 0) return;
  std::lock_guard<std::mutex> l(mu_);
  if (ch != channels_) configure(ch);
  place(pair, std::min(2, ch - pair * 2), pcm, frames, tsUs);
}

void AudioRing::place(int pair, int pc, const float* pcm, int frames, double tsUs) {
  if (!std::isfinite(tsUs)) return;
  long long at = llround(tsUs * kRate / 1e6);
  if (read_ == LLONG_MIN) read_ = at - target_;
  if (at + frames <= read_) {
    // too late, already played – but many far behind in a row: the sharer's time went back (it
    // started again), so from these on (before, the sound stayed away until its time caught up)
    if (at + frames > read_ - kMaxTarget - kRate / 10 || ++behind_ < 20) return;
    restart(at);
    since_ = -1;
  }
  behind_ = 0;
  if (since_ >= 0) {
    // sound after it ran dry (while dry and a little after: a burst can come out of order) – how late?
    // One from inside the gap (the network held it back) is as late as it is into the gap: the wait
    // grows by that much (the one that waited longest counts). One from after the gap (nothing was sent
    // – the sharer had nothing to play – or it was lost) is not late: no reason to wait longer
    // (before, a pause made the wait 100 ms for minutes)
    long long late = std::min(since_, dryEnd_ + since_ - at);
    if (late > 96) target_ = int(std::max<long long>(target_, std::min<long long>(kMaxTarget, dryTarget_ + late)));
  }
  if (at - read_ > kRing - 2048) restart(at); // far ahead (clock jump): start over
  for (int c = 0; c < pc; c++) {
    std::vector<float>& r = ring_[size_t(pair * 2 + c)];
    for (int i = 0; i < frames; i++) {
      long long a = at + i;
      if (a >= read_) r[wrap(a)] = pcm[size_t(i * pc + c)];
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
    // ran dry: since when, and where the sound ended, is kept (the packets that come decide what it was)
    if (since_ < 0) {
      since_ = dryLen_ = 0;
      dryEnd_ = end_;
      dryTarget_ = target_;
    }
    since_ += n;
    dryLen_ += n;
    calm_ = 0;
    low_ = LLONG_MAX;
    lowFor_ = 0;
    if (buffered <= 0) read_ = end_ - target_;
    return;
  }
  if (since_ >= 0 && (since_ += n) > dryLen_ + 2400) since_ = -1; // (50 ms on, what comes is not from the gap)
  // more than the wait needs – a burst after the network held packets back, the clocks' drift, a
  // wait that shrank – is played past: at once when much, else what the buffer never went below in
  // the last half second (before, up to 50 ms more stayed for good: the sound behind the picture)
  long long skip = 0;
  low_ = std::min(low_, buffered);
  lowFor_ += n;
  if (buffered > target_ + 2400) skip = buffered - target_;
  else if (lowFor_ >= kWindow && low_ > target_ + 240) skip = low_ - target_;
  if (skip > 0 || lowFor_ >= kWindow) { // (a new window: what was before a skip says nothing now)
    low_ = LLONG_MAX;
    lowFor_ = 0;
  }
  skip = std::max(0LL, std::min(skip, buffered - n));
  calm_ += n;
  if (calm_ > kRate * 3 && target_ > kMinTarget) {
    target_ -= 48;
    calm_ = 0;
  }
  float v = volume_;
  int ch = channels_;
  for (int i = 0; i < n; i++) {
    size_t idx = wrap(read_ + skip + i), from = wrap(read_ + i);
    // (from where it was to where it goes, without a click)
    float w = skip && i < kFade ? (float(i) + 0.5f) / kFade : 1.0f;
    auto s = [&](int c) {
      if (c >= ch) return 0.0f;
      const std::vector<float>& r = ring_[size_t(c)];
      return w < 1 ? r[from] * (1 - w) + r[idx] * w : r[idx];
    };
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
  }
  // never play a sample twice (nor one played past)
  for (long long a = read_; a < read_ + skip + n; a++) {
    size_t idx = wrap(a);
    for (int c = 0; c < ch; c++) ring_[size_t(c)][idx] = 0;
  }
  read_ += skip + n;
}

void AudioRing::setVolume(float v) { volume_ = std::max(0.0f, std::min(1.0f, v)); }

void AudioRing::reset() {
  std::lock_guard<std::mutex> l(mu_);
  configure(std::max(1, channels_));
}

}  // namespace ss
