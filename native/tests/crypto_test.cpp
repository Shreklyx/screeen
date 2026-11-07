// Our own cryptography (core/crypto.cpp) against the standards' own test vectors, and long sweeps against
// values computed apart from our code (Python's hashlib / hmac and OpenSSL, through the cryptography package):
//   SHA-256, SHA-1 (FIPS 180-4) · MD5 (RFC 1321) · HMAC (RFC 4231, RFC 2202) · HKDF (RFC 5869)
//   X25519 (RFC 7748) · AES-128-GCM (the GCM specification's test cases, McGrew & Viega)
// Built twice: crypto_test runs on this CPU's AES and carry-less multiply instructions, crypto_portable_test
// on the portable code – the same answers from both. "--expect hardware" / "--expect portable" also checks
// which one ran (CI: the fast one must be the one in use).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "crypto.h"

using namespace ss;

static int failures = 0;
#define CHECK(c, what)                                     \
  do {                                                     \
    if (c) std::printf("ok   %s\n", what);                 \
    else { std::printf("FAIL %s\n", what); failures++; }   \
  } while (0)

static Bytes hex(const char* s) {
  Bytes b;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    unsigned v;
    std::sscanf(s + i, "%2x", &v);
    b.push_back(uint8_t(v));
  }
  return b;
}
static std::string hexOf(const uint8_t* p, size_t n) { return toHex(p, n); }
static std::string hexOf(const Bytes& b) { return toHex(b.data(), b.size()); }
static Bytes text(const char* s) { return Bytes(s, s + std::strlen(s)); }
/** n bytes of (i·mul + add) – the same as the Python that computed the sweeps' values. */
static Bytes pattern(size_t n, unsigned mul, unsigned add) {
  Bytes b(n);
  for (size_t i = 0; i < n; i++) b[i] = uint8_t(i * mul + add);
  return b;
}
static void append(Bytes& to, const uint8_t* p, size_t n) { to.insert(to.end(), p, p + n); }
static std::string sha256Hex(const Bytes& b) {
  uint8_t d[32];
  sha256(b.data(), b.size(), d);
  return hexOf(d, 32);
}

