#include "crypto.h"

#include <cstring>
#include <stdexcept>

extern "C" {
#include "bearssl.h"
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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

/* ------------------------------------------------ X25519 */

static const br_ec_impl* curve() {
  static const br_ec_impl* impl = [] {
    const br_ec_impl* m64 = br_ec_c25519_m64_get();
    return m64 ? m64 : &br_ec_c25519_m31;
  }();
  return impl;
}

X25519 X25519::generate() {
  X25519 k;
  randomBytes(k.secret.data(), 32);
  // RFC 7748 clamping (BearSSL clamps too; done here so the stored key is the real one)
  k.secret[0] &= 248;
  k.secret[31] = uint8_t((k.secret[31] & 127) | 64);
  uint8_t be[32];
  for (int i = 0; i < 32; i++) be[i] = k.secret[31 - i]; // BearSSL takes the scalar big-endian (RFC 7748: little-endian)
  curve()->mulgen(k.pub.data(), be, 32, BR_EC_curve25519);
  return k;
}

bool X25519::shared(const uint8_t peer[32], uint8_t out[32]) const {
  std::memcpy(out, peer, 32);
  uint8_t be[32];
  for (int i = 0; i < 32; i++) be[i] = secret[31 - i];
  if (!curve()->mul(out, 32, be, 32, BR_EC_curve25519)) return false;
  uint8_t zero = 0;
  for (int i = 0; i < 32; i++) zero |= out[i];
  return zero != 0; // a low-order point gives all zeros
}

/* ------------------------------------------------ hashes, MACs, KDF */

void sha256(const uint8_t* data, size_t n, uint8_t out[32]) {
  br_sha256_context c;
  br_sha256_init(&c);
  br_sha256_update(&c, data, n);
  br_sha256_out(&c, out);
}

static void hmac(const br_hash_class* h, const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t* out) {
  br_hmac_key_context kc;
  br_hmac_context hc;
  br_hmac_key_init(&kc, h, key, keyLen);
  br_hmac_init(&hc, &kc, 0);
  br_hmac_update(&hc, data, n);
  br_hmac_out(&hc, out);
}

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[32]) { hmac(&br_sha256_vtable, key, keyLen, data, n, out); }
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[20]) { hmac(&br_sha1_vtable, key, keyLen, data, n, out); }

void md5(const uint8_t* data, size_t n, uint8_t out[16]) {
  br_md5_context c;
  br_md5_init(&c);
  br_md5_update(&c, data, n);
  br_md5_out(&c, out);
}

uint32_t crc32(const uint8_t* data, size_t n) {
  static uint32_t table[256];
  static bool ready = [] {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)ready;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

Bytes hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen, size_t outLen) {
  br_hkdf_context hc;
  br_hkdf_init(&hc, &br_sha256_vtable, salt, saltLen);
  br_hkdf_inject(&hc, ikm, ikmLen);
  br_hkdf_flip(&hc);
  Bytes out(outLen);
  br_hkdf_produce(&hc, info, infoLen, out.data(), outLen);
  return out;
}

/* ------------------------------------------------ AES-128-GCM */

struct AesGcm::Impl {
  union {
    br_aes_x86ni_ctr_keys x86ni;
    br_aes_ct64_ctr_keys ct64;
  } keys;
  const br_block_ctr_class* vtable = nullptr;
  br_ghash ghash = nullptr;
  br_gcm_context gcm;
};

AesGcm::AesGcm(const uint8_t key[16]) : impl_(new Impl) {
  const br_block_ctr_class* ni = br_aes_x86ni_ctr_get_vtable();
  impl_->vtable = ni ? ni : &br_aes_ct64_ctr_vtable;
  impl_->vtable->init(reinterpret_cast<const br_block_ctr_class**>(&impl_->keys), key, 16);
  br_ghash g = br_ghash_pclmul_get();
  impl_->ghash = g ? g : &br_ghash_ctmul64;
  br_gcm_init(&impl_->gcm, reinterpret_cast<const br_block_ctr_class**>(&impl_->keys), impl_->ghash);
}

AesGcm::~AesGcm() = default;

void AesGcm::seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) {
  br_gcm_reset(&impl_->gcm, iv, 12);
  br_gcm_aad_inject(&impl_->gcm, aad, aadLen);
  br_gcm_flip(&impl_->gcm);
  br_gcm_run(&impl_->gcm, 1, data, n);
  br_gcm_get_tag(&impl_->gcm, tag);
}

bool AesGcm::open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) {
  br_gcm_reset(&impl_->gcm, iv, 12);
  br_gcm_aad_inject(&impl_->gcm, aad, aadLen);
  br_gcm_flip(&impl_->gcm);
  br_gcm_run(&impl_->gcm, 0, data, n);
  return br_gcm_check_tag(&impl_->gcm, tag) == 1;
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
