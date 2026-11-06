#include "lossless.h"

#include <string.h>

namespace ss {
namespace lossless {

namespace {

const int kFrameCap = kMaxFrames;

/** A sample as a 24-bit integer: the same scale both ways (± 2^23), clipped at full scale. */
inline int32_t toInt(float f) {
  double x = double(f) * 8388608.0;
  if (!(x == x)) return 0;
  if (x >= 8388607.0) return 8388607;
  if (x <= -8388608.0) return -8388608;
  return int32_t(x < 0 ? x - 0.5 : x + 0.5);
}

inline uint32_t mask(int n) { return n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1; }
inline uint32_t zigzag(int32_t e) { return (uint32_t(e) << 1) ^ uint32_t(e >> 31); }
inline int32_t unzigzag(uint32_t u) { return int32_t(u >> 1) ^ -int32_t(u & 1); }
inline int32_t signExtend(uint32_t v, int bits) { return bits >= 32 ? int32_t(v) : int32_t(v << (32 - bits)) >> (32 - bits); }

uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int b = 0; b < 8; b++) c = uint8_t(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}

uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= uint16_t(p[i] << 8);
    for (int b = 0; b < 8; b++) c = uint16_t(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
  }
  return c;
}

struct Writer {
  uint8_t* p;
  size_t cap, pos = 0;
  uint64_t acc = 0;
  int bits = 0;
  bool ok = true;
  Writer(uint8_t* out, size_t c) : p(out), cap(c) {}
  void put(uint32_t v, int n) {
    if (!n) return;
    acc = acc << n | (v & mask(n));
    bits += n;
    while (bits >= 8) {
      bits -= 8;
      if (pos < cap) p[pos++] = uint8_t(acc >> bits);
      else ok = false;
    }
    acc &= bits ? (uint64_t(1) << bits) - 1 : 0;
  }
  /** q zeros, then a one (FLAC's unary). */
  void unary(uint32_t q) {
    while (q >= 31) {
      put(0, 31);
      q -= 31;
    }
    put(1, int(q) + 1);
  }
  void align() {
    if (bits) put(0, 8 - bits);
  }
};

struct Reader {
  const uint8_t* p;
  size_t n, pos = 0;
  uint64_t acc = 0;
  int bits = 0;
  bool ok = true;
  Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
  uint32_t get(int k) {
    if (!k) return 0;
    while (bits < k) {
      if (pos < n) acc = acc << 8 | p[pos];
      else {
        acc <<= 8;
        ok = false;
      }
      pos++;
      bits += 8;
    }
    bits -= k;
    uint32_t v = uint32_t(acc >> bits) & mask(k);
    acc &= bits ? (uint64_t(1) << bits) - 1 : 0;
    return v;
  }
  /** Zeros up to a one: how many (a stream that ends first is bad). */
  uint32_t unary() {
    uint32_t q = 0;
    while (ok && !get(1)) q++;
    return q;
  }
  /** Where the next whole byte starts (after align). */
  size_t bytePos() const { return pos - size_t(bits / 8); }
  void align() {
    bits -= bits % 8;
    acc &= bits ? (uint64_t(1) << bits) - 1 : 0;
  }
};

/** The residual of FLAC's fixed predictor `order` at i (i ≥ order). */
inline int64_t residual(const int32_t* x, int i, int order) {
  switch (order) {
    case 0: return x[i];
    case 1: return int64_t(x[i]) - x[i - 1];
    case 2: return int64_t(x[i]) - 2 * int64_t(x[i - 1]) + x[i - 2];
    case 3: return int64_t(x[i]) - 3 * int64_t(x[i - 1]) + 3 * int64_t(x[i - 2]) - x[i - 3];
    default: return int64_t(x[i]) - 4 * int64_t(x[i - 1]) + 6 * int64_t(x[i - 2]) - 4 * int64_t(x[i - 3]) + x[i - 4];
  }
}

/** Bits a signed value takes in two's complement. */
inline int signedBits(int32_t v) {
  uint32_t m = uint32_t(v < 0 ? ~v : v);
  int b = 1;
  while (m) {
    b++;
    m >>= 1;
  }
  return b;
}

const int kMaxPartitionOrder = 4;

/** How one channel is coded (a subframe). */
struct Sub {
  int type = 1; // 0 constant, 1 verbatim, 2 fixed
  int order = 0, porder = 0;
  bool wide = false; // 5-bit Rice parameters
  int param[1 << kMaxPartitionOrder] = {};
  int raw[1 << kMaxPartitionOrder] = {}; // ≥ 0: an escaped partition, its values in that many bits
  uint64_t bits = ~uint64_t(0);
};

/** The shortest subframe for one channel of bps bits. */
Sub choose(const int32_t* x, int n, int bps) {
  Sub best;
  best.type = 1;
  best.bits = 8 + uint64_t(n) * uint64_t(bps);
  bool constant = true;
  for (int i = 1; i < n; i++)
    if (x[i] != x[0]) constant = false;
  if (constant) {
    best.type = 0;
    best.bits = 8 + uint64_t(bps);
    return best;
  }
  int32_t r[kFrameCap];
  for (int order = 0; order <= 4 && order < n; order++) {
    for (int i = order; i < n; i++) r[i] = int32_t(residual(x, i, order)); // (≤ 29 bits for 25-bit samples)
    // partitions: the block halved up to four times while each part stays longer than the warm-up
    for (int porder = 0; porder <= kMaxPartitionOrder; porder++) {
      int parts = 1 << porder;
      if (n % parts || (n >> porder) <= order) break;
      Sub s;
      s.type = 2;
      s.order = order;
      s.porder = porder;
      uint64_t bits = 8 + uint64_t(order) * uint64_t(bps) + 2 + 4;
      bool wide = false;
      for (int j = 0; j < parts; j++) {
        int from = j == 0 ? order : j * (n >> porder), to = (j + 1) * (n >> porder);
        uint64_t sum = 0;
        int need = 0;
        for (int i = from; i < to; i++) {
          sum += zigzag(r[i]);
          int b = signedBits(r[i]);
          if (b > need) need = b;
        }
        int count = to - from;
        uint64_t mean = count ? sum / uint64_t(count) : 0;
        int k0 = 0;
        while (k0 < 29 && (uint64_t(1) << (k0 + 1)) <= mean) k0++;
        uint64_t partBest = ~uint64_t(0);
        s.raw[j] = -1;
        for (int k = k0 > 0 ? k0 - 1 : 0; k <= k0 + 1 && k <= 30; k++) {
          uint64_t b = 0;
          for (int i = from; i < to; i++) b += (zigzag(r[i]) >> k) + 1 + uint64_t(k);
          if (b < partBest) {
            partBest = b;
            s.param[j] = k;
          }
        }
        // escaped: each value in `need` bits (noise, where Rice would be longer)
        uint64_t rawBits = 5 + uint64_t(count) * uint64_t(need);
        if (rawBits < partBest && need <= 31) {
          partBest = rawBits;
          s.raw[j] = need;
        }
        if (s.raw[j] < 0 && s.param[j] > 14) wide = true;
        bits += partBest;
      }
      s.wide = wide;
      bits += uint64_t(parts) * (wide ? 5 : 4);
      s.bits = bits;
      if (bits < best.bits) best = s;
    }
  }
  return best;
}

void writeSub(Writer& w, const int32_t* x, int n, int bps, const Sub& s) {
  if (s.type == 0) {
    w.put(0x00, 8);
    w.put(uint32_t(x[0]), bps);
    return;
  }
  if (s.type == 1) {
    w.put(0x02, 8);
    for (int i = 0; i < n; i++) w.put(uint32_t(x[i]), bps);
    return;
  }
  w.put(uint32_t(8 + s.order) << 1, 8);
  for (int i = 0; i < s.order; i++) w.put(uint32_t(x[i]), bps);
  w.put(s.wide ? 1 : 0, 2);
  w.put(uint32_t(s.porder), 4);
  int parts = 1 << s.porder, pbits = s.wide ? 5 : 4;
  for (int j = 0; j < parts; j++) {
    int from = j == 0 ? s.order : j * (n >> s.porder), to = (j + 1) * (n >> s.porder);
    if (s.raw[j] >= 0) {
      w.put(mask(pbits), pbits); // the escape
      w.put(uint32_t(s.raw[j]), 5);
      for (int i = from; i < to; i++) w.put(uint32_t(residual(x, i, s.order)), s.raw[j]);
      continue;
    }
    int k = s.param[j];
    w.put(uint32_t(k), pbits);
    for (int i = from; i < to; i++) {
      uint32_t u = zigzag(int32_t(residual(x, i, s.order)));
      w.unary(u >> k);
      w.put(u, k);
    }
  }
}

/** One subframe back into x (bps bits a sample); false when it is bad or not one of ours to read. */
bool readSub(Reader& r, int32_t* x, int n, int bps) {
  uint32_t h = r.get(8);
  if (h & 0x80) return false;
  int type = int(h >> 1) & 0x3F;
  int wasted = 0;
  if (h & 1) wasted = int(r.unary()) + 1;
  int full = bps;
  bps -= wasted;
  if (bps < 1 || wasted > 31) return false;
  if (type == 0) {
    int32_t v = signExtend(r.get(bps), bps);
    for (int i = 0; i < n; i++) x[i] = v;
  } else if (type == 1) {
    for (int i = 0; i < n; i++) x[i] = signExtend(r.get(bps), bps);
  } else if (type >= 8 && type <= 12) {
    int order = type - 8;
    if (order > n) return false;
    for (int i = 0; i < order; i++) x[i] = signExtend(r.get(bps), bps);
    uint32_t method = r.get(2);
    if (method > 1) return false;
    int pbits = method ? 5 : 4;
    int porder = int(r.get(4));
    int parts = 1 << porder;
    if (n % parts || (n >> porder) < order) return false;
    for (int j = 0; j < parts && r.ok; j++) {
      int from = j == 0 ? order : j * (n >> porder), to = (j + 1) * (n >> porder);
      uint32_t k = r.get(pbits);
      if (k == mask(pbits)) { // escaped: the values as they are
        int raw = int(r.get(5));
        for (int i = from; i < to; i++) x[i] = raw ? signExtend(r.get(raw), raw) : 0;
      } else {
        for (int i = from; i < to && r.ok; i++) {
          uint64_t u = uint64_t(r.unary()) << k | r.get(int(k));
          if (u > 0xFFFFFFFFu) return false; // (more than any residual of 25-bit samples)
          x[i] = unzigzag(uint32_t(u));
        }
      }
    }
    // the residuals into samples
    int64_t limit = int64_t(1) << (bps - 1);
    for (int i = order; i < n; i++) {
      int64_t e = x[i], v;
      switch (order) {
        case 0: v = e; break;
        case 1: v = e + x[i - 1]; break;
        case 2: v = e + 2 * int64_t(x[i - 1]) - x[i - 2]; break;
        case 3: v = e + 3 * int64_t(x[i - 1]) - 3 * int64_t(x[i - 2]) + x[i - 3]; break;
        default: v = e + 4 * int64_t(x[i - 1]) - 6 * int64_t(x[i - 2]) + 4 * int64_t(x[i - 3]) - x[i - 4]; break;
      }
      if (v >= limit || v < -limit) return false;
      x[i] = int32_t(v);
    }
  } else {
    return false; // (LPC: not what our encoder makes)
  }
  if (wasted)
    for (int i = 0; i < n; i++) x[i] = signExtend(uint32_t(x[i]) << wasted, full);
  return r.ok;
}

/** FLAC's "UTF-8" coded number. */
size_t putNumber(uint8_t* p, uint32_t v) {
  if (v < 0x80) {
    p[0] = uint8_t(v);
    return 1;
  }
  int extra = v < 0x800 ? 1 : v < 0x10000 ? 2 : v < 0x200000 ? 3 : v < 0x4000000 ? 4 : 5;
  static const uint8_t lead[] = {0, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};
  p[0] = uint8_t(lead[extra] | (v >> (6 * extra)));
  for (int i = 1; i <= extra; i++) p[i] = uint8_t(0x80 | ((v >> (6 * (extra - i))) & 0x3F));
  return size_t(extra) + 1;
}

/** The frame header's fields; false when it is not one we take. Sets the header's length. */
bool parseHeader(const uint8_t* d, size_t n, int* channels, int* frames, int* assign, size_t* length) {
  if (n < 6 || d[0] != 0xFF || (d[1] & 0xFE) != 0xF8) return false;
  int bsCode = d[2] >> 4, srCode = d[2] & 15, chCode = d[3] >> 4, ssCode = (d[3] >> 1) & 7;
  if (bsCode == 0 || (d[3] & 1) || chCode > 10 || (ssCode != 6 && ssCode != 0)) return false;
  if (srCode != 10 && srCode != 0) return false; // 48 kHz (or as the stream says: ours is)
  size_t pos = 4;
  uint8_t lead = d[pos];
  int extra = lead < 0x80 ? 0 : lead >= 0xFE ? -1 : lead >= 0xFC ? 5 : lead >= 0xF8 ? 4 : lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : -1;
  if (extra < 0 || pos + 1 + size_t(extra) > n) return false;
  pos += 1 + size_t(extra);
  int fr;
  if (bsCode == 1) fr = 192;
  else if (bsCode <= 5) fr = 576 << (bsCode - 2);
  else if (bsCode == 6) {
    if (pos + 1 > n) return false;
    fr = d[pos++] + 1;
  } else if (bsCode == 7) {
    if (pos + 2 > n) return false;
    fr = (d[pos] << 8 | d[pos + 1]) + 1;
    pos += 2;
  } else {
    fr = 256 << (bsCode - 8);
  }
  if (pos + 1 > n || crc8(d, pos) != d[pos]) return false;
  pos++;
  int ch = chCode < 8 ? chCode + 1 : 2;
  if (fr < 1 || fr > kFrameCap || ch > kMaxChannels) return false;
  *channels = ch;
  *frames = fr;
  *assign = chCode;
  *length = pos;
  return true;
}

}  // namespace

