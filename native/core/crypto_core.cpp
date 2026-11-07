// Screen Share – our own cryptography: the standard algorithms direct mode uses, written from their
// specifications, no library. (Standard algorithms on purpose: a cipher of one's own invention would be the
// weak point; our own code for well-studied ones is not.) Plain bytes, no C++ library (crypto_core.h): the
// same code runs in the apps, in the website's WebAssembly (native/web) and in the server.
//
//   X25519             RFC 7748 – a constant-time Montgomery ladder over 16-bit limbs
//   SHA-256, SHA-1     FIPS 180-4;  MD5  RFC 1321;  HMAC  RFC 2104;  HKDF  RFC 5869;  scrypt  RFC 7914
//   AES-128-GCM        FIPS 197 + NIST SP 800-38D (96-bit nonce, 128-bit tag) – between two apps
//   ChaCha20-Poly1305  RFC 8439 – with a browser (no AES instructions there: several times faster)
//
// AES runs on the CPU's own AES instructions (x86 AES-NI, ARMv8's AES – every Apple silicon Mac), eight
// counter blocks at a time, and GHASH on its carry-less multiply (PCLMULQDQ, PMULL), eight blocks to one
// reduction with three multiplies a block: several GB/s on one core, in constant time. Where there are no
// such instructions (an x86 from before ~2011; WebAssembly) the same algorithms run in portable code that
// is constant-time too: AES bitsliced – on SIMD vectors where the compiler has them (SSE2, NEON,
// WebAssembly's SIMD), eight blocks at once – and GHASH with integer multiplies. Nothing branches on a
// secret or looks a table up by one (CRC-32's table only sees STUN messages, which are public; scrypt's
// memory is read by a value from the password – its memory-hardness, as the standard has it).
// tests/crypto_test.cpp checks it all against the standards' test vectors and against OpenSSL.
#include "crypto_core.h"

#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#define SS_X86 1
#include <immintrin.h>
#include <wmmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define SS_TARGET(t)
#else
#include <cpuid.h>
#define SS_TARGET(t) __attribute__((target(t)))
#endif
#elif defined(__aarch64__) && (defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO))
#define SS_ARM 1
#include <arm_neon.h>
#endif

namespace ss {
namespace crypto {
namespace {

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t load32be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }
inline uint32_t load32le(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void store32be(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void store32le(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}
inline void store64be(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i));
}

inline uint64_t load64le(const uint8_t* p) {
  return uint64_t(p[0]) | uint64_t(p[1]) << 8 | uint64_t(p[2]) << 16 | uint64_t(p[3]) << 24 | uint64_t(p[4]) << 32 | uint64_t(p[5]) << 40 |
         uint64_t(p[6]) << 48 | uint64_t(p[7]) << 56;
}
inline void store64le(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i));
}

/** CRC-32's table, made by the compiler. */
struct CrcTable {
  uint32_t t[256];
  constexpr CrcTable() : t() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
  }
};
constexpr CrcTable kCrc;

/* ------------------------------------------------ hashes: a 64-byte block each, Merkle–Damgård */

template <typename Self>
struct BlockHash {
  uint8_t buf[64];
  size_t used = 0;
  uint64_t total = 0;
  void update(const uint8_t* p, size_t n) {
    total += n;
    while (n > 0) {
      size_t take = 64 - used < n ? 64 - used : n;
      memcpy(buf + used, p, take);
      used += take;
      p += take;
      n -= take;
      if (used == 64) {
        static_cast<Self*>(this)->block(buf);
        used = 0;
      }
    }
  }
  /** The padding: 0x80, zeros, the length in bits (big- or little-endian). */
  void pad(bool bigEndian) {
    uint64_t bits = total * 8;
    buf[used++] = 0x80;
    if (used > 56) {
      memset(buf + used, 0, 64 - used);
      static_cast<Self*>(this)->block(buf);
      used = 0;
    }
    memset(buf + used, 0, 56 - used);
    for (int i = 0; i < 8; i++) buf[56 + i] = uint8_t(bigEndian ? bits >> (56 - 8 * i) : bits >> (8 * i));
    static_cast<Self*>(this)->block(buf);
    used = 0;
  }
};

struct Sha256 : BlockHash<Sha256> {
  static const size_t kSize = 32;
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  void block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load32be(p + 4 * i);
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  void final(uint8_t* out) {
    pad(true);
    for (int i = 0; i < 8; i++) store32be(out + 4 * i, h[i]);
  }
};

