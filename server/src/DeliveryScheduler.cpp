//
// DeliveryScheduler — per-Destination timer for JMS 2.0 delivery delay (§7.8, spec 13).
//

#include "DeliveryScheduler.h"
#include "Destination.h"
#include "DeliveryClock.h"
#include <Poco/Format.h>
#include <Poco/Timestamp.h>
#include "LogTracer.h"

namespace tiny_mq {


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

void DeliveryScheduler::enqueueOrSchedule(std::shared_ptr<DeliveryState> queue, Message::Ptr msg) {
  schedule({std::move(queue), std::move(msg)});
}

void DeliveryScheduler::enqueueOrSchedule(std::shared_ptr<QueueT> queue, Message::Ptr msg) {
  schedule({std::move(queue), std::move(msg)});
}

void DeliveryScheduler::Entry::deliver() {
  if (msg && msg->jmsHeaders.expiration && msg->isExpired(DeliveryClock::now())) return;
  std::visit([&](auto& target) { target->enqueue(std::move(msg)); }, queue);
}

void DeliveryScheduler::schedule(Entry entry) {
  TRACE(_logger);
  auto& msg = entry.msg;
  auto deadline = msg ? std::max(msg->jmsHeaders.deliveryTime, msg->_nextRedeliveryAt) : 0;
  if (msg && msg->jmsHeaders.expiration > 0) deadline = std::min(deadline, msg->jmsHeaders.expiration);
  if (deadline <= DeliveryClock::now()) {
    entry.deliver();
    return;
  }

  std::unique_lock<std::mutex> lock(_mutex);
  const auto* target = std::get_if<std::shared_ptr<DeliveryState>>(&entry.queue);
  if (target && (*target)->isClosed()) return;
  if (_stopped) {
    lock.unlock();
    // Preserve the standalone QueueT scheduler contract after stop.
    if (!target && msg && !msg->isPersistent()) entry.deliver();
    return;
  }
  if (!_threadStarted) {
    _thread = std::thread(&DeliveryScheduler::run, this);
    _threadStarted = true;
  }
  _pending.emplace(deadline, std::move(entry));
  ++_version;
  lock.unlock();
  _cv.notify_one();
}

/*static*/ int64_t DeliveryScheduler::cappedWaitDeltaMs(int64_t deadlineMs, int64_t nowMsValue) {
  if (deadlineMs <= nowMsValue) return 0;
  if (nowMsValue <= std::numeric_limits<int64_t>::max() - kMaxWaitMs &&
      deadlineMs > nowMsValue + kMaxWaitMs) return kMaxWaitMs;
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
    auto deltaMs = cappedWaitDeltaMs(deadlineMs, DeliveryClock::now());
#ifdef ENABLE_TESTS
    if (DeliveryClock::isManual()) deltaMs = std::min<int64_t>(deltaMs, 2);
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deltaMs);
    const bool woken = _cv.wait_until(lock, deadline, [this, startVersion] { return _stop || _version != startVersion; });
    if (woken) {
      // Stopped, or a nearer deadline was inserted — re-evaluate the heap top.
      continue;
    }

    // Timed out: deliver every entry that is now due.
    const int64_t deliverUpToMs = DeliveryClock::now();
    while (!_pending.empty() && _pending.begin()->first <= deliverUpToMs) {
      auto it = _pending.begin();
      Entry entry = std::move(it->second);
      _pending.erase(it);
      lock.unlock();
      entry.deliver();
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
  std::lock_guard<std::mutex> lock(_mutex);
  _pending.clear();
}

void DeliveryScheduler::cancel(const DeliveryState& queue) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    std::erase_if(_pending, [&queue](const auto& entry) {
      const auto* target = std::get_if<std::shared_ptr<DeliveryState>>(&entry.second.queue);
      return target && target->get() == &queue;
    });
    ++_version;
  }
  _cv.notify_one();
}

}  // namespace tiny_mq
