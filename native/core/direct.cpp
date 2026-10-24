#include "direct.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "clock.h"

namespace ss {

namespace {

const uint8_t PEER = 0x53; // 'S': packets between the two ends (the relay forwards them as they are)
const uint8_t REG = 0x52;  // 'R': registration at the server
const uint8_t REG_ACK = 0x41;
enum T : uint8_t { PROBE = 1, PROBE_ACK = 2, MEDIA = 3, NACK = 4, PLI = 5, PING = 6, PONG = 7, REPORT = 8, BYE = 9 };
const uint8_t WHOLE = 4; // media flag: the whole frame in this one packet (the WebSocket path)
const size_t RING = 1 << 14; // video packets kept for retransmission
const size_t MAX_CANDS = 8;
const char* INFO = "screenshare-direct-v1";
const double NaN = std::nan("");

Bytes hmac16(const Bytes& secret, const uint8_t* data, size_t n) {
  uint8_t mac[32];
  hmacSha256(secret.data(), secret.size(), data, n, mac);
  return Bytes(mac, mac + 16);
}

bool parseIPv4(const std::string& s, uint8_t out[4]) {
  int a, b, c, d;
  char tail;
  if (std::sscanf(s.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
  for (int v : {a, b, c, d}) if (v < 0 || v > 255) return false;
  out[0] = uint8_t(a); out[1] = uint8_t(b); out[2] = uint8_t(c); out[3] = uint8_t(d);
  return true;
}

std::string ipString(const uint8_t* p) {
  return std::to_string(p[0]) + "." + std::to_string(p[1]) + "." + std::to_string(p[2]) + "." + std::to_string(p[3]);
}

int rankOf(const std::string& kind) {
  if (kind == "lan") return 0;
  if (kind == "internet") return 1;
  if (kind == "relay") return 2;
  if (kind == "turn") return 3;
  return 4; // tcp
}

}  // namespace

/* ------------------------------------------------ crypto & packets */

namespace direct_detail {

bool deriveKeys(const X25519& mine, const Bytes& peerPub, uint32_t session, Keys& out) {
  if (peerPub.size() != 32) return false;
  uint8_t shared[32];
  if (!mine.shared(peerPub.data(), shared)) return false;
  uint8_t salt[4];
  putU32(salt, session);
  Bytes okm = hkdfSha256(salt, 4, shared, 32, reinterpret_cast<const uint8_t*>(INFO), std::strlen(INFO), 32);
  std::memcpy(out.toViewer.data(), okm.data(), 16);
  std::memcpy(out.toHost.data(), okm.data() + 16, 16);
  return true;
}

static void nonceFor(uint8_t dir, const uint8_t* packet, uint8_t iv[12]) {
  std::memset(iv, 0, 12);
  iv[0] = dir;
  std::memcpy(iv + 4, packet + 6, 8);
}

Bytes seal(AesGcm& key, uint8_t dir, uint8_t type, uint32_t session, uint64_t counter, const uint8_t* payload, size_t n) {
  Bytes out(kHeader + n + kTag);
  out[0] = PEER;
  out[1] = type;
  putU32(&out[2], session);
  putU64(&out[6], counter);
  if (n) std::memcpy(&out[kHeader], payload, n);
  uint8_t iv[12];
  nonceFor(dir, out.data(), iv);
  key.seal(iv, out.data(), kHeader, &out[kHeader], n, &out[kHeader + n]);
  return out;
}

bool unseal(AesGcm& key, uint8_t dir, const uint8_t* packet, size_t n, Bytes& body) {
  if (n < kHeader + kTag) return false;
  uint8_t iv[12];
  nonceFor(dir, packet, iv);
  body.assign(packet + kHeader, packet + n - kTag);
  return key.open(iv, packet, kHeader, body.data(), body.size(), packet + n - kTag);
}

Bytes registration(int role, uint32_t session, const Bytes& secret) {
  Bytes b(64, 0);
  b[0] = REG;
  b[1] = uint8_t(role);
  putU32(&b[2], session);
  randomBytes(&b[6], 8);
  Bytes mac = hmac16(secret, b.data(), 14);
  std::memcpy(&b[14], mac.data(), 16);
  return b;
}

bool isPrivate(const std::string& address) {
  uint8_t ip[4];
  if (!parseIPv4(address, ip)) return false;
  int a = ip[0], b = ip[1];
  return a == 10 || a == 127 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b < 128);
}

bool validCandidate(const Json& c) {
  if (!c.isObject() || !c["address"].isString()) return false;
  uint8_t ip[4];
  if (!parseIPv4(c["address"].str(), ip) || ip[0] == 0 || ip[0] >= 224) return false;
  double port = c["port"].num(-1);
  return port == std::floor(port) && port > 0 && port < 65536;
}

}  // namespace direct_detail

using namespace direct_detail;

Json Offer::toJson() const {
  Json c = Json::array();
  for (const Candidate& k : cands) {
    Json o = Json::object();
    o.set("address", k.address).set("port", int(k.port));
    if (k.turn) o.set("turn", true);
    c.push(o);
  }
  Json j = Json::object();
  j.set("pub", pub).set("cands", c);
  if (tcp) j.set("tcp", true);
  return j;
}

Offer Offer::fromJson(const Json& j) {
  Offer o;
  o.pub = j["pub"].str("");
  o.tcp = j["tcp"].b(false);
  for (const Json& c : j["cands"].items()) {
    if (o.cands.size() >= MAX_CANDS) break;
    if (!validCandidate(c)) continue;
    o.cands.push_back({c["address"].str(), uint16_t(c["port"].num()), c["turn"].b(false)});
  }
  return o;
}

/* ------------------------------------------------ TURN (RFC 8656), the fallback relay */

namespace {

const uint32_t STUN_COOKIE = 0x2112A442;
enum Attr : uint16_t {
  USERNAME = 0x0006, MESSAGE_INTEGRITY = 0x0008, ERROR_CODE = 0x0009, CHANNEL_NUMBER = 0x000C, LIFETIME = 0x000D,
  XOR_PEER_ADDRESS = 0x0012, DATA = 0x0013, REALM = 0x0014, NONCE = 0x0015, XOR_RELAYED_ADDRESS = 0x0016,
  REQUESTED_TRANSPORT = 0x0019, XOR_MAPPED_ADDRESS = 0x0020,
};
enum Method : uint16_t { ALLOCATE = 0x0003, REFRESH = 0x0004, CHANNEL_BIND = 0x0009 };
const uint16_t DATA_INDICATION = 0x0017;

struct StunMsg {
  uint16_t type = 0;
  std::string tid;
  std::map<uint16_t, Bytes> attrs;
  const Bytes* attr(uint16_t t) const {
    auto it = attrs.find(t);
    return it == attrs.end() ? nullptr : &it->second;
  }
};

using Attrs = std::vector<std::pair<uint16_t, Bytes>>;

Bytes u32b(uint32_t v) {
  Bytes b(4);
  putU32(b.data(), v);
  return b;
}

Bytes stunEncode(uint16_t type, const uint8_t tid[12], const Attrs& attrs, const Bytes* key) {
  Bytes body;
  for (const auto& a : attrs) {
    uint8_t h[4];
    putU16(h, a.first);
    putU16(h + 2, uint16_t(a.second.size()));
    body.insert(body.end(), h, h + 4);
    body.insert(body.end(), a.second.begin(), a.second.end());
    body.resize(body.size() + (4 - a.second.size() % 4) % 4, 0);
  }
  Bytes header(20, 0);
  putU16(&header[0], type);
  putU32(&header[4], STUN_COOKIE);
  std::memcpy(&header[8], tid, 12);
  if (key) {
    // MESSAGE-INTEGRITY: HMAC-SHA1 over everything before it, the length already counting it
    putU16(&header[2], uint16_t(body.size() + 24));
    Bytes all = header;
    all.insert(all.end(), body.begin(), body.end());
    uint8_t mac[20];
    hmacSha1(key->data(), key->size(), all.data(), all.size(), mac);
    uint8_t h[4];
    putU16(h, MESSAGE_INTEGRITY);
    putU16(h + 2, 20);
    body.insert(body.end(), h, h + 4);
    body.insert(body.end(), mac, mac + 20);
  }
  putU16(&header[2], uint16_t(body.size()));
  header.insert(header.end(), body.begin(), body.end());
  return header;
}

bool stunParse(const uint8_t* m, size_t n, StunMsg& out) {
  if (n < 20 || (m[0] & 0xC0) != 0 || getU32(m + 4) != STUN_COOKIE) return false;
  size_t end = 20 + getU16(m + 2);
  if (end > n) return false;
  out.type = getU16(m);
  out.tid = toHex(m + 8, 12);
  for (size_t o = 20; o + 4 <= end;) {
    uint16_t t = getU16(m + o);
    uint16_t l = getU16(m + o + 2);
    if (o + 4 + l > end) break;
    if (!out.attrs.count(t)) out.attrs[t] = Bytes(m + o + 4, m + o + 4 + l);
    o += 4 + l + (4 - l % 4) % 4;
  }
  return true;
}

std::optional<Candidate> xorDecode(const Bytes* v) {
  if (!v || v->size() < 8 || (*v)[1] != 1) return std::nullopt;
  uint8_t ip[4];
  for (int i = 0; i < 4; i++) ip[i] = uint8_t((*v)[4 + i] ^ ((STUN_COOKIE >> (24 - i * 8)) & 0xFF));
  return Candidate{ipString(ip), uint16_t(getU16(v->data() + 2) ^ (STUN_COOKIE >> 16)), false};
}

Bytes xorEncode(const Candidate& c) {
  Bytes v(8, 0);
  v[1] = 1;
  putU16(&v[2], uint16_t(c.port ^ (STUN_COOKIE >> 16)));
  uint8_t ip[4] = {0, 0, 0, 0};
  parseIPv4(c.address, ip);
  for (int i = 0; i < 4; i++) v[4 + i] = uint8_t(ip[i] ^ ((STUN_COOKIE >> (24 - i * 8)) & 0xFF));
  return v;
}

int stunError(const StunMsg& m) {
  const Bytes* v = m.attr(ERROR_CODE);
  return v && v->size() >= 4 ? ((*v)[2] & 7) * 100 + (*v)[3] : 0;
}

}  // namespace

/** One allocation on the TURN server; the other end is reached through channels (4-byte header). */
class TurnClient {
 public:
  struct Channel {
    uint16_t number;
    Candidate peer;
    bool ready = false;
  };

