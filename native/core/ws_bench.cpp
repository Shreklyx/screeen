#include "ws_bench.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "clock.h"

namespace ss {

namespace {

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

Json wsBench(const WebSocketFactory& open, const std::string& url, int count) {
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

}  // namespace ss
