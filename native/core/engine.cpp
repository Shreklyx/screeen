#include "engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <chrono>
#include <regex>
#include <thread>

#include "clock.h"

namespace ss {

namespace {

const char* kCodec = "avc1.640034"; // H.264 High 5.2 (up to 4K 60): every GPU decodes it
const double kMinKbps = 3000;

// why a viewer is not served, or why a connection is made again – the page's WHY_TEXT
const std::map<std::string, std::string> kWhy = {
    {"viewer-browser", "a néző böngészője nem tud közvetlen módban fogadni (régebbi böngésző vagy iPhone)"},
    {"server", "a szerver még nem ismeri – futtasd rajta újra: sudo bash deploy/install.sh"},
    {"encoder", "nincs használható H.264 kódoló"},
    {"udp", "a két gép nem érte el egymást"},
    {"udp-blocked", "sem UDP-n, sem a szerveren át nem jött létre kapcsolat – tűzfal?"},
    {"timeout", "a közvetlen kapcsolat nem jött létre időben"},
    {"unavailable", "a közvetlen átvitel nem indult el"},
    {"failed", "a közvetlen kapcsolat megszakadt"},
};

std::string whyText(const std::string& why) {
  auto it = kWhy.find(why);
  return it == kWhy.end() ? why : it->second;
}

std::string randomHex(size_t n) {
  Bytes b(n);
  randomBytes(b.data(), n);
  return toHex(b.data(), n);
}

/** The TURN server (from the ICE servers the server gives) for direct mode's last-resort relay. */
std::optional<TurnInfo> turnInfo(const Json& iceServers) {
  static const std::regex re("^turn:([^:?]+)(?::(\\d+))?(?:\\?transport=(\\w+))?$", std::regex::icase);
  for (const Json& s : iceServers.items()) {
    if (!s["username"].isString() || !s["credential"].isString()) continue;
    std::vector<std::string> urls;
    if (s["urls"].isString()) urls.push_back(s["urls"].str());
    for (const Json& u : s["urls"].items()) if (u.isString()) urls.push_back(u.str());
    for (const std::string& url : urls) {
      std::smatch m;
      if (!std::regex_match(url, m, re)) continue;
      std::string transport = m[3].str();
      std::transform(transport.begin(), transport.end(), transport.begin(), ::tolower);
      if (!transport.empty() && transport != "udp") continue;
      TurnInfo t;
      t.host = m[1].str();
      t.port = m[2].matched ? uint16_t(std::stoi(m[2].str())) : 3478;
      t.username = s["username"].str();
      t.credential = s["credential"].str();
      return t;
    }
  }
  return std::nullopt;
}

}  // namespace

struct Engine::Impl {
  EngineConfig cfg;
  EngineCallbacks cb;
  Loop loop;
  std::string wsUrl, tcpUrl, serverHost, site;

  /* ---- the signaling connection (reconnects by itself) */
  std::unique_ptr<WebSocket> ws;
  bool wsOpen = false, wsWanted = false;
  int wsGen = 0, wsAttempt = 0, wsTimer = 0;
  std::string mode; // "", "host", "viewer"

  /* ---- sharer */
  struct Viewer {
    std::string id;
    int n = 0;
    Json caps;
    std::string session;
    double createdAt = 0;
    bool direct = false;
    std::string state; // starting / connecting / connected / failed / lost / closed
    std::string path;
    double rtt = NAN, minRtt = INFINITY;
    Json report;
    int directTimer = 0, retryTimer = 0, retries = 0;
    std::string why;      // why it is being connected again / not served
    bool unsupported = false;
    Json remote;          // what the viewer measured
  };
  std::string password, code, token;
  Json iceServers;
  bool serverDirect = false, live = true, hostPending = false;
  int hostTimer = 0;
  std::map<std::string, Viewer> viewers;
  int seq = 0;
  std::unique_ptr<DirectEndpoint> ep;
  double kbps = 0, lastCut = 0, lastRaise = 0, targetKbps = 0, sentKbps = 0;
  bool streaming = false;
  double lastKey = -1e9;
  int keyTimer = 0;
  double encodeMs = NAN;
  int encW = 0, encH = 0;
  int statsTimer = 0, hostTick = 0;