size_t encodeInt(const int32_t* pcm, int channels, int frames, uint8_t* out, size_t cap, uint32_t number) {
  if (channels < 1 || channels > kMaxChannels || frames < 1 || frames > kFrameCap || cap < 16) return 0;
  size_t h = 0;
  out[h++] = 0xFF;
  out[h++] = 0xF8; // fixed block size, frames numbered
  bool small = frames <= 256;
  out[h++] = uint8_t((small ? 6 : 7) << 4 | 10); // the block size at the end of the header; 48 kHz
  size_t assignAt = h++;
  h += putNumber(out + h, number & 0x7FFFFFFF);
  if (small) {
    out[h++] = uint8_t(frames - 1);
  } else {
    out[h++] = uint8_t((frames - 1) >> 8);
    out[h++] = uint8_t(frames - 1);
  }
  size_t crcAt = h++;
  Writer w(out, cap);
  w.pos = h;
  int32_t a[kFrameCap], b[kFrameCap];
  int assign = channels - 1;
  if (channels == 2) {
    // stereo: left and right as they are, or one of FLAC's side codings – whichever is shortest
    int32_t m[kFrameCap], s[kFrameCap];
    for (int i = 0; i < frames; i++) {
      a[i] = pcm[i * 2];
      b[i] = pcm[i * 2 + 1];
      m[i] = (a[i] + b[i]) >> 1;
      s[i] = a[i] - b[i];
    }
    Sub l = choose(a, frames, kBits), r = choose(b, frames, kBits), cm = choose(m, frames, kBits), cs = choose(s, frames, kBits + 1);
    uint64_t lr = l.bits + r.bits, ls = l.bits + cs.bits, sr = cs.bits + r.bits, ms = cm.bits + cs.bits;
    if (ms <= lr && ms <= ls && ms <= sr) {
      assign = 10;
      writeSub(w, m, frames, kBits, cm);
      writeSub(w, s, frames, kBits + 1, cs);
    } else if (ls <= lr && ls <= sr) {
      assign = 8;
      writeSub(w, a, frames, kBits, l);
      writeSub(w, s, frames, kBits + 1, cs);
    } else if (sr <= lr) {
      assign = 9;
      writeSub(w, s, frames, kBits + 1, cs);
      writeSub(w, b, frames, kBits, r);
    } else {
      assign = 1;
      writeSub(w, a, frames, kBits, l);
      writeSub(w, b, frames, kBits, r);
    }
  } else {
    for (int c = 0; c < channels; c++) {
      for (int i = 0; i < frames; i++) a[i] = pcm[i * channels + c];
      writeSub(w, a, frames, kBits, choose(a, frames, kBits));
    }
  }
  out[assignAt] = uint8_t(assign << 4 | 6 << 1); // the channels; 24-bit
  out[crcAt] = crc8(out, crcAt);
  w.align();
  if (!w.ok || w.pos + 2 > cap) return 0;
  uint16_t crc = crc16(out, w.pos);
  out[w.pos] = uint8_t(crc >> 8);
  out[w.pos + 1] = uint8_t(crc);
  return w.pos + 2;
}

