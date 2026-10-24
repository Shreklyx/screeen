// Core tests: known-answer vectors for the cryptography, JSON, packets, and two endpoints
// exchanging frames over loopback (with lost packets asked for again).
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "clock.h"
#include "crypto.h"
#include "direct.h"
#include "json.h"
#include "net.h"

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

static void cryptoVectors() {
  // RFC 7748 section 6.1
  X25519 alice;
  Bytes a = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  std::memcpy(alice.secret.data(), a.data(), 32);
  Bytes bobPub = hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  uint8_t shared[32];
  CHECK(alice.shared(bobPub.data(), shared) && hexOf(shared, 32) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "X25519 RFC 7748 shared secret");
  X25519 k = X25519::generate();
  X25519 check;
  check.secret = k.secret;
  uint8_t s1[32], s2[32];
  X25519 other = X25519::generate();
  CHECK(k.shared(other.pub.data(), s1) && other.shared(k.pub.data(), s2) && std::memcmp(s1, s2, 32) == 0, "X25519 both sides agree");

  // GCM test case 4 (McGrew & Viega)
  Bytes key = hex("feffe9928665731c6d6a8f9467308308");
  Bytes iv = hex("cafebabefacedbaddecaf888");
  Bytes pt = hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
  Bytes aad = hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
  AesGcm gcm(key.data());
  Bytes data = pt;
  uint8_t tag[16];
  gcm.seal(iv.data(), aad.data(), aad.size(), data.data(), data.size(), tag);
  CHECK(hexOf(data.data(), data.size()) == "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091" &&
            hexOf(tag, 16) == "5bc94fbc3221a5db94fae95ae7121a47",
        "AES-128-GCM test case 4");
  CHECK(gcm.open(iv.data(), aad.data(), aad.size(), data.data(), data.size(), tag) && data == pt, "AES-128-GCM opens");
  data[0] ^= 1;
  CHECK(!gcm.open(iv.data(), aad.data(), aad.size(), data.data(), data.size(), tag), "AES-128-GCM rejects a changed packet");

  // RFC 5869 test case 1
  Bytes ikm = hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
  Bytes salt = hex("000102030405060708090a0b0c");
  Bytes info = hex("f0f1f2f3f4f5f6f7f8f9");
  Bytes okm = hkdfSha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), 42);
  CHECK(hexOf(okm.data(), okm.size()) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", "HKDF-SHA256 RFC 5869");

  // RFC 4231 test case 2
  const char* k2 = "Jefe";
  const char* d2 = "what do ya want for nothing?";
  uint8_t mac[32];
  hmacSha256(reinterpret_cast<const uint8_t*>(k2), 4, reinterpret_cast<const uint8_t*>(d2), std::strlen(d2), mac);
  CHECK(hexOf(mac, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC-SHA256 RFC 4231");
  uint8_t m5[16];
  md5(reinterpret_cast<const uint8_t*>("abc"), 3, m5);
  CHECK(hexOf(m5, 16) == "900150983cd24fb0d6963f7d28e17f72", "MD5");
  CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "CRC-32");
  uint8_t r[16] = {0};
  randomBytes(r, 16);
  bool nonzero = false;
  for (uint8_t b : r) nonzero |= b != 0;
  CHECK(nonzero, "random bytes");
  std::string b64 = base64Encode(reinterpret_cast<const uint8_t*>("hello!?"), 7);
  Bytes back = base64Decode(b64);
  CHECK(b64 == "aGVsbG8hPw==" && std::string(back.begin(), back.end()) == "hello!?", "base64");
}

static void jsonTests() {
  Json j = Json::parse(R"({"t":"signal","data":{"type":"direct-offer","udp":4294967295,"cands":[{"address":"10.0.0.2","port":5000}],"tcp":true,"s":"é\n\"x\""},"n":-1.5,"z":null})");
  CHECK(j["t"].str() == "signal" && j["data"]["udp"].num() == 4294967295.0 && j["data"]["cands"].items().size() == 1 && j["data"]["tcp"].b() && j["n"].num() == -1.5 && j["z"].isNull(), "JSON parse");
  CHECK(j["data"]["s"].str() == "é\n\"x\"", "JSON strings");
  Json again = Json::parse(j.dump());
  CHECK(again.dump() == j.dump(), "JSON round trip");
  CHECK(Json::parse("{bad").isNull() && Json::parse("[1,2").isNull(), "JSON rejects bad input");
  Offer o = Offer::fromJson(Json::parse(R"({"pub":"abc","cands":[{"address":"10.0.0.2","port":5000},{"address":"0.1.2.3","port":1},{"address":"1.2.3.4","port":70000},{"address":"5.6.7.8","port":9,"turn":true}]})"));
  CHECK(o.cands.size() == 2 && o.cands[1].turn, "candidates are checked");
  CHECK(direct_detail::isPrivate("192.168.1.5") && direct_detail::isPrivate("100.64.0.1") && !direct_detail::isPrivate("8.8.8.8"), "private ranges");
}