  /* ---- viewer */
  std::string viewCode, viewSession;
  bool viewClosed = false, joinPending = false;
  int joinTimer = 0, retryJoin = 0;
  struct View {
    uint32_t udp = 0;
    int ready = 0; // 0 waiting, 1 ok, -1 failed
    Offer mine;
    std::string state = "starting";
    std::string path;
    double rtt = NAN;
    Json report;
    uint64_t frames = 0;
    bool picture = false;
  };
  std::unique_ptr<View> view;
  Json pendingOffer;
  int pendingOfferTimer = 0;
  Json hostStats;
  bool viewLive = false;
  std::atomic<double> offset{NAN};
  double lat = NAN, shownFps = NAN;
  uint64_t lastFrames = 0;
  double fpsAt = 0;
  int viewTimer = 0, viewTick = 0;

  Impl(EngineConfig c, EngineCallbacks k) : cfg(std::move(c)), cb(std::move(k)) {
    std::string s = cfg.server;
    while (!s.empty() && s.back() == '/') s.pop_back();
    bool tls = s.compare(0, 8, "https://") == 0;
    std::string hostport = s.substr(s.find("://") == std::string::npos ? 0 : s.find("://") + 3);
    hostport = hostport.substr(0, hostport.find('/'));
    serverHost = hostport.substr(0, hostport.find(':'));
    wsUrl = std::string(tls ? "wss://" : "ws://") + hostport + "/ws";
    tcpUrl = std::string(tls ? "wss://" : "ws://") + hostport + "/direct";
    site = std::string(tls ? "https://" : "http://") + hostport + "/";
    if (!cfg.webSocket) cfg.webSocket = openPlainWebSocket;
    loop.start();
  }

  ~Impl() {
    std::atomic<bool> done{false};
    loop.post([this, &done] {
      shutdown();
      done = true;
    });
    while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    loop.stop();
  }

  void emit(Json ev) {
    if (cb.event) cb.event(ev);
  }

  void status(const std::string& text, const std::string& kind = "") {
    Json e = Json::object();
    e.set("type", "status").set("text", text).set("kind", kind);
    emit(e);
  }

  void viewState(const std::string& text, bool picture = false) {
    Json e = Json::object();
    e.set("type", "view-state").set("text", text).set("picture", picture);
    emit(e);
  }

  void shutdown() {
    if (mode == "host" && wsOpen) send(Json::object().set("t", "close"));
    if (mode == "viewer" && wsOpen) send(Json::object().set("t", "leave"));
    clearHost();
    closeView();
    if (ep) ep->close();
    ep.reset();
    closeSignal();
    mode.clear();
  }

  /* ------------------------------------------------ signaling */

  void openSignal() {
    wsWanted = true;
    if (ws) return;
    int gen = ++wsGen;
    WebSocketEvents ev;
    ev.open = [this, gen] { loop.post([this, gen] { if (gen == wsGen) onSignalOpen(); }); };
    ev.text = [this, gen](std::string text) {
      loop.post([this, gen, text] {
        if (gen != wsGen) return;
        Json msg = Json::parse(text);
        if (msg.isObject() && msg["t"].isString()) onSignalMessage(msg);
      });
    };
    ev.closed = [this, gen] { loop.post([this, gen] { if (gen == wsGen) onSignalClosed(); }); };
    ws = cfg.webSocket(wsUrl, ev);
  }

  void closeSignal() {
    wsWanted = false;
    loop.cancel(wsTimer);
    wsGen++;
    if (ws) ws->close();
    ws.reset();
    wsOpen = false;
  }

  void send(const Json& msg) {
    if (ws && wsOpen) ws->sendText(msg.dump());
  }

  void onSignalOpen() {
    wsOpen = true;
    wsAttempt = 0;
    if (mode == "host") sendHost();
    else if (mode == "viewer") sendJoin();
  }

  void onSignalClosed() {
    ws.reset();
    wsOpen = false;
    if (!wsWanted) return;
    if (mode == "host") {
      if (!code.empty()) status("Megszakadt a kapcsolat a szerverrel, újracsatlakozás… (a nézők látnak tovább)", "warn");
      else if (hostPending) hostFailed("offline");
    } else if (mode == "viewer" && !(view && view->state == "connected")) {
      viewState("Megszakadt a kapcsolat a szerverrel, újracsatlakozás…");
    }
    double delay = std::min(10000.0, 500.0 * std::pow(2, wsAttempt++));
    wsTimer = loop.setTimer(delay, [this] { if (wsWanted && !ws) openSignal(); });
  }

  void onSignalMessage(const Json& msg) {
    const std::string& t = msg["t"].str();
    if (mode == "host") onHostMessage(t, msg);
    else if (mode == "viewer") onViewerMessage(t, msg);
  }

  /* ------------------------------------------------ sharer */

