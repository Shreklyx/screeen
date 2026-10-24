// Sound on Windows. Sharer: everything the computer plays (WASAPI loopback, 5.1 / 7.1 too), as
// Opus in 5 ms packets, one encoder per channel pair (what the viewers' decoders expect). Viewer:
// the packets are decoded and placed by their timestamps; a small buffer that grows only when the
// network makes it run dry (10–100 ms), played through WASAPI (downmixed for fewer speakers).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ss {

class AudioCapture {
 public:
  /** pair, channels (all of them), Opus packet, timestamp (µs) – from the capture thread. */
  using Packet = std::function<void(int pair, int channels, const uint8_t* data, size_t n, double tsUs)>;
  explicit AudioCapture(Packet packet);
  ~AudioCapture();
  /** "" or why not. */
  std::string start();
  void stop();
  /** "7.1", "5.1", "sztereó" – once started. */
  std::string layout() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class AudioPlayer {
 public:
  AudioPlayer();
  ~AudioPlayer();
  /** One Opus packet: sub = pair | channels << 4 (as sent), ts in µs – any thread. */
  void push(int sub, const uint8_t* data, size_t n, double tsUs);
  void setVolume(float volume); // 0..1
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ss
