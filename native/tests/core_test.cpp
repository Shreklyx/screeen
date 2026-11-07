// Core tests: JSON, packets, room keys, and two endpoints exchanging frames over loopback (with lost
// packets asked for again). The cryptography's own tests: crypto_test.cpp.
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "clock.h"
#include "crypto.h"
#include "crypto_core.h"
#include "direct.h"
#include "engine.h"
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
  // the room's own key mixed in: the same on both sides, other keys than without it (or with another)
  Bytes psk(16, 7), other(16, 8);
  direct_detail::Keys ph, pv, po;
  CHECK(direct_detail::deriveKeys(h, vp, 42, ph, psk) && direct_detail::deriveKeys(v, hp, 42, pv, psk) && ph.toViewer == pv.toViewer && ph.toHost == pv.toHost, "with the room's key: both sides the same keys");
  CHECK(direct_detail::deriveKeys(v, hp, 42, po, other) && po.toViewer != ph.toViewer && ph.toViewer != kh.toViewer, "another room key (or none): other keys");
  // the cipher: AEGIS-128L between two apps (with AES instructions), ChaCha20-Poly1305 with a browser (or a
  // CPU without them), AES-128-GCM with an older side – the same answer on both sides
  using direct_detail::chooseAead;
  const std::vector<std::string> app = {kAegis128L, kAes128Gcm, kChaCha20Poly1305}, web = {kChaCha20Poly1305, kAes128Gcm}, slowCpu = web,
                                 lastVersion = {kAes128Gcm, kChaCha20Poly1305}, older;
  auto both = [&](const std::vector<std::string>& a, const std::vector<std::string>& b, const char* want) {
    return std::string(chooseAead(a, b)) == want && std::string(chooseAead(b, a)) == want;
  };
  CHECK(both(app, app, kAegis128L) && both(app, web, kChaCha20Poly1305) && both(web, web, kChaCha20Poly1305) && both(app, slowCpu, kChaCha20Poly1305),
        "the cipher: AEGIS-128L between two apps, ChaCha20-Poly1305 with a browser or a CPU without AES");
  CHECK(both(app, lastVersion, kAes128Gcm) && both(app, older, kAes128Gcm) && both(web, older, kAes128Gcm), "an older app: AES-128-GCM");
  direct_detail::Keys ch, cv, ah, av;
  CHECK(direct_detail::deriveKeys(h, vp, 42, ch, psk, kChaCha20Poly1305) && direct_detail::deriveKeys(v, hp, 42, cv, psk, kChaCha20Poly1305) && ch.toViewer == cv.toViewer &&
            ch.toHost == cv.toHost && !std::equal(ch.toViewer.begin(), ch.toViewer.begin() + 16, ph.toViewer.begin()),
        "ChaCha20-Poly1305's keys: the same on both sides, 32 bytes, not AES's");
  CHECK(direct_detail::deriveKeys(h, vp, 42, ah, psk, kAegis128L) && direct_detail::deriveKeys(v, hp, 42, av, psk, kAegis128L) && ah.toViewer == av.toViewer && ah.toHost == av.toHost &&
            !std::equal(ah.toViewer.begin(), ah.toViewer.begin() + 16, ph.toViewer.begin()) && !std::equal(ah.toViewer.begin(), ah.toViewer.begin() + 16, ch.toViewer.begin()),
        "AEGIS-128L's keys: the same on both sides, its own");
  Aegis128L asend(ah.toViewer.data()), arecv(av.toViewer.data()), awrong(av.toHost.data());
  packet = direct_detail::seal(asend, 1, 3, 42, 9, reinterpret_cast<const uint8_t*>(msg), 5);
  CHECK(direct_detail::unseal(arecv, 1, packet.data(), packet.size(), body) && std::string(body.begin(), body.end()) == "frame" && !direct_detail::unseal(awrong, 1, packet.data(), packet.size(), body),
        "AEGIS-128L packets: open with the right key only");
  ChaCha20Poly1305 csend(ch.toViewer.data()), crecv(cv.toViewer.data()), cwrong(cv.toHost.data());
  packet = direct_detail::seal(csend, 1, 3, 42, 8, reinterpret_cast<const uint8_t*>(msg), 5);
  CHECK(direct_detail::unseal(crecv, 1, packet.data(), packet.size(), body) && std::string(body.begin(), body.end()) == "frame" && !direct_detail::unseal(cwrong, 1, packet.data(), packet.size(), body),
        "ChaCha20-Poly1305 packets: open with the right key only");
  Offer withList;
  withList.aead = web;
  Offer back = Offer::fromJson(withList.toJson());
  CHECK(back.aead == web && Offer::fromJson(Offer().toJson()).aead.empty(), "the offer carries the list (none from an older side)");
}