  void ensureEndpoint(DirectEndpoint::Role role) {
    if (ep && ep->role() == role) return;
    if (ep) ep->close();
    DirectEndpoint::Events ev;
    if (role == DirectEndpoint::Role::Host) {
      ev.state = [this](const std::string& id, const Json& info) { onHostDirectState(id, info); };
      ev.report = [this](const std::string& id, const Json& r) { onHostReport(id, r); };
      ev.keyframe = [this](const std::string&) { encoderKeyframe(); };
    } else {
      ev.state = [this](const std::string&, const Json& info) { onViewDirectState(info); };
      ev.clock = [this](const std::string&, double rtt, double off) {
        offset = off;
        if (view) view->rtt = rtt;
      };
      ev.stats = [this](const std::string&, const Json& r) { if (view) view->report = r; };
      ev.media = [this](const MediaMeta& m, const uint8_t* d, size_t n) { onViewMedia(m, d, n); };
    }
    ev.log = [](const std::string&) {};
    ep.reset(new DirectEndpoint(loop, role, serverHost, tcpUrl, cfg.webSocket, ev));
    ep->forceRelay = cfg.forceRelay;
    ep->forceTurn = cfg.forceTurn;
    ep->forceTcp = cfg.forceTcp;
    if (!ep->open()) status("A közvetlen átvitel nem indult el (UDP).", "error");
  }

  void startHost(const std::string& pw) {
    if (mode == "viewer") stopView();
    mode = "host";
    password = pw;
    hostPending = true;
    ensureEndpoint(DirectEndpoint::Role::Host);
    loop.cancel(hostTimer);
    hostTimer = loop.setTimer(12000, [this] { if (hostPending) hostFailed("timeout"); });
    status("Kapcsolódás a szerverhez…");
    if (wsOpen) sendHost();
    else openSignal();
    if (!statsTimer) statsTimer = loop.setTimer(1000, [this] { hostStatsTick(); }, true);
  }

  void sendHost() {
    Json m = Json::object();
    m.set("t", "host").set("password", password);
    if (!code.empty() && !token.empty()) m.set("resume", Json::object().set("code", code).set("token", token));
    send(m);
  }

  void hostFailed(const std::string& why) {
    hostPending = false;
    loop.cancel(hostTimer);
    Json e = Json::object();
    e.set("type", "host-error").set("code", why);
    emit(e);
    if (code.empty()) {
      mode.clear();
      closeSignal();
    }
  }

  void onHostMessage(const std::string& t, const Json& msg) {
    if (t == "hosted") applyHosted(msg);
    else if (t == "error" && hostPending) hostFailed(msg["code"].str("error"));
    else if (t == "viewer") ensureViewer(msg["id"].str(), msg["caps"]);
    else if (t == "viewer-left") dropViewer(msg["id"].str());
    else if (t == "signal") onViewerSignal(msg["from"].str(), msg["data"]);
    else if (t == "direct") onDirectSession(msg);
    else if (t == "count") emitViewers(int(msg["n"].num()));
    else if (t == "replaced") {
      clearHost();
      status("A szobát egy másik ablak vette át.", "warn");
      Json e = Json::object();
      e.set("type", "closed").set("reason", "replaced");
      emit(e);
      mode.clear();
      closeSignal();
    }
  }

  void applyHosted(const Json& res) {
    bool changed = !code.empty() && res["code"].str() != code;
    hostPending = false;
    loop.cancel(hostTimer);
    code = res["code"].str();
    token = res["token"].str();
    iceServers = res["iceServers"];
    serverDirect = res["direct"].b(false);
    Json e = Json::object();
    e.set("type", "hosted").set("code", code).set("link", site + "#" + code).set("changed", changed);
    emit(e);
    status("Kapcsolódva. A nézők a linkkel vagy a kóddal csatlakozhatnak.", "ok");
    send(Json::object().set("t", "state").set("live", live));
    std::map<std::string, bool> present;
    for (const Json& v : res["viewers"].items()) present[v["id"].str()] = true;
    std::vector<std::string> gone;
    for (auto& kv : viewers) if (!present.count(kv.first)) gone.push_back(kv.first);
    for (auto& id : gone) dropViewer(id);
    for (const Json& v : res["viewers"].items()) ensureViewer(v["id"].str(), v["caps"]);
  }