struct Sha1 : BlockHash<Sha1> {
  static const size_t kSize = 20;
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  void block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = load32be(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  void final(uint8_t* out) {
    pad(true);
    for (int i = 0; i < 5; i++) store32be(out + 4 * i, h[i]);
  }
};

struct Md5 : BlockHash<Md5> {
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  void block(const uint8_t* p) {
    static const uint32_t T[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = load32le(p + 4 * i);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      uint32_t t = d;
      d = c;
      c = b;
      b = b + rotl(a + f + T[i] + m[g], R[i]);
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
  }
  void final(uint8_t* out) {
    pad(false);
    for (int i = 0; i < 4; i++) store32le(out + 4 * i, h[i]);
  }
};

/** HMAC (RFC 2104) over a hash with 64-byte blocks: keyed once, then as many messages as wanted (copies). */
template <typename H>
struct Hmac {
  H inner, outer;
  Hmac(const uint8_t* key, size_t keyLen) {
    uint8_t k[64] = {0}, pad[64];
    if (keyLen > 64) {
      H kh;
      kh.update(key, keyLen);
      kh.final(k);
    } else if (keyLen) {
      memcpy(k, key, keyLen);
    }
    for (int i = 0; i < 64; i++) pad[i] = uint8_t(k[i] ^ 0x36);
    inner.update(pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = uint8_t(k[i] ^ 0x5c);
    outer.update(pad, 64);
    wipe(k, sizeof k);
    wipe(pad, sizeof pad);
  }
  void update(const uint8_t* p, size_t n) { inner.update(p, n); }
  void final(uint8_t* out) {
    uint8_t ih[H::kSize];
    inner.final(ih);
    outer.update(ih, H::kSize);
    outer.final(out);
  }
};

/* ------------------------------------------------ X25519 (RFC 7748): GF(2^255 − 19) in 16 limbs of 16 bits */

typedef int64_t Fe[16];

void carry(Fe o) {
  for (int i = 0; i < 16; i++) {
    o[i] += int64_t(1) << 16;
    int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15); // (2^256 = 38: the top carry wraps around ×38)
    o[i] -= c * 65536;
  }
}

/** Swaps p and q when b is 1 – without a branch (constant time). */
void cswap(Fe p, Fe q, int64_t b) {
  int64_t mask = ~(b - 1);
  for (int i = 0; i < 16; i++) {
    int64_t t = mask & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

void pack(uint8_t* o, const Fe n) {
  Fe m, t;
  for (int i = 0; i < 16; i++) t[i] = n[i];
  carry(t);
  carry(t);
  carry(t);
  for (int j = 0; j < 2; j++) { // fully reduced: t − p when that is not negative
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; i++) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int64_t b = (m[15] >> 16) & 1;
    m[14] &= 0xffff;
    cswap(t, m, 1 - b);
  }
  for (int i = 0; i < 16; i++) {
    o[2 * i] = uint8_t(t[i] & 0xff);
    o[2 * i + 1] = uint8_t(t[i] >> 8);
  }
}

void unpack(Fe o, const uint8_t* n) {
  for (int i = 0; i < 16; i++) o[i] = int64_t(n[2 * i]) + (int64_t(n[2 * i + 1]) << 8);
  o[15] &= 0x7fff; // (RFC 7748: the top bit of a u-coordinate is ignored)
}

void add(Fe o, const Fe a, const Fe b) {
  for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}
void sub(Fe o, const Fe a, const Fe b) {
  for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}
void mul(Fe o, const Fe a, const Fe b) {
  int64_t t[31] = {0};
  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; i++) o[i] = t[i];
  carry(o);
  carry(o);
}
void sqr(Fe o, const Fe a) { mul(o, a, a); }

/** o = i^(p−2) = 1/i. */
void invert(Fe o, const Fe in) {
  Fe c;
  for (int a = 0; a < 16; a++) c[a] = in[a];
  for (int a = 253; a >= 0; a--) {
    sqr(c, c);
    if (a != 2 && a != 4) mul(c, c, in);
  }
  for (int a = 0; a < 16; a++) o[a] = c[a];
}

/** q = n · p (the u-coordinates); the scalar is clamped as RFC 7748 says. */
void scalarmult(uint8_t* q, const uint8_t* n, const uint8_t* p) {
  static const Fe a24 = {0xDB41, 1}; // 121665
  uint8_t z[32];
  for (int i = 0; i < 32; i++) z[i] = n[i];
  z[31] = uint8_t((n[31] & 127) | 64);
  z[0] &= 248;
  Fe x1, a, b, c, d, e, f;
  unpack(x1, p);
  for (int i = 0; i < 16; i++) {
    b[i] = x1[i];
    d[i] = a[i] = c[i] = 0;
  }
  a[0] = d[0] = 1; // (x2, z2) = (1, 0), (x3, z3) = (u, 1)
  for (int i = 254; i >= 0; --i) {
    int64_t r = (z[i >> 3] >> (i & 7)) & 1;
    cswap(a, b, r);
    cswap(c, d, r);
    add(e, a, c);  // A = x2 + z2
    sub(a, a, c);  // B = x2 − z2
    add(c, b, d);  // C = x3 + z3
    sub(b, b, d);  // D = x3 − z3
    sqr(d, e);     // AA
    sqr(f, a);     // BB
    mul(a, c, a);  // CB
    mul(c, b, e);  // DA
    add(e, a, c);  // CB + DA
    sub(a, a, c);  // CB − DA
    sqr(b, a);     // (CB − DA)²
    sub(c, d, f);  // E = AA − BB
    mul(a, c, a24);
    add(a, a, d);  // AA + a24·E
    mul(c, c, a);  // z2 = E·(AA + a24·E)
    mul(a, d, f);  // x2 = AA·BB
    mul(d, b, x1); // z3 = x1·(DA − CB)²
    sqr(b, e);     // x3 = (DA + CB)²
    cswap(a, b, r);
    cswap(c, d, r);
  }
  invert(c, c);
  mul(a, a, c);
  pack(q, a);
  wipe(z, sizeof z);
}

/* ------------------------------------------------ AES-128 (FIPS 197), portable and in constant time */

// Bitsliced: plane b holds bit b of 64 bytes – four blocks, one in each 16-bit lane (bit p of a plane ↔
// byte p, block p / 16; in a block's 16 bits, byte r + 4c is row r of column c) – so SubBytes is a fixed
// circuit of ANDs and XORs: the same instructions whatever the key and the data, no table looked up by a
// secret, nothing for the CPU's cache to give away. Where the compiler has SIMD vectors (SSE2, NEON,
// WebAssembly's SIMD) a plane is two such words: eight blocks at once. (The CPU's own AES instructions,
// further down, are the fast way; this is for a CPU without them, for WebAssembly and the key schedule.)
typedef uint64_t Planes[8];
#if defined(__GNUC__) || defined(__clang__)
typedef uint64_t Vec2 __attribute__((vector_size(16)));
#define SS_VEC2 1
#endif

/** v in every lane of W (a word, or a vector of them). */
template <typename W>
inline W splat(uint64_t v) { return W{} + v; }
/** A 16-bit mask in each of the four lanes (a block each). */
constexpr uint64_t lanes(uint64_t m) { return m * 0x0001000100010001ull; }

// SubBytes as a circuit through a tower field: AES's GF(2^8) is the same field as GF(16)[w] / (w² + w + λ),
// with GF(16) = GF(2)[z] / (z⁴ + z + 1) and λ = z³ + z² + z – a byte b(x) is b(β) there, for
// β = (z + 1)·w + z³ + 1 (a root of AES's x⁸ + x⁴ + x³ + x + 1 in that field). An inverse is cheap there:
//   (a1·w + a0)⁻¹ = (a1·w + a0 ⊕ a1) · Δ⁻¹,   Δ = a1²·λ ⊕ a1·a0 ⊕ a0²   (all in GF(16))
// and the way back is one linear map, together with the affine map. (Derived by a program that checked it
// on all 256 bytes; the tests check the whole cipher against FIPS 197's and SP 800-38D's vectors.)

/** a · b in GF(16): each value four planes. */
template <typename W>
inline void gf16Mul(const W a[4], const W b[4], W o[4]) {
  W p0 = a[0] & b[0], p1 = (a[0] & b[1]) ^ (a[1] & b[0]), p2 = (a[0] & b[2]) ^ (a[1] & b[1]) ^ (a[2] & b[0]);
  W p3 = (a[0] & b[3]) ^ (a[1] & b[2]) ^ (a[2] & b[1]) ^ (a[3] & b[0]);
  W p4 = (a[1] & b[3]) ^ (a[2] & b[2]) ^ (a[3] & b[1]), p5 = (a[2] & b[3]) ^ (a[3] & b[2]), p6 = a[3] & b[3];
  o[0] = p0 ^ p4; // z⁴ = z + 1, z⁵ = z² + z, z⁶ = z³ + z²
  o[1] = p1 ^ p4 ^ p5;
  o[2] = p2 ^ p5 ^ p6;
  o[3] = p3 ^ p6;
}
/** n⁻¹ in GF(16), 0 for 0: each bit's algebraic normal form. */
template <typename W>
inline void gf16Inv(const W n[4], W o[4]) {
  const W d01 = n[0] & n[1], d02 = n[0] & n[2], d12 = n[1] & n[2], d03 = n[0] & n[3], d13 = n[1] & n[3], d23 = n[2] & n[3];
  const W d012 = d01 & n[2], d013 = d01 & n[3], d023 = d02 & n[3], d123 = d12 & n[3];
  o[0] = n[0] ^ n[1] ^ n[2] ^ n[3] ^ d02 ^ d12 ^ d012 ^ d123;
  o[1] = n[3] ^ d01 ^ d02 ^ d12 ^ d13 ^ d013;
  o[2] = n[2] ^ n[3] ^ d01 ^ d02 ^ d03 ^ d023;
  o[3] = n[1] ^ n[2] ^ n[3] ^ d03 ^ d13 ^ d23 ^ d123;
}

/** SubBytes on every byte of the planes at once. */
template <typename W>
inline void subBytes(W x[8]) {
  const W a0[4] = {x[0] ^ x[1] ^ x[6], x[2] ^ x[3] ^ x[6] ^ x[7], x[2] ^ x[4] ^ x[7], x[1] ^ x[2] ^ x[6] ^ x[7]};
  const W a1[4] = {x[1] ^ x[2] ^ x[3] ^ x[5] ^ x[7], x[1] ^ x[4] ^ x[5] ^ x[6], x[2] ^ x[3], x[5] ^ x[7]};
  W a1a0[4], d[4], di[4], s[4], o0[4], o1[4];
  gf16Mul(a1, a0, a1a0);
  d[0] = a1[1] ^ a1[2] ^ a1a0[0] ^ a0[0] ^ a0[2]; // Δ: a1²·λ, a1·a0, a0²
  d[1] = a1[0] ^ a1a0[1] ^ a0[2];
  d[2] = a1[0] ^ a1[1] ^ a1[3] ^ a1a0[2] ^ a0[1] ^ a0[3];
  d[3] = a1[0] ^ a1[1] ^ a1a0[3] ^ a0[3];
  gf16Inv(d, di);
  for (int i = 0; i < 4; i++) s[i] = a0[i] ^ a1[i];
  gf16Mul(s, di, o0); // the inverse: (a0 ⊕ a1)·Δ⁻¹ ...
  gf16Mul(a1, di, o1); // ... + a1·Δ⁻¹·w
  x[0] = ~(o0[0] ^ o0[1] ^ o1[1] ^ o1[2]); // back to AES's bytes, the affine map, ⊕ 0x63
  x[1] = ~(o0[0] ^ o1[3]);
  x[2] = o0[0] ^ o0[1] ^ o0[2] ^ o1[0] ^ o1[1];
  x[3] = o0[0] ^ o0[1];
  x[4] = o0[0] ^ o0[2] ^ o0[3] ^ o1[0] ^ o1[3];
  x[5] = ~(o0[1] ^ o0[2] ^ o0[3] ^ o1[3]);
  x[6] = ~(o1[0] ^ o1[1] ^ o1[3]);
  x[7] = o0[1] ^ o0[2] ^ o1[3];
}

// In a block's 16 bits, byte r + 4c is row r of column c (FIPS 197's order).
template <typename W>
inline void shiftRows(W x[8]) { // row r turns left by r columns
  const W keep = splat<W>(lanes(0x1111)), r1a = splat<W>(lanes(0x2220)), r1b = splat<W>(lanes(0x0002)), r2a = splat<W>(lanes(0x4400)),
          r2b = splat<W>(lanes(0x0044)), r3a = splat<W>(lanes(0x0888)), r3b = splat<W>(lanes(0x8000));
  for (int b = 0; b < 8; b++) {
    W v = x[b];
    x[b] = (v & keep) | ((v & r1a) >> 4) | ((v & r1b) << 12) | ((v & r2a) >> 8) | ((v & r2b) << 8) | ((v & r3a) << 4) | ((v & r3b) >> 12);
  }
}
/** Each column's rows moved up by one (row r gets row r + 1) / by two. */
template <typename W>
inline W rowsUp1(W v) { return ((v >> 1) & splat<W>(lanes(0x7777))) | ((v << 3) & splat<W>(lanes(0x8888))); }
template <typename W>
inline W rowsUp2(W v) { return ((v >> 2) & splat<W>(lanes(0x3333))) | ((v << 2) & splat<W>(lanes(0xCCCC))); }
template <typename W>
inline void mixColumns(W x[8]) { // b_r = 2·t_r ⊕ a_r+1 ⊕ t_r+2, with t_r = a_r ⊕ a_r+1
  W t[8], s[8];
  for (int b = 0; b < 8; b++) {
    W u1 = rowsUp1(x[b]);
    t[b] = x[b] ^ u1;
    s[b] = u1 ^ rowsUp2(t[b]);
  }
  x[0] = t[7] ^ s[0]; // 2·t: x^8 = x^4 + x^3 + x + 1
  x[1] = t[0] ^ t[7] ^ s[1];
  x[2] = t[1] ^ s[2];
  x[3] = t[2] ^ t[7] ^ s[3];
  x[4] = t[3] ^ t[7] ^ s[4];
  x[5] = t[4] ^ s[5];
  x[6] = t[5] ^ s[6];
  x[7] = t[6] ^ s[7];
}
template <typename W>
inline void aesRounds(W x[8], const Planes rk[11]) {
  for (int b = 0; b < 8; b++) x[b] ^= splat<W>(rk[0][b]);
  for (int r = 1; r <= 10; r++) {
    subBytes(x);
    shiftRows(x);
    if (r < 10) mixColumns(x);
    for (int b = 0; b < 8; b++) x[b] ^= splat<W>(rk[r][b]);
  }
}

/** An 8 × 8 bit square turned around: bit j of byte i ↔ bit i of byte j. */
inline uint64_t transpose8(uint64_t x) {
  uint64_t t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAull;
  x ^= t ^ (t << 7);
  t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCull;
  x ^= t ^ (t << 14);
  t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ull;
  return x ^ t ^ (t << 28);
}
/** An 8 × 8 square of bytes turned around: byte b of word k ↔ byte k of word b. */
inline void transposeBytes(uint64_t w[8]) {
  for (int k = 0; k < 4; k++) { // the 4 × 4 corners
    uint64_t a = w[k], c = w[k + 4];
    w[k] = (a & 0x00000000FFFFFFFFull) | (c << 32);
    w[k + 4] = (a >> 32) | (c & 0xFFFFFFFF00000000ull);
  }
  const int pairs[4] = {0, 1, 4, 5}; // 2 × 2 (with words 2, 3, 6, 7)
  for (int k : pairs) {
    uint64_t a = w[k], c = w[k + 2];
    w[k] = (a & 0x0000FFFF0000FFFFull) | ((c & 0x0000FFFF0000FFFFull) << 16);
    w[k + 2] = ((a >> 16) & 0x0000FFFF0000FFFFull) | (c & 0xFFFF0000FFFF0000ull);
  }
  for (int k = 0; k < 8; k += 2) { // single bytes
    uint64_t a = w[k], c = w[k + 1];
    w[k] = (a & 0x00FF00FF00FF00FFull) | ((c & 0x00FF00FF00FF00FFull) << 8);
    w[k + 1] = ((a >> 8) & 0x00FF00FF00FF00FFull) | (c & 0xFF00FF00FF00FF00ull);
  }
}
void toPlanes(const uint8_t in[64], Planes x) {
  for (int k = 0; k < 8; k++) x[k] = transpose8(load64le(in + 8 * k)); // byte b: bit b of bytes 8k .. 8k + 7
  transposeBytes(x);                                                    // plane b, byte k: the same
}
void fromPlanes(const Planes x, uint8_t out[64]) {
  uint64_t w[8];
  for (int k = 0; k < 8; k++) w[k] = x[k];
  transposeBytes(w);
  for (int k = 0; k < 8; k++) store64le(out + 8 * k, transpose8(w[k]));
}

#if SS_VEC2
const size_t kSoftBlocks = 8; // (blocks at once in the portable code)
#else
const size_t kSoftBlocks = 4;
#endif
/** kSoftBlocks blocks encrypted with the round keys as planes. */
void aesSoft(const Planes rk[11], const uint8_t* in, uint8_t* out) {
#if SS_VEC2
  Planes lo, hi;
  toPlanes(in, lo);
  toPlanes(in + 64, hi);
  Vec2 x[8];
  for (int b = 0; b < 8; b++) x[b] = Vec2{lo[b], hi[b]};
  aesRounds(x, rk);
  for (int b = 0; b < 8; b++) {
    lo[b] = x[b][0];
    hi[b] = x[b][1];
  }
  fromPlanes(lo, out);
  fromPlanes(hi, out + 64);
#else
  Planes x;
  toPlanes(in, x);
  aesRounds(x, rk);
  fromPlanes(x, out);
#endif
}

/** The S-box on each byte of a word (the same circuit: bit i of the planes ↔ byte i). */
uint32_t subWord(uint32_t w) {
  Planes x;
  for (int b = 0; b < 8; b++) {
    x[b] = 0;
    for (int i = 0; i < 4; i++) x[b] |= uint64_t((w >> (8 * i + b)) & 1) << i;
  }
  subBytes(x);
  uint32_t r = 0;
  for (int b = 0; b < 8; b++)
    for (int i = 0; i < 4; i++) r |= uint32_t((x[b] >> i) & 1) << (8 * i + b);
  return r;
}

void aesExpand(const uint8_t key[16], uint8_t rk[176]) {
  memcpy(rk, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
    if (i % 16 == 0) { // RotWord, SubWord, Rcon
      store32le(t, subWord(uint32_t(t[1]) | uint32_t(t[2]) << 8 | uint32_t(t[3]) << 16 | uint32_t(t[0]) << 24));
      t[0] ^= rcon;
      rcon = uint8_t((rcon << 1) ^ ((rcon >> 7) * 0x1B));
    }
    for (int k = 0; k < 4; k++) rk[i + k] = uint8_t(rk[i - 16 + k] ^ t[k]);
  }
}

/** The round keys as planes (each in all four lanes). */
void keyPlanes(const uint8_t rk[176], Planes out[11]) {
  uint8_t four[64];
  for (int r = 0; r < 11; r++) {
    for (int b = 0; b < 4; b++) memcpy(four + 16 * b, rk + 16 * r, 16);
    toPlanes(four, out[r]);
  }
  wipe(four, sizeof four);
}

/* ------------------------------------------------ GHASH (SP 800-38D) */

// A GHASH value as an integer: bit i is the coefficient of x^i (GCM writes a block's first bit as x^0,
// so each byte's bits are turned around); the field is GF(2)[x] / (x^128 + x^7 + x^2 + x + 1).
struct U128 {
  uint64_t lo, hi;
};

/** Each byte's bits turned around. */
inline uint64_t rev8(uint64_t v) {
  v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
  v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
  return ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
}
U128 toField(const uint8_t b[16]) { return {rev8(load64le(b)), rev8(load64le(b + 8))}; }
void fromField(U128 v, uint8_t b[16]) {
  store64le(b, rev8(v.lo));
  store64le(b + 8, rev8(v.hi));
}

/** The low 64 bits of x · y, carry-less, with integer multiplies – in constant time: each operand split into
 *  four (every fourth bit), so the partial products' sums never carry into a bit that counts (a sum reaches
 *  16 – a carry four bits up – only from bit 60 on, past what is kept). */
inline uint64_t bmul64(uint64_t x, uint64_t y) {
  const uint64_t m0 = 0x1111111111111111ull, m1 = m0 << 1, m2 = m0 << 2, m3 = m0 << 3;
  uint64_t x0 = x & m0, x1 = x & m1, x2 = x & m2, x3 = x & m3;
  uint64_t y0 = y & m0, y1 = y & m1, y2 = y & m2, y3 = y & m3;
  uint64_t z0 = (x0 * y0) ^ (x1 * y3) ^ (x2 * y2) ^ (x3 * y1);
  uint64_t z1 = (x0 * y1) ^ (x1 * y0) ^ (x2 * y3) ^ (x3 * y2);
  uint64_t z2 = (x0 * y2) ^ (x1 * y1) ^ (x2 * y0) ^ (x3 * y3);
  uint64_t z3 = (x0 * y3) ^ (x1 * y2) ^ (x2 * y1) ^ (x3 * y0);
  return (z0 & m0) | (z1 & m1) | (z2 & m2) | (z3 & m3);
}
/** All 64 bits turned around. */
inline uint64_t rev64(uint64_t v) {
  v = rev8(v);
  v = ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
  v = ((v >> 16) & 0x0000FFFF0000FFFFull) | ((v & 0x0000FFFF0000FFFFull) << 16);
  return (v >> 32) | (v << 32);
}
/** r3·x^192 + r2·x^128 + r1·x^64 + r0 modulo x^128 + x^7 + x^2 + x + 1. */
inline U128 reduce128(uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3) {
  // x^128 = x^7 + x^2 + x + 1: the top half times that (and once more what that carries past x^128)
  uint64_t t0 = r2 ^ (r2 << 1) ^ (r2 << 2) ^ (r2 << 7);
  uint64_t t1 = r3 ^ (r3 << 1) ^ (r3 << 2) ^ (r3 << 7) ^ (r2 >> 63) ^ (r2 >> 62) ^ (r2 >> 57);
  uint64_t t2 = (r3 >> 63) ^ (r3 >> 62) ^ (r3 >> 57);
  t0 ^= t2 ^ (t2 << 1) ^ (t2 << 2) ^ (t2 << 7);
  return {r0 ^ t0, r1 ^ t1};
}
/** A factor (H) ready for gfMulSoft: its halves, their sum, and the same turned around. */
struct Factor {
  uint64_t b0, b1, b2, r0, r1, r2;
  explicit Factor(U128 b) : b0(b.lo), b1(b.hi), b2(b.lo ^ b.hi), r0(rev64(b.lo)), r1(rev64(b.hi)), r2(r0 ^ r1) {}
};
/** a · b in GHASH's field: Karatsuba over the 64-bit halves (three products); each 64 × 64 product's
 *  high half from both factors turned around (rev(a)·rev(b) = rev(a·b) over 127 bits). */
U128 gfMulSoft(U128 a, const Factor& b) {
  uint64_t a2 = a.lo ^ a.hi, ar0 = rev64(a.lo), ar1 = rev64(a.hi), ar2 = ar0 ^ ar1;
  uint64_t z0 = bmul64(a.lo, b.b0), z1 = bmul64(a.hi, b.b1), z2 = bmul64(a2, b.b2);
  uint64_t z0h = rev64(bmul64(ar0, b.r0)) >> 1, z1h = rev64(bmul64(ar1, b.r1)) >> 1, z2h = rev64(bmul64(ar2, b.r2)) >> 1;
  z2 ^= z0 ^ z1; // (the middle: Karatsuba)
  z2h ^= z0h ^ z1h;
  return reduce128(z0, z0h ^ z2, z1 ^ z2h, z1h);
}

/** AES-128-GCM, portable: the counter blocks kSoftBlocks at a time (J0 with the first ones); GHASH
 *  block by block. */
void gcmSoft(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, bool encrypt, uint8_t tag[16]) {
  uint8_t ctr[16 * kSoftBlocks], ks[16 * kSoftBlocks], block[16], ekj0[16];
  const Factor h(U128{g.h[0][0], g.h[0][1]});
  for (size_t b = 0; b < kSoftBlocks; b++) memcpy(ctr + 16 * b, iv, 12);
  uint32_t counter = 1; // (J0 first)
  size_t used = kSoftBlocks;
  auto keystream = [&]() -> const uint8_t* {
    if (used == kSoftBlocks) {
      for (size_t b = 0; b < kSoftBlocks; b++) store32be(ctr + 16 * b + 12, counter + uint32_t(b));
      counter += uint32_t(kSoftBlocks);
      aesSoft(g.planes, ctr, ks);
      used = 0;
    }
    return ks + 16 * used++;
  };
  memcpy(ekj0, keystream(), 16);
  U128 y{0, 0};
  auto ghash = [&](const uint8_t b[16]) {
    U128 x = toField(b);
    y = gfMulSoft({y.lo ^ x.lo, y.hi ^ x.hi}, h);
  };
  for (size_t i = 0; i < aadLen; i += 16) {
    size_t len = aadLen - i < 16 ? aadLen - i : 16;
    memset(block, 0, 16);
    memcpy(block, aad + i, len);
    ghash(block);
  }
  for (size_t i = 0; i < n; i += 16) {
    size_t len = n - i < 16 ? n - i : 16;
    const uint8_t* k = keystream();
    uint8_t* d = data + i;
    memset(block, 0, 16);
    if (!encrypt) memcpy(block, d, len); // (GHASH runs over the ciphertext)
    for (size_t j = 0; j + 8 <= len; j += 8) { // (eight bytes at a time)
      uint64_t a, b;
      memcpy(&a, d + j, 8);
      memcpy(&b, k + j, 8);
      a ^= b;
      memcpy(d + j, &a, 8);
    }
    for (size_t j = len & ~size_t(7); j < len; j++) d[j] ^= k[j];
    if (encrypt) memcpy(block, d, len);
    ghash(block);
  }
  store64be(block, uint64_t(aadLen) * 8);
  store64be(block + 8, uint64_t(n) * 8);
  ghash(block);
  fromField(y, tag);
  for (int i = 0; i < 16; i++) tag[i] ^= ekj0[i];
  wipe(ks, sizeof ks);
  wipe(ekj0, sizeof ekj0);
}

/* ------------------------------------------------ the same on the CPU's own instructions */

// Eight counter blocks at a time through the AES rounds (the AES units are pipelined, and newer CPUs
// have two: eight in flight run about as fast as one), and GHASH over eight blocks with H⁸ … H and one
// reduction, three carry-less multiplies a block (Karatsuba):
//   (…((y ⊕ x0)·H ⊕ x1)·H … ⊕ x7)·H = (y ⊕ x0)·H⁸ ⊕ x1·H⁷ ⊕ … ⊕ x7·H.
// The field's form is the same as above: a block's bytes as they are, each byte's bits turned around.
#define SS_EACH8(f) f(0) f(1) f(2) f(3) f(4) f(5) f(6) f(7)

#if SS_X86
#define SS_X86_FN SS_TARGET("aes,pclmul,ssse3") inline

alignas(16) const uint8_t kRevLow[16] = {0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0};
alignas(16) const uint8_t kRevHigh[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
alignas(16) const uint8_t kCounterSwap[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15, 14, 13, 12};

SS_X86_FN __m128i loadX86(const void* p) { return _mm_loadu_si128(static_cast<const __m128i*>(p)); }
SS_X86_FN void storeX86(void* p, __m128i v) { _mm_storeu_si128(static_cast<__m128i*>(p), v); }

/** Each byte's bits turned around (a nibble at a time, through a shuffle). */
SS_X86_FN __m128i revX86(__m128i x) {
  const __m128i nibble = _mm_set1_epi8(0x0F);
  __m128i low = _mm_shuffle_epi8(loadX86(kRevLow), _mm_and_si128(x, nibble));
  __m128i high = _mm_shuffle_epi8(loadX86(kRevHigh), _mm_and_si128(_mm_srli_epi16(x, 4), nibble));
  return _mm_or_si128(low, high);
}
/** lo, mid, hi ⊕= a · b with three multiplies (Karatsuba; bk: b's two halves XORed, in its low half).
 *  mid still holds lo and hi too – finishX86 takes them out, once, after the sums. */
SS_X86_FN void mulX86(__m128i a, __m128i b, __m128i bk, __m128i& lo, __m128i& mid, __m128i& hi) {
  lo = _mm_xor_si128(lo, _mm_clmulepi64_si128(a, b, 0x00));
  hi = _mm_xor_si128(hi, _mm_clmulepi64_si128(a, b, 0x11));
  mid = _mm_xor_si128(mid, _mm_clmulepi64_si128(_mm_xor_si128(a, _mm_shuffle_epi32(a, 0x4E)), bk, 0x00));
}
/** The 256-bit lo + mid·x^64 + hi·x^128 modulo x^128 + x^7 + x^2 + x + 1 – with x^128 = 0x87. */
SS_X86_FN __m128i finishX86(__m128i lo, __m128i mid, __m128i hi) {
  mid = _mm_xor_si128(mid, _mm_xor_si128(lo, hi)); // (the middle without the two ends)
  lo = _mm_xor_si128(lo, _mm_slli_si128(mid, 8));
  hi = _mm_xor_si128(hi, _mm_srli_si128(mid, 8));
  const __m128i r = _mm_set_epi64x(0, 0x87);
  __m128i t1 = _mm_clmulepi64_si128(hi, r, 0x00); // hi's low half · 0x87
  __m128i t2 = _mm_clmulepi64_si128(hi, r, 0x01); // hi's high half · 0x87 (at x^64)
  __m128i t3 = _mm_clmulepi64_si128(t2, r, 0x01); // what that put past x^128, once more
  return _mm_xor_si128(_mm_xor_si128(lo, t1), _mm_xor_si128(_mm_slli_si128(t2, 8), t3));
}
SS_X86_FN __m128i gfMulX86(__m128i a, __m128i b, __m128i bk) {
  __m128i lo = _mm_setzero_si128(), mid = lo, hi = lo;
  mulX86(a, b, bk, lo, mid, hi);
  return finishX86(lo, mid, hi);
}
SS_X86_FN __m128i aesX86(const __m128i k[11], __m128i x) {
  x = _mm_xor_si128(x, k[0]);
  for (int r = 1; r < 10; r++) x = _mm_aesenc_si128(x, k[r]);
  return _mm_aesenclast_si128(x, k[10]);
}

SS_TARGET("aes,pclmul,ssse3") void aesBlockX86(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
  __m128i k[11];
  for (int i = 0; i < 11; i++) k[i] = loadX86(rk + 16 * i);
  storeX86(out, aesX86(k, loadX86(in)));
}

SS_TARGET("aes,pclmul,ssse3")
void gcmX86(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, bool encrypt, uint8_t tag[16]) {
  __m128i k[11], h[8], hk[8];
  for (int i = 0; i < 11; i++) k[i] = loadX86(g.rk + 16 * i);
  for (int i = 0; i < 8; i++) {
    h[i] = loadX86(g.h[i]); // (the low half first: the field's form)
    hk[i] = _mm_xor_si128(h[i], _mm_shuffle_epi32(h[i], 0x4E));
  }
  uint8_t j0[16], buf[16];
  memcpy(j0, iv, 12);
  store32be(j0 + 12, 1);
  const __m128i swap = loadX86(kCounterSwap), one = _mm_set_epi32(1, 0, 0, 0);
  __m128i ctr = _mm_shuffle_epi8(loadX86(j0), swap); // the counter as a number, in the last lane
  const __m128i ekj0 = aesX86(k, loadX86(j0));
  __m128i y = _mm_setzero_si128();
  for (size_t i = 0; i < aadLen; i += 16) {
    size_t len = aadLen - i < 16 ? aadLen - i : 16;
    memset(buf, 0, 16);
    memcpy(buf, aad + i, len);
    y = gfMulX86(_mm_xor_si128(y, revX86(loadX86(buf))), h[0], hk[0]);
  }
  size_t p = 0;
  for (; p + 128 <= n; p += 128) {
    __m128i b[8];
#define SS_START(i)                    \
  ctr = _mm_add_epi32(ctr, one);       \
  b[i] = _mm_xor_si128(_mm_shuffle_epi8(ctr, swap), k[0]);
    SS_EACH8(SS_START)
#undef SS_START
    for (int r = 1; r < 10; r++) {
      const __m128i key = k[r];
#define SS_ROUND(i) b[i] = _mm_aesenc_si128(b[i], key);
      SS_EACH8(SS_ROUND)
#undef SS_ROUND
    }
    __m128i lo = _mm_setzero_si128(), mid = lo, hi = lo;
#define SS_DATA(i)                                                                                   \
  {                                                                                                  \
    __m128i d = loadX86(data + p + 16 * i), o = _mm_xor_si128(d, _mm_aesenclast_si128(b[i], k[10])); \
    storeX86(data + p + 16 * i, o);                                                                  \
    __m128i x = revX86(encrypt ? o : d); /* (GHASH runs over the ciphertext) */                      \
    mulX86(i == 0 ? _mm_xor_si128(x, y) : x, h[7 - i], hk[7 - i], lo, mid, hi);                      \
  }
    SS_EACH8(SS_DATA)
#undef SS_DATA
    y = finishX86(lo, mid, hi);
  }
  // the full blocks left (fewer than eight) together; then a partial one
  size_t m = (n - p) / 16;
  if (m) {
    __m128i b[8];
    for (size_t i = 0; i < m; i++) {
      ctr = _mm_add_epi32(ctr, one);
      b[i] = _mm_xor_si128(_mm_shuffle_epi8(ctr, swap), k[0]);
    }
    for (int r = 1; r < 10; r++)
      for (size_t i = 0; i < m; i++) b[i] = _mm_aesenc_si128(b[i], k[r]);
    __m128i lo = _mm_setzero_si128(), mid = lo, hi = lo;
    for (size_t i = 0; i < m; i++) {
      __m128i d = loadX86(data + p + 16 * i), o = _mm_xor_si128(d, _mm_aesenclast_si128(b[i], k[10]));
      storeX86(data + p + 16 * i, o);
      __m128i x = revX86(encrypt ? o : d);
      mulX86(i == 0 ? _mm_xor_si128(x, y) : x, h[m - 1 - i], hk[m - 1 - i], lo, mid, hi);
    }
    y = finishX86(lo, mid, hi);
    p += 16 * m;
  }
  if (p < n) {
    size_t len = n - p;
    memset(buf, 0, 16);
    memcpy(buf, data + p, len);
    ctr = _mm_add_epi32(ctr, one);
    __m128i in = loadX86(buf), out = _mm_xor_si128(in, aesX86(k, _mm_shuffle_epi8(ctr, swap)));
    storeX86(buf, out);
    memcpy(data + p, buf, len);
    if (encrypt) { // (the ciphertext, padded with zeros)
      memset(buf + len, 0, 16 - len);
      in = loadX86(buf);
    }
    y = gfMulX86(_mm_xor_si128(y, revX86(in)), h[0], hk[0]);
  }
  store64be(buf, uint64_t(aadLen) * 8);
  store64be(buf + 8, uint64_t(n) * 8);
  y = gfMulX86(_mm_xor_si128(y, revX86(loadX86(buf))), h[0], hk[0]);
  storeX86(tag, _mm_xor_si128(revX86(y), ekj0));
}
#endif

#if SS_ARM
inline uint8x16_t aesArm(const uint8x16_t k[11], uint8x16_t x) {
  for (int r = 0; r < 9; r++) x = vaesmcq_u8(vaeseq_u8(x, k[r]));
  return veorq_u8(vaeseq_u8(x, k[9]), k[10]);
}
/** The low halves of a and b multiplied (carry-less) / the high halves. */
inline uint8x16_t pmullLow(uint8x16_t a, uint8x16_t b) {
  return vreinterpretq_u8_p128(vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u8(a), 0), vgetq_lane_p64(vreinterpretq_p64_u8(b), 0)));
}
inline uint8x16_t pmullHigh(uint8x16_t a, uint8x16_t b) { return vreinterpretq_u8_p128(vmull_high_p64(vreinterpretq_p64_u8(a), vreinterpretq_p64_u8(b))); }
/** As mulX86. */
inline void mulArm(uint8x16_t a, uint8x16_t b, uint8x16_t bk, uint8x16_t& lo, uint8x16_t& mid, uint8x16_t& hi) {
  lo = veorq_u8(lo, pmullLow(a, b));
  hi = veorq_u8(hi, pmullHigh(a, b));
  mid = veorq_u8(mid, pmullLow(veorq_u8(a, vextq_u8(a, a, 8)), bk));
}
/** As finishX86. */
inline uint8x16_t finishArm(uint8x16_t lo, uint8x16_t mid, uint8x16_t hi) {
  mid = veorq_u8(mid, veorq_u8(lo, hi));
  const uint8x16_t zero = vdupq_n_u8(0), r = vreinterpretq_u8_u64(vdupq_n_u64(0x87));
  lo = veorq_u8(lo, vextq_u8(zero, mid, 8));
  hi = veorq_u8(hi, vextq_u8(mid, zero, 8));
  uint8x16_t t1 = pmullLow(hi, r), t2 = pmullHigh(hi, r), t3 = pmullHigh(t2, r);
  return veorq_u8(veorq_u8(lo, t1), veorq_u8(vextq_u8(zero, t2, 8), t3));
}
inline uint8x16_t gfMulArm(uint8x16_t a, uint8x16_t b, uint8x16_t bk) {
  uint8x16_t lo = vdupq_n_u8(0), mid = lo, hi = lo;
  mulArm(a, b, bk, lo, mid, hi);
  return finishArm(lo, mid, hi);
}
inline uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }

