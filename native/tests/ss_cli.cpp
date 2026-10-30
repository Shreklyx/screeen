// ss-cli – the session engine without a window: shares a recorded H.264 stream, or watches a room
// and reports what arrived. For tests against the server, the Electron app and the browsers.
//
//   ss-cli host --server http://127.0.0.1:8090 --password titok123 --h264 file.h264 [--fps 60] [--seconds 30]
//   ss-cli view --server http://127.0.0.1:8090 --code abc123 [--seconds 10] [--out received.h264]
//   the room's chat, both: [--name NAME] [--chat TEXT (once in the room)] [--chat-reply TEXT ({name}: who wrote)]
//   host: [--resume CODE:TOKEN (take that room back)] [--keep (leave the room open at the end, as an app quitting)]
//   ss-cli ws-bench --url ws://127.0.0.1:8095 [--count 240]   (round trips through an echo server)
//
// Prints the engine's events as JSON lines; "host" prints "CODE <code>" and "TOKEN <token>" once the room is open,
// "view" ends with "RESULT {…}" (frames, keyframes, bytes, latency); chat messages as "CHAT {…}".
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "clock.h"
#include "engine.h"
#include "ws_bench.h"

using namespace ss;

static std::string arg(int argc, char** argv, const char* name, const char* fallback = "") {
  for (int i = 1; i + 1 < argc; i++) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return fallback;
}

static bool flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; i++) if (std::strcmp(argv[i], name) == 0) return true;
  return false;
}

/** Access units of an Annex B stream, split at the access unit delimiters (00 00 00 01 09). */
static std::vector<Bytes> splitAccessUnits(const Bytes& data) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 < data.size(); i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1 && (data[i + 4] & 0x1F) == 9) starts.push_back(i);
  }
  std::vector<Bytes> out;
  for (size_t k = 0; k < starts.size(); k++) out.emplace_back(data.begin() + long(starts[k]), data.begin() + long(k + 1 < starts.size() ? starts[k + 1] : data.size()));
  return out;
}

static std::mutex printMu;
static void line(const std::string& s) {
  std::lock_guard<std::mutex> l(printMu);
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}

/** The chat options (both modes): prints what is said, says --chat once in the room, answers others
 *  with --chat-reply. Returns true for the chat's events (printed here). */
struct Chat {
  std::string say, reply;
  bool said = false;
  Engine* engine = nullptr;
  Chat(int argc, char** argv) : say(arg(argc, argv, "--chat")), reply(arg(argc, argv, "--chat-reply")) {}
  bool on(const Json& e) {
    const std::string& type = e["type"].str();
    if (type == "name" && !say.empty() && !said && engine) {
      said = true;
      engine->sendChat(say);
    }
    if (type == "chat" && !reply.empty() && !e["mine"].b(false) && engine) {
      std::string text = reply;
      size_t at = text.find("{name}");
      if (at != std::string::npos) text.replace(at, 6, e["name"].str());
      engine->sendChat(text);
    }
    if (type.compare(0, 4, "chat") != 0) return false;
    line("CHAT " + e.dump());
    return true;
  }
};

