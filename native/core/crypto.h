// Screen Share native core – the cryptography of direct mode (same as test/direct-reference.cjs and
// public/direct-web.js): X25519 key agreement, HKDF-SHA256, AES-128-GCM (between apps) or
// ChaCha20-Poly1305 (with a browser), HMAC-SHA256; for the
// TURN client MD5, HMAC-SHA1 and CRC-32; SHA-1 for the WebSocket handshake. Our own code, the standard
// algorithms from their specifications – no library: crypto_core.cpp (which the website's WebAssembly and
// the server run too); crypto.cpp is its C++ face.
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

/** What runs AES and GHASH here: "x86 AES-NI + PCLMULQDQ", "ARMv8 AES + PMULL" or "portable". */
const char* cryptoEngine();

void sha256(const uint8_t* data, size_t n, uint8_t out[32]);
void sha1(const uint8_t* data, size_t n, uint8_t out[20]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[32]);
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[20]);
void md5(const uint8_t* data, size_t n, uint8_t out[16]);
uint32_t crc32(const uint8_t* data, size_t n);
Bytes hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen, size_t outLen);

/** An AEAD with a 12-byte nonce and a 16-byte tag: AesGcm or ChaCha20Poly1305. */
class Aead {
 public:
  virtual ~Aead() = default;
  /** data is encrypted in place; the tag goes to tag[16]. */
  virtual void seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) = 0;
  /** data is decrypted in place; false (and data undefined) when the tag does not match. */
  virtual bool open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) = 0;
};

/** AES-128-GCM (hardware AES / carry-less multiply when the CPU has them): between two apps. */
class AesGcm : public Aead {
 public:
  explicit AesGcm(const uint8_t key[16]);
  ~AesGcm() override;
  AesGcm(const AesGcm&) = delete;
  AesGcm& operator=(const AesGcm&) = delete;
  void seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) override;
  bool open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** ChaCha20-Poly1305 (RFC 8439), a 32-byte key: with a browser – fast in software, where AES is not. */
class ChaCha20Poly1305 : public Aead {
 public:
  explicit ChaCha20Poly1305(const uint8_t key[32]);
  ~ChaCha20Poly1305() override;
  ChaCha20Poly1305(const ChaCha20Poly1305&) = delete;
  ChaCha20Poly1305& operator=(const ChaCha20Poly1305&) = delete;
  void seal(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]) override;
  bool open(const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]) override;

 private:
  uint8_t key_[32];
};

}  // namespace ss