void aesBlockArm(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
  uint8x16_t k[11];
  for (int i = 0; i < 11; i++) k[i] = vld1q_u8(rk + 16 * i);
  vst1q_u8(out, aesArm(k, vld1q_u8(in)));
}

void gcmArm(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, bool encrypt, uint8_t tag[16]) {
  uint8x16_t k[11], h[8], hk[8];
  for (int i = 0; i < 11; i++) k[i] = vld1q_u8(g.rk + 16 * i);
  for (int i = 0; i < 8; i++) {
    h[i] = vreinterpretq_u8_u64(vld1q_u64(g.h[i]));
    hk[i] = veorq_u8(h[i], vextq_u8(h[i], h[i], 8));
  }
  uint8_t j0[16], buf[16];
  memcpy(j0, iv, 12);
  store32be(j0 + 12, 1);
  const uint32x4_t base = vreinterpretq_u32_u8(vld1q_u8(j0));
  uint32_t counter = 1;
  auto next = [&] { return vreinterpretq_u8_u32(vsetq_lane_u32(bswap32(++counter), base, 3)); };
  const uint8x16_t ekj0 = aesArm(k, vld1q_u8(j0));
  uint8x16_t y = vdupq_n_u8(0);
  for (size_t i = 0; i < aadLen; i += 16) {
    size_t len = aadLen - i < 16 ? aadLen - i : 16;
    memset(buf, 0, 16);
    memcpy(buf, aad + i, len);
    y = gfMulArm(veorq_u8(y, vrbitq_u8(vld1q_u8(buf))), h[0], hk[0]);
  }
  size_t p = 0;
  for (; p + 128 <= n; p += 128) {
    uint8x16_t b[8];
#define SS_START(i) b[i] = next();
    SS_EACH8(SS_START)
#undef SS_START
    for (int r = 0; r < 9; r++) {
      const uint8x16_t key = k[r];
#define SS_ROUND(i) b[i] = vaesmcq_u8(vaeseq_u8(b[i], key));
      SS_EACH8(SS_ROUND)
#undef SS_ROUND
    }
    uint8x16_t lo = vdupq_n_u8(0), mid = lo, hi = lo;
#define SS_DATA(i)                                                                                         \
  {                                                                                                        \
    uint8x16_t d = vld1q_u8(data + p + 16 * i), o = veorq_u8(d, veorq_u8(vaeseq_u8(b[i], k[9]), k[10])); \
    vst1q_u8(data + p + 16 * i, o);                                                                        \
    uint8x16_t x = vrbitq_u8(encrypt ? o : d);                                                             \
    mulArm(i == 0 ? veorq_u8(x, y) : x, h[7 - i], hk[7 - i], lo, mid, hi);                                 \
  }
    SS_EACH8(SS_DATA)
#undef SS_DATA
    y = finishArm(lo, mid, hi);
  }
  size_t m = (n - p) / 16;
  if (m) {
    uint8x16_t b[8];
    for (size_t i = 0; i < m; i++) b[i] = next();
    for (int r = 0; r < 9; r++)
      for (size_t i = 0; i < m; i++) b[i] = vaesmcq_u8(vaeseq_u8(b[i], k[r]));
    uint8x16_t lo = vdupq_n_u8(0), mid = lo, hi = lo;
    for (size_t i = 0; i < m; i++) {
      uint8x16_t d = vld1q_u8(data + p + 16 * i), o = veorq_u8(d, veorq_u8(vaeseq_u8(b[i], k[9]), k[10]));
      vst1q_u8(data + p + 16 * i, o);
      uint8x16_t x = vrbitq_u8(encrypt ? o : d);
      mulArm(i == 0 ? veorq_u8(x, y) : x, h[m - 1 - i], hk[m - 1 - i], lo, mid, hi);
    }
    y = finishArm(lo, mid, hi);
    p += 16 * m;
  }
  if (p < n) {
    size_t len = n - p;
    memset(buf, 0, 16);
    memcpy(buf, data + p, len);
    uint8x16_t in = vld1q_u8(buf), out = veorq_u8(in, aesArm(k, next()));
    vst1q_u8(buf, out);
    memcpy(data + p, buf, len);
    if (encrypt) {
      memset(buf + len, 0, 16 - len);
      in = vld1q_u8(buf);
    }
    y = gfMulArm(veorq_u8(y, vrbitq_u8(in)), h[0], hk[0]);
  }
  store64be(buf, uint64_t(aadLen) * 8);
  store64be(buf + 8, uint64_t(n) * 8);
  y = gfMulArm(veorq_u8(y, vrbitq_u8(vld1q_u8(buf))), h[0], hk[0]);
  vst1q_u8(tag, veorq_u8(vrbitq_u8(y), ekj0));
}
#endif

