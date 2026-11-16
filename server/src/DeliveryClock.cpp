#include "DeliveryClock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

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
  using namespace std::chrono;

  return std::max<int64_t>(0, duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

int64_t DeliveryClock::add(int64_t instant, int64_t duration) {
  constexpr auto max = std::numeric_limits<int64_t>::max();
  return max - instant < duration ? max : instant + duration;
}
}