static void roomKeyTests() {
  // the same values as the website's (test/room-key.test.js) – computed apart, with Python's hmac
  Bytes key;
  for (int i = 0; i < 16; i++) key.push_back(uint8_t(i));
  CHECK(roomkey::base32(key) == "aaaqeayeaudaocajbifqydiob4" && roomkey::unbase32("aaaqeayeaudaocajbifqydiob4") == key, "the key in a link: base32 both ways");
  CHECK(roomkey::fromRoom("abc123.aaaqeayeaudaocajbifqydiob4") == key && roomkey::fromRoom("abc123").empty() && roomkey::fromRoom("abc123.aaaq").empty() && roomkey::fromRoom("abc123.AAAQEAYEAUDAOCAJBIFQYDIOB4").empty(), "the key from code.key");
  const char* pub = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIzNDU=";
  CHECK(roomkey::proof(key, "host", "0011223344556677", pub) == "UK0D35ordS81pSx+UUzQHQ==", "the sharer's proof");
  CHECK(roomkey::proof(key, "viewer", "0011223344556677", pub) == "svWv4TCw/ObLMyKUqLi+xg==", "the viewer's proof");
}

/** Two endpoints in one loop exchange frames over loopback. `lossy`: the third packet of every frame
 *  never arrives (nor when it is asked for again) – only the parity can bring it back. */
static void endpointTest(bool lossy, bool chacha = false) {
  Loop loop;
  loop.start();
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<uint32_t, size_t>> got;
  std::string hostState, viewState, hostAead, viewAead;
  int keyframes = 0;
  DirectEndpoint *host = nullptr, *view = nullptr;
  bool ready = false;
  loop.post([&] {
    DirectEndpoint::Events he, ve;
    he.state = [&](const std::string&, const Json& i) {
      std::lock_guard<std::mutex> l(mu);
      hostState = i["state"].str() + "/" + i["path"].str("");
      hostAead = i["aead"].str(hostAead);
      cv.notify_all();
    };
    he.keyframe = [&](const std::string&) { keyframes++; };
    ve.state = [&](const std::string&, const Json& i) {
      std::lock_guard<std::mutex> l(mu);
      viewState = i["state"].str();
      viewAead = i["aead"].str(viewAead);
      cv.notify_all();
    };
    ve.media = [&](const MediaMeta& m, const uint8_t* d, size_t n) {
      std::lock_guard<std::mutex> l(mu);
      if (m.stream == 0) got.push_back({d[0] | d[1] << 8, n});
      cv.notify_all();
    };
    host = new DirectEndpoint(loop, DirectEndpoint::Role::Host, "127.0.0.1", "", nullptr, he);
    view = new DirectEndpoint(loop, DirectEndpoint::Role::Viewer, "127.0.0.1", "", nullptr, ve);
    view->preferChaCha = chacha; // (the viewer as a browser: ChaCha20-Poly1305)
    if (lossy) {
      host->forceFec = 5;
      view->dropIncoming = [](uint8_t type, const uint8_t* b, size_t n) { return type == 3 && n >= kMediaHeader && b[0] == 0 && getU16(b + 12) == 2; };
    }
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
  const std::string want = chacha || !crypto::hasAesInstructions() ? kChaCha20Poly1305 : kAegis128L;
  CHECK(hostAead == want && viewAead == want, ("both use " + want).c_str());
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
  if (!lossy) {
    CHECK(inOrder, "30 frames arrive whole and in order");
  } else {
    std::atomic<uint64_t> rebuilt{0};
    std::atomic<bool> asked{false};
    loop.post([&] {
      rebuilt = view->fecRebuilt();
      asked = true;
    });
    while (!asked) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(inOrder && rebuilt >= 30, ("a lost packet in every frame: parity rebuilds them, the frames arrive whole and in order (" + std::to_string(got.size()) + " frames, " +
                                     std::to_string(rebuilt.load()) + " rebuilt)").c_str());
  }
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
  jsonTests();
  packetTests();
  roomKeyTests();
  endpointTest(false);
  endpointTest(true);
  endpointTest(false, true); // (with ChaCha20-Poly1305, as with a browser)
  std::printf(failures ? "%d FAILED\n" : "ALL PASS\n", failures);
  return failures ? 1 : 0;
}
