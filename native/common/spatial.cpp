#include "spatial.h"

#include <math.h>
#include <string.h>

namespace ss {
namespace spatial {

namespace {

const float kPi = 3.14159265358979f;
const float kRate = 48000;
const float kHead = 0.0875f / 343.0f; // the head's radius over the speed of sound (s)
const float kBeta = kRate * kHead;    // the shadow filter's corner in samples (fs / ω0, ω0 = c / a)
const float kAlphaMin = 0.1f, kThetaMin = 150.0f / 180.0f * kPi;

float deg(float d) { return d / 180.0f * kPi; }

/** Each channel's speaker (degrees, right positive; 999: the LFE – no direction) as Windows orders them. */
void layout(int channels, float* az, bool* lfe) {
  static const float k1[] = {0}, k2[] = {-30, 30}, k3[] = {-30, 30, 0}, k4[] = {-30, 30, -110, 110}, k5[] = {-30, 30, 0, -110, 110},
                     k6[] = {-30, 30, 0, 999, -110, 110}, k7[] = {-30, 30, 0, 999, 180, -100, 100}, k8[] = {-30, 30, 0, 999, -150, 150, -100, 100};
  static const float* all[] = {k1, k2, k3, k4, k5, k6, k7, k8};
  const float* l = all[channels - 1];
  for (int c = 0; c < channels; c++) {
    lfe[c] = l[c] > 900;
    az[c] = lfe[c] ? 0 : deg(l[c]);
  }
}

/** A channel's level in the mix (as a downmix weighs them: the centre and the surrounds a little less). */
float weight(int channels, int c, bool lfe) {
  if (lfe) return 0.5f;
  if (channels <= 2) return 1.0f;
  return c < 2 ? 1.0f : 0.7071f;
}

/** Soft limiting above 0.9 (several channels at full scale must not clip hard). */
float soft(float x) { return x > 0.9f ? 0.9f + 0.1f * tanhf((x - 0.9f) / 0.1f) : x < -0.9f ? -0.9f + 0.1f * tanhf((x + 0.9f) / 0.1f) : x; }

// the room: early echoes (ms, gain) for each ear – apart, so that the two ears hear a space
const float kEchoMs[2][4] = {{7.3f, 11.9f, 17.1f, 23.7f}, {8.1f, 13.3f, 19.9f, 26.3f}};
const float kEchoGain[2][4] = {{0.20f, -0.15f, 0.11f, -0.08f}, {-0.19f, 0.14f, -0.10f, 0.08f}};

}  // namespace

Renderer::Renderer() { configure(2); }

void Renderer::configure(int channels) {
  channels_ = channels < 1 ? 1 : channels > kMaxChannels ? kMaxChannels : channels;
  layout(channels_, azimuth_, lfe_);
  aimedYaw_ = 1e9f;
  reset();
}

void Renderer::reset() {
  memset(history_, 0, sizeof history_);
  memset(room_, 0, sizeof room_);
  for (auto& pair : ears_)
    for (Ear& e : pair) e.x1 = e.y1 = 0;
  aimedYaw_ = 1e9f;
}

/** One channel's speaker as the two ears hear it from where the head looks now. */
void Renderer::aim(int c, float azimuth, Ear& left, Ear& right) {
  for (int side = 0; side < 2; side++) {
    Ear& e = side ? right : left;
    if (lfe_[c]) { // the LFE: both ears the same, no delay, no shadow
      e.b0 = 1;
      e.b1 = 0;
      e.a1 = 0;
      e.delay = targets_[c][side] = kHead * kRate;
      continue;
    }
    // the angle between where the sound comes from and this ear's axis (0 … π)
    float ear = side ? kPi / 2 : -kPi / 2;
    float t = fabsf(azimuth - ear);
    while (t > 2 * kPi) t -= 2 * kPi;
    if (t > kPi) t = 2 * kPi - t;
    // the head's shadow (Brown & Duda): a one-pole/one-zero filter, its zero by the angle
    float alpha = (1 + kAlphaMin / 2) + (1 - kAlphaMin / 2) * cosf(t / kThetaMin * kPi);
    float a0 = 1 + kBeta;
    e.b0 = (1 + alpha * kBeta) / a0;
    e.b1 = (1 - alpha * kBeta) / a0;
    e.a1 = (1 - kBeta) / a0;
    // the delay to this ear (Woodworth): shorter on the near side, around the head on the far one
    float tau = t < kPi / 2 ? -kHead * cosf(t) : kHead * (t - kPi / 2);
    float target = (tau + kHead) * kRate; // (≥ 0: from the nearest an ear can be)
    if (aimedYaw_ > 1e8f) e.delay = target; // (the first time: there at once; then it glides – process)
    targets_[c][side] = target;
  }
}

void Renderer::process(const float* in, int frames, float* out) {
  if (fabsf(yaw_ - aimedYaw_) > 0.002f) {
    for (int c = 0; c < channels_; c++) aim(c, azimuth_[c] - yaw_, ears_[c][0], ears_[c][1]);
    aimedYaw_ = yaw_;
  }
  const int echoes = 4;
  int echoAt[2][4];
  for (int s = 0; s < 2; s++)
    for (int k = 0; k < echoes; k++) echoAt[s][k] = int(kEchoMs[s][k] * kRate / 1000);
  float scale = channels_ <= 2 ? 0.75f : 0.6f;
  for (int i = 0; i < frames; i++) {
    pos_ = (pos_ + 1) & (kHistory - 1);
    float ears[2] = {0, 0}, dry = 0;
    for (int c = 0; c < channels_; c++) {
      float x = in[i * channels_ + c] * weight(channels_, c, lfe_[c]);
      history_[c][pos_] = x;
      dry += lfe_[c] ? 0 : x;
      for (int s = 0; s < 2; s++) {
        Ear& e = ears_[c][s];
        // the delay glides to where the head turned (a step at once would click)
        float target = targets_[c][s];
        float d = e.delay + (target - e.delay > 0.05f ? 0.05f : target - e.delay < -0.05f ? -0.05f : target - e.delay);
        e.delay = d;
        int whole = int(d);
        float frac = d - float(whole);
        float a = history_[c][(pos_ - whole) & (kHistory - 1)], b = history_[c][(pos_ - whole - 1) & (kHistory - 1)];
        float delayed = a + (b - a) * frac;
        float y = e.b0 * delayed + e.b1 * e.x1 - e.a1 * e.y1;
        e.x1 = delayed;
        e.y1 = y;
        ears[s] += y;
      }
    }
    // the room: early echoes of the speakers' sum, different for the two ears
    roomPos_ = (roomPos_ + 1) & (kRoom - 1);
    room_[roomPos_] = dry;
    for (int s = 0; s < 2; s++)
      for (int k = 0; k < echoes; k++) ears[s] += kEchoGain[s][k] * room_[(roomPos_ - echoAt[s][k]) & (kRoom - 1)];
    out[i * 2] = soft(ears[0] * scale);
    out[i * 2 + 1] = soft(ears[1] * scale);
  }
}

}  // namespace spatial
}  // namespace ss
