// Plain ws:// client (RFC 6455): a thread that connects and reads, one that writes.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "bytes.h"
#include "crypto.h"
#include "net.h"
#include "websocket.h"

extern "C" {
#include "bearssl.h"
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ss {

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
void shutdownSock(Sock s) { shutdown(s, SD_BOTH); }
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
void shutdownSock(Sock s) { shutdown(s, SHUT_RDWR); }
#endif

class PlainWebSocket : public WebSocket, public std::enable_shared_from_this<PlainWebSocket> {
 public:
  PlainWebSocket(std::string url, WebSocketEvents ev) : url_(std::move(url)), ev_(std::move(ev)) {}

  void start() {
    // the threads keep the object alive (it may end on one of them): never joined, detached at once
    auto self = shared_from_this();
    std::thread([self] { self->readLoop(); }).detach();
    std::thread([self] { self->writeLoop(); }).detach();
  }

  ~PlainWebSocket() override {
    if (sock_ != kNoSock) closeSock(sock_);
  }

  void sendText(const std::string& text) override { enqueue(1, reinterpret_cast<const uint8_t*>(text.data()), text.size()); }
  void sendBinary(const uint8_t* data, size_t n) override { enqueue(2, data, n); }
  size_t bufferedAmount() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return queued_;
  }
  bool isOpen() const override { return open_; }

  void close() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (closing_) return;
      closing_ = true;
      ev_ = WebSocketEvents{}; // no more callbacks after close()
    }
    cv_.notify_all();
    if (sock_ != kNoSock) shutdownSock(sock_);
  }

 private:
  void enqueue(uint8_t opcode, const uint8_t* data, size_t n) {
    Bytes frame;
    frame.reserve(n + 14);
    frame.push_back(uint8_t(0x80 | opcode));
    if (n < 126) frame.push_back(uint8_t(0x80 | n));
    else if (n < 65536) { frame.push_back(0x80 | 126); frame.push_back(uint8_t(n >> 8)); frame.push_back(uint8_t(n)); }
    else { frame.push_back(0x80 | 127); for (int i = 7; i >= 0; i--) frame.push_back(uint8_t(uint64_t(n) >> (8 * i))); }
    uint8_t mask[4];
    randomBytes(mask, 4);
    frame.insert(frame.end(), mask, mask + 4);
    size_t at = frame.size();
    frame.resize(at + n);
    for (size_t i = 0; i < n; i++) frame[at + i] = data[i] ^ mask[i & 3];
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (closing_) return;
      queued_ += n;
      out_.push_back(std::move(frame));
    }
    cv_.notify_all();
  }

  bool sendAll(const uint8_t* p, size_t n) {
    while (n) {
      int r = int(send(sock_, reinterpret_cast<const char*>(p), int(n > (1 << 20) ? 1 << 20 : n), 0));
      if (r <= 0) return false;
      p += r;
      n -= size_t(r);
    }
    return true;
  }

  bool recvAll(uint8_t* p, size_t n) {
    while (n) {
      int r = int(recv(sock_, reinterpret_cast<char*>(p), int(n), 0));
      if (r <= 0) return false;
      p += r;
      n -= size_t(r);
    }
    return true;
  }

  void writeLoop() {
    for (;;) {
      Bytes frame;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return closing_ || (open_ && !out_.empty()); });
        if (closing_) return;
        frame = std::move(out_.front());
        out_.pop_front();
      }
      bool ok = sendAll(frame.data(), frame.size());
      {
        std::lock_guard<std::mutex> lock(mu_);
        queued_ -= std::min(queued_, payloadSize(frame)); // counted as the payload, as in browsers
      }
      if (!ok) return;
    }
  }

  static size_t payloadSize(const Bytes& f) {
    size_t len = f[1] & 127;
    if (len == 126) return size_t(f[2]) << 8 | f[3];
    if (len == 127) {
      uint64_t v = 0;
      for (int i = 0; i < 8; i++) v = v << 8 | f[2 + i];
      return size_t(v);
    }
    return len;
  }

  void finish() {
    bool wasOpen = open_;
    open_ = false;
    std::function<void()> closed;
    {
      std::lock_guard<std::mutex> lock(mu_);
      closed = ev_.closed;
      closing_ = true;
    }
    cv_.notify_all();
    if (sock_ != kNoSock) shutdownSock(sock_); // closed when both threads are done (destructor)
    (void)wasOpen;
    if (closed) closed();
  }

  void readLoop() {
    // ws://host[:port]/path
    std::string rest = url_.substr(url_.find("://") + 3);
    std::string hostport = rest.substr(0, rest.find('/'));
    std::string path = rest.find('/') == std::string::npos ? "/" : rest.substr(rest.find('/'));
    std::string host = hostport;
    std::string port = "80";
    if (hostport.find(':') != std::string::npos) {
      host = hostport.substr(0, hostport.find(':'));
      port = hostport.substr(hostport.find(':') + 1);
    }
    netInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return finish();
    Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSock || connect(s, res->ai_addr, int(res->ai_addrlen)) != 0) {
      freeaddrinfo(res);
      if (s != kNoSock) closeSock(s);
      return finish();
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    sock_ = s;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (closing_) return finish();
    }
    uint8_t keyRaw[16];
    randomBytes(keyRaw, 16);
    std::string key = base64Encode(keyRaw, 16);
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ScreenShare\r\n\r\n";
    if (!sendAll(reinterpret_cast<const uint8_t*>(req.data()), req.size())) return finish();
    std::string head;
    char c;
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
      if (recv(s, &c, 1, 0) != 1) return finish();
      head += c;
    }
    std::string accept = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    br_sha1_context sc;
    br_sha1_init(&sc);
    br_sha1_update(&sc, accept.data(), accept.size());
    br_sha1_out(&sc, digest);
    if (head.compare(0, 12, "HTTP/1.1 101") != 0 || head.find(base64Encode(digest, 20)) == std::string::npos) return finish();
    open_ = true;
    cv_.notify_all();
    {
      std::function<void()> opened;
      {
        std::lock_guard<std::mutex> lock(mu_);
        opened = ev_.open;
      }
      if (opened) opened();
    }
    Bytes message;
    uint8_t messageOp = 0;
    for (;;) {
      uint8_t h[2];
      if (!recvAll(h, 2)) break;
      uint8_t op = h[0] & 15;
      bool fin = h[0] & 0x80;
      uint64_t len = h[1] & 127;
      if (len == 126) {
        uint8_t e[2];
        if (!recvAll(e, 2)) break;
        len = uint64_t(e[0]) << 8 | e[1];
      } else if (len == 127) {
        uint8_t e[8];
        if (!recvAll(e, 8)) break;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | e[i];
      }
      if (len > (64u << 20)) break;
      Bytes payload(static_cast<size_t>(len));
      if (len && !recvAll(payload.data(), payload.size())) break;
      if (op == 8) break; // close
      if (op == 9) { enqueue(10, payload.data(), payload.size()); continue; } // ping → pong
      if (op == 10) continue;
      if (op == 1 || op == 2) { message = std::move(payload); messageOp = op; }
      else if (op == 0) message.insert(message.end(), payload.begin(), payload.end());
      else continue;
      if (!fin) continue;
      WebSocketEvents ev;
      {
        std::lock_guard<std::mutex> lock(mu_);
        ev = ev_;
      }
      if (messageOp == 1 && ev.text) ev.text(std::string(message.begin(), message.end()));
      else if (messageOp == 2 && ev.binary) ev.binary(message.data(), message.size());
      message.clear();
    }
    finish();
  }

  std::string url_;
  WebSocketEvents ev_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Bytes> out_;
  size_t queued_ = 0;
  std::atomic<bool> open_{false};
  bool closing_ = false;
  std::atomic<Sock> sock_{kNoSock};
};

/** Keeps the shared object alive for its threads; close() lets them finish on their own. */
class PlainHandle : public WebSocket {
 public:
  explicit PlainHandle(std::shared_ptr<PlainWebSocket> ws) : ws_(std::move(ws)) {}
  ~PlainHandle() override { ws_->close(); }
  void sendText(const std::string& t) override { ws_->sendText(t); }
  void sendBinary(const uint8_t* d, size_t n) override { ws_->sendBinary(d, n); }
  size_t bufferedAmount() const override { return ws_->bufferedAmount(); }
  bool isOpen() const override { return ws_->isOpen(); }
  void close() override { ws_->close(); }

 private:
  std::shared_ptr<PlainWebSocket> ws_;
};

}  // namespace

std::unique_ptr<WebSocket> openPlainWebSocket(const std::string& url, WebSocketEvents events) {
  auto ws = std::make_shared<PlainWebSocket>(url, std::move(events));
  ws->start();
  return std::unique_ptr<WebSocket>(new PlainHandle(ws));
}

}  // namespace ss