size_t encode(const float* pcm, int channels, int frames, uint8_t* out, size_t cap, uint32_t number) {
  if (channels < 1 || channels > kMaxChannels || frames < 1 || frames > kFrameCap) return 0;
  int32_t v[kFrameCap * kMaxChannels];
  for (int i = 0; i < frames * channels; i++) v[i] = toInt(pcm[i]);
  return encodeInt(v, channels, frames, out, cap, number);
}

bool info(const uint8_t* data, size_t n, int* channels, int* frames) {
  int ch = 0, fr = 0, assign = 0;
  size_t length = 0;
  if (!parseHeader(data, n, &ch, &fr, &assign, &length)) return false;
  if (channels) *channels = ch;
  if (frames) *frames = fr;
  return true;
}

int decodeInt(const uint8_t* data, size_t n, int32_t* out, int maxFrames, int* channels) {
  int ch = 0, frames = 0, assign = 0;
  size_t length = 0;
  if (!parseHeader(data, n, &ch, &frames, &assign, &length) || frames > maxFrames || n < length + 2) return 0;
  Reader r(data + length, n - length);
  int32_t x[kFrameCap], y[kFrameCap];
  if (assign >= 8) {
    // stereo with a side channel (25 bits)
    if (!readSub(r, x, frames, assign == 9 ? kBits + 1 : kBits) || !readSub(r, y, frames, assign == 9 ? kBits : kBits + 1)) return 0;
    for (int i = 0; i < frames; i++) {
      int32_t left, right;
      if (assign == 8) { // left, side
        left = x[i];
        right = x[i] - y[i];
      } else if (assign == 9) { // side, right
        right = y[i];
        left = x[i] + y[i];
      } else { // mid, side
        int32_t mid = int32_t(uint32_t(x[i]) << 1) | (y[i] & 1);
        left = (mid + y[i]) >> 1;
        right = (mid - y[i]) >> 1;
      }
      if (left > 8388607 || left < -8388608 || right > 8388607 || right < -8388608) return 0;
      out[i * 2] = left;
      out[i * 2 + 1] = right;
    }
  } else {
    for (int c = 0; c < ch; c++) {
      if (!readSub(r, x, frames, kBits)) return 0;
      for (int i = 0; i < frames; i++) out[i * ch + c] = x[i];
    }
  }
  // the end: zeros to a whole byte, then the frame's checksum
  r.align();
  size_t end = length + r.bytePos();
  if (!r.ok || end + 2 > n) return 0;
  uint16_t crc = crc16(data, end);
  if (data[end] != uint8_t(crc >> 8) || data[end + 1] != uint8_t(crc)) return 0;
  if (channels) *channels = ch;
  return frames;
}

