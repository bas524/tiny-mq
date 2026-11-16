#ifndef TINY_MQ_DELIVERY_CLOCK_H
#define TINY_MQ_DELIVERY_CLOCK_H

#include <cstdint>

namespace tiny_mq {
// Internal wall clock for delivery headers and scheduling. Receive budgets
// use steady_clock directly.
class DeliveryClock {
 public:
  static int64_t now();
  static int64_t add(int64_t instant, int64_t duration);

#ifdef ENABLE_TESTS
  static void setTestTime(int64_t instant);
  static bool isManual();
#endif
};
}
#endif
