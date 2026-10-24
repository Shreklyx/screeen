// Screen Share native core – byte helpers (big-endian, as on the wire).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ss {

using Bytes = std::vector<uint8_t>;

inline void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void putU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void putU64(uint8_t* p, uint64_t v) { putU32(p, uint32_t(v >> 32)); putU32(p + 4, uint32_t(v)); }
inline void putF64(uint8_t* p, double v) { uint64_t u; std::memcpy(&u, &v, 8); putU64(p, u); }
inline uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t getU32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) << 32 | getU32(p + 4); }
inline double getF64(const uint8_t* p) { uint64_t u = getU64(p); double v; std::memcpy(&v, &u, 8); return v; }

inline std::string toHex(const uint8_t* p, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
  return s;
}

std::string base64Encode(const uint8_t* p, size_t n);
Bytes base64Decode(const std::string& s); // empty on invalid input

}  // namespace ss