  void ensureViewer(const std::string& id, const Json& caps) {
    if (id.empty()) return;
    auto it = viewers.find(id);
    if (it != viewers.end() && it->second.direct && it->second.state != "failed" && it->second.state != "lost" && it->second.state != "closed") return;
    if (it == viewers.end()) {
      Viewer v;
      v.id = id;
      v.n = ++seq;
      v.caps = caps;
      it = viewers.emplace(id, v).first;
    }
    Viewer& v = it->second;
    if (!serverDirect || !v.caps["direct"].b(false)) {
      // only WebRTC there – this app has none
      v.unsupported = true;
      v.why = !serverDirect ? "server" : "viewer-browser";
      Json data = Json::object();
      data.set("type", "unsupported").set("why", v.why);
      send(Json::object().set("t", "signal").set("to", id).set("data", data));
    } else {
      startDirectViewer(v);
    }
    emitViewers();
  }

  void dropViewer(const std::string& id) {
    auto it = viewers.find(id);
    if (it == viewers.end()) return;
    loop.cancel(it->second.retryTimer);
    stopDirectViewer(it->second);
    viewers.erase(id);
    updateStreaming();
    emitViewers();
  }

  void startDirectViewer(Viewer& v) {
    loop.cancel(v.retryTimer);
    stopDirectViewer(v);
    v.session = randomHex(8);
    v.createdAt = nowMs();
    v.direct = true;
    v.state = "starting";
    v.path.clear();
    v.rtt = NAN;
    v.minRtt = INFINITY;
    v.report = Json();
    send(Json::object().set("t", "direct").set("to", v.id));
    std::string id = v.id;
    // no direct picture in time: try again
    v.directTimer = loop.setTimer(12000, [this, id] {
      auto it = viewers.find(id);
      if (it != viewers.end() && it->second.direct && it->second.state != "connected") directFailed(it->second, "timeout");
    });
  }

  void stopDirectViewer(Viewer& v) {
    loop.cancel(v.directTimer);
    if (!v.direct) return;
    v.direct = false;
    if (ep) ep->removePeer(v.id);
    updateStreaming();
  }

  void directFailed(Viewer& v, const std::string& why) {
    stopDirectViewer(v);
    v.why = why;
    v.retries++;
    loop.cancel(v.retryTimer);
    std::string id = v.id;
    v.retryTimer = loop.setTimer(std::min(15000.0, 1500.0 * v.retries), [this, id] {
      auto it = viewers.find(id);
      if (it != viewers.end() && !it->second.direct) startDirectViewer(it->second);
    });
    emitViewers();
  }

  void onDirectSession(const Json& msg) {
    std::string id = msg["viewer"].str();
    auto it = viewers.find(id);
    if (it == viewers.end() || !it->second.direct || it->second.state != "starting" || !ep) return;
    std::string session = it->second.session;
    PeerInfo info;
    info.session = uint32_t(msg["session"].num());
    info.secret = msg["secret"].str();
    info.port = uint16_t(msg["port"].num());
    info.turn = turnInfo(iceServers);
    uint32_t udp = info.session;
    ep->addPeer(id, info, [this, id, session, udp](bool ok, Offer offer) {
      auto vi = viewers.find(id);
      if (vi == viewers.end() || vi->second.session != session || !vi->second.direct) return;
      if (!ok) return directFailed(vi->second, "unavailable");
      vi->second.state = "connecting";
      Json data = offer.toJson();
      data.set("type", "direct-offer").set("session", session).set("udp", double(udp)).set("codec", kCodec);
      send(Json::object().set("t", "signal").set("to", id).set("data", data));
      emitViewers();
    });
  }

  void onViewerSignal(const std::string& from, const Json& data) {
    auto it = viewers.find(from);
    if (it == viewers.end() || !data.isObject()) return;
    Viewer& v = it->second;
    const std::string& type = data["type"].str();
    if (type == "recall") {
      if (!v.unsupported && nowMs() - v.createdAt > 3000) startDirectViewer(v);
      return;
    }
    if (data["session"].str() != v.session) return;
    if (type == "direct-answer" && v.direct && v.state == "connecting") {
      ep->connectPeer(v.id, Offer::fromJson(data));
    } else if (type == "direct-fail") {
      std::string why = data["why"].str("failed");
      directFailed(v, kWhy.count(why) ? why : "failed");
    } else if (type == "stats") {
      v.remote = data;
    }
  }

