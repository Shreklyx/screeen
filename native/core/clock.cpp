#include "clock.h"

#include <chrono>

namespace ss {

namespace {
struct Anchor {
  std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::now();
  double epochMs = std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
};
}  // namespace

double nowMs() {
  static const Anchor anchor;
  return anchor.epochMs + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - anchor.steady).count();
}

}  // namespace ss