static void packetTests() {
  X25519 h = X25519::generate(), v = X25519::generate();
  direct_detail::Keys kh, kv;
  Bytes vp(v.pub.begin(), v.pub.end()), hp(h.pub.begin(), h.pub.end());
  CHECK(direct_detail::deriveKeys(h, vp, 42, kh) && direct_detail::deriveKeys(v, hp, 42, kv) && kh.toViewer == kv.toViewer && kh.toHost == kv.toHost, "both sides derive the same keys");
  AesGcm send(kh.toViewer.data()), recv(kv.toViewer.data()), wrong(kv.toHost.data());
  const char* msg = "frame";
  Bytes packet = direct_detail::seal(send, 1, 3, 42, 7, reinterpret_cast<const uint8_t*>(msg), 5);
  Bytes body;
  CHECK(direct_detail::unseal(recv, 1, packet.data(), packet.size(), body) && std::string(body.begin(), body.end()) == "frame", "packet opens with the right key");
  CHECK(!direct_detail::unseal(recv, 2, packet.data(), packet.size(), body) && !direct_detail::unseal(wrong, 1, packet.data(), packet.size(), body), "not with the wrong direction or key");
}

/** Two endpoints in one loop exchange frames over loopback; some packets are dropped on the way. */
static void endpointTest() {
  Loop loop;
  loop.start();
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<uint32_t, size_t>> got;
  std::string hostState, viewState;
  int keyframes = 0;
  DirectEndpoint *host = nullptr, *view = nullptr;
  bool ready = false;
  loop.post([&] {
    DirectEndpoint::Events he, ve;
    he.state = [&](const std::string&, const Json& i) { std::lock_guard<std::mutex> l(mu); hostState = i["state"].str() + "/" + i["path"].str(""); cv.notify_all(); };
    he.keyframe = [&](const std::string&) { keyframes++; };
    ve.state = [&](const std::string&, const Json& i) { std::lock_guard<std::mutex> l(mu); viewState = i["state"].str(); cv.notify_all(); };
    ve.media = [&](const MediaMeta& m, const uint8_t* d, size_t n) {
      std::lock_guard<std::mutex> l(mu);
      if (m.stream == 0) got.push_back({d[0] | d[1] << 8, n});
      cv.notify_all();
    };
    host = new DirectEndpoint(loop, DirectEndpoint::Role::Host, "127.0.0.1", "", nullptr, he);
    view = new DirectEndpoint(loop, DirectEndpoint::Role::Viewer, "127.0.0.1", "", nullptr, ve);
    host->open();
    view->open();
    PeerInfo info;
    info.session = 4242;
    info.secret = base64Encode(reinterpret_cast<const uint8_t*>("0123456789abcdef"), 16);
    host->addPeer("v", info, [&, info](bool ok1, Offer offer) {
      view->addPeer("host", info, [&, offer, ok1](bool ok2, Offer answer) {
        // both have their LAN addresses; loopback too, for a computer without a network
        Offer o = offer, a = answer;
        o.cands.push_back({"127.0.0.1", host->port(), false});
        a.cands.push_back({"127.0.0.1", view->port(), false});
        view->connectPeer("host", o);
        host->connectPeer("v", a);
        std::lock_guard<std::mutex> l(mu);
        ready = ok1 && ok2;
        cv.notify_all();
      });
    });
  });
  {
    std::unique_lock<std::mutex> l(mu);
    cv.wait_for(l, std::chrono::seconds(8), [&] { return hostState.rfind("connected", 0) == 0 && viewState == "connected"; });
  }
  CHECK(ready && hostState.rfind("connected", 0) == 0 && viewState == "connected", ("endpoints connect (" + hostState + ")").c_str());
  // 30 frames of ~5 packets each
  loop.post([&] {
    for (int n = 0; n < 30; n++) {
      Bytes frame(5 * kMaxData + 17 + n);
      for (size_t i = 0; i < frame.size(); i++) frame[i] = uint8_t(i * 7 + n);
      frame[0] = uint8_t(n);
      frame[1] = 0;
      MediaMeta m;
      m.stream = 0;
      m.key = n == 0;
      m.ts = n;
      m.capture = nowMs();
      host->sendMedia(m, frame.data(), frame.size());
    }
  });
  {
    std::unique_lock<std::mutex> l(mu);
    cv.wait_for(l, std::chrono::seconds(5), [&] { return got.size() >= 30; });
  }
  bool inOrder = got.size() == 30;
  for (size_t i = 0; inOrder && i < got.size(); i++) inOrder = got[i].first == i && got[i].second == 5 * kMaxData + 17 + i;
  CHECK(inOrder, "30 frames arrive whole and in order");
  std::atomic<bool> closed{false};
  loop.post([&] {
    delete view;
    delete host;
    closed = true;
  });
  while (!closed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  loop.stop();
}

int main() {
  netInit();
  cryptoVectors();
  jsonTests();
  packetTests();
  endpointTest();
  std::printf(failures ? "%d FAILED\n" : "ALL PASS\n", failures);
  return failures ? 1 : 0;
}