  void onHostDirectState(const std::string& id, const Json& info) {
    auto it = viewers.find(id);
    if (it == viewers.end() || !it->second.direct) return;
    Viewer& v = it->second;
    const std::string& state = info["state"].str();
    v.state = state;
    v.path = info["path"].str("");
    v.rtt = info["rtt"].num(NAN);
    if (state == "connected") {
      loop.cancel(v.directTimer);
      if (!kbps) kbps = v.path == "lan" ? target() : std::min(target(), 25000.0);
      applyBitrate(true);
      updateStreaming();
      encoderKeyframe();
    } else if (state == "failed") {
      const Json& d = info["diag"];
      bool nothing = d.isObject() && !d["server"].b() && !d["turn"].b() && !d["tcp"].b();
      // the viewer's object may go away in directFailed's callbacks: copy first
      directFailed(v, nothing ? "udp-blocked" : "udp");
      return;
    } else if (state == "lost" || state == "closed") {
      directFailed(v, "failed"); // the viewer's network changed or its app restarted: connect again
      return;
    }
    emitViewers();
  }

  void onHostReport(const std::string& id, const Json& r) {
    auto it = viewers.find(id);
    if (it == viewers.end() || !it->second.direct) return;
    Viewer& v = it->second;
    v.report = r;
    if (r["rtt"].isNumber()) v.minRtt = std::min(v.minRtt, r["rtt"].num());
    if (r["fps"].num() > 0) {
      v.retries = 0;
      v.why.clear();
    }
    adaptBitrate();
  }

  double target() const {
    if (targetKbps > 0) return targetKbps;
    return cfg.hardware ? 120000 : 20000; // the most beautiful picture the network allows
  }

  // No bandwidth estimate as in WebRTC: lower the bitrate when a viewer loses packets, has to skip
  // frames or its round trip grows (queues filling up), raise it slowly while all is well.
  void adaptBitrate() {
    std::vector<Viewer*> list;
    for (auto& kv : viewers) if (kv.second.direct && kv.second.state == "connected" && kv.second.report.isObject()) list.push_back(&kv.second);
    if (list.empty()) return;
    double now = nowMs();
    bool congested = false, calm = true;
    for (Viewer* v : list) {
      const Json& r = v->report;
      if (r["loss"].num() > 2 || r["skips"].num() > 0) congested = true;
      if (r["rtt"].isNumber() && r["rtt"].num() > v->minRtt + 40) congested = true;
      if (r["loss"].num() >= 0.5) calm = false;
    }
    if (congested && now - lastCut > 2000) {
      kbps = std::max(kMinKbps, kbps * 0.7);
      lastCut = now;
    } else if (!congested && now - lastCut > 3000 && now - lastRaise > 1000 && calm) {
      kbps = std::min(target(), kbps * 1.12 + 500);
      lastRaise = now;
    }
    kbps = std::min(kbps, target());
    applyBitrate(false);
  }

  void applyBitrate(bool force) {
    if (!ep || kbps <= 0) return;
    ep->setBitrate(kbps * 1000);
    if (force || std::fabs(kbps - sentKbps) > sentKbps * 0.05) {
      sentKbps = kbps;
      if (cb.bitrate) cb.bitrate(kbps * 1000);
    }
  }

  void encoderKeyframe() {
    // at most 4 keyframes a second (each is big); a request in between is served after
    if (keyTimer) return;
    double wait = std::max(0.0, lastKey + 250 - nowMs());
    keyTimer = loop.setTimer(wait, [this] {
      keyTimer = 0;
      lastKey = nowMs();
      if (cb.keyframe) cb.keyframe();
    });
  }

  void updateStreaming() {
    bool any = false;
    for (auto& kv : viewers) if (kv.second.direct && kv.second.state == "connected") any = true;
    bool on = mode == "host" && live && any;
    if (on == streaming) return;
    streaming = on;
    if (cb.streaming) cb.streaming(on);
    if (on) encoderKeyframe();
  }

  void hostStatsTick() {
    if (mode != "host") return;
    if (++hostTick % 2 == 0) {
      for (auto& kv : viewers) {
        Viewer& v = kv.second;
        if (!v.direct || v.state != "connected") continue;
        Json data = Json::object();
        data.set("type", "stats").set("session", v.session).set("hw", cfg.hardware).set("encoder", cfg.hardware ? Json(cfg.encoderName) : Json()).set("codec", "H264");
        data.set("encodeMs", std::isnan(encodeMs) ? Json() : Json(encodeMs));
        send(Json::object().set("t", "signal").set("to", v.id).set("data", data));
      }
    }
    emitViewers();
  }