  TurnClient(DirectEndpoint& ep, std::string address, uint16_t port, std::string username, std::string password)
      : server{std::move(address), port, false}, ep_(ep), username_(std::move(username)), password_(std::move(password)) {}

  ~TurnClient() { close(); }

  Candidate server;
  std::optional<Candidate> relayed;
  std::optional<Candidate> mapped;

  void allocate(std::function<void(bool)> done) {
    Attrs attrs = {{REQUESTED_TRANSPORT, Bytes{17, 0, 0, 0}}, {LIFETIME, u32b(600)}};
    request(ALLOCATE, attrs, 0, [this, done](const StunMsg* res) {
      if (res) {
        relayed = xorDecode(res->attr(XOR_RELAYED_ADDRESS));
        mapped = xorDecode(res->attr(XOR_MAPPED_ADDRESS));
      }
      if (!relayed) return done(false);
      // allocations live 10 minutes, channels 10, permissions 5: refresh well before
      refreshTimer_ = ep_.loop().setTimer(120000, [this] {
        request(REFRESH, {{LIFETIME, u32b(600)}}, 0, [](const StunMsg*) {});
        for (auto& kv : channels_) if (kv.second.ready) channelBind(kv.second, [](bool) {});
      }, true);
      done(true);
    });
  }

  /** A channel to the other end's relayed address (also opens the permission for it). */
  void bind(const Candidate& peer) {
    std::string key = peer.address + ":" + std::to_string(peer.port);
    auto it = channels_.find(key);
    if (it == channels_.end()) {
      if (nextChannel_ > 0x7FFE) return;
      Channel ch{nextChannel_++, peer, false};
      it = channels_.emplace(key, ch).first;
      byNumber_[ch.number] = key;
    }
    if (it->second.ready) return;
    channelBind(it->second, [this, key](bool ok) {
      auto c = channels_.find(key);
      if (ok && c != channels_.end()) c->second.ready = true;
      if (!ok) ep_.log("direct: TURN channel failed");
    });
  }

  bool bound(const std::string& address, uint16_t port) const {
    auto it = channels_.find(address + ":" + std::to_string(port));
    return it != channels_.end() && it->second.ready;
  }

  /** The packet in a ChannelData message, or empty when there is no channel to that address. */
  Bytes wrap(const std::string& address, uint16_t port, const Bytes& packet) const {
    auto it = channels_.find(address + ":" + std::to_string(port));
    if (it == channels_.end() || !it->second.ready) return {};
    Bytes out(4 + packet.size());
    putU16(&out[0], it->second.number);
    putU16(&out[2], uint16_t(packet.size()));
    std::memcpy(&out[4], packet.data(), packet.size());
    return out;
  }

  /** A packet from the TURN server: true and the other end's packet, or false (handled here). */
  bool onMessage(const uint8_t* m, size_t n, Candidate& peer, Bytes& data) {
    if (n >= 4 && m[0] >= 0x40 && m[0] <= 0x7F) {
      auto key = byNumber_.find(getU16(m));
      size_t len = getU16(m + 2);
      if (key == byNumber_.end() || 4 + len > n) return false;
      peer = channels_.at(key->second).peer;
      data.assign(m + 4, m + 4 + len);
      return true;
    }
    StunMsg msg;
    if (!stunParse(m, n, msg)) return false;
    if (msg.type == DATA_INDICATION) {
      auto p = xorDecode(msg.attr(XOR_PEER_ADDRESS));
      const Bytes* d = msg.attr(DATA);
      if (!p || !d) return false;
      peer = *p;
      data = *d;
      return true;
    }
    auto it = pending_.find(msg.tid);
    if (it != pending_.end()) {
      auto cb = it->second.done;
      ep_.loop().cancel(it->second.timer);
      pending_.erase(it);
      cb(&msg);
    }
    return false;
  }

