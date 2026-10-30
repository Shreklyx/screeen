// Screen Share native core – direct mode's transport, the C++ form of desktop/direct.js (the same
// packets, so the native apps, the Electron app and the browsers understand each other).
//
// One UDP socket. Every packet is encrypted (AES-128-GCM, keys from an X25519 exchange over the
// signaling server, so the server cannot read the picture). The sharer's encoded frames are cut
// into ~1.1 KB packets and sent the moment they are ready; the viewer hands every complete frame
// over at once – there is no jitter buffer. Lost packets are asked for again (NACK); a frame that
// cannot be completed in time is skipped and the viewer asks for a keyframe.
//
// Connection: both ends register at the server's UDP port (the answer is their public address),
// exchange addresses over signaling and probe each other (UDP hole punching). When no direct path
// answers, packets go through the server's UDP relay – or the TURN server – or, when no UDP gets
// through at all, in a WebSocket to the server (wss://…/direct), where a frame is one packet and the
// viewer confirms each one (a queue on the way shows at the sharer, which drops frames instead).
//
// Everything runs on the Loop's thread.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bytes.h"
#include "crypto.h"
#include "json.h"
#include "net.h"
#include "websocket.h"

namespace ss {

constexpr size_t kMtu = 1200;
constexpr size_t kHeader = 14;
constexpr size_t kTag = 16;
constexpr size_t kMediaHeader = 32;
constexpr size_t kMaxData = kMtu - kHeader - kTag - kMediaHeader;

struct Candidate {
  std::string address;
  uint16_t port = 0;
  bool turn = false;
};

struct TurnInfo {
  std::string host;
  uint16_t port = 3478;
  std::string username;
  std::string credential;
};

struct PeerInfo {
  uint32_t session = 0;
  std::string secret; // base64
  uint16_t port = 0;  // the server's UDP port (0: none)
  std::optional<TurnInfo> turn;
};

/** What one side tells the other (offer / answer). */
struct Offer {
  std::string pub; // base64, 32 bytes
  std::vector<Candidate> cands;
  bool tcp = false; // only the WebSocket path reaches this side (a browser)
  Json toJson() const;
  static Offer fromJson(const Json& j);
};

struct MediaMeta {
  int stream = 0; // 0 video, 1 audio
  bool key = false;
  int sub = 0; // audio: pair | channels << 4
  double ts = 0;
  double capture = 0;
  uint32_t id = 0;
};

class TurnClient;

class DirectEndpoint {
 public:
  enum class Role { Host, Viewer };
  struct Events {
    std::function<void(const std::string& id, const Json& info)> state; // {state, path, rtt[, diag]}
    std::function<void(const std::string& id)> keyframe;               // host: a viewer needs a keyframe
    std::function<void(const std::string& id, const Json& report)> report;
    std::function<void(const std::string& id, double rtt, double offset)> clock;
    std::function<void(const std::string& id, const Json& stats)> stats; // viewer: its own report
    std::function<void(const MediaMeta& meta, const uint8_t* data, size_t n)> media;
    std::function<void(const std::string& line)> log;
  };

  DirectEndpoint(Loop& loop, Role role, std::string serverHost, std::string tcpUrl, WebSocketFactory ws, Events events);
  ~DirectEndpoint();
  DirectEndpoint(const DirectEndpoint&) = delete;
  DirectEndpoint& operator=(const DirectEndpoint&) = delete;

  bool open();
  void close();
  Role role() const { return role_; }
  uint16_t port() const { return socket_.port(); }

  /** Starts a connection; done(true, what the other side needs) – or done(false, {}). */
  void addPeer(const std::string& id, const PeerInfo& info, std::function<void(bool, Offer)> done);
  /** The other side's answer: derive the keys and find a path. */
  void connectPeer(const std::string& id, const Offer& other);
  void removePeer(const std::string& id);
  /** Viewer: ask the sharer for a keyframe (the decoder could not go on). */
  void keyframe(const std::string& id);
  void setBitrate(double bps);
  /** Host: one encoded frame (video) or Opus packet (audio) to every connected viewer. */
  void sendMedia(const MediaMeta& meta, const uint8_t* data, size_t n);

  // tests / troubleshooting: only the server's relay, only TURN, only the WebSocket
  bool forceRelay = false;
  bool forceTurn = false;
  bool forceTcp = false;
  // tests: parity for every viewer, one per this many packets (0: as the viewers' losses say); a
  // packet "lost" on its way in (true: dropped)
  int forceFec = 0;
  bool noAcks = false; // tests: a viewer that does not confirm its frames (as an older one)
  std::function<bool(uint8_t type, const uint8_t* body, size_t n)> dropIncoming;
  /** Viewer: packets rebuilt from parity (without waiting for them again). */
  uint64_t fecRebuilt() const { return fecRebuilt_; }