/* ------------------------------------------------ scrypt (RFC 7914): the server's password hashes */

/** Salsa20/8's core over 16 little-endian words. */
void salsa208(uint32_t b[16]) {
  uint32_t x[16];
  memcpy(x, b, 64);
  for (int i = 0; i < 8; i += 2) {
#define SS_R(a, c, d, s) x[a] ^= rotl(x[c] + x[d], s)
    SS_R(4, 0, 12, 7), SS_R(8, 4, 0, 9), SS_R(12, 8, 4, 13), SS_R(0, 12, 8, 18); // the columns
    SS_R(9, 5, 1, 7), SS_R(13, 9, 5, 9), SS_R(1, 13, 9, 13), SS_R(5, 1, 13, 18);
    SS_R(14, 10, 6, 7), SS_R(2, 14, 10, 9), SS_R(6, 2, 14, 13), SS_R(10, 6, 2, 18);
    SS_R(3, 15, 11, 7), SS_R(7, 3, 15, 9), SS_R(11, 7, 3, 13), SS_R(15, 11, 7, 18);
    SS_R(1, 0, 3, 7), SS_R(2, 1, 0, 9), SS_R(3, 2, 1, 13), SS_R(0, 3, 2, 18); // the rows
    SS_R(6, 5, 4, 7), SS_R(7, 6, 5, 9), SS_R(4, 7, 6, 13), SS_R(5, 4, 7, 18);
    SS_R(11, 10, 9, 7), SS_R(8, 11, 10, 9), SS_R(9, 8, 11, 13), SS_R(10, 9, 8, 18);
    SS_R(12, 15, 14, 7), SS_R(13, 12, 15, 9), SS_R(14, 13, 12, 13), SS_R(15, 14, 13, 18);
#undef SS_R
  }
  for (int i = 0; i < 16; i++) b[i] += x[i];
}
/** scryptBlockMix: 2r 64-byte blocks → out, the even ones' results first, then the odd ones'. */
void blockMix(const uint32_t* in, uint32_t* out, uint32_t r) {
  uint32_t x[16];
  memcpy(x, in + (2 * r - 1) * 16, 64);
  for (uint32_t i = 0; i < 2 * r; i++) {
    for (int k = 0; k < 16; k++) x[k] ^= in[16 * i + k];
    salsa208(x);
    memcpy(out + 16 * ((i & 1) * r + i / 2), x, 64);
  }
}
/** scryptROMix on one 128·r-byte block (V: 128·r·N bytes, x and y: 128·r each). */
void roMix(uint8_t* block, uint32_t r, uint64_t N, uint32_t* v, uint32_t* x, uint32_t* y) {
  const size_t words = 32 * size_t(r);
  for (size_t k = 0; k < words; k++) x[k] = load32le(block + 4 * k);
  for (uint64_t i = 0; i < N; i++) {
    memcpy(v + i * words, x, words * 4);
    blockMix(x, y, r);
    memcpy(x, y, words * 4);
  }
  for (uint64_t i = 0; i < N; i++) {
    const uint32_t* last = x + (2 * r - 1) * 16; // Integerify: the last 64 bytes, little-endian
    const uint32_t* vj = v + ((uint64_t(last[0]) | uint64_t(last[1]) << 32) & (N - 1)) * words;
    for (size_t k = 0; k < words; k++) x[k] ^= vj[k];
    blockMix(x, y, r);
    memcpy(x, y, words * 4);
  }
  for (size_t k = 0; k < words; k++) store32le(block + 4 * k, x[k]);
}
/** PBKDF2-HMAC-SHA256 with one iteration (all scrypt needs). */
void pbkdf2Once(const uint8_t* pass, size_t passLen, const uint8_t* salt, size_t saltLen, uint8_t* out, size_t outLen) {
  const Hmac<Sha256> keyed(pass, passLen);
  uint8_t t[32];
  for (uint32_t i = 1; outLen; i++) {
    Hmac<Sha256> m = keyed;
    uint8_t index[4];
    store32be(index, i);
    m.update(salt, saltLen);
    m.update(index, 4);
    m.final(t);
    size_t take = outLen < 32 ? outLen : 32;
    memcpy(out, t, take);
    out += take;
    outLen -= take;
  }
  wipe(t, sizeof t);
}