  void emitViewers(int count = -1) {
    Json list = Json::array();
    for (auto& kv : viewers) {
      Viewer& v = kv.second;
      Json o = Json::object();
      o.set("id", v.id).set("n", v.n);
      std::string state = v.unsupported ? "unsupported" : !v.direct ? "retrying" : v.state;
      o.set("state", state).set("path", v.path);
      const Json& r = v.report;
      if (r.isObject() && v.state == "connected") o.set("fps", r["fps"]).set("kbps", r["kbps"]).set("loss", r["loss"]);
      if (!std::isnan(v.rtt)) o.set("rtt", r["rtt"].isNumber() ? r["rtt"] : Json(v.rtt));
      if (v.remote["lat"].isNumber()) o.set("lat", v.remote["lat"]);
      if (!v.why.empty()) o.set("why", v.why).set("whyText", whyText(v.why));
      list.push(o);
    }
    Json e = Json::object();
    e.set("type", "viewers").set("list", list).set("width", encW).set("height", encH);
    if (count >= 0) e.set("count", count);
    emit(e);
  }

  void clearHost() {
    std::vector<std::string> ids;
    for (auto& kv : viewers) ids.push_back(kv.first);
    for (auto& id : ids) dropViewer(id);
    loop.cancel(statsTimer);
    statsTimer = 0;
    loop.cancel(hostTimer);
    loop.cancel(keyTimer);
    keyTimer = 0;
    code.clear();
    token.clear();
    hostPending = false;
    kbps = 0;
    sentKbps = 0;
    if (streaming) {
      streaming = false;
      if (cb.streaming) cb.streaming(false);
    }
  }

  /* ------------------------------------------------ viewer */

  void startView(const std::string& c) {
    if (mode == "host") {
      send(Json::object().set("t", "close"));
      clearHost();
    }
    mode = "viewer";
    viewCode = c;
    viewClosed = false;
    ensureEndpoint(DirectEndpoint::Role::Viewer);
    viewState("Csatlakozás a szobához…");
    if (wsOpen) sendJoin();
    else openSignal();
    if (!viewTimer) viewTimer = loop.setTimer(1000, [this] { viewStatsTick(); }, true);
  }

  void sendJoin() {
    loop.cancel(retryJoin);
    joinPending = true;
    Json caps = Json::object();
    caps.set("direct", true);
    send(Json::object().set("t", "join").set("code", viewCode).set("caps", caps));
  }

  void onViewerMessage(const std::string& t, const Json& msg) {
    if (t == "joined") {
      joinPending = false;
      iceServers = msg["iceServers"];
      viewLive = msg["live"].b(false);
      Json e = Json::object();
      e.set("type", "joined").set("code", msg["code"].str(viewCode));
      emit(e);
      if (!msg["hostPresent"].b(false)) viewState("A megosztó épp nincs bent. Várakozás…");
      else if (!viewLive) viewState("A megosztás szünetel. Amint folytatódik, itt lesz a kép.");
      else if (!(view && view->picture)) viewState("Csatlakozva. Kép betöltése…");
    } else if (t == "error" && joinPending) {
      joinPending = false;
      std::string c = msg["code"].str();
      std::string text = c == "notfound" ? "Nincs ilyen szoba (lejárt vagy elírtad a kódot)." : c == "full" ? "A szoba megtelt." : c == "rate" ? "Túl sok próbálkozás, várj egy kicsit." : "Nem sikerült csatlakozni.";
      viewState(text + " Újrapróbálom…");
      retryJoin = loop.setTimer(c == "rate" ? 30000 : 5000, [this] { if (mode == "viewer" && wsOpen) sendJoin(); });
    } else if (t == "signal" && msg["from"].str() == "host") {
      onHostSignal(msg["data"]);
    } else if (t == "direct") {
      onViewDirectSession(msg);
    } else if (t == "state" || t == "host-back") {
      viewLive = msg["live"].b(false);
      if (!viewLive) viewState("A megosztás szünetel. Amint folytatódik, itt lesz a kép.");
      else if (view && view->picture) viewState("", true);
      else viewState("Kép betöltése…");
    } else if (t == "host-away") {
      if (!(view && view->state == "connected" && view->picture)) viewState("A megosztó kapcsolata megszakadt. Várakozás…");
    } else if (t == "closed") {
      viewClosed = true;
      closeView();
      Json e = Json::object();
      e.set("type", "closed").set("reason", msg["reason"].str("closed"));
      emit(e);
      viewState(msg["reason"].str() == "timeout" ? "A megosztó nem tért vissza, a szoba bezárult." : "A megosztó befejezte a megosztást. Köszi, hogy néztél!");
      mode.clear();
      closeSignal();
    }
  }

