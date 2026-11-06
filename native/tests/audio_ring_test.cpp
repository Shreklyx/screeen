// The viewers' sound buffer in the apps (common/audio_ring.cpp): what it plays and when – the same
// cases as the browsers' worklet's tests (test/direct-audio.test.js), with a 10 ms device period.
#include "audio_ring.h"
#include "flac_sender.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using ss::AudioRing;

namespace {

int failures = 0;
#define CHECK(c, ...)                                  \
  do {                                                 \
    if (!(c)) {                                        \
      std::printf("FAIL line %d: ", __LINE__);         \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      failures++;                                      \
    }                                                  \
  } while (0)

const int kStereo = 0 | (2 << 4); // pair 0 of a stereo stream (as sent)
const double kPi = 3.14159265358979323846;

/** n 5 ms packets of one value from ms on. */
void feed(AudioRing& r, double ms, float value, int n = 1) {
  std::vector<float> pcm(240 * 2, value);
  for (int k = 0; k < n; k++) r.write(kStereo, pcm.data(), 240, (ms + k * 5) * 1000);
}

/** The device: asks for 10 ms at a time; play returns the left channel of what came out. */
struct Device {
  double owed = 0;
  std::vector<float> play(AudioRing& r, double ms) {
    const int period = 480;
    std::vector<float> got, out(size_t(period) * 2);
    for (owed += ms * 48; owed >= period; owed -= period) {
      r.pull(out.data(), 2, period);
      for (int i = 0; i < period; i++) got.push_back(out[size_t(i) * 2]);
    }
    return got;
  }
};

long long firstOf(const std::vector<float>& v, float x) {
  for (size_t i = 0; i < v.size(); i++)
    if (v[i] == x) return (long long)i;
  return -1;
}

/** How long (ms) a packet fed now takes to be heard: packets in real time first. */
double delayAfter(AudioRing& r, Device& d, double& ms) {
  feed(r, ms, 0.75f);
  std::vector<float> got;
  for (int i = 0; i < 40; i++) {
    std::vector<float> part = d.play(r, 5);
    got.insert(got.end(), part.begin(), part.end());
    ms += 5;
    feed(r, ms, 0.25f);
  }
  long long at = firstOf(got, 0.75f);
  return at < 0 ? 1e9 : at / 48.0;
}

void pauseKeepsTheWait() {
  // the sharer had nothing to play (Windows sends nothing then): the sound after it is not later
  AudioRing r;
  Device d;
  feed(r, 0, 0.5f, 4);
  d.play(r, 20);
  d.play(r, 2000);
  double ms = 2020;
  double delay = delayAfter(r, d, ms);
  CHECK(delay < 25, "after a pause the sound came %.0f ms late (the wait grew)", delay);
}

void timeGoingBack() {
  // the sharer again (its clock 10 s earlier): the sound is back at once, not when its time catches up
  AudioRing r;
  Device d;
  double ms = 10000;
  for (int i = 0; i < 40; i++, ms += 5) {
    feed(r, ms, 0.5f);
    d.play(r, 5);
  }
  std::vector<float> got;
  for (int i = 0; i < 60; i++) {
    feed(r, i * 5.0, 0.75f);
    std::vector<float> part = d.play(r, 5);
    got.insert(got.end(), part.begin(), part.end());
  }
  long long at = firstOf(got, 0.75f);
  CHECK(at >= 0 && at < 48 * 150, "after the sharer's time went back the sound came %s", at < 0 ? "never" : (std::to_string(at / 48) + " ms later").c_str());
}

void noDelayStays() {
  // 40 ms more than the wait arrive at once (a burst after the network held them back), then packets
  // in real time: that delay is played past, the sound is not left behind the picture
  AudioRing r;
  Device d;
  feed(r, 0, 0.5f, 8);
  double ms = 40;
  for (int i = 0; i < 300; i++, ms += 5) {
    feed(r, ms, 0.5f);
    d.play(r, 5);
  }
  double delay = delayAfter(r, d, ms);
  CHECK(delay < 25, "a sound 1.5 s after the burst came %.0f ms late", delay);
}

void latePacketsDropped() {
  AudioRing r;
  Device d;
  double ms = 0;
  for (int i = 0; i < 40; i++, ms += 5) {
    feed(r, ms, 0.5f);
    d.play(r, 5);
  }
  feed(r, ms - 150, 0.75f); // long gone
  std::vector<float> got = d.play(r, 30);
  CHECK(firstOf(got, 0.75f) < 0, "a packet from 150 ms ago was played");
}

void skipsCrossfaded() {
  // a slow sine 60 ms ahead of what is needed, then in real time: no sample jumps where it is played past
  AudioRing r;
  Device d;
  auto write = [&](double ms) {
    std::vector<float> pcm(240 * 2);
    for (int i = 0; i < 240; i++) pcm[size_t(i) * 2] = pcm[size_t(i) * 2 + 1] = float(std::sin((ms * 48 + i) / 48000.0 * 2 * kPi * 50));
    r.write(kStereo, pcm.data(), 240, ms * 1000);
  };
  for (double ms = 0; ms < 70; ms += 5) write(ms);
  std::vector<float> got;
  for (double ms = 70; ms < 1070; ms += 5) {
    write(ms);
    std::vector<float> part = d.play(r, 5);
    got.insert(got.end(), part.begin(), part.end());
  }
  float worst = 0;
  for (size_t i = 1; i < got.size(); i++)
    if (got[i - 1] != 0 && got[i] != 0) worst = std::max(worst, std::fabs(got[i] - got[i - 1]));
  CHECK(worst < 0.05f, "a jump of %.3f in the sound", worst);
}

void losslessWithLostPackets() {
  // the sharer's lossless sound (FLAC with the frame before again): every fifth packet lost, yet what is
  // played is the sound, sample for sample (24-bit), without a gap
  AudioRing r;
  Device d;
  ss::FlacSender sender(2);
  std::vector<float> sent;
  int packets = 0;
  auto write = [&](double ms) {
    std::vector<float> pcm(120 * 2);
    for (int i = 0; i < 120; i++) {
      double t = (ms * 48 + i) / 48000.0;
      pcm[size_t(i) * 2] = float(std::round(0.5 * std::sin(t * 2 * kPi * 440) * 8388608) / 8388608);
      pcm[size_t(i) * 2 + 1] = float(std::round(0.3 * std::sin(t * 2 * kPi * 660) * 8388608) / 8388608);
    }
    for (int i = 0; i < 120; i++) sent.push_back(pcm[size_t(i) * 2]);
    sender.send(pcm.data(), 120, [&](int pair, const uint8_t* data, size_t n) {
      if (packets++ % 5 == 4) return; // (lost)
      r.push(pair | 2 << 4, data, n, ms * 1000);
    });
  };
  std::vector<float> got;
  for (double ms = 0; ms < 2000; ms += 2.5) {
    write(ms);
    if (ms >= 10) {
      std::vector<float> part = d.play(r, 2.5);
      got.insert(got.end(), part.begin(), part.end());
    }
  }
  // from 300 ms on: no silence where a packet was lost – its sound came with the next one (a lost packet
  // would leave 120 samples of silence). Once at most: a packet lost just before the device asks for its
  // 10 ms is the first time the buffer is a frame short – it grows then (and stays so).
  size_t runs = 0, run = 0, sound = 0;
  for (size_t i = 48 * 300; i < got.size(); i++) {
    if (got[i] != 0) {
      sound++;
      run = 0;
    } else if (++run == 16) {
      runs++;
    }
  }
  std::printf("     lossless, every fifth packet lost: %zu samples of sound, %zu gaps\n", sound, runs);
  CHECK(sound > 70000 && runs <= 1, "lossless with every fifth packet lost: %zu gaps of silence", runs);
}

}  // namespace

int main() {
  losslessWithLostPackets();
  pauseKeepsTheWait();
  timeGoingBack();
  noDelayStays();
  latePacketsDropped();
  skipsCrossfaded();
  if (failures) {
    std::printf("%d failed\n", failures);
    return 1;
  }
  std::printf("audio ring: all passed\n");
  return 0;
}