/* ------------------------------------------------ ChaCha20-Poly1305 (RFC 8439) */

// For the links with a browser: there AES has no CPU instructions (WebAssembly has none), and ChaCha20 –
// additions, XORs and rotations of 32-bit words – is several times faster in software than AES, in
// constant time by its nature. (TLS 1.3 and WireGuard use it for the same reason.) Four blocks at once
// on SIMD vectors where the compiler has them; Poly1305 in 26-bit limbs (64-bit products: portable,
// WebAssembly too).
#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t Vec4 __attribute__((vector_size(16)));
#define SS_VEC4 1
#endif

template <typename W>
inline W rotl32(W x, int n) { return (x << n) | (x >> (32 - n)); }
template <typename W>
inline void quarter(W& a, W& b, W& c, W& d) {
  a += b;
  d = rotl32(d ^ a, 16);
  c += d;
  b = rotl32(b ^ c, 12);
  a += b;
  d = rotl32(d ^ a, 8);
  c += d;
  b = rotl32(b ^ c, 7);
}
/** ChaCha20's 20 rounds over a state (columns, then diagonals – ten times). */
template <typename W>
inline void chachaRounds(W x[16]) {
  for (int i = 0; i < 10; i++) {
    quarter(x[0], x[4], x[8], x[12]);
    quarter(x[1], x[5], x[9], x[13]);
    quarter(x[2], x[6], x[10], x[14]);
    quarter(x[3], x[7], x[11], x[15]);
    quarter(x[0], x[5], x[10], x[15]);
    quarter(x[1], x[6], x[11], x[12]);
    quarter(x[2], x[7], x[8], x[13]);
    quarter(x[3], x[4], x[9], x[14]);
  }
}
/** The state for one block: the constant, the key, the counter, the nonce (all little-endian words). */
inline void chachaState(uint32_t s[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
  s[0] = 0x61707865; // "expand 32-byte k"
  s[1] = 0x3320646e;
  s[2] = 0x79622d32;
  s[3] = 0x6b206574;
  for (int i = 0; i < 8; i++) s[4 + i] = load32le(key + 4 * i);
  s[12] = counter;
  for (int i = 0; i < 3; i++) s[13 + i] = load32le(nonce + 4 * i);
}
#if SS_VEC4
const size_t kChachaBlocks = 4; // (blocks at once)
#else
const size_t kChachaBlocks = 1;
#endif
/** kChachaBlocks blocks of key stream (64 bytes each) from the counter on. */
void chachaBlocks(const uint32_t state[16], uint32_t counter, uint8_t* out) {
#if SS_VEC4
  Vec4 x[16], in[16];
  for (int i = 0; i < 16; i++) in[i] = Vec4{state[i], state[i], state[i], state[i]};
  in[12] = Vec4{counter, counter + 1, counter + 2, counter + 3};
  for (int i = 0; i < 16; i++) x[i] = in[i];
  chachaRounds(x);
  for (int i = 0; i < 16; i++) x[i] += in[i];
  for (int b = 0; b < 4; b++)
    for (int i = 0; i < 16; i++) store32le(out + 64 * b + 4 * i, x[i][b]);
#else
  uint32_t x[16];
  for (int i = 0; i < 16; i++) x[i] = state[i];
  x[12] = counter;
  chachaRounds(x);
  for (int i = 0; i < 16; i++) store32le(out + 4 * i, x[i] + (i == 12 ? counter : state[i]));
#endif
}
/** to ⊕= from, n bytes – eight at a time. */
inline void xorBytes(uint8_t* to, const uint8_t* from, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t a, b;
    memcpy(&a, to + i, 8);
    memcpy(&b, from + i, 8);
    a ^= b;
    memcpy(to + i, &a, 8);
  }
  for (; i < n; i++) to[i] ^= from[i];
}
#if SS_VEC4
#if defined(__clang__)
#define SS_SHUFFLE4(a, b, i, j, k, l) __builtin_shufflevector(a, b, i, j, k, l)
#else
#define SS_SHUFFLE4(a, b, i, j, k, l) __builtin_shuffle(a, b, Vec4{i, j, k, l})
#endif
/** Four rows of four words turned into four columns (lane b of each row → the words of block b). */
inline void transpose4(Vec4& a, Vec4& b, Vec4& c, Vec4& d) {
  Vec4 t0 = SS_SHUFFLE4(a, b, 0, 4, 1, 5), t1 = SS_SHUFFLE4(a, b, 2, 6, 3, 7), t2 = SS_SHUFFLE4(c, d, 0, 4, 1, 5), t3 = SS_SHUFFLE4(c, d, 2, 6, 3, 7);
  a = SS_SHUFFLE4(t0, t2, 0, 1, 4, 5);
  b = SS_SHUFFLE4(t0, t2, 2, 3, 6, 7);
  c = SS_SHUFFLE4(t1, t3, 0, 1, 4, 5);
  d = SS_SHUFFLE4(t1, t3, 2, 3, 6, 7);
}
#endif
/** data ⊕= the key stream from block 1 on (block 0 made the Poly1305 key). */
void chachaXor(const uint32_t state[16], uint8_t* data, size_t n) {
  uint32_t counter = 1;
  size_t p = 0;
#if SS_VEC4 && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // whole runs of four blocks: the rows turned into blocks with shuffles, XORed 16 bytes at a time
  Vec4 in[16];
  for (int i = 0; i < 16; i++) in[i] = Vec4{state[i], state[i], state[i], state[i]};
  for (; p + 256 <= n; p += 256, counter += 4) {
    Vec4 x[16];
    in[12] = Vec4{counter, counter + 1, counter + 2, counter + 3};
    for (int i = 0; i < 16; i++) x[i] = in[i];
    chachaRounds(x);
    for (int i = 0; i < 16; i++) x[i] += in[i];
    for (int g = 0; g < 4; g++) transpose4(x[4 * g], x[4 * g + 1], x[4 * g + 2], x[4 * g + 3]);
    for (int b = 0; b < 4; b++)
      for (int g = 0; g < 4; g++) { // block b's words 4g … 4g + 3
        Vec4 d;
        memcpy(&d, data + p + 64 * b + 16 * g, 16);
        d ^= x[4 * g + b];
        memcpy(data + p + 64 * b + 16 * g, &d, 16);
      }
  }
#endif
  uint8_t ks[64 * kChachaBlocks];
  for (; p < n; p += sizeof ks) {
    chachaBlocks(state, counter, ks);
    counter += uint32_t(kChachaBlocks);
    xorBytes(data + p, ks, n - p < sizeof ks ? n - p : sizeof ks);
  }
  wipe(ks, sizeof ks);
}

