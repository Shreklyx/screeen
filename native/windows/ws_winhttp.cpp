// WebSocket client over WinHTTP (wss:// and ws://): one thread connects and reads, one writes.
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "websocket.h"

namespace ss {

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

class WinHttpSocket : public std::enable_shared_from_this<WinHttpSocket> {
 public:
  WinHttpSocket(std::string url, WebSocketEvents ev) : url_(std::move(url)), ev_(std::move(ev)) {}

  ~WinHttpSocket() {
    if (ws_) WinHttpCloseHandle(ws_);
    if (request_) WinHttpCloseHandle(request_);
    if (connect_) WinHttpCloseHandle(connect_);
    if (session_) WinHttpCloseHandle(session_);
  }

  void start() {
    auto self = shared_from_this();
    std::thread([self] { self->readLoop(); }).detach();
    std::thread([self] { self->writeLoop(); }).detach();
  }

  void send(bool text, const uint8_t* data, size_t n) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (closing_ || closeRequested_) return;
      queued_ += n;
      out_.push_back({text, std::vector<uint8_t>(data, data + n)});
    }
    cv_.notify_all();
  }

  size_t buffered() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queued_;
  }

  bool isOpen() const { return open_; }

  /** What is queued still goes (a goodbye to the server), then the socket closes. */
  void close() {
    bool first;
    {
      std::lock_guard<std::mutex> lock(mu_);
      first = !closeRequested_;
      closeRequested_ = true;
      ev_ = WebSocketEvents{}; // no callbacks after close()
      if (first && !open_) closing_ = true; // still connecting: the reader stops once the handshake is over
    }
    if (first) cv_.notify_all();
    std::lock_guard<std::recursive_mutex> gate(gate_); // nor one still running on the reader
  }

 private:
  struct Out {
    bool text;
    std::vector<uint8_t> data;
  };

  /** Calls fn with the events as they are now: nothing once close() has been called (and close()
   *  waits for a call already under way, so that after it nothing runs). */
  template <typename Fn>
  void fire(Fn fn) {
    std::lock_guard<std::recursive_mutex> gate(gate_);
    WebSocketEvents ev;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ev = ev_;
    }
    fn(ev);
  }

  void finish() {
    open_ = false;
    fire([this](const WebSocketEvents& ev) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        closing_ = true;
        ev_ = WebSocketEvents{};
      }
      cv_.notify_all();
      if (ev.closed) ev.closed();
    });
  }

  void readLoop() {
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    wchar_t host[256] = L"", path[1024] = L"", extra[1024] = L"";
    u.lpszHostName = host;
    u.dwHostNameLength = 255;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 1023;
    u.lpszExtraInfo = extra; // ?query
    u.dwExtraInfoLength = 1023;
    std::wstring wurl = widen(url_);
    // WinHTTP knows http(s): the same URL with ws → http
    if (wurl.rfind(L"wss://", 0) == 0) wurl = L"https://" + wurl.substr(6);
    else if (wurl.rfind(L"ws://", 0) == 0) wurl = L"http://" + wurl.substr(5);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) return finish();
    bool secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    session_ = WinHttpOpen(L"ScreenShare", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) session_ = WinHttpOpen(L"ScreenShare", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) return finish();
    WinHttpSetTimeouts(session_, 10000, 10000, 10000, 0); // no receive timeout: a quiet socket is fine
    connect_ = WinHttpConnect(session_, host, u.nPort, 0);
    if (!connect_) return finish();
    std::wstring target = std::wstring(path) + extra;
    request_ = WinHttpOpenRequest(connect_, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request_) return finish();
    if (!WinHttpSetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return finish();
    if (!WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request_, nullptr)) return finish();
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) return finish();
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request_, 0);
    if (!ws) return finish();
    {
      std::lock_guard<std::mutex> lock(mu_);
      ws_ = ws;
      if (closing_ || closeRequested_) {
        WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        return finish();
      }
    }
    WinHttpCloseHandle(std::exchange(request_, nullptr));
    open_ = true;
    cv_.notify_all();
    fire([](const WebSocketEvents& ev) {
      if (ev.open) ev.open();
    });
    std::vector<uint8_t> message;
    std::vector<uint8_t> buf(64 * 1024);
    for (;;) {
      DWORD read = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
      if (WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &read, &type) != NO_ERROR) break;
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
      message.insert(message.end(), buf.begin(), buf.begin() + read);
      if (message.size() > (64u << 20)) break;
      if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
      fire([&](const WebSocketEvents& ev) {
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
          if (ev.text) ev.text(std::string(message.begin(), message.end()));
        } else if (ev.binary) {
          ev.binary(message.data(), message.size());
        }
      });
      message.clear();
    }
    finish();
  }

  void writeLoop() {
    for (;;) {
      Out o;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] { return closing_ || (open_ && (!out_.empty() || closeRequested_)); });
        if (closing_) return;
        if (out_.empty()) {
          // close() asked and everything has gone: ends the reader's receive too
          closing_ = true;
          HINTERNET ws = ws_;
          lock.unlock();
          WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
          return;
        }
        o = std::move(out_.front());
        out_.pop_front();
      }
      DWORD r = WinHttpWebSocketSend(ws_, o.text ? WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                     o.data.empty() ? nullptr : o.data.data(), DWORD(o.data.size()));
      {
        std::lock_guard<std::mutex> lock(mu_);
        queued_ -= std::min(queued_, o.data.size());
      }
      if (r != NO_ERROR) return;
    }
  }

  std::string url_;
  WebSocketEvents ev_;
  mutable std::mutex mu_;
  std::recursive_mutex gate_; // held while a callback runs
  std::condition_variable cv_;
  std::deque<Out> out_;
  size_t queued_ = 0;
  std::atomic<bool> open_{false};
  bool closing_ = false, closeRequested_ = false;
  HINTERNET session_ = nullptr, connect_ = nullptr, request_ = nullptr, ws_ = nullptr;
};

class Handle : public WebSocket {
 public:
  explicit Handle(std::shared_ptr<WinHttpSocket> s) : s_(std::move(s)) {}
  ~Handle() override { s_->close(); }
  void sendText(const std::string& t) override { s_->send(true, reinterpret_cast<const uint8_t*>(t.data()), t.size()); }
  void sendBinary(const uint8_t* d, size_t n) override { s_->send(false, d, n); }
  size_t bufferedAmount() const override { return s_->buffered(); }
  bool isOpen() const override { return s_->isOpen(); }
  void close() override { s_->close(); }

 private:
  std::shared_ptr<WinHttpSocket> s_;
};

}  // namespace

std::unique_ptr<WebSocket> openWinHttpWebSocket(const std::string& url, WebSocketEvents events) {
  auto s = std::make_shared<WinHttpSocket>(url, std::move(events));
  s->start();
  return std::unique_ptr<WebSocket>(new Handle(s));
}

}  // namespace ss
