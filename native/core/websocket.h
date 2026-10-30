// Screen Share native core – WebSocket client interface. The signaling connection (wss://…/ws)
// and direct mode's WebSocket path (wss://…/direct) use it. Each platform gives a TLS one
// (Windows: WinHTTP – and for /direct our own over the system's TLS, see TlsChannel; macOS:
// NSURLSession); ws_plain.cpp is our own: plain ws:// (tests, a local server), wss:// with a TlsChannel.
#pragma once

#include <cstddef>
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
  /** After it returns no callback runs any more (one under way on another thread is waited for). */
  virtual void close() = 0;
};

using WebSocketFactory = std::function<std::unique_ptr<WebSocket>(const std::string& url, WebSocketEvents events)>;

/** TLS over a connected TCP socket (wss://): the platform's own, its certificate store and checks. The
 *  reading and the writing thread use it at the same time (one each). */
class TlsChannel {
 public:
  virtual ~TlsChannel() = default;
  /** Right after the TCP connection: the handshake with `host` (its certificate checked). */
  virtual bool handshake(uintptr_t sock, const std::string& host) = 0;
  /** All of it, encrypted. */
  virtual bool send(uintptr_t sock, const uint8_t* data, size_t n) = 0;
  /** Some of what came, decrypted: > 0 bytes, 0 closed, < 0 an error. */
  virtual int recv(uintptr_t sock, uint8_t* data, size_t n) = 0;
};
using TlsFactory = std::function<std::unique_ptr<TlsChannel>()>;

/** Our own connection – Nagle's algorithm off, so the end of a message goes at once: ws://, and wss://
 *  with `tls`. */
std::unique_ptr<WebSocket> openOwnWebSocket(const std::string& url, WebSocketEvents events, TlsFactory tls);

/** ws:// only (no TLS). */
std::unique_ptr<WebSocket> openPlainWebSocket(const std::string& url, WebSocketEvents events);

}  // namespace ss