/** Poly1305 (RFC 8439 2.5) over whole 16-byte blocks: the AEAD pads everything to them. */
struct Poly1305 {
  uint32_t r[5], s4[4], h[5] = {0, 0, 0, 0, 0}, pad[4];
  explicit Poly1305(const uint8_t key[32]) {
    r[0] = load32le(key) & 0x3ffffff; // r, clamped as the standard has it
    r[1] = (load32le(key + 3) >> 2) & 0x3ffff03;
    r[2] = (load32le(key + 6) >> 4) & 0x3ffc0ff;
    r[3] = (load32le(key + 9) >> 6) & 0x3f03fff;
    r[4] = (load32le(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 4; i++) s4[i] = r[i + 1] * 5;
    for (int i = 0; i < 4; i++) pad[i] = load32le(key + 16 + 4 * i);
  }
  /** h = (h + block + 2^128) · r  mod 2^130 − 5, block by block. */
  void blocks(const uint8_t* m, size_t n) {
    const uint64_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4], s1 = s4[0], s2 = s4[1], s3 = s4[2], s4_ = s4[3];
    uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
    for (; n >= 16; m += 16, n -= 16) {
      h0 += load32le(m) & 0x3ffffff;
      h1 += (load32le(m + 3) >> 2) & 0x3ffffff;
      h2 += (load32le(m + 6) >> 4) & 0x3ffffff;
      h3 += (load32le(m + 9) >> 6) & 0x3ffffff;
      h4 += (load32le(m + 12) >> 8) | (1u << 24);
      uint64_t d0 = h0 * r0 + h1 * s4_ + h2 * s3 + h3 * s2 + h4 * s1;
      uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4_ + h3 * s3 + h4 * s2;
      uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4_ + h4 * s3;
      uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4_;
      uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;
      uint32_t c = uint32_t(d0 >> 26);
      h0 = uint32_t(d0) & 0x3ffffff;
      d1 += c;
      c = uint32_t(d1 >> 26);
      h1 = uint32_t(d1) & 0x3ffffff;
      d2 += c;
      c = uint32_t(d2 >> 26);
      h2 = uint32_t(d2) & 0x3ffffff;
      d3 += c;
      c = uint32_t(d3 >> 26);
      h3 = uint32_t(d3) & 0x3ffffff;
      d4 += c;
      c = uint32_t(d4 >> 26);
      h4 = uint32_t(d4) & 0x3ffffff;
      h0 += c * 5;
      c = h0 >> 26;
      h0 &= 0x3ffffff;
      h1 += c;
    }
    h[0] = h0;
    h[1] = h1;
    h[2] = h2;
    h[3] = h3;
    h[4] = h4;
  }
  /** The tag: h fully reduced (in constant time), plus the pad. */
  void final(uint8_t tag[16]) {
    uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4], c;
    c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;
    // g = h + 5 − 2^130: if that does not go below zero, h is at least p – then g is h mod p
    uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t useG = (g4 >> 31) - 1; // all ones when g4 did not go below zero
    h0 = (h0 & ~useG) | (g0 & useG);
    h1 = (h1 & ~useG) | (g1 & useG);
    h2 = (h2 & ~useG) | (g2 & useG);
    h3 = (h3 & ~useG) | (g3 & useG);
    h4 = (h4 & ~useG) | (g4 & useG);
    // to four 32-bit words (mod 2^128), plus the pad
    uint32_t w0 = h0 | (h1 << 26), w1 = (h1 >> 6) | (h2 << 20), w2 = (h2 >> 12) | (h3 << 14), w3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = uint64_t(w0) + pad[0];
    store32le(tag, uint32_t(f));
    f = uint64_t(w1) + pad[1] + (f >> 32);
    store32le(tag + 4, uint32_t(f));
    f = uint64_t(w2) + pad[2] + (f >> 32);
    store32le(tag + 8, uint32_t(f));
    f = uint64_t(w3) + pad[3] + (f >> 32);
    store32le(tag + 12, uint32_t(f));
  }
  /** These bytes, zero-padded to a whole block. */
  void padded(const uint8_t* m, size_t n) {
    blocks(m, n & ~size_t(15));
    if (n & 15) {
      uint8_t last[16] = {0};
      memcpy(last, m + (n & ~size_t(15)), n & 15);
      blocks(last, 16);
    }
  }
};