  void sendToHost(Json data) {
    data.set("session", viewSession);
    send(Json::object().set("t", "signal").set("to", "host").set("data", data));
  }

  void onViewDirectSession(const Json& msg) {
    closeView();
    view.reset(new View);
    View* d = view.get();
    d->udp = uint32_t(msg["session"].num());
    PeerInfo info;
    info.session = d->udp;
    info.secret = msg["secret"].str();
    info.port = uint16_t(msg["port"].num());
    info.turn = turnInfo(iceServers); // the last resort
    ensureEndpoint(DirectEndpoint::Role::Viewer);
    ep->addPeer("host", info, [this, d](bool ok, Offer mine) {
      if (view.get() != d) return;
      d->ready = ok ? 1 : -1;
      d->mine = mine;
      if (pendingOffer.isObject() && uint32_t(pendingOffer["udp"].num()) == d->udp) {
        Json offer = pendingOffer;
        pendingOffer = Json();
        loop.cancel(pendingOfferTimer);
        onDirectOffer(offer);
      }
    });
  }

  void onHostSignal(const Json& data) {
    if (!data.isObject()) return;
    const std::string& type = data["type"].str();
    if (type == "direct-offer") {
      onDirectOffer(data);
    } else if (type == "offer") {
      // the sharer chose WebRTC (it shares from a browser that cannot do direct mode)
      closeView();
      viewState("A megosztó böngészője nem tud közvetlen módban küldeni (WebRTC kellene). Kérd meg, hogy Chrome-ból, Edge-ből vagy az alkalmazásból ossza meg.");
    } else if (type == "stats") {
      if (data["session"].str() == viewSession) hostStats = data;
    }
  }

  void onDirectOffer(const Json& data) {
    uint32_t udp = uint32_t(data["udp"].num());
    if (!view || view->udp != udp || view->ready == 0) {
      // the server's session message normally comes first; wait a moment if not
      pendingOffer = data;
      loop.cancel(pendingOfferTimer);
      pendingOfferTimer = loop.setTimer(2000, [this] {
        if (!pendingOffer.isObject()) return;
        viewSession = pendingOffer["session"].str();
        pendingOffer = Json();
        closeView();
        Json fail = Json::object();
        fail.set("type", "direct-fail").set("why", "unavailable");
        sendToHost(fail);
      });
      return;
    }
    viewSession = data["session"].str();
    if (view->ready < 0) {
      closeView();
      Json fail = Json::object();
      fail.set("type", "direct-fail").set("why", "unavailable");
      sendToHost(fail);
      return;
    }
    view->state = "connecting";
    ep->connectPeer("host", Offer::fromJson(data));
    Json answer = view->mine.toJson();
    answer.set("type", "direct-answer");
    sendToHost(answer);
    if (!view->picture) viewState("Közvetlen kapcsolat…");
  }

  void onViewDirectState(const Json& info) {
    if (!view) return;
    const std::string& state = info["state"].str();
    view->state = state;
    view->path = info["path"].str("");
    if (info["rtt"].isNumber()) view->rtt = info["rtt"].num();
    if (state == "failed") {
      const Json& d = info["diag"];
      bool nothing = d.isObject() && !d["server"].b() && !d["turn"].b() && !d["tcp"].b();
      std::string why = nothing ? "udp-blocked" : "udp";
      closeView();
      Json fail = Json::object();
      fail.set("type", "direct-fail").set("why", why);
      sendToHost(fail);
      viewState("A kapcsolat nem jött létre: " + whyText(why) + ". Újrapróbálom…");
    } else if (state == "lost" || state == "closed") {
      closeView();
      if (!viewClosed) {
        viewState("A közvetlen kapcsolat megszakadt. Újracsatlakozás…");
        Json recall = Json::object();
        recall.set("type", "recall");
        sendToHost(recall);
      }
    }
  }

  void onViewMedia(const MediaMeta& m, const uint8_t* d, size_t n) {
    if (!view) return;
    if (m.stream == 0) {
      view->frames++;
      if (!view->picture) {
        view->picture = true;
        viewState("", true);
      }
    }
    if (cb.media) cb.media(m, d, n);
  }

  void closeView() {
    loop.cancel(pendingOfferTimer);
    if (!view) return;
    view.reset();
    if (ep && ep->role() == DirectEndpoint::Role::Viewer) ep->removePeer("host");
  }

