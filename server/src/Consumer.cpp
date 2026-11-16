#include "Consumer.h"
#include "Destination.h"
#include "Exceptions.h"
#include "Session.h"
#include <Poco/Timestamp.h>
#include <cmath>

namespace tiny_mq {
Consumer::Consumer(Destination& destination, Session& session, std::shared_ptr<DeliveryState> queue,
                   std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage,
                   std::shared_ptr<Selector> selector, bool durable)
    : _uuid(session.createRandomUUID()),
      _destination(destination.shared_from_this()),
      _session(&session),
      _queue(std::move(queue)),
      _storage(std::move(storage)),
      _selector(std::move(selector)),
      _logger(Poco::Logger::get("tiny_mq.consumer")),
      _durable(durable) {}

Consumer::~Consumer() { clearInFlight(); }

const Poco::UUID& Consumer::id() const { return _uuid; }
QueueT::producer_token_t Consumer::getProducerToken() { return {}; }

const Session& Consumer::session() const {
  if (!_session) throw IllegalStateException("consumer session is closed");
  return *_session;
}

Message::Ptr Consumer::recv(int64_t usec_timeout) {
  if (_closed) throw IllegalStateException("consumer is closed");
  std::optional<Poco::Timestamp> started;
  int64_t remaining = usec_timeout;
  const auto mode = _session->acknowledgeMode();

  while (auto message = _queue->claim(_selector.get(), remaining)) {
    const bool expired = message->jmsHeaders.expiration != 0 &&
        message->isExpired(Poco::Timestamp().epochMicroseconds() / 1000);
    if (expired || mode == Session::AUTO_ACKNOWLEDGE) {
      try {
        acknowledge(*message);
      } catch (...) {
        _queue->enqueue(std::move(message));
        throw;
      }
    }
    if (expired) {
      if (usec_timeout > 0) {
        if (!started) started.emplace();
        remaining = std::min<int64_t>(remaining, usec_timeout - started->elapsed());
        if (remaining <= 0) return {};
      }
      continue;
    }

    if (mode != Session::AUTO_ACKNOWLEDGE) {
      if (mode == Session::SESSION_TRANSACTED) {
        _transacted.push_back(message);
      } else {
        message->_inFlightLink.prev = _inFlightTail;
        message->_inFlightLink.owner = this;
        if (_inFlightTail) _inFlightTail->_inFlightLink.next = message;
        else _inFlightHead = message;
        _inFlightTail = message.get();
      }
      if (_durable) ++_queue->pending;
    }
    return message;
  }
  return {};
}

linear_storage::Record Consumer::record(const Message& message) const {
  linear_storage::Record rec;
  rec.tomId = message._storageTomId;
  rec.offset = message._storageOffset;
  message._storageId.copyTo(rec.header.uuid.data());
  return rec;
}

void Consumer::acknowledge(const Message& message) {
  if (message._storageTomId == std::numeric_limits<Poco::UInt32>::max()) return;
  std::lock_guard lock(_queue->storageMutex);
  if (!_queue->retired && !_storage->removeDelivery(record(message))) {
    throw Poco::IOException("cannot acknowledge stored delivery");
  }
}

void Consumer::acknowledgeOn(const Message& message) {
  if (!_session) throw IllegalStateException("consumer session is closed");
  if (message._inFlightLink.owner != this) return;

  if (_session->acknowledgeMode() == Session::DUPS_OK_ACKNOWLEDGE) {
    if (message._storageTomId != std::numeric_limits<Poco::UInt32>::max()) {
      _pendingAcks.push_back(record(message));
      if (_closed || _pendingAcks.size() >= 100) flushPendingAcks();
    }
  } else {
    acknowledge(message);
  }

  auto* prev = message._inFlightLink.prev;
  auto self = prev ? prev->_inFlightLink.next : _inFlightHead;  // pins a bare Message& through unlink
  auto next = std::move(message._inFlightLink.next);
  if (next) next->_inFlightLink.prev = prev;
  else _inFlightTail = prev;
  if (prev) prev->_inFlightLink.next = std::move(next);
  else _inFlightHead = std::move(next);
  message._inFlightLink.prev = nullptr;
  message._inFlightLink.owner = nullptr;
  if (_durable) --_queue->pending;
}

void Consumer::commit() {
  while (!_transacted.empty()) {
    acknowledge(*_transacted.front());
    _transacted.pop_front();
    if (_durable) --_queue->pending;
  }
}

void Consumer::rollback(bool sessionClosing) {
  while (!_transacted.empty()) {
    redeliver(*_transacted.front(), sessionClosing);
    _transacted.pop_front();
    if (_durable) --_queue->pending;
  }
}

void Consumer::recover(bool sessionClosing) {
  while (_inFlightHead) {
    auto message = _inFlightHead;
    redeliver(*message, sessionClosing);
    _inFlightHead = std::move(message->_inFlightLink.next);
    if (_inFlightHead) _inFlightHead->_inFlightLink.prev = nullptr;
    else _inFlightTail = nullptr;
    message->_inFlightLink.owner = nullptr;
    if (_durable) --_queue->pending;
  }
}

void Consumer::redeliver(Message& message, bool sessionClosing) {
  // An application may still hold the old delivery. Give the next claim its
  // own object so the old handle cannot acknowledge the next attempt.
  auto next = message.copy();
  next->jmsHeaders.redelivered = true;
  ++next->jmsHeaders.deliveryCount;
  message.jmsHeaders.redelivered = next->jmsHeaders.redelivered;
  message.jmsHeaders.deliveryCount = next->jmsHeaders.deliveryCount;
  const auto policy = _destination->redeliveryPolicy();
  const auto count = next->jmsHeaders.deliveryCount;

  if (policy.maxRedeliveries >= 0 && count > policy.maxRedeliveries) {
    _destination->deadLetter(std::move(next), "maxRedeliveries exceeded");
    acknowledge(message);
    return;
  }

  next->jmsHeaders.deliveryTime = 0;
  if (!sessionClosing && policy.backoffMs > 0) {
    const double delay = std::ldexp(static_cast<double>(policy.backoffMs), count - 1);
    next->jmsHeaders.deliveryTime = Poco::Timestamp().epochMicroseconds() / 1000 +
        static_cast<int64_t>(std::min(delay, static_cast<double>(policy.maxBackoffMs)));
  }
  message.jmsHeaders.deliveryTime = next->jmsHeaders.deliveryTime;
  // Ready messages are fully materialized and do not carry serialized caches.
  _destination->enqueueOrSchedule(_queue, std::move(next));
}

bool Consumer::hasClaims() const { return _inFlightHead || !_transacted.empty(); }

void Consumer::close() {
  if (_closed) return;
  flushPendingAcks();
  _destination->detach(*this);
}

void Consumer::finishSession() noexcept {
  try {
    rollback(true);
    recover(true);
    close();
  } catch (const std::exception& error) {
    poco_error(_logger.get(), std::string("consumer teardown: ") + error.what());
    _destination->detach(*this);
  } catch (...) {
    poco_error(_logger.get(), "consumer teardown failed");
    _destination->detach(*this);
  }
  _closed = true;
  _session = nullptr;
  if (_durable) {
    _queue->pending -= _transacted.size();
    for (auto message = _inFlightHead.get(); message; message = message->_inFlightLink.next.get()) {
      --_queue->pending;
    }
  }
  _transacted.clear();
  clearInFlight();
}

void Consumer::clearInFlight() noexcept {
  while (_inFlightHead) {
    auto node = std::move(_inFlightHead);
    _inFlightHead = std::move(node->_inFlightLink.next);
    node->_inFlightLink.prev = nullptr;
    node->_inFlightLink.owner = nullptr;
  }
  _inFlightTail = nullptr;
}

void Consumer::flushPendingAcks() {
  if (_pendingAcks.empty()) return;
  std::lock_guard lock(_queue->storageMutex);
  if (!_queue->retired) {
    for (const auto& record : _pendingAcks) {
      if (!_storage->remove(record)) throw Poco::IOException("cannot flush stored acknowledgements");
    }
  }
  _pendingAcks.clear();
}
}  // namespace tiny_mq
