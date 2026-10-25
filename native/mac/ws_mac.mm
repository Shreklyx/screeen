// WebSocket client on macOS: NSURLSessionWebSocketTask (wss:// and ws://).
#import <Foundation/Foundation.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "websocket.h"

namespace ss {

namespace {

class MacWebSocket : public WebSocket, public std::enable_shared_from_this<MacWebSocket> {
 public:
  MacWebSocket(const std::string& url, WebSocketEvents ev) : ev_(std::move(ev)) {
    @autoreleasepool {
      NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
      NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
      cfg.timeoutIntervalForRequest = 15;
      cfg.timeoutIntervalForResource = 0x7fffffff;
      session_ = [NSURLSession sessionWithConfiguration:cfg];
      task_ = [session_ webSocketTaskWithURL:u];
      task_.maximumMessageSize = 64 << 20;
    }
  }

  ~MacWebSocket() override { close(); }

  void start() {
    std::weak_ptr<MacWebSocket> weak = shared_from_this();
    [task_ resume];
    // NSURLSession has no "open" callback for tasks without a delegate: a ping that comes back says it
    [task_ sendPingWithPongReceiveHandler:^(NSError* error) {
      auto self = weak.lock();
      if (!self) return;
      if (error) return self->finish();
      self->open_ = true;
      self->fire([](const WebSocketEvents& ev) {
        if (ev.open) ev.open();
      });
    }];
    receive();
  }

  void sendText(const std::string& text) override {
    send([[NSURLSessionWebSocketMessage alloc] initWithString:[[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding]], text.size());
  }

  void sendBinary(const uint8_t* data, size_t n) override {
    send([[NSURLSessionWebSocketMessage alloc] initWithData:[NSData dataWithBytes:data length:n]], n);
  }

  size_t bufferedAmount() const override { return queued_; }
  bool isOpen() const override { return open_; }

  void close() override {
    bool first;
    {
      std::lock_guard<std::mutex> l(mu_);
      first = !closed_;
      closed_ = true;
      ev_ = WebSocketEvents{};
    }
    {
      std::lock_guard<std::recursive_mutex> gate(gate_); // nor one still running
    }
    if (!first) return;
    // what was sent still goes: cancel after the queue is empty (or a moment)
    NSURLSessionWebSocketTask* task = task_;
    NSURLSession* session = session_;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(queued_ > 0 ? 200 : 0) * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      [task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
      [session finishTasksAndInvalidate];
    });
  }

 private:
  /** Calls fn with the events as they are now: nothing once close() has been called (and close()
   *  waits for a call already under way, so that after it nothing runs). */
  template <typename Fn>
  void fire(Fn fn) {
    std::lock_guard<std::recursive_mutex> gate(gate_);
    WebSocketEvents ev;
    {
      std::lock_guard<std::mutex> l(mu_);
      ev = ev_;
    }
    fn(ev);
  }

  void send(NSURLSessionWebSocketMessage* m, size_t n) {
    queued_ += n;
    std::weak_ptr<MacWebSocket> weak = shared_from_this();
    [task_ sendMessage:m
        completionHandler:^(NSError* error) {
          auto self = weak.lock();
          if (!self) return;
          self->queued_ -= n;
          if (error) self->finish();
        }];
  }

  void receive() {
    std::weak_ptr<MacWebSocket> weak = shared_from_this();
    [task_ receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* m, NSError* error) {
      auto self = weak.lock();
      if (!self) return;
      if (error || !m) return self->finish();
      self->fire([m](const WebSocketEvents& ev) {
        if (m.type == NSURLSessionWebSocketMessageTypeString) {
          if (ev.text) ev.text(std::string(m.string.UTF8String ?: ""));
        } else if (ev.binary) {
          ev.binary(static_cast<const uint8_t*>(m.data.bytes), m.data.length);
        }
      });
      self->receive();
    }];
  }

  void finish() {
    std::lock_guard<std::recursive_mutex> gate(gate_);
    std::function<void()> closed;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (finished_) return;
      finished_ = true;
      closed = ev_.closed;
      ev_ = WebSocketEvents{};
    }
    open_ = false;
    [task_ cancel];
    if (closed) closed();
  }

  WebSocketEvents ev_;
  std::mutex mu_;
  std::recursive_mutex gate_; // held while a callback runs
  bool closed_ = false, finished_ = false;
  std::atomic<bool> open_{false};
  std::atomic<size_t> queued_{0};
  NSURLSession* session_ = nil;
  NSURLSessionWebSocketTask* task_ = nil;
};

class Handle : public WebSocket {
 public:
  explicit Handle(std::shared_ptr<MacWebSocket> s) : s_(std::move(s)) {}
  ~Handle() override { s_->close(); }
  void sendText(const std::string& t) override { s_->sendText(t); }
  void sendBinary(const uint8_t* d, size_t n) override { s_->sendBinary(d, n); }
  size_t bufferedAmount() const override { return s_->bufferedAmount(); }
  bool isOpen() const override { return s_->isOpen(); }
  void close() override { s_->close(); }

 private:
  std::shared_ptr<MacWebSocket> s_;
};

}  // namespace

std::unique_ptr<WebSocket> openMacWebSocket(const std::string& url, WebSocketEvents events) {
  auto s = std::make_shared<MacWebSocket>(url, std::move(events));
  s->start();
  return std::unique_ptr<WebSocket>(new Handle(s));
}

}  // namespace ss