  void close() {
    if (closed_) return;
    ep_.loop().cancel(refreshTimer_);
    if (relayed && !key_.empty()) {
      // give the allocation back (sent once, no waiting)
      uint8_t tid[12];
      randomBytes(tid, 12);
      Attrs attrs = {{LIFETIME, u32b(0)}};
      addAuth(attrs);
      Bytes m = stunEncode(REFRESH, tid, attrs, &key_);
      ep_.transmitRaw(server.address, server.port, m.data(), m.size());
    }
    closed_ = true;
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& kv : pending) {
      ep_.loop().cancel(kv.second.timer);
      kv.second.done(nullptr);
    }
  }

 private:
  struct Pending {
    Bytes msg;
    int tries = 0;
    int timer = 0;
    std::function<void(const StunMsg*)> done;
  };

  void addAuth(Attrs& attrs) {
    if (realm_.empty()) return;
    attrs.push_back({USERNAME, Bytes(username_.begin(), username_.end())});
    attrs.push_back({REALM, Bytes(realm_.begin(), realm_.end())});
    attrs.push_back({NONCE, Bytes(nonce_.begin(), nonce_.end())});
  }

  void transaction(const uint8_t tid[12], Bytes msg, std::function<void(const StunMsg*)> done) {
    std::string key = toHex(tid, 12);
    pending_[key] = Pending{std::move(msg), 0, 0, std::move(done)};
    resend(key);
  }

  void resend(const std::string& key) {
    auto it = pending_.find(key);
    if (it == pending_.end()) return;
    Pending& p = it->second;
    if (closed_ || p.tries++ >= 4) {
      auto cb = p.done;
      pending_.erase(it);
      cb(nullptr);
      return;
    }
    ep_.transmitRaw(server.address, server.port, p.msg.data(), p.msg.size());
    p.timer = ep_.loop().setTimer(250.0 * p.tries, [this, key] { resend(key); });
  }

  /** A request, authenticated with the long-term credential once the server asks for it. */
  void request(uint16_t method, Attrs attrs, int attempt, std::function<void(const StunMsg*)> done) {
    if (attempt >= 4 || closed_) return done(nullptr);
    Attrs all = attrs;
    addAuth(all);
    uint8_t tid[12];
    randomBytes(tid, 12);
    Bytes msg = stunEncode(method, tid, all, realm_.empty() ? nullptr : &key_);
    transaction(tid, std::move(msg), [this, method, attrs, attempt, done](const StunMsg* answer) {
      if (!answer) return done(nullptr);
      if ((answer->type & 0x0110) == 0x0100) return done(answer);
      int code = stunError(*answer);
      if (code == 401 || code == 438) {
        // (new) realm and nonce: authenticate with the long-term credential
        if (const Bytes* r = answer->attr(REALM)) realm_.assign(r->begin(), r->end());
        if (const Bytes* nn = answer->attr(NONCE)) nonce_.assign(nn->begin(), nn->end());
        std::string k = username_ + ":" + realm_ + ":" + password_;
        key_.resize(16);
        md5(reinterpret_cast<const uint8_t*>(k.data()), k.size(), key_.data());
        return request(method, attrs, attempt + 1, done);
      }
      ep_.log("direct: TURN error " + std::to_string(code));
      done(nullptr);
    });
  }

  void channelBind(Channel& ch, std::function<void(bool)> done) {
    Bytes n(4, 0);
    putU16(n.data(), ch.number);
    request(CHANNEL_BIND, {{CHANNEL_NUMBER, n}, {XOR_PEER_ADDRESS, xorEncode(ch.peer)}}, 0, [done](const StunMsg* r) { done(r != nullptr); });
  }

  DirectEndpoint& ep_;
  std::string username_, password_, realm_, nonce_;
  Bytes key_;
  std::map<std::string, Pending> pending_;
  std::map<std::string, Channel> channels_;
  std::map<uint16_t, std::string> byNumber_;
  uint16_t nextChannel_ = 0x4000;
  int refreshTimer_ = 0;
  bool closed_ = false;
};

/* ------------------------------------------------ one connection */

struct DirectEndpoint::Peer {
  struct Path {
    std::string address;
    uint16_t port = 0;
    std::string kind;
    double rtt = 0;
  };
  struct Missing {
    double first, last;
    int tries;
  };
  struct Frame {
    uint32_t id;
    uint16_t count;
    std::vector<Bytes> parts;
    std::vector<bool> have;
    uint16_t got = 0;
    size_t bytes = 0;
    bool key;
    double ts, capture, first;
  };

  std::string id;
  uint32_t session;
  Bytes secret;
  X25519 kp = X25519::generate();
  std::unique_ptr<AesGcm> sendKey, recvKey;
  uint64_t counter = 0;
  std::string state = "new";
  std::optional<Path> path;
  std::vector<Candidate> cands;
  std::map<std::string, std::pair<double, bool>> probes; // id → sent, via relay
  double probeStart = 0, probeUntil = 0, nextProbe = 0;
  double lastRecv = 0, lastSent = 0;
  std::optional<Candidate> publicAddr;
  int regTimer = 0, regTries = 0, failTimer = 0;
  std::function<void()> onRegistered;
  bool tcpOnly = false;
  std::unique_ptr<WebSocket> tcp;
  bool tcpReady = false;
  int tcpGen = 0;
  double tcpAt = -1e9;
  // sending (pacing)
  std::deque<std::shared_ptr<const Bytes>> out;
  size_t outBytes = 0;
  double tokens = 0, lastFill = nowMs();
  uint64_t sentBytes = 0;
  bool dropping = false;
  uint32_t wholeSeq = 0;
  // receiving video (viewer)
  std::optional<uint32_t> expect;
  std::map<uint32_t, Missing> missing;
  std::map<uint32_t, Frame> frames;
  std::optional<uint32_t> next;
  uint32_t minNext = 0;
  double waitSince = 0, lastPli = -1e9;
  bool nackPending = false;
  struct Stats {
    uint64_t packets = 0, lost = 0, recovered = 0, nacks = 0, skips = 0, bytes = 0, frames = 0;
    double at = 0;
  } stats, prevStats;
  bool havePrev = false;
  struct Clock {
    double rtt = NaN, offset = NaN;
    std::deque<std::pair<double, double>> samples; // rtt, offset
  } clock;
  double lastPing = 0, lastReport = 0;