static int host(int argc, char** argv) {
  std::ifstream f(arg(argc, argv, "--h264"), std::ios::binary);
  Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<Bytes> frames = splitAccessUnits(data);
  if (frames.empty()) {
    line("no frames in --h264");
    return 2;
  }
  int fps = std::stoi(arg(argc, argv, "--fps", "60"));
  double seconds = std::stod(arg(argc, argv, "--seconds", "30"));
  std::atomic<bool> streaming{false}, key{true};
  std::atomic<int> keyframes{0};
  EngineConfig cfg;
  cfg.server = arg(argc, argv, "--server", "http://127.0.0.1:8090");
  cfg.encoderName = "ss-cli";
  cfg.forceTcp = flag(argc, argv, "--tcp");
  cfg.forceRelay = flag(argc, argv, "--relay");
  Chat chat(argc, argv);
  EngineCallbacks cb;
  cb.event = [&chat](const Json& e) {
    if (e["type"].str() == "hosted") {
      line("CODE " + e["code"].str());
      line("TOKEN " + e["token"].str());
    }
    if (!chat.on(e) && e["type"].str() != "viewers") line("EVENT " + e.dump());
  };
  cb.streaming = [&](bool on) {
    streaming = on;
    line(std::string("STREAMING ") + (on ? "on" : "off"));
  };
  cb.keyframe = [&] { key = true; keyframes++; };
  cb.bitrate = [](double bps) { line("BITRATE " + std::to_string(int(bps))); };
  Engine engine(cfg, cb);
  chat.engine = &engine;
  engine.setName(arg(argc, argv, "--name"));
  std::string resume = arg(argc, argv, "--resume");
  size_t colon = resume.find(':');
  engine.host(arg(argc, argv, "--password"), colon == std::string::npos ? "" : resume.substr(0, colon), colon == std::string::npos ? "" : resume.substr(colon + 1));
  double start = nowMs(), next = start;
  size_t n = 0;
  double lastViewers = 0;
  while (nowMs() - start < seconds * 1000) {
    next += 1000.0 / fps;
    double wait = next - nowMs();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds(long(wait * 1000)));
    if (!streaming) continue;
    // a keyframe asked for: the stream again from its first (IDR) frame
    if (key.exchange(false)) n = 0;
    const Bytes& fr = frames[n % frames.size()];
    engine.sendVideo(fr.data(), fr.size(), n % frames.size() == 0, nowMs(), double(n) * 1e6 / fps);
    n++;
    if (nowMs() - lastViewers > 2000) {
      lastViewers = nowMs();
      engine.setEncoderStats(1.0, 1280, 720);
    }
  }
  line("SENT " + std::to_string(n) + " keyframe-requests " + std::to_string(keyframes.load()));
  if (flag(argc, argv, "--keep")) engine.keepRoom();
  chat.engine = nullptr;
  return 0;
}

static int view(int argc, char** argv) {
  double seconds = std::stod(arg(argc, argv, "--seconds", "10"));
  std::string out = arg(argc, argv, "--out");
  std::mutex mu;
  uint64_t frames = 0, keys = 0, bytes = 0, audio = 0;
  bool firstKey = false, sawFrame = false;
  std::vector<double> lat;
  std::ofstream file;
  if (!out.empty()) file.open(out, std::ios::binary);
  Engine* enginePtr = nullptr;
  EngineConfig cfg;
  cfg.server = arg(argc, argv, "--server", "http://127.0.0.1:8090");
  cfg.forceTcp = flag(argc, argv, "--tcp");
  Chat chat(argc, argv);
  EngineCallbacks cb;
  cb.event = [&chat](const Json& e) { if (!chat.on(e)) line("EVENT " + e.dump()); };
  cb.media = [&](const MediaMeta& m, const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> l(mu);
    if (m.stream == 1) {
      audio++;
      return;
    }
    if (!sawFrame) {
      sawFrame = true;
      firstKey = m.key;
    }
    frames++;
    keys += m.key;
    bytes += n;
    if (file) file.write(reinterpret_cast<const char*>(d), std::streamsize(n));
    double off = enginePtr ? enginePtr->clockOffset() : NAN;
    if (!std::isnan(off)) lat.push_back(nowMs() + off - m.capture);
  };
  Engine engine(cfg, cb);
  enginePtr = &engine;
  chat.engine = &engine;
  engine.setName(arg(argc, argv, "--name"));
  engine.join(arg(argc, argv, "--code"));
  double start = nowMs();
  while (nowMs() - start < seconds * 1000) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::lock_guard<std::mutex> l(mu);
  std::sort(lat.begin(), lat.end());
  Json r = Json::object();
  r.set("frames", double(frames)).set("keyframes", double(keys)).set("bytes", double(bytes)).set("audio", double(audio)).set("firstKey", firstKey);
  r.set("latMedian", lat.empty() ? Json() : Json(std::round(lat[lat.size() / 2] * 10) / 10));
  r.set("latP90", lat.empty() ? Json() : Json(std::round(lat[lat.size() * 9 / 10] * 10) / 10));
  line("RESULT " + r.dump());
  enginePtr = nullptr;
  chat.engine = nullptr;
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: ss-cli host|view …\n");
    return 2;
  }
  std::string mode = argv[1];
  if (mode == "host") return host(argc, argv);
  if (mode == "view") return view(argc, argv);
  if (mode == "ws-bench") {
    line("BENCH " + wsBench(openPlainWebSocket, arg(argc, argv, "--url"), std::atoi(arg(argc, argv, "--count", "240").c_str())).dump());
    return 0;
  }
  return 2;
}