int decode(const uint8_t* data, size_t n, float* out, int maxFrames, int* channels) {
  int32_t v[kFrameCap * kMaxChannels];
  int ch = 0;
  int got = decodeInt(data, n, v, maxFrames < kFrameCap ? maxFrames : kFrameCap, &ch);
  for (int i = 0; i < got * ch; i++) out[i] = float(v[i]) * (1.0f / 8388608.0f);
  if (channels) *channels = ch;
  return got;
}

size_t streamHeader(int channels, int frames, uint8_t out[42]) {
  memset(out, 0, 42);
  memcpy(out, "fLaC", 4);
  out[4] = 0x80; // the last (only) metadata block: STREAMINFO, 34 bytes
  out[7] = 34;
  uint8_t* s = out + 8;
  s[0] = uint8_t(frames >> 8);
  s[1] = uint8_t(frames);
  s[2] = uint8_t(frames >> 8);
  s[3] = uint8_t(frames);
  // (the frame sizes: unknown – zeros)
  uint64_t v = uint64_t(kRate) << 44 | uint64_t(channels - 1) << 41 | uint64_t(kBits - 1) << 36;
  for (int i = 0; i < 8; i++) s[10 + i] = uint8_t(v >> (56 - 8 * i));
  return 42;
}

}  // namespace lossless
}  // namespace ss