  void stopView() {
    if (wsOpen) send(Json::object().set("t", "leave"));
    viewClosed = true;
    closeView();
    loop.cancel(viewTimer);
    viewTimer = 0;
    loop.cancel(retryJoin);
    mode.clear();
    closeSignal();
  }

  void viewStatsTick() {
    if (mode != "viewer" || !view) return;
    double now = nowMs();
    double fps = fpsAt ? double(view->frames - lastFrames) * 1000 / std::max(1.0, now - fpsAt) : NAN;
    lastFrames = view->frames;
    fpsAt = now;
    const Json& r = view->report;
    if (++viewTick % 2 == 0 && !std::isnan(lat)) {
      Json s = Json::object();
      s.set("type", "stats").set("lat", std::round(lat)).set("fps", std::round(std::isnan(shownFps) ? fps : shownFps)).set("loss", r["loss"].num(0)).set("measured", true);
      sendToHost(s);
    }
    Json e = Json::object();
    e.set("type", "view-stats").set("fps", std::isnan(fps) ? Json() : Json(fps)).set("kbps", r["kbps"]).set("loss", r["loss"]);
    e.set("rtt", std::isnan(view->rtt) ? Json() : Json(view->rtt)).set("path", view->path).set("lat", std::isnan(lat) ? Json() : Json(lat));
    e.set("encoder", hostStats["encoder"]).set("hw", hostStats["hw"]).set("encodeMs", hostStats["encodeMs"]);
    emit(e);
  }
};

/* ------------------------------------------------ the public face: everything onto the loop */

Engine::Engine(EngineConfig config, EngineCallbacks callbacks) : impl_(new Impl(std::move(config), std::move(callbacks))) {}

Engine::~Engine() = default;

void Engine::host(const std::string& password) {
  impl_->loop.post([this, password] { impl_->startHost(password); });
}

void Engine::setLive(bool live) {
  impl_->loop.post([this, live] {
    impl_->live = live;
    impl_->send(Json::object().set("t", "state").set("live", live));
    impl_->updateStreaming();
  });
}

void Engine::stopHosting() {
  impl_->loop.post([this] {
    if (impl_->mode != "host") return;
    impl_->send(Json::object().set("t", "close"));
    impl_->clearHost();
    impl_->mode.clear();
    impl_->closeSignal();
  });
}

void Engine::setTargetKbps(double kbps) {
  impl_->loop.post([this, kbps] {
    impl_->targetKbps = kbps;
    if (impl_->kbps) {
      impl_->kbps = std::min(impl_->kbps, impl_->target());
      impl_->applyBitrate(true);
    }
  });
}

void Engine::sendVideo(const uint8_t* data, size_t n, bool key, double captureMs, double tsUs) {
  auto copy = std::make_shared<Bytes>(data, data + n);
  impl_->loop.post([this, copy, key, captureMs, tsUs] {
    if (!impl_->ep || impl_->mode != "host") return;
    MediaMeta m;
    m.stream = 0;
    m.key = key;
    m.ts = tsUs;
    m.capture = captureMs;
    impl_->ep->sendMedia(m, copy->data(), copy->size());
  });
}

void Engine::sendAudio(int pair, int channels, const uint8_t* data, size_t n, double tsUs) {
  auto copy = std::make_shared<Bytes>(data, data + n);
  impl_->loop.post([this, copy, pair, channels, tsUs] {
    if (!impl_->ep || impl_->mode != "host") return;
    MediaMeta m;
    m.stream = 1;
    m.sub = pair | channels << 4;
    m.ts = tsUs;
    m.capture = nowMs();
    impl_->ep->sendMedia(m, copy->data(), copy->size());
  });
}

void Engine::setEncoderStats(double encodeMs, int width, int height) {
  impl_->loop.post([this, encodeMs, width, height] {
    impl_->encodeMs = encodeMs;
    impl_->encW = width;
    impl_->encH = height;
  });
}

void Engine::join(const std::string& code) {
  impl_->loop.post([this, code] { impl_->startView(code); });
}

void Engine::leave() {
  impl_->loop.post([this] { if (impl_->mode == "viewer") impl_->stopView(); });
}

void Engine::requestKeyframe() {
  impl_->loop.post([this] { if (impl_->ep && impl_->view) impl_->ep->keyframe("host"); });
}

void Engine::reportLatency(double ms, double fps) {
  impl_->loop.post([this, ms, fps] {
    impl_->lat = ms;
    impl_->shownFps = fps;
  });
}

double Engine::clockOffset() const { return impl_->offset.load(); }

std::string Engine::siteUrl() const { return impl_->site; }

}  // namespace ss