/** The AEAD's tag over the header and the ciphertext (RFC 8439 2.8). */
void chachaTag(const uint32_t state[16], const uint8_t* aad, size_t aadLen, const uint8_t* ct, size_t n, uint8_t tag[16]) {
  uint8_t block0[64 * kChachaBlocks];
  chachaBlocks(state, 0, block0); // its first 32 bytes: the one-time Poly1305 key
  Poly1305 mac(block0);
  wipe(block0, sizeof block0);
  mac.padded(aad, aadLen);
  mac.padded(ct, n);
  uint8_t lengths[16];
  for (int i = 0; i < 8; i++) {
    lengths[i] = uint8_t(uint64_t(aadLen) >> (8 * i));
    lengths[8 + i] = uint8_t(uint64_t(n) >> (8 * i));
  }
  mac.blocks(lengths, 16);
  mac.final(tag);
  wipe(&mac, sizeof mac);
}

/* ------------------------------------------------ which code runs here */

enum class Engine { Portable, X86, Arm };

Engine engine() {
#if defined(SS_CRYPTO_PORTABLE) || !(SS_X86 || SS_ARM) // (tests: the portable code, on any CPU)
  return Engine::Portable;
#elif SS_X86
  static const Engine e = [] {
    unsigned c = 0;
#ifdef _MSC_VER
    int info[4];
    __cpuid(info, 1);
    c = unsigned(info[2]);
#else
    unsigned a = 0, b = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) c = 0;
#endif
    bool aes = (c >> 25) & 1, pclmul = (c >> 1) & 1, ssse3 = (c >> 9) & 1;
    return aes && pclmul && ssse3 ? Engine::X86 : Engine::Portable;
  }();
  return e;
