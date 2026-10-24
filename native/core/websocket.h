// Screen Share native core – WebSocket client interface. The signaling connection (wss://…/ws)
// and direct mode's WebSocket path (wss://…/direct) use it. Each platform gives a TLS one
// (Windows: WinHTTP, macOS: NSURLSession); ws_plain.cpp speaks plain ws:// (tests, a local server).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ss {

struct WebSocketEvents {
  // called on any thread; the engine moves them onto its loop
  std::function<void()> open;
  std::function<void(std::string)> text;
  std::function<void(const uint8_t*, size_t)> binary;
  std::function<void()> closed; // also when it never opened
};

class WebSocket {
 public:
  virtual ~WebSocket() = default;
  virtual void sendText(const std::string& text) = 0;
  virtual void sendBinary(const uint8_t* data, size_t n) = 0;
  /** Bytes handed to send() that have not left yet (as the browsers' bufferedAmount). */
  virtual size_t bufferedAmount() const = 0;
  virtual bool isOpen() const = 0;
  virtual void close() = 0;
};

using WebSocketFactory = std::function<std::unique_ptr<WebSocket>(const std::string& url, WebSocketEvents events)>;

/** ws:// only (no TLS). */
std::unique_ptr<WebSocket> openPlainWebSocket(const std::string& url, WebSocketEvents events);

}  // namespace ss
