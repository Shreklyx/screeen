// Lossless sound (native/common/lossless.cpp – FLAC, our own encoder and decoder): every frame comes back
// bit for bit – music-like sound, noise, silence, full scale, 1–8 channels – it takes less than the raw
// 24 bits, and a damaged frame is refused (its checksums).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lossless.h"

using namespace ss;

static const double kPi = 3.14159265358979323846;

static int failures = 0;
#define CHECK(cond, what)                     \
  do {                                        \
    if (cond) std::printf("ok   %s\n", what); \
    else {                                    \
      std::printf("FAIL %s\n", what);         \
      failures++;                             \
    }                                         \
  } while (0)

static uint32_t rng = 12345;
static int32_t rnd(int bits) {
  rng = rng * 1664525u + 1013904223u;
  return int32_t(rng >> (32 - bits)) - (1 << (bits - 1));
}

/** Encodes and decodes; true when bit for bit. Adds the packet's size to *bytes. */
static bool roundTrip(const std::vector<int32_t>& pcm, int channels, size_t* bytes) {
  int frames = int(pcm.size()) / channels;
  std::vector<uint8_t> packet(lossless::maxPacket(channels, frames));
  size_t n = lossless::encodeInt(pcm.data(), channels, frames, packet.data(), packet.size());
  if (!n) return false;
  if (bytes) *bytes += n;
  std::vector<int32_t> back(pcm.size());
  int ch = 0;
  int got = lossless::decodeInt(packet.data(), n, back.data(), frames, &ch);
  return got == frames && ch == channels && back == pcm;
}

int main() {
  const int F = 120; // 2.5 ms
  // music-like: a few sines with a slow envelope, stereo apart a little
  {
    size_t bytes = 0;
    bool all = true;
    for (int p = 0; p < 400; p++) { // a second
      std::vector<int32_t> pcm(F * 2);
      for (int i = 0; i < F; i++) {
        double t = (p * F + i) / 48000.0;
        double env = 0.5 + 0.4 * std::sin(t * 2.1);
        double l = env * (0.4 * std::sin(2 * kPi * 220 * t) + 0.2 * std::sin(2 * kPi * 660 * t + 0.3) + 0.1 * std::sin(2 * kPi * 1870 * t));
        double r = env * (0.4 * std::sin(2 * kPi * 220 * t + 0.2) + 0.2 * std::sin(2 * kPi * 660 * t) + 0.08 * std::sin(2 * kPi * 2450 * t));
        pcm[size_t(i * 2)] = int32_t(std::lround(l * 8388607)) + rnd(4);
        pcm[size_t(i * 2 + 1)] = int32_t(std::lround(r * 8388607)) + rnd(4);
      }
      all = all && roundTrip(pcm, 2, &bytes);
    }
    CHECK(all, "music-like stereo: bit for bit");
    double kbps = bytes * 8 / 1000.0;
    std::printf("     %.0f kbit/s (raw 24-bit stereo: 2304)\n", kbps);
    CHECK(kbps < 1600, "music-like stereo: smaller than raw");
  }
  // silence: a byte a channel
  {
    size_t bytes = 0;
    CHECK(roundTrip(std::vector<int32_t>(F * 2, 0), 2, &bytes) && bytes <= 20, "silence: a few bytes");
  }
  // white noise at full scale, the extremes, a square wave (escapes)
  {
    std::vector<int32_t> noise(F * 2), edge(F * 2), square(F * 2);
    for (size_t i = 0; i < noise.size(); i++) {
      noise[i] = rnd(24);
      edge[i] = (i / 2) % 2 ? 8388607 : -8388608;
      square[i] = (i / 16) % 2 ? 8388607 : -8388608;
    }
    CHECK(roundTrip(noise, 2, nullptr), "full-scale noise: bit for bit");
    CHECK(roundTrip(edge, 2, nullptr) && roundTrip(square, 2, nullptr), "the extremes, a square wave: bit for bit");
  }
  // 1–8 channels, odd sizes
  {
    bool all = true;
    for (int ch = 1; ch <= 8; ch++)
      for (int frames : {1, 2, 3, 4, 7, 120, 240, 480, 512}) {
        std::vector<int32_t> pcm(size_t(frames * ch));
        for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (i % 3 ? rnd(12) : rnd(24)) / int32_t(i % 5 + 1);
        all = all && roundTrip(pcm, ch, nullptr);
      }
    CHECK(all, "1–8 channels, 1–512 frames: bit for bit");
  }
  // floats in, floats out (what the apps do): 24-bit exact, clipped at full scale
  {
    std::vector<float> in(F * 2), out(F * 2);
    for (int i = 0; i < F * 2; i++) in[size_t(i)] = float(std::sin(i * 0.05) * 0.9);
    in[0] = 1.5f;
    in[1] = -2.0f;
    std::vector<uint8_t> packet(lossless::maxPacket(2, F));
    size_t n = lossless::encode(in.data(), 2, F, packet.data(), packet.size());
    int ch = 0;
    int got = lossless::decode(packet.data(), n, out.data(), F, &ch);
    double worst = 0;
    for (int i = 2; i < F * 2; i++) worst = std::max(worst, double(std::fabs(out[size_t(i)] - in[size_t(i)])));
    CHECK(got == F && ch == 2 && worst < 1.0 / 8388608 && out[0] > 0.99f && out[1] <= -0.99f, "floats: within one 24-bit step, clipped at full scale");
  }
  // a damaged packet is refused (never read past its end)
  {
    std::vector<int32_t> pcm(F * 2);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = rnd(20);
    std::vector<uint8_t> packet(lossless::maxPacket(2, F));
    size_t n = lossless::encodeInt(pcm.data(), 2, F, packet.data(), packet.size());
    std::vector<int32_t> back(pcm.size());
    int ch = 0;
    bool refused = lossless::decodeInt(packet.data(), n / 2, back.data(), F, &ch) == 0 && lossless::decodeInt(packet.data(), 3, back.data(), F, &ch) == 0;
    packet[0] = 0;
    refused = refused && lossless::decodeInt(packet.data(), n, back.data(), F, &ch) == 0;
    CHECK(refused, "a cut or foreign packet is refused");
  }
  std::printf(failures ? "SOME FAILED\n" : "lossless: all passed\n");
  return failures ? 1 : 0;
}
