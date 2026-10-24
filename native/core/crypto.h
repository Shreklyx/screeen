// Screen Share native core – the cryptography of direct mode (same as desktop/direct.js and
// public/direct-web.js): X25519 key agreement, HKDF-SHA256, AES-128-GCM, HMAC-SHA256; for the
// TURN client MD5, HMAC-SHA1 and CRC-32. BearSSL underneath (third_party/bearssl, MIT).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "bytes.h"

namespace ss {

void randomBytes(uint8_t* out, size_t n);

struct X25519 {
  std::array<uint8_t, 32> secret{};
  std::array<uint8_t, 32> pub{};
  static X25519 generate();
  /** The shared secret with the other side's public key; false for an invalid key. */
  bool shared(const uint8_t peer[32], uint8_t out[32]) const;
};

void sha256(const uint8_t* data, size_t n, uint8_t out[32]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[32]);
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[20]);
void md5(const uint8_t* data, size_t n, uint8_t out[16]);
uint32_t crc32(const uint8_t* data, size_t n);
Bytes hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen, size_t outLen);

/** AES-128-GCM with a 12-byte nonce and a 16-byte tag (hardware AES / carry-less multiply when the CPU has them). */
class AesGcm {
 public:
  explicit AesGcm(const uint8_t key[16]);
  ~AesGcm();
  AesGcm(const AesGcm&) = delete;
  AesGcm& operator=(const AesGcm&) = delete;
  /** data is encrypted in place; the tag goes to tag[16]. */
  void seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]);
  /** data is decrypted in place; false (and data undefined) when the tag does not match. */
  bool open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
