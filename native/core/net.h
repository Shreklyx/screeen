// Screen Share native core – UDP sockets, addresses and the event loop the transport runs on.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ss {

#ifdef _WIN32
using SocketHandle = uintptr_t;
#else
using SocketHandle = int;
#endif

void netInit();
/** This computer's IPv4 addresses (not loopback). */
std::vector<std::string> localIPv4();
/** Blocking DNS lookup (IPv4); empty on failure. */
std::string resolveIPv4(const std::string& host);
bool isIPv4(const std::string& s);

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  /** Binds to a free port on all addresses (or loopback only). */
  bool open(bool loopbackOnly = false);
  void close();
  bool valid() const;
  SocketHandle handle() const { return sock_; }
  uint16_t port() const { return port_; }
  void sendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t n);
  /** -1 when nothing is waiting. */
  int recvFrom(uint8_t* buf, size_t cap, std::string& ip, uint16_t& port);

 private:
  SocketHandle sock_ = SocketHandle(-1);
  uint16_t port_ = 0;
};

/**
 * One thread that waits on sockets and timers; other threads hand it work with post().
 * Everything the transport and the session engine do happens on this thread.
 */
class Loop {
 public:
  Loop();
  ~Loop();
  void start();
  void stop();
  void post(std::function<void()> fn);
  bool onLoopThread() const { return std::this_thread::get_id() == threadId_; }
  /** Calls fn after ms (repeating every ms when repeat); returns an id for cancel(). */
  int setTimer(double ms, std::function<void()> fn, bool repeat = false);
  void cancel(int id);
  void watch(SocketHandle s, std::function<void()> readable);
  void unwatch(SocketHandle s);

 private:
  void run();
  void wake();
  struct Timer {
    double due;
    double every;
    std::function<void()> fn;
  };
  std::thread thread_;
  std::thread::id threadId_;
  std::atomic<bool> running_{false};
  std::mutex mu_;
  std::vector<std::function<void()>> posted_;
  std::map<int, Timer> timers_;
  int nextTimer_ = 1;
  std::map<SocketHandle, std::function<void()>> watched_;
  UdpSocket wakeSock_;
};

}  // namespace ss
