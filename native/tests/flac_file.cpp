// Lossless sound: a .flac file of our own encoder's frames (a second of tones, silence and full-scale
// bursts) and the same samples raw (s32le, as ffmpeg gives 24-bit) – CI decodes the file with ffmpeg and
// compares: our FLAC is standard FLAC.   flac_file <channels> <out.flac> <out.raw>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lossless.h"

using namespace ss;

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  int ch = std::atoi(argv[1]);
  const int F = 120;
  FILE* f = std::fopen(argv[2], "wb");
  FILE* raw = std::fopen(argv[3], "wb");
  if (!f || !raw || ch < 1 || ch > 8) return 2;
  uint8_t header[42];
  lossless::streamHeader(ch, F, header);
  std::fwrite(header, 1, sizeof header, f);
  uint32_t rng = 1;
  for (int p = 0; p < 400; p++) {
    std::vector<int32_t> pcm(size_t(F * ch));
    for (int i = 0; i < F; i++)
      for (int c = 0; c < ch; c++) {
        double t = (p * F + i) / 48000.0;
        double v = 0.5 * std::sin(2 * M_PI * (220 + 110 * c) * t) * (0.6 + 0.4 * std::sin(t * 3)) + (p % 50 == 7 ? (i % 2 ? 0.99 : -0.99) : 0);
        rng = rng * 1664525u + 1013904223u;
        int32_t s = int32_t(std::lround(v * 8388000)) + int32_t(rng >> 28) - 8;
        s = s > 8388607 ? 8388607 : s < -8388608 ? -8388608 : s;
        if (p >= 100 && p < 120) s = 0; // silence
        pcm[size_t(i * ch + c)] = s;
        int32_t left = int32_t(uint32_t(s) << 8);
        std::fwrite(&left, 4, 1, raw);
      }
    std::vector<uint8_t> out(lossless::maxPacket(ch, F));
    size_t n = lossless::encodeInt(pcm.data(), ch, F, out.data(), out.size(), uint32_t(p));
    if (!n) return 1;
    std::fwrite(out.data(), 1, n, f);
  }
  std::fclose(f);
  std::fclose(raw);
  return 0;
}
