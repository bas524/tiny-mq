//
// DeliveryScheduler — per-Destination timer for JMS 2.0 delivery delay (§7.8, spec 13).
//

#include "DeliveryScheduler.h"
#include <Poco/Format.h>
#include <Poco/Timestamp.h>
#include "LogTracer.h"

namespace tiny_mq {

namespace {
int64_t nowMs() { return Poco::Timestamp().epochMicroseconds() / 1000; }
}  // namespace

DeliveryScheduler::DeliveryScheduler()
    : _logger(Poco::Logger::get("tiny_mq.deliveryscheduler")) {}

DeliveryScheduler::~DeliveryScheduler() {
  try {
    stop();
  } catch (const std::exception& e) {
    poco_error(_logger.get(), Poco::format("stop() threw in destructor — ignoring: %s", std::string(e.what())));
  } catch (...) {
    poco_error(_logger.get(), "stop() threw a non-std exception in destructor — ignoring");
  }
}

void DeliveryScheduler::enqueueOrSchedule(std::shared_ptr<QueueT> queue, Message::Ptr msg) {
  TRACE(_logger);
  int64_t deliveryTime = msg ? msg->jmsHeaders.deliveryTime : 0;
  if (deliveryTime > 0) {
    // Defense-in-depth (spec 13 review round 2, B4): validateSendOptions only
    // guards the SendOptions ingress. jmsHeaders.deliveryTime can also arrive
    // here directly from a public-field send() (no SendOptions involved at
    // all) or from a replayed storage record (see
    // Destination.cpp's deliveryTimeFromStorageBytes, which applies the same
    // bound independently since it never reaches this function's msg
    // parameter path through a fresh header write — it constructs the shell's
    // header itself). A deliveryTime absurdly far in the future is treated as
    // corrupt/garbage input, not honored: clamp to "immediate" rather than
    // schedule it, and fix up the header so a consumer doesn't see a bogus
    // JMSDeliveryTime on a message that was actually delivered right away.
    // (DeliveryScheduler::run additionally caps how long it ever sleeps for,
    // so even a value that somehow slipped past this check cannot overflow
    // the wait_until conversion or spin the worker — this check exists to
    // give a diagnosable "why" and correct JMSDeliveryTime, not as the sole
    // guard against the CPU-spin/hang failure mode.)
    const int64_t now = nowMs();
    if (deliveryTime - now > kMaxFutureHeaderMs) {
      poco_warning(_logger.get(),
                   Poco::format("DeliveryScheduler: deliveryTime=%?d (now=%?d) exceeds the "
                                "%?d ms sanity bound — treating as immediate delivery instead "
                                "of scheduling",
                                deliveryTime, now, kMaxFutureHeaderMs));
      deliveryTime = 0;
      if (msg) {
        msg->jmsHeaders.deliveryTime = 0;
        // The header alone is not enough: preparePush already cached this
        // message's serialized bytes (persistent messages only) before this
        // clamp ran, and Consumer::recv rehydrates jmsHeaders from that cache
        // via fromBytes, silently reverting the fix-up above. This is the
        // same defect class as spec 13 review round 2's B2 (header and
        // cached-bytes drifting apart) recurring in new code — see
        // docs/reviews/13-delivery-delay.review.md, round 3, N15.
        msg->patchCachedDeliveryTime(0);
      }
    }
  }
  if (deliveryTime == 0 || deliveryTime <= nowMs()) {
    queue->enqueue(std::move(msg));
    return;
  }

  std::unique_lock<std::mutex> lock(_mutex);
  if (_stopped) {
    // stop() has already run (possibly before any thread was ever started —
    // e.g. the destination never saw a delayed message until now). Starting a
    // new worker thread here would race stop(): stop() already returned via
    // its "if (_stopped) return" fast path without joining anything, so a
    // thread spun up after that point would never be joined and ~std::thread
    // would call std::terminate on a joinable thread (the exact failure mode
    // ADR-0006 exists to prevent, and one try/catch in the destructor cannot
    // catch since std::terminate is not a throw).
    //
    // What to do with the message differs by durability (spec 13 review N9,
    // making the policy consistent with stop()'s own draining behavior
    // below, which discards not-yet-due entries rather than delivering them
    // early): a persistent message is still an un-acked record in storage:
    // delivering it now would let the consumer ack it, erasing that record
    // and losing the delay (and the message) forever, whereas dropping the
    // in-memory delivery here leaves the record for a future replay to
    // re-arm correctly. A non-persistent message has no such second life —
    // nothing will ever redeliver it — so late-but-delivered beats losing it
    // outright, matching "provider is shutting down" semantics.
    lock.unlock();
    if (msg && msg->isPersistent()) {
      poco_warning(_logger.get(),
                   "DeliveryScheduler: enqueueOrSchedule after stop() for a persistent "
                   "message — dropping the in-memory delivery instead of delivering early; "
                   "the storage record remains and a future replay will re-arm it");
      return;
    }
    queue->enqueue(std::move(msg));
    return;
  }
  if (!_threadStarted) {
    _thread = std::thread(&DeliveryScheduler::run, this);
    _threadStarted = true;
  }
  _pending.emplace(deliveryTime, Entry{std::move(queue), std::move(msg)});
  ++_version;
  lock.unlock();
  _cv.notify_one();
}

/*static*/ int64_t DeliveryScheduler::cappedWaitDeltaMs(int64_t deadlineMs, int64_t nowMsValue) {
  // Compare before subtracting: deadlineMs - nowMsValue can overflow a
  // signed int64_t when deadlineMs is huge (e.g. INT64_MAX), which is UB and
  // lets an optimizer treat the "delta < 0" check below as unreachable and
  // remove it. nowMsValue is a real clock reading (~1.7e12), so
  // nowMsValue + kMaxWaitMs cannot overflow, and these two guards can be
  // evaluated without ever forming an out-of-range difference.
  if (deadlineMs <= nowMsValue) return 0;
  if (deadlineMs >= nowMsValue + kMaxWaitMs) return kMaxWaitMs;
  // At this point nowMsValue < deadlineMs < nowMsValue + kMaxWaitMs, so the
  // difference fits in (0, kMaxWaitMs] and cannot overflow.
  return deadlineMs - nowMsValue;
}

void DeliveryScheduler::run() {
  std::unique_lock<std::mutex> lock(_mutex);
  while (!_stop) {
    if (_pending.empty()) {
      _cv.wait(lock, [this] { return _stop || !_pending.empty(); });
      continue;
    }

    const int64_t deadlineMs = _pending.begin()->first;
    const uint64_t startVersion = _version;
    // cappedWaitDeltaMs bounds a single wait_until sleep to at most kMaxWaitMs,
    // independent of how far out deadlineMs claims to be. This is the last
    // line of defense for spec 13 review round 2's B4: if a deliveryTime ever
    // reaches _pending corrupt or absurd (storage bit-rot, a future network
    // ingress, or simply a bound this file doesn't know about yet),
    // "deadlineMs - nowMs()" can be enormous, and system_clock::now() +
    // milliseconds(that) overflows system_clock's native
    // (finer-than-millisecond) duration representation and wraps into the
    // past. Unbounded, that produces two compounding failures: wait_until
    // returns instantly with woken == false, the due-delivery scan below
    // finds nothing actually due, and the outer while(!_stop) loop spins at
    // 100% CPU while holding _mutex — which in turn means stop() hangs
    // forever on that same mutex and the owning Destination can never be torn
    // down. Capping the sleep means the worker always genuinely sleeps (up to
    // a day) before re-evaluating, no matter what value is sitting at
    // _pending.begin(): no overflow, no spin, no unkillable process. This is
    // deliberately not the only guard (see enqueueOrSchedule's own clamp and
    // Destination.cpp's deliveryTimeFromStorageBytes) but it is the one that
    // holds even if every other check is somehow bypassed — see
    // DeliveryDelayTest.testSchedulerWaitDeltaCappedAgainstOverflow, which
    // exercises this arithmetic directly.
    const int64_t deltaMs = cappedWaitDeltaMs(deadlineMs, nowMs());
    const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(deltaMs);
    const bool woken = _cv.wait_until(lock, deadline, [this, startVersion] { return _stop || _version != startVersion; });
    if (woken) {
      // Stopped, or a nearer deadline was inserted — re-evaluate the heap top.
      continue;
    }

    // Timed out: deliver every entry that is now due.
    const int64_t deliverUpToMs = nowMs();
    while (!_pending.empty() && _pending.begin()->first <= deliverUpToMs) {
      auto it = _pending.begin();
      Entry entry = std::move(it->second);
      _pending.erase(it);
      lock.unlock();
      entry.queue->enqueue(std::move(entry.msg));
      lock.lock();
    }
  }
}

void DeliveryScheduler::stop() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stopped) return;
    _stop = true;
    _stopped = true;
  }
  _cv.notify_all();
  if (_thread.joinable()) {
    try {
      _thread.join();
    } catch (const std::exception& e) {
      poco_error(_logger.get(), Poco::format("DeliveryScheduler worker join() threw: %s", std::string(e.what())));
    }
  }
  // After join() (or if the worker never started), no other thread can still be
  // touching _pending: enqueueOrSchedule() now bails out (via the _stopped check
  // above) before inserting anything new, and the worker loop exits its "while
  // (!_stop)" as soon as _stop flips without draining what is left. Anything
  // still here is a not-yet-due delivery that is being discarded — log how many
  // so "message never arrived" is diagnosable instead of silent.
  if (!_pending.empty()) {
    poco_warning(_logger.get(),
                 Poco::format("DeliveryScheduler stop(): discarding %z not-yet-due pending "
                              "delivery(ies)",
                              _pending.size()));
  }
}

}  // namespace tiny_mq