  struct Peer;
  struct Rinfo {
    std::string address;
    uint16_t port = 0;
    bool tcp = false;
    bool turn = false;
  };
  void onMessage(const uint8_t* msg, size_t n, const Rinfo& from);
  void transmitRaw(const std::string& ip, uint16_t port, const uint8_t* data, size_t n) { socket_.sendTo(ip, port, data, n); }
  void log(const std::string& line) { if (ev_.log) ev_.log(line); }
  Loop& loop() { return loop_; }

 private:
  friend class TurnClient;
  void onReadable();
  void tick();
  void transmit(const Bytes& packet, const std::string& address, uint16_t port);
  void sendTo(Peer& p, const std::string& address, uint16_t port, uint8_t type, const uint8_t* payload, size_t n);
  void send(Peer& p, uint8_t type, const uint8_t* payload, size_t n);
  void resolveServer(uint16_t port, std::function<void(bool)> done);
  void registerPeer(const std::string& id, std::function<void()> done);
  void ensureTurn(const TurnInfo& info, std::function<void()> done);
  void openTcp(Peer& p);
  void setState(Peer& p, const std::string& state, const Json& diag = Json());
  void startProbing(Peer& p, double ms);
  void probe(Peer& p, double t);
  void onProbeAck(Peer& p, const uint8_t* body, size_t n, const Rinfo& from);
  void onPong(Peer& p, double t1, double t2, double t3);
  void onMedia(Peer& p, const uint8_t* body, size_t n, double t);
  void onFec(Peer& p, const uint8_t* body, size_t n, double t);
  bool rebuild(Peer& p, uint32_t frameId, uint16_t first);
  int fecGroup(const Peer& p, double t) const;
  void trackSequence(Peer& p, uint32_t seq, double t);
  double giveUpMs(Peer& p);
  void deliver(Peer& p, double t);
  void requestKeyframe(Peer& p, double t);
  void scheduleNack(Peer& p);
  void sendNacks(Peer& p, double t);
  void sendWhole(Peer& p, const MediaMeta& meta, uint32_t frameId, const uint8_t* data, size_t n);
  void enqueue(Peer& p, std::shared_ptr<const Bytes> payload, bool urgent, uint8_t type);
  void drain();
  void onNack(Peer& p, const uint8_t* body, size_t n);
  void onAck(Peer& p, const uint8_t* body, size_t n, double t);
  void report(Peer& p, double t);
  Peer* bySession(uint32_t s);

  Loop& loop_;
  Role role_;
  std::string serverHost_;
  std::string tcpUrl_;
  WebSocketFactory wsFactory_;
  Events ev_;
  UdpSocket socket_;
  std::optional<Candidate> server_; // the server's UDP address
  std::unique_ptr<TurnClient> turn_;
  int turnState_ = 0; // 0 none, 1 allocating, 2 ready
  std::vector<std::function<void()>> turnWaiters_;
  std::map<std::string, std::unique_ptr<Peer>> peers_;
  uint32_t videoSeq_ = 0;
  uint64_t fecRebuilt_ = 0;
  uint32_t videoFrame_ = 0;
  uint32_t audioSeq_ = 0;
  std::vector<std::shared_ptr<const Bytes>> ring_;
  std::vector<uint32_t> ringSeq_;
  double paceRate_ = (200e6 / 8) / 1000; // bytes per ms until the page tells the bitrate
  double bitrate_ = 0;
  bool draining_ = false;
  int tickTimer_ = 0;
  bool closed_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // for callbacks from other threads
};

/* exposed for tests */
namespace direct_detail {
struct Keys {
  std::array<uint8_t, 16> toViewer{};
  std::array<uint8_t, 16> toHost{};
};
bool deriveKeys(const X25519& mine, const Bytes& peerPub, uint32_t session, Keys& out);
Bytes seal(AesGcm& key, uint8_t dir, uint8_t type, uint32_t session, uint64_t counter, const uint8_t* payload, size_t n);
bool unseal(AesGcm& key, uint8_t dir, const uint8_t* packet, size_t n, Bytes& body);
Bytes registration(int role, uint32_t session, const Bytes& secret);
bool isPrivate(const std::string& address);
bool validCandidate(const Json& c);
}  // namespace direct_detail

}  // namespace ss
