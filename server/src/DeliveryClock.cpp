#include "DeliveryClock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#ifndef _WIN32
#include <sys/time.h>
#endif

namespace tiny_mq {
#ifdef ENABLE_TESTS
namespace {
std::atomic<int64_t> testTime{-1};
}
void DeliveryClock::setTestTime(int64_t instant) { testTime.store(instant, std::memory_order_relaxed); }
bool DeliveryClock::isManual() { return testTime.load(std::memory_order_relaxed) >= 0; }
#endif

int64_t DeliveryClock::now() {
#ifdef ENABLE_TESTS
  const auto manual = testTime.load(std::memory_order_relaxed);
  if (manual >= 0) return manual;
#endif
#ifndef _WIN32
  // The delivery contract uses integer wall-clock milliseconds. Read them
  // directly without constructing a system_clock time point on each send.
  timeval wall;
  if (gettimeofday(&wall, nullptr) == 0) {
    return std::max<int64_t>(0, static_cast<int64_t>(wall.tv_sec) * 1000 + wall.tv_usec / 1000);
  }
#endif
  using namespace std::chrono;
  return std::max<int64_t>(0, duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

int64_t DeliveryClock::add(int64_t instant, int64_t duration) {
  constexpr auto max = std::numeric_limits<int64_t>::max();
  return max - instant < duration ? max : instant + duration;
}
}
