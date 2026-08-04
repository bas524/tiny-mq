//
// DeliveryScheduler — per-Destination timer for JMS 2.0 delivery delay (§7.8, spec 13).
//

#ifndef TINY_MQ__DELIVERY_SCHEDULER_H_
#define TINY_MQ__DELIVERY_SCHEDULER_H_

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <Poco/Logger.h>
#include "Message.h"

namespace tiny_mq {

// Holds messages whose jmsHeaders.deliveryTime is still in the future, keeping them
// out of the visible QueueT until due. One instance per Destination (not a single
// global timer) so a destination with heavy delayed traffic cannot delay firing
// on any other destination — see Destination::_scheduler and ADR-0007
// (arch/0007-delivery-scheduler-per-destination-lazy-thread.md) for the
// per-destination-vs-global trade-off, why it doesn't violate ADR-0005, and the
// condition under which this gets revisited (reactor integration at M3+, or
// hundreds of destinations with concurrent delayed traffic).
//
// The worker thread is started lazily, on the first call that actually needs to
// schedule a future delivery. Destinations that never send a delayed message never
// spawn a thread, so the common (non-delayed) path pays nothing beyond the caller's
// own "is this delayed?" check (see Consumer::push / Destination::deliverCommitted).
//
// Per ADR-0006 (destructors must not throw): stop() is the explicit teardown entry
// point and must be called by the owner (Destination's destructor) before this
// object is destroyed. The destructor only acts as a safety net, wrapping stop() in
// try/catch so a std::thread::join() failure can never escape as an exception.
class DeliveryScheduler {
 public:
  DeliveryScheduler();
  DeliveryScheduler(const DeliveryScheduler&) = delete;
  DeliveryScheduler& operator=(const DeliveryScheduler&) = delete;
  ~DeliveryScheduler();

  // Enqueue msg onto *queue, immediately if its deliveryTime has already elapsed
  // (or is unset — the common case), otherwise arm/refresh the timer so it becomes
  // visible exactly when due.
  void enqueueOrSchedule(std::shared_ptr<QueueT> queue, Message::Ptr msg);

  // Idempotent explicit teardown: stop the worker thread and join it. Safe to call
  // more than once (e.g. once explicitly, once from the destructor safety net).
  void stop();

  // Upper bound (ms) on how long a single wait_until sleep in run() is
  // allowed to be, regardless of how far out the next deadline claims to be.
  // See run()'s definition and spec 13 review round 2 (B4) for why this
  // exists: without it, an absurd deliveryTime (storage corruption, a
  // deliveryTime set via the public jmsHeaders field bypassing SendOptions
  // validation entirely, or a future network ingress) can overflow the
  // ms -> chrono-duration conversion and wrap the wait deadline into the
  // past, spinning the worker at 100% CPU while holding _mutex and hanging
  // stop() forever.
  static constexpr int64_t kMaxWaitMs = 24LL * 60 * 60 * 1000;  // 1 day

  // Pure helper exposed for direct testing (DeliveryDelayTest): clamps
  // (deadlineMs - nowMsValue) into [0, kMaxWaitMs] so converting it to a
  // chrono duration can never overflow, no matter how corrupt/absurd
  // deadlineMs is (including std::numeric_limits<int64_t>::max()). This is
  // the exact arithmetic run() uses to pick wait_until's deadline.
  static int64_t cappedWaitDeltaMs(int64_t deadlineMs, int64_t nowMsValue);

 private:
  struct Entry {
    std::shared_ptr<QueueT> queue;
    Message::Ptr msg;
  };

  void run();

  std::reference_wrapper<Poco::Logger> _logger;
  std::mutex _mutex;
  std::condition_variable _cv;
  std::multimap<int64_t, Entry> _pending;  // ordered by deliveryTime, begin() = next due
  std::thread _thread;
  bool _threadStarted{false};
  bool _stop{false};
  bool _stopped{false};
  uint64_t _version{0};  // bumped on every insert so a waiting thread can re-check its deadline
};

}  // namespace tiny_mq
#endif  // TINY_MQ__DELIVERY_SCHEDULER_H_
