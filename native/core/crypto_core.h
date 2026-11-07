// Screen Share – our own cryptography's core (crypto_core.cpp): the standard algorithms on plain bytes,
// with no C++ library and nothing allocated (scrypt's memory aside). So the very same code builds into the
// apps (crypto.h wraps it), into the website's WebAssembly (native/web) and, through that, the server.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {
namespace crypto {

void sha256(const uint8_t* data, size_t n, uint8_t out[32]);
void sha1(const uint8_t* data, size_t n, uint8_t out[20]);
void md5(const uint8_t* data, size_t n, uint8_t out[16]);
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[32]);
void hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n, uint8_t out[20]);
/** HKDF-SHA256 (RFC 5869) into out[outLen]; false when outLen is over 255 · 32. */
bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen);
uint32_t crc32(const uint8_t* data, size_t n);
/** X25519 (RFC 7748): out = the scalar (clamped) times the point; false when that is all zeros (a
 *  low-order point). */
bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/** scrypt (RFC 7914) – the server's password hashes; false for parameters out of range or no memory. */
bool scrypt(const uint8_t* pass, size_t passLen, const uint8_t* salt, size_t saltLen, uint64_t N, uint32_t r, uint32_t p, uint8_t* out, size_t outLen);

/** AES-128-GCM's state for one key: plain data, made by gcmInit (and best wiped after). */
struct Gcm {
  int engine;             // what runs it (engineName)
  uint8_t rk[176];        // the round keys, in FIPS 197's byte order
  uint64_t h[8][2];       // H, H², … H⁸ (H = E(K, 0)) in GHASH's field: the low, the high 64 bits
  uint64_t planes[11][8]; // (the portable code: the round keys as bit planes)
};
void gcmInit(Gcm& g, const uint8_t key[16]);
/** data encrypted in place (a 12-byte iv); the tag into tag[16]. */
void gcmSeal(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, uint8_t tag[16]);
/** data decrypted in place; false (and data undefined) when the tag does not match. */
bool gcmOpen(const Gcm& g, const uint8_t iv[12], const uint8_t* aad, size_t aadLen, uint8_t* data, size_t n, const uint8_t tag[16]);

/** What runs AES and GHASH here: "x86 AES-NI + PCLMULQDQ", "ARMv8 AES + PMULL" or "portable". */
const char* engineName();
/** Zeros the compiler keeps (a key out of memory). */
void wipe(void* p, size_t n);

}  // namespace crypto
}  // namespace ss