static void hashes() {
  const Bytes m448 = text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), million(1000000, 'a');
  uint8_t d[32];
  CHECK(sha256Hex(text("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 \"abc\" (FIPS 180-4)");
  CHECK(sha256Hex(Bytes()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
  CHECK(sha256Hex(m448) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 448 bits");
  CHECK(sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 a million a's");
  sha1(reinterpret_cast<const uint8_t*>("abc"), 3, d);
  CHECK(hexOf(d, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 \"abc\"");
  sha1(m448.data(), m448.size(), d);
  CHECK(hexOf(d, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA-1 448 bits");
  sha1(million.data(), million.size(), d);
  CHECK(hexOf(d, 20) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA-1 a million a's");
  const char* md5Suite[][2] = {{"", "d41d8cd98f00b204e9800998ecf8427e"},
                               {"a", "0cc175b9c0f1b6a831c399e269772661"},
                               {"abc", "900150983cd24fb0d6963f7d28e17f72"},
                               {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
                               {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
                               {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
                               {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"}};
  bool md5ok = true;
  for (auto& t : md5Suite) {
    md5(reinterpret_cast<const uint8_t*>(t[0]), std::strlen(t[0]), d);
    md5ok &= hexOf(d, 16) == t[1];
  }
  CHECK(md5ok, "MD5 (RFC 1321's test suite)");
  // every length 0 … 200 (each padding case, one block and more)
  Bytes all;
  for (size_t n = 0; n <= 200; n++) {
    Bytes m = pattern(n, 13, 1);
    sha256(m.data(), n, d);
    append(all, d, 32);
    sha1(m.data(), n, d);
    append(all, d, 20);
    md5(m.data(), n, d);
    append(all, d, 16);
  }
  CHECK(sha256Hex(all) == "4a18822535175578f7177ca80f02a7c335983edb8565dfb26a7fb7ce6f99245c", "SHA-256, SHA-1, MD5: every length 0 … 200 (as hashlib)");
  CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "CRC-32");
}

static void macs() {
  uint8_t mac[32];
  Bytes k = Bytes(20, 0x0b), m = text("Hi There");
  hmacSha256(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "HMAC-SHA256 RFC 4231 case 1");
  hmacSha1(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 20) == "b617318655057264e28bc0b6fb378c8ef146be00", "HMAC-SHA1 RFC 2202 case 1");
  k = text("Jefe");
  m = text("what do ya want for nothing?");
  hmacSha256(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC-SHA256 RFC 4231 case 2");
  hmacSha1(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", "HMAC-SHA1 RFC 2202 case 2");
  k = Bytes(20, 0xaa);
  m = Bytes(50, 0xdd);
  hmacSha256(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 32) == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "HMAC-SHA256 RFC 4231 case 3");
  k = Bytes(131, 0xaa); // (a key longer than a block: hashed first)
  m = text("Test Using Larger Than Block-Size Key - Hash Key First");
  hmacSha256(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "HMAC-SHA256 RFC 4231 case 6");
  m = text("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.");
  hmacSha256(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 32) == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "HMAC-SHA256 RFC 4231 case 7");
  k = Bytes(80, 0xaa);
  m = text("Test Using Larger Than Block-Size Key - Hash Key First");
  hmacSha1(k.data(), k.size(), m.data(), m.size(), mac);
  CHECK(hexOf(mac, 20) == "aa4ae5e15272d00e95705637ce8a3b55ed402112", "HMAC-SHA1 RFC 2202 case 6");
  Bytes all;
  for (size_t n = 0; n <= 150; n++) {
    Bytes key = pattern(n, 5, 7), msg = pattern(n * 3, 13, 1);
    hmacSha256(key.data(), n, msg.data(), msg.size(), mac);
    append(all, mac, 32);
    hmacSha1(key.data(), n, msg.data(), msg.size(), mac);
    append(all, mac, 20);
  }
  CHECK(sha256Hex(all) == "181405badb2857143465a7ed151103e100ed247a015006d1017cc4dac112bb9f", "HMAC-SHA256, HMAC-SHA1: keys of 0 … 150 bytes (as Python's hmac)");

  Bytes ikm = Bytes(22, 0x0b), salt = hex("000102030405060708090a0b0c"), info = hex("f0f1f2f3f4f5f6f7f8f9");
  CHECK(hexOf(hkdfSha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), 42)) ==
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
        "HKDF-SHA256 RFC 5869 case 1");
  ikm = pattern(80, 1, 0x00);
  salt = pattern(80, 1, 0x60);
  info = pattern(80, 1, 0xb0);
  CHECK(hexOf(hkdfSha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), 82)) ==
            "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87",
        "HKDF-SHA256 RFC 5869 case 2 (long inputs)");
  ikm = Bytes(22, 0x0b);
  CHECK(hexOf(hkdfSha256(nullptr, 0, ikm.data(), ikm.size(), nullptr, 0, 42)) ==
            "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
        "HKDF-SHA256 RFC 5869 case 3 (no salt, no info)");
  all.clear();
  for (size_t len : {1, 31, 32, 33, 64, 100, 255}) {
    Bytes s = pattern(len % 70, 5, 7), in = pattern(len % 40, 13, 1), key = pattern(len, 13, 1);
    Bytes okm = hkdfSha256(s.data(), s.size(), key.data(), key.size(), in.data(), in.size(), len);
    all.insert(all.end(), okm.begin(), okm.end());
  }
  CHECK(sha256Hex(all) == "3db1eb430f0caf8586da4f798ca1be063de874e8932036e00ec025deb904eaf4", "HKDF-SHA256: lengths 1 … 255 (as OpenSSL)");
}

static void x25519() {
  auto scalarmult = [](const char* scalar, const Bytes& u) {
    X25519 k;
    Bytes s = hex(scalar);
    std::memcpy(k.secret.data(), s.data(), 32);
    uint8_t out[32];
    k.shared(u.data(), out);
    return hexOf(out, 32);
  };
  CHECK(scalarmult("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")) ==
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
        "X25519 RFC 7748 5.2, first");
  CHECK(scalarmult("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", hex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")) ==
            "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
        "X25519 RFC 7748 5.2, second (the top bit of u set: ignored)");
  // 5.2's iterations: k = u = 9; k, u = X25519(k, u), k
  Bytes k(32, 0), u(32, 0);
  k[0] = u[0] = 9;
  std::string after1;
  for (int i = 0; i < 1000; i++) {
    X25519 x;
    std::memcpy(x.secret.data(), k.data(), 32);
    Bytes next(32);
    x.shared(u.data(), next.data());
    u = k;
    k = next;
    if (i == 0) after1 = hexOf(k);
  }
  CHECK(after1 == "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", "X25519 RFC 7748 5.2, one iteration");
  CHECK(hexOf(k) == "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", "X25519 RFC 7748 5.2, 1000 iterations");
  // 6.1: both sides' public keys and the shared secret
  Bytes nine(32, 0);
  nine[0] = 9;
  X25519 alice, bob;
  Bytes a = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"), b = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  std::memcpy(alice.secret.data(), a.data(), 32);
  std::memcpy(bob.secret.data(), b.data(), 32);
  uint8_t ap[32], bp[32], s1[32], s2[32];
  alice.shared(nine.data(), ap);
  bob.shared(nine.data(), bp);
  CHECK(hexOf(ap, 32) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a" && hexOf(bp, 32) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
        "X25519 RFC 7748 6.1 public keys");
  CHECK(alice.shared(bp, s1) && bob.shared(ap, s2) && hexOf(s1, 32) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742" && std::memcmp(s1, s2, 32) == 0,
        "X25519 RFC 7748 6.1 shared secret");
  X25519 g = X25519::generate(), h = X25519::generate();
  uint8_t gp[32];
  g.shared(nine.data(), gp);
  CHECK(std::memcmp(gp, g.pub.data(), 32) == 0 && g.shared(h.pub.data(), s1) && h.shared(g.pub.data(), s2) && std::memcmp(s1, s2, 32) == 0,
        "X25519 new keys: the public key, both sides agree");
  uint8_t zero[32] = {0}, one[32] = {1};
  CHECK(!g.shared(zero, s1) && !g.shared(one, s1), "X25519 refuses low-order points (an all-zero secret)");
  Bytes all;
  for (unsigned i = 0; i < 20; i++) {
    X25519 x;
    for (unsigned j = 0; j < 32; j++) x.secret[j] = uint8_t(j * 11 + i * 3 + 1);
    Bytes peer = pattern(32, 17, i * 29 + 5);
    uint8_t s[32], p[32];
    x.shared(peer.data(), s);
    x.shared(nine.data(), p);
    append(all, s, 32);
    append(all, p, 32);
  }
  CHECK(sha256Hex(all) == "aceb328b135f40e19ae90f82a98c25a24120b76990a3400aafeb77f17155992c", "X25519: 20 key pairs (as OpenSSL)");
}

static void gcm() {
  struct Case {
    const char *key, *iv, *pt, *aad, *ct, *tag;
  } cases[] = {
      {"00000000000000000000000000000000", "000000000000000000000000", "", "", "", "58e2fccefa7e3061367f1d57a4e7455a"},
      {"00000000000000000000000000000000", "000000000000000000000000", "00000000000000000000000000000000", "", "0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf"},
      {"feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
       "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255", "",
       "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985", "4d5c2af327cd64a62cf35abd2ba6fab4"},
      {"feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
       "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
       "feedfacedeadbeeffeedfacedeadbeefabaddad2",
       "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", "5bc94fbc3221a5db94fae95ae7121a47"},
  };
  int n = 1;
  for (const Case& c : cases) {
    Bytes key = hex(c.key), iv = hex(c.iv), pt = hex(c.pt), aad = hex(c.aad), data = pt;
    AesGcm g(key.data());
    uint8_t tag[16];
    g.seal(iv.data(), aad.data(), aad.size(), data.data(), data.size(), tag);
    std::string name = "AES-128-GCM test case " + std::to_string(n++);
    CHECK(hexOf(data) == c.ct && hexOf(tag, 16) == c.tag, name.c_str());
    bool opens = g.open(iv.data(), aad.data(), aad.size(), data.data(), data.size(), tag) && data == pt;
    CHECK(opens, (name + " opens").c_str());
    g.seal(iv.data(), aad.data(), aad.size(), data.data(), data.size(), tag);
    bool refused = true;
    if (!data.empty()) {
      Bytes bad = data;
      bad[bad.size() / 2] ^= 0x10;
      refused &= !g.open(iv.data(), aad.data(), aad.size(), bad.data(), bad.size(), tag);
    }
    uint8_t badTag[16];
    std::memcpy(badTag, tag, 16);
    badTag[15] ^= 1;
    Bytes copy = data;
    refused &= !g.open(iv.data(), aad.data(), aad.size(), copy.data(), copy.size(), badTag);
    Bytes otherAad = aad;
    otherAad.push_back(0);
    copy = data;
    refused &= !g.open(iv.data(), otherAad.data(), otherAad.size(), copy.data(), copy.size(), tag);
    CHECK(refused, (name + ": a changed byte, tag or header is refused").c_str());
  }
  // every tail: data of 0 … 100003 bytes (the four-block path, single blocks, a partial last block) with
  // headers of 0 … 33 bytes – as OpenSSL encrypts them
  Bytes key(16), all;
  for (int i = 0; i < 16; i++) key[i] = uint8_t(i);
  AesGcm g(key.data());
  bool roundTrip = true;
  unsigned idx = 0;
  for (size_t len : {0, 1, 15, 16, 17, 47, 48, 63, 64, 65, 79, 80, 127, 128, 129, 1000, 1200, 4103, 100003})
    for (size_t aadLen : {0, 1, 13, 16, 17, 33}) {
      uint8_t iv[12];
      for (int i = 0; i < 11; i++) iv[i] = uint8_t(100 + i);
      iv[11] = uint8_t(idx++);
      Bytes aad = pattern(aadLen, 7, 3), pt = pattern(len, 31, 11), data = pt;
      uint8_t tag[16];
      g.seal(iv, aad.data(), aadLen, data.data(), len, tag);
      append(all, data.data(), len);
      append(all, tag, 16);
      roundTrip &= g.open(iv, aad.data(), aadLen, data.data(), len, tag) && data == pt;
    }
  CHECK(sha256Hex(all) == "2ef07c6a8c64da29e40c4f9f91c6f35ef5f44cf4276cb944c01b13b80c28f40e", "AES-128-GCM: 114 lengths of data and header (as OpenSSL)");
  CHECK(roundTrip, "AES-128-GCM: all of them open again");
}

/** How fast: sealing 1200-byte packets (what direct mode sends) for a third of a second. */
static void speed() {
  uint8_t key[16] = {7}, iv[12] = {0}, aad[16] = {0}, tag[16];
  AesGcm g(key);
  Bytes packet(1200, 0x5a);
  size_t bytes = 0;
  auto start = std::chrono::steady_clock::now();
  double secs = 0;
  do {
    for (int i = 0; i < 64; i++) {
      iv[0]++;
      g.seal(iv, aad, sizeof aad, packet.data(), packet.size(), tag);
      bytes += packet.size();
    }
    secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  } while (secs < 0.33);
  std::printf("     AES-128-GCM: %.0f MB/s on 1200-byte packets (%s)\n", double(bytes) / secs / 1e6, cryptoEngine());
}

int main(int argc, char** argv) {
  std::printf("     engine: %s\n", cryptoEngine());
  if (argc > 2 && std::strcmp(argv[1], "--expect") == 0) {
    bool hardware = std::strcmp(cryptoEngine(), "portable") != 0;
    bool want = std::strcmp(argv[2], "hardware") == 0;
    CHECK(hardware == want, want ? "the CPU's own AES instructions are in use" : "the portable code is in use");
  }
  hashes();
  macs();
  x25519();
  gcm();
  speed();
  std::printf(failures ? "%d FAILED\n" : "ALL PASS\n", failures);
  return failures ? 1 : 0;
}
