// Spatial sound (native/common/spatial.cpp): a speaker on one side is louder (in the highs) and earlier
// in that ear; one in front is the same in both; turning the head moves the speakers the other way;
// nothing grows without end.
#include <cmath>
#include <cstdio>
#include <vector>

#include "spatial.h"

using ss::spatial::Renderer;

static int failures = 0;
#define CHECK(cond, ...)               \
  do {                                 \
    if (cond) std::printf("ok   ");    \
    else {                             \
      std::printf("FAIL ");            \
      failures++;                      \
    }                                  \
    std::printf(__VA_ARGS__);          \
    std::printf("\n");                 \
  } while (0)

/** Energy of each ear for a tone in one channel of `channels`, the head turned by yaw. */
static void ears(int channels, int channel, double hz, float yaw, double& left, double& right) {
  Renderer r;
  r.configure(channels);
  r.setYaw(yaw);
  const int n = 48000;
  std::vector<float> in(size_t(n * channels), 0.0f), out(size_t(n * 2));
  for (int i = 0; i < n; i++) in[size_t(i * channels + channel)] = float(0.3 * std::sin(2 * M_PI * hz * i / 48000));
  r.process(in.data(), n, out.data());
  left = right = 0;
  for (int i = n / 2; i < n; i++) {
    left += out[size_t(i * 2)] * out[size_t(i * 2)];
    right += out[size_t(i * 2 + 1)] * out[size_t(i * 2 + 1)];
  }
}

/** The sample where each ear first hears an impulse in one channel. */
static void arrival(int channels, int channel, int& left, int& right) {
  Renderer r;
  r.configure(channels);
  std::vector<float> in(size_t(200 * channels), 0.0f), out(400);
  in[size_t(10 * channels + channel)] = 1;
  r.process(in.data(), 200, out.data());
  left = right = -1;
  for (int i = 0; i < 200; i++) {
    if (left < 0 && std::fabs(out[size_t(i * 2)]) > 0.02f) left = i;
    if (right < 0 && std::fabs(out[size_t(i * 2 + 1)]) > 0.02f) right = i;
  }
}

int main() {
  double l, r;
  ears(2, 0, 4000, 0, l, r);
  CHECK(l > r * 2, "stereo left speaker at 4 kHz: the left ear %.1f dB louder", 10 * std::log10(l / r));
  ears(2, 1, 4000, 0, l, r);
  CHECK(r > l * 2, "stereo right speaker: the right ear %.1f dB louder", 10 * std::log10(r / l));
  ears(1, 0, 4000, 0, l, r);
  CHECK(std::fabs(10 * std::log10(l / r)) < 0.5, "mono in front: both ears the same (%.2f dB)", 10 * std::log10(l / r));
  ears(8, 7, 4000, 0, l, r);
  CHECK(r > l * 4, "7.1 right side speaker: the right ear %.1f dB louder", 10 * std::log10(r / l));
  ears(1, 0, 4000, float(M_PI / 2), l, r);
  CHECK(l > r * 4, "the head turned right: the front speaker is on the left (%.1f dB)", 10 * std::log10(l / r));
  int al, ar;
  arrival(2, 1, al, ar);
  CHECK(ar >= 0 && al > ar && al - ar <= 32, "the right speaker reaches the right ear first (%d samples earlier)", al - ar);
  // full scale on every channel of 7.1: bounded, no NaN
  Renderer big;
  big.configure(8);
  std::vector<float> in(8 * 4800, 1.0f), out(2 * 4800);
  for (size_t i = 0; i < in.size(); i++) in[i] = (i / 8) % 50 < 25 ? 1.0f : -1.0f;
  big.process(in.data(), 4800, out.data());
  float peak = 0;
  bool finite = true;
  for (float x : out) {
    peak = std::fmax(peak, std::fabs(x));
    finite = finite && std::isfinite(x);
  }
  CHECK(finite && peak <= 1.0f, "7.1 at full scale: at most %.2f, no NaN", peak);
  std::printf(failures ? "SOME FAILED\n" : "spatial: all passed\n");
  return failures ? 1 : 0;
}
