#include "net.h"

#include <algorithm>
#include <cstring>

#include "clock.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ss {

#ifdef _WIN32
static const SocketHandle kInvalid = SocketHandle(INVALID_SOCKET);
static void closeSock(SocketHandle s) { closesocket(SOCKET(s)); }
#else
static const SocketHandle kInvalid = -1;
static void closeSock(SocketHandle s) { ::close(s); }
#endif

void netInit() {
#ifdef _WIN32
  static bool done = [] {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    return true;
  }();
  (void)done;
#else
  signal(SIGPIPE, SIG_IGN); // a closed TCP peer: an error from send(), not the end of the process
#endif
}

bool isIPv4(const std::string& s) {
  in_addr a;
  return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

std::vector<std::string> localIPv4() {
  std::vector<std::string> out;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR) return out;
  }
  for (auto* a = list; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
      char ip[INET_ADDRSTRLEN];
      if (sin->sin_family == AF_INET && inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) out.emplace_back(ip);
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (ifaddrs* a = list; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    auto* sin = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) continue;
    if (std::strncmp(ip, "127.", 4) == 0) continue;
    out.emplace_back(ip);
  }
  freeifaddrs(list);
#endif
  return out;
}

std::string resolveIPv4(const std::string& host) {
  netInit();
  if (isIPv4(host)) return host;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return "";
  char ip[INET_ADDRSTRLEN] = "";
  inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, ip, sizeof ip);
  freeaddrinfo(res);
  return ip;
}

/* ------------------------------------------------ UDP */

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(bool loopbackOnly) {
  netInit();
  close();
  SocketHandle s = SocketHandle(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (s == kInvalid) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
  a.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    closeSock(s);
    return false;
  }
  socklen_t len = sizeof a;
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  // big frames arrive as bursts: room for a 4K keyframe or two
  int rcv = 8 << 20, snd = 4 << 20;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof rcv);
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&snd), sizeof snd);
#ifdef _WIN32
  u_long nb = 1;
  ioctlsocket(SOCKET(s), FIONBIO, &nb);
  // an ICMP "port unreachable" must not make the next recvfrom fail (Windows reports it)
  BOOL off = FALSE;
  DWORD bytes = 0;
  WSAIoctl(SOCKET(s), _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
  sock_ = s;
  return true;
}

void UdpSocket::close() {
  if (sock_ != kInvalid) closeSock(sock_);
  sock_ = kInvalid;
}

bool UdpSocket::valid() const { return sock_ != kInvalid; }

void UdpSocket::sendTo(const std::string& ip, uint16_t port, const uint8_t* data, size_t n) {
  if (sock_ == kInvalid) return;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return;
  sendto(sock_, reinterpret_cast<const char*>(data), int(n), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
}

int UdpSocket::recvFrom(uint8_t* buf, size_t cap, std::string& ip, uint16_t& port) {
  if (sock_ == kInvalid) return -1;
  sockaddr_in a{};
  socklen_t len = sizeof a;
  int n = int(recvfrom(sock_, reinterpret_cast<char*>(buf), int(cap), 0, reinterpret_cast<sockaddr*>(&a), &len));
  if (n < 0) return -1;
  char s[INET_ADDRSTRLEN] = "";
  inet_ntop(AF_INET, &a.sin_addr, s, sizeof s);
  ip = s;
  port = ntohs(a.sin_port);
  return n;
}

/* ------------------------------------------------ the loop */

Loop::Loop() = default;

Loop::~Loop() { stop(); }

void Loop::start() {
  if (running_) return;
  wakeSock_.open(true);
  running_ = true;
  thread_ = std::thread([this] { run(); });
  threadId_ = thread_.get_id();
}

void Loop::stop() {
  if (!running_) return;
  post([this] { running_ = false; });
  if (thread_.joinable()) {
    if (onLoopThread()) thread_.detach();
    else thread_.join();
  }
  wakeSock_.close();
}

void Loop::wake() {
  uint8_t b = 1;
  wakeSock_.sendTo("127.0.0.1", wakeSock_.port(), &b, 1);
}

void Loop::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    posted_.push_back(std::move(fn));
  }
  if (!onLoopThread()) wake();
}

int Loop::setTimer(double ms, std::function<void()> fn, bool repeat) {
  int id = nextTimer_++;
  timers_[id] = Timer{nowMs() + ms, repeat ? std::max(1.0, ms) : 0, std::move(fn)};
  return id;
}

void Loop::cancel(int id) { timers_.erase(id); }

void Loop::watch(SocketHandle s, std::function<void()> readable) { watched_[s] = std::move(readable); }

void Loop::unwatch(SocketHandle s) { watched_.erase(s); }

void Loop::run() {
  threadId_ = std::this_thread::get_id();
  std::vector<uint8_t> drain(64);
  while (running_) {
    // what other threads handed over
    std::vector<std::function<void()>> work;
    {
      std::lock_guard<std::mutex> lock(mu_);
      work.swap(posted_);
    }
    for (auto& fn : work) {
      fn();
      if (!running_) return;
    }
    // timers that are due
    double now = nowMs();
    std::vector<int> due;
    for (auto& kv : timers_) if (kv.second.due <= now) due.push_back(kv.first);
    for (int id : due) {
      auto it = timers_.find(id);
      if (it == timers_.end()) continue;
      std::function<void()> fn = it->second.fn;
      if (it->second.every > 0) it->second.due = std::max(it->second.due + it->second.every, now);
      else timers_.erase(it);
      fn();
      if (!running_) return;
    }
    // wait for a socket, a timer or a post
    double next = now + 50;
    for (auto& kv : timers_) next = std::min(next, kv.second.due);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!posted_.empty()) next = now;
    }
    double waitMs = std::max(0.0, next - nowMs());
    fd_set set;
    FD_ZERO(&set);
    SocketHandle maxFd = wakeSock_.handle();
    FD_SET(wakeSock_.handle(), &set);
    for (auto& kv : watched_) {
      FD_SET(kv.first, &set);
      maxFd = std::max(maxFd, kv.first);
    }
    timeval tv;
    tv.tv_sec = long(waitMs / 1000);
    tv.tv_usec = long((waitMs - tv.tv_sec * 1000.0) * 1000);
    int r = select(int(maxFd + 1), &set, nullptr, nullptr, &tv);
    if (r <= 0) continue;
    if (FD_ISSET(wakeSock_.handle(), &set)) {
      std::string ip;
      uint16_t port;
      while (wakeSock_.recvFrom(drain.data(), drain.size(), ip, port) >= 0) {}
    }
    std::vector<std::pair<SocketHandle, std::function<void()>>> ready;
    for (auto& kv : watched_) if (FD_ISSET(kv.first, &set)) ready.push_back(kv);
    for (auto& kv : ready) {
      if (watched_.count(kv.first)) kv.second();
      if (!running_) return;
    }
  }
}

}  // namespace ss
