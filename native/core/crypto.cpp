// Screen Share native core – the cryptography's C++ face (crypto.h). The algorithms themselves are ours, in
// crypto_core.cpp (plain bytes, no library – the website's WebAssembly and the server run the very same
// code); here: the system's random numbers, the C++ types, base64.
#include "crypto.h"

#include <cstring>
#include <stdexcept>

#include "crypto_core.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <Security/SecRandom.h>
#else
#include <sys/random.h>
#endif

namespace ss {

void randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) throw std::runtime_error("no random");
#elif defined(__APPLE__)
  if (SecRandomCopyBytes(kSecRandomDefault, n, out) != errSecSuccess) throw std::runtime_error("no random");
#else
  size_t done = 0;
  while (done < n) {
    ssize_t r = getrandom(out + done, n - done, 0);
    if (r <= 0) throw std::runtime_error("no random");
    done += size_t(r);
  }
#endif
}

X25519 X25519::generate() {
  static const uint8_t base[32] = {9};
  X25519 k;
  randomBytes(k.secret.data(), 32);
  k.secret[0] &= 248; // (RFC 7748's clamping, kept in the stored key)
  k.secret[31] = uint8_t((k.secret[31] & 127) | 64);
  crypto::x25519(k.pub.data(), k.secret.data(), base);
  return k;
}

bool X25519::shared(const uint8_t peer[32], uint8_t out[32]) const { return crypto::x25519(out, secret.data(), peer); }

const char* cryptoEngine() { return crypto::engineName(); }

void sha256(const uint8_t* data, size_t n, uint8_t out[32]) { crypto::sha256(data, n, out); }
void sha1(const uint8_t* data, size_t n, uint8_t out[20]) { crypto::sha1(data, n, out); }
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[32]) { crypto::hmacSha256(key, keyLen, data, n, out); }
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[20]) { crypto::hmacSha1(key, keyLen, data, n, out); }
void md5(const uint8_t* data, size_t n, uint8_t out[16]) { crypto::md5(data, n, out); }
uint32_t crc32(const uint8_t* data, size_t n) { return crypto::crc32(data, n); }

Bytes hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen, size_t outLen) {
  Bytes out(outLen);
  if (!crypto::hkdfSha256(salt, saltLen, ikm, ikmLen, info, infoLen, out.data(), outLen)) out.clear();
  return out;
}

/* ------------------------------------------------ AES-128-GCM */

struct AesGcm::Impl {
  crypto::Gcm g;
};

AesGcm::AesGcm(const uint8_t key[16]) : impl_(new Impl) { crypto::gcmInit(impl_->g, key); }

AesGcm::~AesGcm() { crypto::wipe(&impl_->g, sizeof impl_->g); } // (the key gone from memory)

void AesGcm::seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) {
  crypto::gcmSeal(impl_->g, iv, aad, aadLen, data, n, tag);
}

bool AesGcm::open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) {
  return crypto::gcmOpen(impl_->g, iv, aad, aadLen, data, n, tag);
}

/* ------------------------------------------------ AEGIS-128L */

Aegis128L::Aegis128L(const uint8_t key[16]) { std::memcpy(key_, key, 16); }

Aegis128L::~Aegis128L() { crypto::wipe(key_, sizeof key_); }

void Aegis128L::seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) {
  uint8_t nonce[16] = {0};
  std::memcpy(nonce, iv, 12);
  crypto::aegisSeal(key_, nonce, aad, aadLen, data, n, tag);
}

bool Aegis128L::open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) {
  uint8_t nonce[16] = {0};
  std::memcpy(nonce, iv, 12);
  return crypto::aegisOpen(key_, nonce, aad, aadLen, data, n, tag);
}

/* ------------------------------------------------ ChaCha20-Poly1305 */

ChaCha20Poly1305::ChaCha20Poly1305(const uint8_t key[32]) { std::memcpy(key_, key, 32); }

ChaCha20Poly1305::~ChaCha20Poly1305() { crypto::wipe(key_, sizeof key_); }

void ChaCha20Poly1305::seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) {
  crypto::chachaSeal(key_, iv, aad, aadLen, data, n, tag);
}

bool ChaCha20Poly1305::open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) {
  return crypto::chachaOpen(key_, iv, aad, aadLen, data, n, tag);
}

/* ------------------------------------------------ base64 */

std::string base64Encode(const uint8_t* p, size_t n) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string s;
  s.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    s += t[v >> 18 & 63];
    s += t[v >> 12 & 63];
    s += i + 1 < n ? t[v >> 6 & 63] : '=';
    s += i + 2 < n ? t[v & 63] : '=';
  }
  return s;
}

Bytes base64Decode(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  Bytes out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    int v = val(c);
    if (v < 0) return {};
    acc = acc << 6 | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t(acc >> bits));
    }
  }
  return out;
}

}  // namespace ss