  uint8_t sendDir(DirectEndpoint::Role r) const { return r == DirectEndpoint::Role::Host ? 1 : 2; }
  uint8_t recvDir(DirectEndpoint::Role r) const { return r == DirectEndpoint::Role::Host ? 2 : 1; }
};

/* ------------------------------------------------ the endpoint */

DirectEndpoint::DirectEndpoint(Loop& loop, Role role, std::string serverHost, std::string tcpUrl, WebSocketFactory ws, Events events)
    : loop_(loop), role_(role), serverHost_(std::move(serverHost)), tcpUrl_(std::move(tcpUrl)), wsFactory_(std::move(ws)), ev_(std::move(events)),
      ring_(RING), ringSeq_(RING, 0) {}

DirectEndpoint::~DirectEndpoint() { close(); }

bool DirectEndpoint::open() {
  if (!socket_.open()) return false;
  loop_.watch(socket_.handle(), [this] { onReadable(); });
  tickTimer_ = loop_.setTimer(5, [this] { tick(); }, true);
  return true;
}

void DirectEndpoint::close() {
  if (closed_) return;
  closed_ = true;
  *alive_ = false;
  std::vector<std::string> ids;
  for (auto& kv : peers_) ids.push_back(kv.first);
  for (auto& id : ids) removePeer(id);
  loop_.cancel(tickTimer_);
  if (turn_) turn_->close();
  turn_.reset();
  if (socket_.valid()) loop_.unwatch(socket_.handle());
  socket_.close();
}

DirectEndpoint::Peer* DirectEndpoint::bySession(uint32_t s) {
  for (auto& kv : peers_) if (kv.second->session == s) return kv.second.get();
  return nullptr;
}

void DirectEndpoint::onReadable() {
  static thread_local std::vector<uint8_t> buf(65536);
  for (int i = 0; i < 4096; i++) {
    Rinfo from;
    int n = socket_.recvFrom(buf.data(), buf.size(), from.address, from.port);
    if (n < 0) break;
    onMessage(buf.data(), size_t(n), from);
    if (closed_) return;
  }
}

void DirectEndpoint::resolveServer(uint16_t port, std::function<void(bool)> done) {
  if (server_ && server_->port == port) return done(true);
  std::string host = serverHost_;
  std::weak_ptr<bool> alive = alive_;
  std::thread([this, host, port, done, alive] {
    std::string ip = resolveIPv4(host);
    loop_.post([this, ip, port, done, alive] {
      if (alive.expired() || !*alive.lock()) return;
      if (!ip.empty()) server_ = Candidate{ip, port, false};
      done(!ip.empty());
    });
  }).detach();
}

/** Registers at the server (public address, relay); repeated so the NAT mapping stays open. */
void DirectEndpoint::registerPeer(const std::string& id, std::function<void()> done) {
  auto it = peers_.find(id);
  if (it == peers_.end() || !server_) return done();
  Peer* p = it->second.get();
  auto sendReg = [this, id] {
    auto pi = peers_.find(id);
    if (pi == peers_.end() || !server_) return;
    Bytes r = registration(role_ == Role::Host ? 1 : 2, pi->second->session, pi->second->secret);
    socket_.sendTo(server_->address, server_->port, r.data(), r.size());
  };
  auto finished = std::make_shared<bool>(false);
  auto finish = [done, finished] {
    if (*finished) return;
    *finished = true;
    done();
  };
  p->onRegistered = [this, id, sendReg, finish] {
    auto pi = peers_.find(id);
    if (pi == peers_.end()) return;
    loop_.cancel(pi->second->regTimer);
    pi->second->regTimer = loop_.setTimer(15000, sendReg, true);
    finish();
  };
  p->regTries = 0;
  p->regTimer = loop_.setTimer(250, [this, id, sendReg, finish] {
    auto pi = peers_.find(id);
    if (pi == peers_.end()) return finish();
    if (pi->second->publicAddr) return;
    if (++pi->second->regTries > 10) {
      loop_.cancel(pi->second->regTimer);
      return finish();
    }
    sendReg();
  }, true);
  sendReg();
}

/** One TURN allocation per endpoint, shared by all connections. */
void DirectEndpoint::ensureTurn(const TurnInfo& info, std::function<void()> done) {
  if (turnState_ == 2) return done();
  turnWaiters_.push_back(std::move(done));
  if (turnState_ == 1) return;
  turnState_ = 1;
  std::weak_ptr<bool> alive = alive_;
  std::thread([this, info, alive] {
    std::string ip = resolveIPv4(info.host);
    loop_.post([this, ip, info, alive] {
      if (alive.expired() || !*alive.lock()) return;
      auto finish = [this, alive](bool ok) {
        if (!ok) {
          log("direct: no TURN allocation");
          if (turn_) turn_->close();
          // dropped after this call returns (it comes from inside the TURN client)
          std::shared_ptr<TurnClient> gone(turn_.release());
          loop_.post([gone] {});
        }
        turnState_ = ok ? 2 : 0; // a failure: the next connection tries again
        auto waiters = std::move(turnWaiters_);
        turnWaiters_.clear();
        for (auto& w : waiters) w();
      };
      if (ip.empty()) return finish(false);
      turn_.reset(new TurnClient(*this, ip, info.port, info.username, info.credential));
      turn_->allocate(finish);
    });
  }).detach();
}

void DirectEndpoint::addPeer(const std::string& id, const PeerInfo& info, std::function<void(bool, Offer)> done) {
  removePeer(id);
  auto p = std::make_unique<Peer>();
  p->id = id;
  p->session = info.session;
  p->secret = base64Decode(info.secret);
  Peer* peer = p.get();
  peers_[id] = std::move(p);
  // the server's UDP port and the TURN server, side by side
  auto left = std::make_shared<int>(2);
  auto step = [this, id, peer, left, done] {
    if (--*left > 0) return;
    auto it = peers_.find(id);
    if (it == peers_.end() || it->second.get() != peer) return done(false, {});
    bool forced = forceRelay || forceTurn;
    Offer o;
    o.pub = base64Encode(peer->kp.pub.data(), 32);
    if (!forced) for (const std::string& ip : localIPv4()) o.cands.push_back({ip, socket_.port(), false});
    // the public address: from the server's UDP port, or from the TURN server
    std::optional<Candidate> pub = peer->publicAddr;
    if (!pub && turn_ && turn_->mapped) pub = turn_->mapped;
    if (pub && !forced) o.cands.push_back({pub->address, pub->port, false});
    if (turn_ && turn_->relayed) o.cands.push_back({turn_->relayed->address, turn_->relayed->port, true});
    if (o.cands.size() > MAX_CANDS) o.cands.resize(MAX_CANDS);
    done(true, o);
  };
  if (info.port && !forceTurn) {
    resolveServer(info.port, [this, id, step](bool ok) {
      if (!ok) {
        log("direct: server not reachable over UDP");
        return step();
      }
      registerPeer(id, step);
    });
  } else {
    step();
  }
  if (info.turn && !forceRelay) ensureTurn(*info.turn, step);
  else step();
}

void DirectEndpoint::connectPeer(const std::string& id, const Offer& other) {
  auto it = peers_.find(id);
  if (it == peers_.end()) return;
  Peer& p = *it->second;
  Keys keys;
  if (!deriveKeys(p.kp, base64Decode(other.pub), p.session, keys)) {
    setState(p, "failed");
    return;
  }
  bool host = role_ == Role::Host;
  p.sendKey.reset(new AesGcm(host ? keys.toViewer.data() : keys.toHost.data()));
  p.recvKey.reset(new AesGcm(host ? keys.toHost.data() : keys.toViewer.data()));
  p.tcpOnly = other.tcp; // a browser: only the WebSocket reaches it
  p.cands = other.cands;
  // the other end's TURN address is reached through our own allocation
  for (const Candidate& c : p.cands) if (c.turn && turn_) turn_->bind(c);
  p.state = "connecting";
  startProbing(p, 10000);
  p.failTimer = loop_.setTimer(8000, [this, id] {
    auto pi = peers_.find(id);
    if (pi == peers_.end() || pi->second->state != "connecting") return;
    Peer& q = *pi->second;
    // what could be reached, for the page to explain
    Json diag = Json::object();
    diag.set("server", bool(q.publicAddr)).set("turn", bool(turn_ && turn_->relayed)).set("tcp", q.tcpReady).set("remote", int(q.cands.size()));
    setState(q, "failed", diag);
  });
}

/** The WebSocket path: the server passes the packets to the other end. */
void DirectEndpoint::openTcp(Peer& p) {
  double t = nowMs();
  if (tcpUrl_.empty() || p.tcp || !wsFactory_ || t - p.tcpAt < 3000) return;
  p.tcpAt = t;
  int gen = ++p.tcpGen;
  std::string id = p.id;
  std::weak_ptr<bool> alive = alive_;
  // the events come from the socket's threads: onto the loop, and only for this very socket
  auto onLoop = [this, alive, id, gen](std::function<void(Peer&)> fn) {
    loop_.post([this, alive, id, gen, fn] {
      if (alive.expired() || !*alive.lock()) return;
      auto it = peers_.find(id);
      if (it == peers_.end() || it->second->tcpGen != gen || !it->second->tcp) return;
      fn(*it->second);
    });
  };
  WebSocketEvents ev;
  int role = role_ == Role::Host ? 1 : 2;
  ev.open = [onLoop, role] {
    onLoop([role](Peer& q) {
      uint8_t n[8];
      randomBytes(n, 8);
      std::string nonce = toHex(n, 8);
      std::string text = "tcp:" + std::to_string(q.session) + ":" + std::to_string(role) + ":" + nonce;
      Bytes mac = hmac16(q.secret, reinterpret_cast<const uint8_t*>(text.data()), text.size());
      Json auth = Json::object();
      auth.set("session", double(q.session)).set("role", role).set("nonce", nonce).set("mac", base64Encode(mac.data(), mac.size()));
      q.tcp->sendText(auth.dump());
    });
  };
  ev.text = [onLoop](std::string text) {
    if (text == "ok") onLoop([](Peer& q) { q.tcpReady = true; });
  };
  ev.binary = [this, onLoop](const uint8_t* d, size_t n) {
    auto data = std::make_shared<Bytes>(d, d + n);
    onLoop([this, data](Peer& q) {
      Rinfo from{"tcp:" + std::to_string(q.session), 0, true, false};
      onMessage(data->data(), data->size(), from);
    });
  };
  ev.closed = [onLoop] {
    onLoop([](Peer& q) {
      q.tcp.reset();
      q.tcpReady = false;
    });
  };
  p.tcpReady = false;
  p.tcp = wsFactory_(tcpUrl_, ev);
}

void DirectEndpoint::removePeer(const std::string& id) {
  auto it = peers_.find(id);
  if (it == peers_.end()) return;
  Peer& p = *it->second;
  if (p.path) {
    uint8_t b = 0;
    send(p, BYE, &b, 1);
  }
  if (p.tcp) p.tcp->close();
  loop_.cancel(p.regTimer);
  loop_.cancel(p.failTimer);
  peers_.erase(it);
}

void DirectEndpoint::setState(Peer& p, const std::string& state, const Json& diag) {
  p.state = state;
  if (!ev_.state) return;
  Json info = Json::object();
  info.set("state", state);
  info.set("path", p.path ? Json(p.path->kind) : Json());
  info.set("rtt", p.path ? Json(p.path->rtt) : Json());
  if (!diag.isNull()) info.set("diag", diag);
  ev_.state(p.id, info);
}

/* ---- sending packets */

void DirectEndpoint::transmit(const Bytes& packet, const std::string& address, uint16_t port) {
  if (address.compare(0, 4, "tcp:") == 0) {
    Peer* p = bySession(uint32_t(std::stoul(address.substr(4))));
    if (p && p->tcp && p->tcpReady && p->tcp->bufferedAmount() < (4u << 20)) p->tcp->sendBinary(packet.data(), packet.size());
    return;
  }
  if (!socket_.valid()) return;
  if (turn_) {
    Bytes wrapped = turn_->wrap(address, port, packet);
    if (!wrapped.empty()) {
      socket_.sendTo(turn_->server.address, turn_->server.port, wrapped.data(), wrapped.size());
      return;
    }
  }
  socket_.sendTo(address, port, packet.data(), packet.size());
}

void DirectEndpoint::sendTo(Peer& p, const std::string& address, uint16_t port, uint8_t type, const uint8_t* payload, size_t n) {
  if (!p.sendKey) return;
  Bytes packet = seal(*p.sendKey, p.sendDir(role_), type, p.session, ++p.counter, payload, n);
  transmit(packet, address, port);
}

void DirectEndpoint::send(Peer& p, uint8_t type, const uint8_t* payload, size_t n) {
  if (!p.path) return;
  p.lastSent = nowMs();
  sendTo(p, p.path->address, p.path->port, type, payload, n);
}

/* ---- finding a path */

void DirectEndpoint::startProbing(Peer& p, double ms) {
  p.probeStart = nowMs();
  p.probeUntil = p.probeStart + ms;
  p.nextProbe = 0;
}

void DirectEndpoint::probe(Peer& p, double t) {
  if (t < p.nextProbe || t > p.probeUntil || !p.sendKey) return;
  p.nextProbe = t + 100;
  double since = t - p.probeStart;
  bool tcpNow = forceTcp || p.tcpOnly;
  bool forced = forceRelay || forceTurn || tcpNow;
  struct Target {
    std::string address;
    uint16_t port;
    bool relay;
  };
  std::vector<Target> targets;
  if (!forced) for (const Candidate& c : p.cands) if (!c.turn) targets.push_back({c.address, c.port, false});
  // the relays only after a moment: a direct path is better
  if (server_ && (!forced || forceRelay) && (forceRelay || since > 1000)) targets.push_back({server_->address, server_->port, true});
  if (turn_ && (!forced || forceTurn) && (forceTurn || since > 1500)) {
    for (const Candidate& c : p.cands) if (c.turn && turn_->bound(c.address, c.port)) targets.push_back({c.address, c.port, false});
  }
  // no UDP path yet after 2 s: the WebSocket too (opened once, probed like the others)
  if (!tcpUrl_.empty() && (!forced || tcpNow) && (tcpNow || (since > 2000 && p.state != "connected"))) openTcp(p);
  if (p.tcp && p.tcpReady && (!forced || tcpNow) && (tcpNow || since > 2000)) targets.push_back({"tcp:" + std::to_string(p.session), 0, false});
  for (const Target& target : targets) {
    uint8_t id[8];
    randomBytes(id, 8);
    p.probes[toHex(id, 8)] = {t, target.relay};
    sendTo(p, target.address, target.port, PROBE, id, 8);
  }
  if (p.probes.size() > 400) p.probes.clear();
}

void DirectEndpoint::onProbeAck(Peer& p, const uint8_t* body, size_t n, const Rinfo& from) {
  if (n < 8) return;
  auto it = p.probes.find(toHex(body, 8));
  if (it == p.probes.end()) return;
  double t = nowMs();
  bool relay = server_ && from.address == server_->address && from.port == server_->port;
  std::string kind = from.tcp ? "tcp" : from.turn ? "turn" : relay ? "relay" : isPrivate(from.address) ? "lan" : "internet";
  Peer::Path candidate{from.address, from.port, kind, t - it->second.first};
  bool better = !p.path || rankOf(kind) < rankOf(p.path->kind) ||
                (rankOf(kind) == rankOf(p.path->kind) && candidate.rtt + 2 < p.path->rtt && p.path->address != candidate.address);
  if (better) {
    p.path = candidate;
    if (p.state != "connected") {
      loop_.cancel(p.failTimer);
      setState(p, "connected");
    } else if (ev_.state) {
      Json info = Json::object();
      info.set("state", "connected").set("path", kind).set("rtt", candidate.rtt);
      ev_.state(p.id, info);
    }
  } else if (p.path && p.path->address == candidate.address && p.path->port == candidate.port) {
    p.path->rtt = candidate.rtt;
  }
}

/* ---- receiving */

void DirectEndpoint::onMessage(const uint8_t* msg, size_t n, const Rinfo& from) {
  if (n < 1 || closed_) return;
  if (turn_ && !from.tcp && !from.turn && from.address == turn_->server.address && from.port == turn_->server.port) {
    // through the TURN server: the other end's packet, as if it came from its relayed address
    Candidate peer;
    Bytes data;
    if (turn_->onMessage(msg, n, peer, data) && !data.empty() && data[0] == PEER) onMessage(data.data(), data.size(), Rinfo{peer.address, peer.port, false, true});
    return;
  }
  if (msg[0] == REG_ACK) {
    if (n < 37 || msg[14] != 4 || !server_ || from.address != server_->address) return;
    Peer* p = bySession(getU32(msg + 2));
    if (!p) return;
    Bytes mac = hmac16(p->secret, msg, 21);
    if (std::memcmp(mac.data(), msg + 21, 16) != 0) return;
    p->publicAddr = Candidate{ipString(msg + 15), getU16(msg + 19), false};
    if (p->onRegistered) {
      auto cb = std::move(p->onRegistered);
      p->onRegistered = nullptr;
      cb();
    }
    return;
  }
  if (msg[0] != PEER || n < kHeader + kTag) return;
  Peer* pp = bySession(getU32(msg + 2));
  if (!pp || !pp->recvKey) return;
  Peer& p = *pp;
  Bytes body;
  if (!unseal(*p.recvKey, p.recvDir(role_), msg, n, body)) return;
  double t = nowMs();
  p.lastRecv = t;
  p.stats.bytes += n;
  switch (msg[1]) {
    case PROBE:
      if (body.size() >= 8) sendTo(p, from.address, from.port, PROBE_ACK, body.data(), 8);
      break;
    case PROBE_ACK:
      onProbeAck(p, body.data(), body.size(), from);
      break;
    case MEDIA:
      if (role_ == Role::Viewer) onMedia(p, body.data(), body.size(), t);
      break;
    case NACK:
      if (role_ == Role::Host) onNack(p, body.data(), body.size());
      break;
    case PLI:
      if (role_ == Role::Host && ev_.keyframe) ev_.keyframe(p.id);
      break;
    case PING:
      if (body.size() >= 8) {
        uint8_t pong[16];
        std::memcpy(pong, body.data(), 8);
        putF64(pong + 8, t);
        send(p, PONG, pong, 16);
      }
      break;
    case PONG:
      if (body.size() >= 16) onPong(p, getF64(body.data()), getF64(body.data() + 8), t);
      break;
    case REPORT: {
      Json r = Json::parse(std::string(body.begin(), body.end()));
      if (r.isObject() && ev_.report) ev_.report(p.id, r);
      break;
    }
    case BYE:
      p.path.reset();
      setState(p, "closed");
      break;
    default:
      break;
  }
}

void DirectEndpoint::onPong(Peer& p, double t1, double t2, double t3) {
  double rtt = t3 - t1;
  if (!(rtt >= 0 && rtt < 10000)) return;
  auto& c = p.clock;
  c.samples.push_back({rtt, t2 - (t1 + t3) / 2});
  if (c.samples.size() > 20) c.samples.pop_front();
  auto best = c.samples.front();
  for (auto& s : c.samples) if (s.first < best.first) best = s;
  c.rtt = rtt;
  c.offset = best.second; // the least delayed sample says the most about the clocks
  if (p.path) p.path->rtt = rtt;
  if (ev_.clock) ev_.clock(p.id, rtt, c.offset);
}

void DirectEndpoint::onMedia(Peer& p, const uint8_t* body, size_t n, double t) {
  if (n < kMediaHeader) return;
  int stream = body[0];
  uint8_t flags = body[1];
  uint32_t seq = getU32(body + 4);
  uint32_t frameId = getU32(body + 8);
  uint16_t index = getU16(body + 12);
  uint16_t count = getU16(body + 14);
  double capture = getF64(body + 16);
  double ts = getF64(body + 24);
  const uint8_t* data = body + kMediaHeader;
  size_t len = n - kMediaHeader;
  p.stats.packets++;
  if (stream == 1) {
    // audio: one packet per Opus frame, played as it comes (late audio is useless)
    if (count == 1 && ev_.media) {
      MediaMeta m;
      m.stream = 1;
      m.sub = body[2];
      m.ts = ts;
      m.capture = capture;
      m.key = true;
      ev_.media(m, data, len);
    }
    return;
  }
  if (!(flags & WHOLE)) trackSequence(p, seq, t); // a whole frame is never asked for again
  if (p.next && int32_t(frameId - *p.next) < 0) return; // already delivered or skipped
  auto it = p.frames.find(frameId);
  if (it == p.frames.end()) {
    if (count < 1 || p.frames.size() > 600) return;
    Peer::Frame f;
    f.id = frameId;
    f.count = count;
    f.parts.resize(count);
    f.have.assign(count, false);
    f.key = flags & 1;
    f.ts = ts;
    f.capture = capture;
    f.first = t;
    it = p.frames.emplace(frameId, std::move(f)).first;
  }
  Peer::Frame& f = it->second;
  if (count != f.count || index >= count || f.have[index]) return;
  f.parts[index].assign(data, data + len);
  f.have[index] = true;
  f.got++;
  f.bytes += len;
  if (f.got == f.count) deliver(p, t);
}

void DirectEndpoint::trackSequence(Peer& p, uint32_t seq, double t) {
  if (!p.expect) {
    p.expect = seq + 1;
    return;
  }
  int32_t d = int32_t(seq - *p.expect);
  if (d == 0) {
    p.expect = seq + 1;
  } else if (d > 0) {
    if (d < 4000) {
      for (uint32_t s = *p.expect; s != seq; s++) p.missing[s] = {t, 0, 0};
      p.stats.lost += uint64_t(d);
      scheduleNack(p);
    }
    p.expect = seq + 1;
  } else if (p.missing.erase(seq)) {
    p.stats.recovered++;
  }
}

// How long a frame with lost packets waits for the retransmission. Skipping costs more (a keyframe
// has to come), so not too eager – a busy computer can be late by a few ms too.
double DirectEndpoint::giveUpMs(Peer& p) {
  double rtt = !std::isnan(p.clock.rtt) ? p.clock.rtt : p.path ? p.path->rtt : 30;
  return std::min(300.0, std::max(60.0, rtt * 3 + 30));
}

/** Hands complete frames over in order; skips to the next keyframe when one cannot be completed. */
void DirectEndpoint::deliver(Peer& p, double t) {
  for (;;) {
    if (!p.next) {
      const Peer::Frame* key = nullptr;
      for (auto& kv : p.frames) {
        const Peer::Frame& f = kv.second;
        if (f.key && f.got == f.count && int32_t(f.id - p.minNext) >= 0 && (!key || int32_t(f.id - key->id) < 0)) key = &f;
      }
      if (!key) {
        requestKeyframe(p, t);
        // without a keyframe, the frames after it are of no use
        for (auto it = p.frames.begin(); it != p.frames.end();) {
          if (!it->second.key && t - it->second.first > 1000) it = p.frames.erase(it);
          else ++it;
        }
        return;
      }
      uint32_t keyId = key->id;
      p.next = keyId;
      for (auto it = p.frames.begin(); it != p.frames.end();) {
        if (int32_t(it->first - keyId) < 0) it = p.frames.erase(it);
        else ++it;
      }
    }
    auto it = p.frames.find(*p.next);
    if (it != p.frames.end() && it->second.got == it->second.count) {
      Peer::Frame f = std::move(it->second);
      p.frames.erase(it);
      p.next = *p.next + 1;
      p.waitSince = 0;
      p.stats.frames++;
      if (ev_.media) {
        MediaMeta m;
        m.stream = 0;
        m.key = f.key;
        m.ts = f.ts;
        m.capture = f.capture;
        m.id = f.id;
        if (f.count == 1) {
          ev_.media(m, f.parts[0].data(), f.parts[0].size());
        } else {
          Bytes all;
          all.reserve(f.bytes);
          for (auto& part : f.parts) all.insert(all.end(), part.begin(), part.end());
          ev_.media(m, all.data(), all.size());
        }
      }
      if (closed_) return;
      continue;
    }
    // A later frame is here but this one is not complete: packets were lost. Wait for the
    // retransmission, but not long. (Without a later frame it is simply still arriving.)
    bool later = false;
    for (auto& kv : p.frames) if (int32_t(kv.first - *p.next) > 0) { later = true; break; }
    if (!later) return;
    if (!p.waitSince) p.waitSince = t;
    // over the WebSocket nothing comes late: a missing frame was dropped on purpose
    if (!(p.path && p.path->kind == "tcp") && t - p.waitSince < giveUpMs(p)) return;
    p.stats.skips++;
    p.minNext = *p.next + 1;
    p.next.reset();
    p.waitSince = 0;
  }
}

void DirectEndpoint::keyframe(const std::string& id) {
  auto it = peers_.find(id);
  if (it != peers_.end()) requestKeyframe(*it->second, nowMs());
}

void DirectEndpoint::requestKeyframe(Peer& p, double t) {
  if (!p.path || t - p.lastPli < 150) return;
  p.lastPli = t;
  uint8_t b = 0;
  send(p, PLI, &b, 1);
}

void DirectEndpoint::scheduleNack(Peer& p) {
  if (p.nackPending) return;
  p.nackPending = true;
  std::string id = p.id;
  std::weak_ptr<bool> alive = alive_;
  loop_.post([this, id, alive] {
    if (alive.expired() || !*alive.lock()) return;
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    it->second->nackPending = false;
    sendNacks(*it->second, nowMs());
  });
}

void DirectEndpoint::sendNacks(Peer& p, double t) {
  if (p.missing.empty() || !p.path) return;
  double rtt = !std::isnan(p.clock.rtt) ? p.clock.rtt : p.path->rtt;
  double resend = std::max(8.0, rtt * 1.5);
  std::vector<uint32_t> seqs;
  for (auto it = p.missing.begin(); it != p.missing.end();) {
    Peer::Missing& m = it->second;
    if (t - m.first > 1000 || (m.tries >= 4 && t - m.last > resend)) {
      it = p.missing.erase(it);
      continue;
    }
    if (m.tries < 4 && (m.last == 0 || t - m.last > resend)) {
      m.last = t;
      m.tries++;
      seqs.push_back(it->first);
    }
    ++it;
  }
  for (size_t i = 0; i < seqs.size(); i += 280) {
    size_t k = std::min(seqs.size(), i + 280) - i;
    Bytes body(k * 4);
    for (size_t j = 0; j < k; j++) putU32(&body[j * 4], seqs[i + j]);
    send(p, NACK, body.data(), body.size());
    p.stats.nacks += k;
  }
}

/* ---- sending (host) */

void DirectEndpoint::setBitrate(double bps) {
  // a frame leaves at 3× the video bitrate: fast, but not one big burst into the network
  double pace = std::max(3 * bps, 40e6);
  paceRate_ = pace / 8 / 1000;
  bitrate_ = bps;
}

void DirectEndpoint::sendMedia(const MediaMeta& meta, const uint8_t* data, size_t n) {
  if (role_ != Role::Host) return;
  bool video = meta.stream == 0;
  size_t count = std::max<size_t>(1, (n + kMaxData - 1) / kMaxData);
  if (count > 65535) return;
  uint32_t frameId = video ? videoFrame_ : audioSeq_;
  if (video) videoFrame_++;
  std::vector<Peer*> ready;
  for (auto& kv : peers_) {
    Peer& p = *kv.second;
    if (p.state != "connected" || !p.path) continue;
    if (p.path->kind == "tcp") sendWhole(p, meta, frameId, data, n);
    else ready.push_back(&p);
  }
  if (ready.empty() && video) {
    videoSeq_ += uint32_t(count); // the numbering stays the same for UDP viewers
    return;
  }
  for (size_t i = 0; i < count; i++) {
    size_t from = i * kMaxData;
    size_t len = std::min(n, from + kMaxData) - std::min(n, from);
    auto p = std::make_shared<Bytes>(kMediaHeader + len);
    Bytes& b = *p;
    b[0] = video ? 0 : 1;
    b[1] = meta.key ? 1 : 0;
    b[2] = uint8_t(meta.sub);
    b[3] = 0;
    uint32_t seq = video ? videoSeq_++ : audioSeq_++;
    putU32(&b[4], seq);
    putU32(&b[8], frameId);
    putU16(&b[12], uint16_t(i));
    putU16(&b[14], uint16_t(count));
    putF64(&b[16], meta.capture);
    putF64(&b[24], meta.ts);
    if (len) std::memcpy(&b[kMediaHeader], data + from, len);
    if (video) {
      ring_[seq & (RING - 1)] = p;
      ringSeq_[seq & (RING - 1)] = seq;
    }
    for (Peer* peer : ready) enqueue(*peer, p, !video);
  }
  drain();
}

/** The WebSocket path: the frame as one packet, at once – or dropped when the socket is full. */
void DirectEndpoint::sendWhole(Peer& p, const MediaMeta& meta, uint32_t frameId, const uint8_t* data, size_t n) {
  bool video = meta.stream == 0;
  if (!p.tcp || !p.tcpReady) return;
  // what may wait in the socket: ~150 ms of video; more is only delay
  double limit = std::max(256.0 * 1024, bitrate_ / 8 * 0.15);
  double queued = double(p.tcp->bufferedAmount());
  if (video) {
    if (queued > limit || (p.dropping && !meta.key)) {
      if (!p.dropping && ev_.keyframe) ev_.keyframe(p.id); // the next one goes when there is room
      p.dropping = true;
      return;
    }
    p.dropping = false;
  } else if (queued > limit * 4) {
    return;
  }
  Bytes b(kMediaHeader + n);
  b[0] = video ? 0 : 1;
  b[1] = uint8_t((meta.key ? 1 : 0) | WHOLE);
  b[2] = uint8_t(meta.sub);
  b[3] = 0;
  putU32(&b[4], ++p.wholeSeq);
  putU32(&b[8], frameId);
  putU16(&b[12], 0);
  putU16(&b[14], 1);
  putF64(&b[16], meta.capture);
  putF64(&b[24], meta.ts);
  if (n) std::memcpy(&b[kMediaHeader], data, n);
  p.lastSent = nowMs();
  Bytes packet = seal(*p.sendKey, p.sendDir(role_), MEDIA, p.session, ++p.counter, b.data(), b.size());
  p.tcp->sendBinary(packet.data(), packet.size());
}

void DirectEndpoint::enqueue(Peer& p, std::shared_ptr<const Bytes> payload, bool urgent) {
  if (urgent) {
    // audio and retransmissions jump the queue
    send(p, MEDIA, payload->data(), payload->size());
    return;
  }
  if (p.outBytes > (8u << 20)) {
    // the viewer's path cannot keep up: start over from the next keyframe
    p.out.clear();
    p.outBytes = 0;
    if (ev_.keyframe) ev_.keyframe(p.id);
    return;
  }
  p.outBytes += payload->size();
  p.out.push_back(std::move(payload));
}

void DirectEndpoint::drain() {
  double t = nowMs();
  bool pending = false;
  for (auto& kv : peers_) {
    Peer& p = *kv.second;
    // on the local network a frame goes out at (up to) gigabit speed: a 4K frame in ~2 ms
    bool lan = p.path && p.path->kind == "lan";
    double rate = lan ? std::max(paceRate_ * 4, 125000.0) : paceRate_;
    p.tokens = std::min(lan ? 256.0 * 1024 : 64.0 * 1024, p.tokens + (t - p.lastFill) * rate);
    p.lastFill = t;
    while (!p.out.empty() && p.tokens > 0) {
      std::shared_ptr<const Bytes> b = std::move(p.out.front());
      p.out.pop_front();
      p.outBytes -= b->size();
      p.tokens -= double(b->size() + kHeader + kTag + 28);
      p.sentBytes += b->size();
      send(p, MEDIA, b->data(), b->size());
    }
    if (!p.out.empty()) pending = true;
  }
  if (pending && !draining_) {
    draining_ = true;
    std::weak_ptr<bool> alive = alive_;
    loop_.post([this, alive] {
      if (alive.expired() || !*alive.lock()) return;
      draining_ = false;
      drain();
    });
  }
}

void DirectEndpoint::onNack(Peer& p, const uint8_t* body, size_t n) {
  for (size_t i = 0; i + 4 <= n && i < 4 * 1000; i += 4) {
    uint32_t seq = getU32(body + i);
    auto& b = ring_[seq & (RING - 1)];
    if (!b || ringSeq_[seq & (RING - 1)] != seq) continue;
    auto again = std::make_shared<Bytes>(*b);
    (*again)[1] |= 2; // retransmission
    enqueue(p, again, true);
  }
}

/* ---- timers */

void DirectEndpoint::tick() {
  double t = nowMs();
  std::vector<std::string> ids;
  for (auto& kv : peers_) ids.push_back(kv.first);
  for (const std::string& id : ids) {
    auto it = peers_.find(id);
    if (it == peers_.end()) continue;
    Peer& p = *it->second;
    if (p.sendKey) probe(p, t);
    if (p.state != "connected") continue;
    // the path went quiet: look for one again (the relay included)
    if (t - p.lastRecv > 2000 && t > p.probeUntil) startProbing(p, 8000);
    if (t - p.lastRecv > 6000) {
      p.path.reset();
      setState(p, "lost");
      continue;
    }
    if (role_ == Role::Viewer) {
      if (t - p.lastPing >= 500) {
        p.lastPing = t;
        uint8_t ping[8];
        putF64(ping, t);
        send(p, PING, ping, 8);
      }
      if (!p.missing.empty()) sendNacks(p, t);
      if (!p.next || p.waitSince || !p.frames.empty()) deliver(p, t);
      if (closed_) return;
      if (t - p.lastReport >= 1000) report(p, t);
    }
  }
}

void DirectEndpoint::report(Peer& p, double t) {
  auto& s = p.stats;
  Peer::Stats prev = p.havePrev ? p.prevStats : Peer::Stats{};
  if (!p.havePrev) prev.at = t - 1000;
  double packets = double(s.packets - prev.packets);
  double lostRaw = double(s.lost - prev.lost) - double(s.recovered - prev.recovered);
  double lost = std::max(0.0, lostRaw);
  double span = std::max(1.0, t - prev.at);
  Json out = Json::object();
  out.set("loss", packets + lost > 0 ? std::round(lost / (packets + lost) * 1000) / 10 : 0.0);
  out.set("kbps", std::round(double(s.bytes - prev.bytes) * 8 / span));
  out.set("fps", std::round(double(s.frames - prev.frames) * 1000 / span));
  out.set("skips", double(s.skips - prev.skips));
  out.set("rtt", !std::isnan(p.clock.rtt) ? Json(std::round(p.clock.rtt * 10) / 10) : Json());
  out.set("path", p.path ? Json(p.path->kind) : Json());
  p.prevStats = s;
  p.prevStats.at = t;
  p.havePrev = true;
  p.lastReport = t;
  std::string text = out.dump();
  send(p, REPORT, reinterpret_cast<const uint8_t*>(text.data()), text.size());
  if (ev_.stats) ev_.stats(p.id, out);
}

}  // namespace ss
