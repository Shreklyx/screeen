// Screen Share native core – the session engine: the signaling connection to the server, the
// sharer's and the viewer's logic in direct mode (the C++ form of public/app.js's DIRECT MODE),
// on top of DirectEndpoint. The platform app gives it encoded frames (sharer) or takes the
// received ones (viewer), and shows what the events say.
//
// Thread model: the engine runs on its own Loop thread. Every public call may come from any thread
// (it is posted onto the loop); every callback comes from the loop thread – the app moves what it
// needs onto its UI / decoder threads.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "direct.h"
#include "json.h"
#include "net.h"
#include "websocket.h"

namespace ss {

struct EngineConfig {
  std::string server = "https://mozizas.hu"; // http(s)://host[:port]
  WebSocketFactory webSocket;                // wss:// (and ws://) connections
  std::string encoderName = "hardveres";     // shown to the viewers ("NVENC", "VideoToolbox", …)
  bool hardware = true;
  // tests / troubleshooting
  bool forceRelay = false, forceTurn = false, forceTcp = false;
};

struct EngineCallbacks {
  /**
   * What the UI shows, as JSON: {"type": …}
   *   status       {text, kind: ok|warn|error|""}         the connection to the server
   *   hosted       {code, link}                            the room is ready
   *   host-error   {code: auth|rate|full|offline|…}        sharing could not start
   *   viewers      {list: [{id, n, state, path, fps, kbps, rtt, lat, why}], count}
   *   joined       {code}  view-state {text, picture}      the viewer's overlay text
   *   view-stats   {fps, kbps, rtt, lat, path, loss, encoder}
   *   closed       {reason}                                the room is over
   *   the room's chat (both sides; viewers' list entries have their names too):
   *   name         {name}                                  the name the room knows us by
   *   chat-history {list: [chat…], supported}              what was said so far (in the room again: all of it);
   *                                                         supported: false – the server has no chat yet
   *   chat         {id, from, name, text, at, host, mine}  a message (ours too: then it was delivered)
   *   chat-limit {}  chat-offline {}                       too fast / not in a room right now: not sent
   */
  std::function<void(const Json& event)> event;
  // sharer
  std::function<void(bool on)> streaming;   // start / stop the encoders (a viewer is connected or not)
  std::function<void()> keyframe;           // the encoder's next frame must be a keyframe
  std::function<void(double bps)> bitrate;  // new encoder bitrate
  // viewer
  std::function<void(const MediaMeta& meta, const uint8_t* data, size_t n)> media; // H.264 Annex B / Opus
};

class Engine {
 public:
  Engine(EngineConfig config, EngineCallbacks callbacks);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /** Sharer: opens a room with the sharing password (or takes back the last one). */
  void host(const std::string& password);
  /** Sharer: the picture is paused (true) or goes on (false); viewers see "szünetel". */
  void setLive(bool live);
  void stopHosting();
  /** Sharer: the encoder's target, to start with and as an upper limit (kbit/s; 0 = automatic). */
  void setTargetKbps(double kbps);
  /** Sharer: an encoded H.264 frame (Annex B) – any thread. */
  void sendVideo(const uint8_t* data, size_t n, bool key, double captureMs, double tsUs);
  /** Sharer: one Opus packet of channel pair `pair` (of `channels`) – any thread. */
  void sendAudio(int pair, int channels, const uint8_t* data, size_t n, double tsUs);
  /** Sharer: what the encoder measured, for the viewers' statistics. */
  void setEncoderStats(double encodeMs, int width, int height);
  /** Sharer: which encoder works ("NVENC", "Intel Quick Sync", …) and whether it is the GPU. */
  void setEncoder(const std::string& name, bool hardware);

  /** Both: the name shown with our chat messages ("" = the server's: Megosztó / Néző N). */
  void setName(const std::string& name);
  /** Both: a chat message to everyone in the room. */
  void sendChat(const std::string& text);

  /** Viewer: joins a room by its code. */
  void join(const std::string& code);
  void leave();
  /** Viewer: the decoder lost track – a keyframe, please. */
  void requestKeyframe();
  /** Viewer: the picture's latency as measured where it is shown (ms). */
  void reportLatency(double ms, double fps);
  /** Viewer: the sharer's clock minus ours (ms; NaN until known) – latency = nowMs() + offset − capture. */
  double clockOffset() const;

  std::string siteUrl() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