#else
  return Engine::Arm;
#endif
}

}  // namespace

void sha256(const uint8_t* data, size_t n, uint8_t out[32]) {
  Sha256 h;
  h.update(data, n);
  h.final(out);
}
void sha1(const uint8_t* data, size_t n, uint8_t out[20]) {
  Sha1 h;
  h.update(data, n);
  h.final(out);
}
void md5(const uint8_t* data, size_t n, uint8_t out[16]) {
  Md5 h;
  h.update(data, n);
  h.final(out);
}
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[32]) {
  Hmac<Sha256> m(key, keyLen);
  m.update(data, n);
  m.final(out);
}
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[20]) {
  Hmac<Sha1> m(key, keyLen);
  m.update(data, n);
  m.final(out);
}

bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen) {
  if (outLen > 255 * 32) return false;
  uint8_t prk[32], t[32];
  hmacSha256(salt, saltLen, ikm, ikmLen, prk); // (no salt: an empty key – the same as 32 zeros)
  const Hmac<Sha256> keyed(prk, 32);
  for (unsigned i = 1; outLen; i++) {
    Hmac<Sha256> m = keyed;
    if (i > 1) m.update(t, 32);
    m.update(info, infoLen);
    const uint8_t c = uint8_t(i);
    m.update(&c, 1);
    m.final(t);
    size_t take = outLen < 32 ? outLen : 32;
    memcpy(out, t, take);
    out += take;
    outLen -= take;
  }
  wipe(prk, sizeof prk);
  wipe(t, sizeof t);
  return true;
}

uint32_t crc32(const uint8_t* data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = kCrc.t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
  scalarmult(out, scalar, point);
  uint8_t any = 0;
  for (int i = 0; i < 32; i++) any |= out[i];
  return any != 0; // a low-order point gives all zeros
}

bool scrypt(const uint8_t* pass, size_t passLen, const uint8_t* salt, size_t saltLen, uint64_t N, uint32_t r, uint32_t p, uint8_t* out, size_t outLen) {
  if (N < 2 || (N & (N - 1)) || !r || !p || uint64_t(r) * p >= (uint64_t(1) << 30)) return false;
  const uint64_t blockBytes = 128ull * r, vBytes = blockBytes * N, bBytes = blockBytes * p;
  if (vBytes > (uint64_t(1) << 31) || bBytes > (uint64_t(1) << 31)) return false; // (2 GB at most: also on 32 bits)
  uint8_t* b = static_cast<uint8_t*>(malloc(size_t(bBytes)));
  uint32_t* v = static_cast<uint32_t*>(malloc(size_t(vBytes)));
  uint32_t* xy = static_cast<uint32_t*>(malloc(size_t(2 * blockBytes)));
  bool ok = b && v && xy;
  if (ok) {
    pbkdf2Once(pass, passLen, salt, saltLen, b, size_t(bBytes));
    for (uint32_t i = 0; i < p; i++) roMix(b + blockBytes * i, r, N, v, xy, xy + blockBytes / 4);
    pbkdf2Once(pass, passLen, b, size_t(bBytes), out, outLen);
    wipe(b, size_t(bBytes));
    wipe(v, size_t(vBytes));
    wipe(xy, size_t(2 * blockBytes));
  }
  free(b);
  free(v);
  free(xy);
  return ok;
}

void chachaSeal(const uint8_t key[32], const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) {
  uint32_t state[16];
  chachaState(state, key, 0, iv);
  chachaXor(state, data, n);
  chachaTag(state, aad, aadLen, data, n, tag);
  wipe(state, sizeof state);
}

bool chachaOpen(const uint8_t key[32], const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) {
  uint32_t state[16];
  chachaState(state, key, 0, iv);
  uint8_t mine[16];
  chachaTag(state, aad, aadLen, data, n, mine); // (over the ciphertext as it came)
  uint8_t diff = 0;                             // (compared in constant time)
  for (int i = 0; i < 16; i++) diff |= uint8_t(mine[i] ^ tag[i]);
  if (diff == 0) chachaXor(state, data, n);     // (only what is genuine is decrypted)
  wipe(state, sizeof state);
  return diff == 0;
}

const char* engineName() {
  switch (engine()) {
    case Engine::X86: return "x86 AES-NI + PCLMULQDQ";
    case Engine::Arm: return "ARMv8 AES + PMULL";
    default: return "portable";
  }
}

void gcmInit(Gcm& g, const uint8_t key[16]) {
  memset(&g, 0, sizeof g);
  g.engine = int(engine());
  aesExpand(key, g.rk);
  uint8_t h[16 * kSoftBlocks] = {0}; // E(K, 0)
#if SS_X86
  if (g.engine == int(Engine::X86)) aesBlockX86(g.rk, h, h);
#endif
#if SS_ARM
  if (g.engine == int(Engine::Arm)) aesBlockArm(g.rk, h, h);
#endif
  if (g.engine == int(Engine::Portable)) {
    keyPlanes(g.rk, g.planes);
    aesSoft(g.planes, h, h);
  }
  const U128 h1 = toField(h);
  const Factor f(h1);
  U128 power = h1;
  for (int i = 0; i < 8; i++) { // H, H², … H⁸
    g.h[i][0] = power.lo;
    g.h[i][1] = power.hi;
    power = gfMulSoft(power, f);
  }
  wipe(h, sizeof h);
}

namespace {
void gcmRun(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, bool encrypt, uint8_t tag[16]) {
#if SS_X86
  if (g.engine == int(Engine::X86)) return gcmX86(g, iv, aad, aadLen, data, n, encrypt, tag);
#endif
#if SS_ARM
  if (g.engine == int(Engine::Arm)) return gcmArm(g, iv, aad, aadLen, data, n, encrypt, tag);
#endif
  gcmSoft(g, iv, aad, aadLen, data, n, encrypt, tag);
}
}  // namespace

void gcmSeal(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) {
  gcmRun(g, iv, aad, aadLen, data, n, true, tag);
}

bool gcmOpen(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) {
  uint8_t mine[16];
  gcmRun(g, iv, aad, aadLen, data, n, false, mine);
  uint8_t diff = 0; // (compared in constant time)
  for (int i = 0; i < 16; i++) diff |= uint8_t(mine[i] ^ tag[i]);
  return diff == 0;
}

void wipe(void* p, size_t n) {
  volatile uint8_t* b = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < n; i++) b[i] = 0;
}

}  // namespace crypto
}  // namespace ss
