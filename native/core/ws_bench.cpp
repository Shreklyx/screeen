#include "ws_bench.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "bytes.h"
#include "clock.h"
#include "net.h"

extern "C" {
#include "bearssl.h"
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
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
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
#endif

bool readAll(Sock s, uint8_t* p, size_t n) {
  while (n) {
    int r = int(recv(s, reinterpret_cast<char*>(p), int(n), 0));
    if (r <= 0) return false;
    p += r;
    n -= size_t(r);
  }
  return true;
}

bool writeAll(Sock s, const uint8_t* p, size_t n) {
  while (n) {
    int r = int(send(s, reinterpret_cast<const char*>(p), int(n), 0));
    if (r <= 0) return false;
    p += r;
    n -= size_t(r);
  }
  return true;
}

bool sendFrame(Sock s, uint8_t op, const uint8_t* p, size_t n) {
  Bytes f{uint8_t(0x80 | op)};
  if (n < 126) {
    f.push_back(uint8_t(n));
  } else if (n < 65536) {
    f.push_back(126);
    f.push_back(uint8_t(n >> 8));
    f.push_back(uint8_t(n));
  } else {
    f.push_back(127);
    for (int i = 7; i >= 0; i--) f.push_back(uint8_t(uint64_t(n) >> (8 * i)));
  }
  f.insert(f.end(), p, p + n);
  return writeAll(s, f.data(), f.size());
}

void serveEcho(Sock c, bool slowAcks) {
  int one = 1;
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef _WIN32
  if (slowAcks) {
    DWORD freq = 255, got = 0;
    WSAIoctl(c, SIO_TCP_SET_ACK_FREQUENCY, &freq, sizeof freq, nullptr, 0, &got, nullptr, nullptr);
  }
#else
  (void)slowAcks;
#endif
  std::string head;
  char ch;
  while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
    if (recv(c, &ch, 1, 0) != 1) return;
    head += ch;
  }
  std::string lower = head;
  for (char& x : lower) x = char(std::tolower(static_cast<unsigned char>(x)));
  size_t at = lower.find("sec-websocket-key:");
  if (at == std::string::npos) return;
  size_t from = head.find_first_not_of(" \t", at + 18), to = head.find("\r\n", at);
  std::string key = head.substr(from, to - from) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t digest[20];
  br_sha1_context sc;
  br_sha1_init(&sc);
  br_sha1_update(&sc, key.data(), key.size());
  br_sha1_out(&sc, digest);
  std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + base64Encode(digest, 20) + "\r\n\r\n";
  if (!writeAll(c, reinterpret_cast<const uint8_t*>(reply.data()), reply.size())) return;
  Bytes message;
  uint8_t messageOp = 0;
  for (;;) {
    uint8_t h[2];
    if (!readAll(c, h, 2)) return;
    uint8_t op = h[0] & 15;
    uint64_t len = h[1] & 127;
    if (len >= 126) {
      uint8_t ext[8];
      size_t k = len == 126 ? 2 : 8;
      if (!readAll(c, ext, k)) return;
      len = 0;
      for (size_t i = 0; i < k; i++) len = len << 8 | ext[i];
    }
    if (len > (64u << 20)) return;
    uint8_t mask[4] = {0, 0, 0, 0};
    if ((h[1] & 128) && !readAll(c, mask, 4)) return;
    Bytes payload(static_cast<size_t>(len));
    if (len && !readAll(c, payload.data(), payload.size())) return;
    for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask[i & 3];
    if (op == 8) {
      sendFrame(c, 8, payload.data(), std::min<size_t>(payload.size(), 2));
      return;
    }
    if (op == 9) {
      if (!sendFrame(c, 10, payload.data(), payload.size())) return;
      continue;
    }
    if (op == 10) continue;
    if (op != 0) {
      messageOp = op;
      message.clear();
    }
    message.insert(message.end(), payload.begin(), payload.end());
    if ((h[0] & 128) && !sendFrame(c, messageOp, message.data(), message.size())) return;
  }
}

Json summary(std::vector<double> ms) {
  Json j = Json::object();
  if (ms.empty()) return j;
  std::sort(ms.begin(), ms.end());
  auto at = [&](double q) { return ms[std::min(ms.size() - 1, size_t(q * double(ms.size())))]; };
  j.set("n", int(ms.size()));
  j.set("med", std::round(at(0.5) * 10) / 10);
  j.set("p90", std::round(at(0.9) * 10) / 10);
  j.set("max", std::round(ms.back() * 10) / 10);
  return j;
}

}  // namespace

Json wsBench(const WebSocketFactory& open, const std::string& url, int count, const std::function<Json()>& socket) {
  struct State {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false, closed = false;
    std::map<size_t, std::vector<double>> rtt; // by size
    int back = 0;
  };
  auto st = std::make_shared<State>();
  WebSocketEvents ev;
  ev.open = [st] {
    std::lock_guard<std::mutex> l(st->mu);
    st->open = true;
    st->cv.notify_all();
  };
  ev.closed = [st] {
    std::lock_guard<std::mutex> l(st->mu);
    st->closed = true;
    st->cv.notify_all();
  };
  ev.binary = [st](const uint8_t* d, size_t n) {
    if (n < 8) return;
    double sent;
    std::memcpy(&sent, d, 8);
    double ms = nowMs() - sent;
    std::lock_guard<std::mutex> l(st->mu);
    st->rtt[n].push_back(ms);
    st->back++;
    st->cv.notify_all();
  };
  std::unique_ptr<WebSocket> ws = open(url, ev);
  Json out = Json::object();
  {
    std::unique_lock<std::mutex> l(st->mu);
    st->cv.wait_for(l, std::chrono::seconds(10), [&] { return st->open || st->closed; });
    if (!st->open) {
      out.set("error", "no connection");
      return out;
    }
  }
  static const size_t kSizes[] = {1000, 8000, 30000, 120000};
  std::vector<uint8_t> buf(120000);
  for (size_t i = 0; i < buf.size(); i++) buf[i] = uint8_t(i * 131 + 7);
  double start = nowMs();
  for (int i = 0; i < count; i++) {
    double due = start + i * (1000.0 / 60);
    while (nowMs() < due) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    double t = nowMs();
    std::memcpy(buf.data(), &t, 8);
    ws->sendBinary(buf.data(), kSizes[i % 4]);
  }
  if (socket) out.set("socket", socket());
  std::vector<double> all;
  {
    std::unique_lock<std::mutex> l(st->mu);
    st->cv.wait_for(l, std::chrono::seconds(5), [&] { return st->back >= count || st->closed; });
    out.set("sent", count);
    out.set("back", st->back);
    for (auto& [size, ms] : st->rtt) {
      out.set(std::to_string(size), summary(ms));
      all.insert(all.end(), ms.begin(), ms.end());
    }
  }
  ws->close();
  out.set("all", summary(all));
  return out;
}

bool wsEchoServe(uint16_t port, bool slowAcks) {
  netInit();
  Sock l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == kNoSock) return false;
  int one = 1;
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(l, 8) != 0) {
    closeSock(l);
    return false;
  }
  for (;;) {
    Sock c = accept(l, nullptr, nullptr);
    if (c == kNoSock) continue;
    std::thread([c, slowAcks] {
      serveEcho(c, slowAcks);
      closeSock(c);
    }).detach();
  }
}

}  // namespace ss
