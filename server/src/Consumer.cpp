#include "Consumer.h"
#include "Connection.h"
#include "DeliveryClock.h"
#include "Destination.h"
#include "Exceptions.h"
#include "Session.h"

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

Message::Ptr Consumer::receive() { return recv(-1); }
Message::Ptr Consumer::receiveNoWait() { return recv(0); }

Message::Ptr Consumer::receive(int64_t timeout) {
  if (timeout < 0) throw std::invalid_argument("negative receive timeout");
  return recv(timeout == 0 ? -1 : std::min(timeout, INT64_MAX / 1000) * 1000);
}

Message::Ptr Consumer::recv(int64_t usec_timeout) {
  return receiveImpl(usec_timeout, nullptr, false).message;
}

ReceiveBodyResult Consumer::receiveChecked(const std::function<bool(const Message&)>& predicate, int64_t timeout) {
  if (!predicate) throw std::invalid_argument("receive body predicate is empty");
  return receiveImpl(timeout, &predicate, false);
}

ReceiveBodyResult Consumer::receiveImpl(int64_t timeout, const std::function<bool(const Message&)>* predicate,
                                        bool listener) {
  struct ReceiveGuard {
    Consumer* consumer;
    ~ReceiveGuard() {
      if (consumer) {
        consumer->_receiving = false;
        if (consumer->_closing) consumer->_receiving.notify_all();
      }
    }
  } guard{nullptr};

  if (!listener) {
    if (_receiving.exchange(true)) throw IllegalStateException("receive already active");
    guard.consumer = this;
    if (_closing || _closed) throw IllegalStateException("consumer is closed");
    _session->ensureOpen();
    if (_session->_dispatch.hasListeners()) throw IllegalStateException("session has message listeners");
  } else if (_closing || _closed) {
    return {};
  }

  const auto mode = _session->acknowledgeMode();
  const bool dups = mode == Session::DUPS_OK_ACKNOWLEDGE;
  const bool automatic = mode == Session::AUTO_ACKNOWLEDGE;
  if (dups && _pendingAcks >= 100) flushPendingAcks();
  const auto* readyCheck = automatic || dups ? predicate : nullptr;
  auto& started = _session->connection()._started;
  bool incompatible = false;
  std::chrono::steady_clock::time_point deadline;
  for (;;) {
    auto message = _queue->claim(_selector.get(), timeout, deadline, started, _closing, readyCheck, incompatible);
    if (incompatible) return {ReceiveBodyDisposition::incompatible, {}};
    if (!message) return {};
    const bool expired = message->jmsHeaders.expiration != 0 && message->isExpired(DeliveryClock::now());
    try {
      if (expired) {
        acknowledge(*message);
      } else {
        if (hasStorage(*message)) {
          _destination->registerAttempt(*_storage, *message);
          LocalTransactionJournal::point("ack", "before-ack");
        }
        if (automatic && !listener) acknowledge(*message);
        else if (!automatic) track(message);
      }
    } catch (...) {
      _queue->enqueue(std::move(message));
      throw;
    }
    if (!expired) {
      if (predicate && !readyCheck && !(*predicate)(*message)) return {ReceiveBodyDisposition::incompatible, {}};
      return {ReceiveBodyDisposition::compatible, std::move(message)};
    }

  }
}

void Consumer::setMessageListener(MessageListener listener) {
  if (_closing || _closed) throw IllegalStateException("consumer is closed");
  _session->ensureOpen();
  auto& dispatch = _session->_dispatch;
  const bool enable = static_cast<bool>(listener);
  {
    std::lock_guard lock(_listenerMutex);
    const bool hadListener = static_cast<bool>(_listener);
    if (hadListener != enable) {
      if (enable) dispatch.add(_session->_consumers.at(_uuid));
      else dispatch.remove(*this);
    }
    _listener = std::move(listener);
  }
}

bool Consumer::dispatchOne() {
  MessageListener listener;
  {
    std::lock_guard lock(_listenerMutex);
    listener = _listener;
  }
  if (!listener) return false;
  auto message = receiveImpl(0, nullptr, true).message;
  if (!message) return false;
  const auto mode = _session->acknowledgeMode();
  struct DurableCallback {
    std::atomic_size_t* pending;
    ~DurableCallback() { if (pending) --*pending; }
  } active{_durable && mode == Session::AUTO_ACKNOWLEDGE ? &_queue->pending : nullptr};
  if (active.pending) ++*active.pending;
  try {
    listener(message);
  } catch (...) {
    if (mode == Session::AUTO_ACKNOWLEDGE) redeliver(*message);
    if (mode == Session::DUPS_OK_ACKNOWLEDGE && message->_inFlightLink.owner == this) {
      redeliver(*message);
      unlink(*message);
    }
    return true;
  }
  if (mode == Session::AUTO_ACKNOWLEDGE) acknowledge(*message);
  else if (mode == Session::DUPS_OK_ACKNOWLEDGE && (_closing || _closed)) flushPendingAcks();
  return true;
}

bool Consumer::hasStorage(const Message& message) const {
  return message._storageTomId != std::numeric_limits<Poco::UInt32>::max();
}

linear_storage::Record Consumer::record(const Message& message) const {
  linear_storage::Record rec;
  rec.tomId = message._storageTomId;
  rec.offset = message._storageOffset;
  message._storageId.copyTo(rec.header.uuid.data());
  return rec;
}

void Consumer::acknowledge(const Message& message) {
  if (!hasStorage(message)) return;
  std::lock_guard lock(_queue->storageMutex);
  if (_queue->retired) return;
  if (!_storage->removeDelivery(record(message))) throw Poco::IOException("cannot acknowledge stored delivery");
  LocalTransactionJournal::point("ack", "after-ack");
}

void Consumer::track(const Message::Ptr& message) {
  message->_receiveSequence = _session->_receiveSequence++;
  if (_session->acknowledgeMode() == Session::SESSION_TRANSACTED) {
    _transacted.push_back(message);
  } else {
    message->_inFlightLink.prev = _inFlightTail;
    message->_inFlightLink.owner = this;
    if (_inFlightTail) _inFlightTail->_inFlightLink.next = message;
    else _inFlightHead = message;
    _inFlightTail = message.get();
    ++_pendingAcks;
  }
  if (_durable) ++_queue->pending;
}

void Consumer::acknowledgeOn(const Message& message) {
  if (!_session) throw IllegalStateException("consumer session is closed");
  if (message._inFlightLink.owner != this) return;
  acknowledge(message);
  unlink(message);
}

void Consumer::unlink(const Message& message) {
  auto* prev = message._inFlightLink.prev;
  auto self = prev ? prev->_inFlightLink.next : _inFlightHead;
  auto next = std::move(message._inFlightLink.next);
  if (next) next->_inFlightLink.prev = prev;
  else _inFlightTail = prev;
  if (prev) prev->_inFlightLink.next = std::move(next);
  else _inFlightHead = std::move(next);
  message._inFlightLink.prev = nullptr;
  message._inFlightLink.owner = nullptr;
  --_pendingAcks;
  if (_durable) --_queue->pending;
}

void Consumer::redeliver(const Message& message, uint64_t batch) {
  auto next = message.copy();
  next->_recoveryBatch = batch;
  _destination->retry(_queue, hasStorage(message) ? _storage.get() : nullptr, std::move(next));
}

bool Consumer::hasClaims() const { return _inFlightHead || !_transacted.empty(); }

void Consumer::close() {
  const bool self = SessionDispatch::current && !SessionDispatch::inCompletion &&
                    SessionDispatch::current->_activeConsumer == this;
  if (_closed) {
    if (!self) _callbackActive.wait(true);
    return;
  }
  if (_closing.exchange(true)) {
    if (self) return;
    while (_closing) _closing.wait(true);
    if (!_closed) close();
    else _callbackActive.wait(true);
    return;
  }
  _queue->wake();
  _receiving.wait(true);
  try {
    auto& dispatch = _session->_dispatch;
    dispatch.remove(*this);
    dispatch.wait(*this);
    if (_session->acknowledgeMode() == Session::DUPS_OK_ACKNOWLEDGE &&
        SessionDispatch::current != &dispatch) flushPendingAcks();
    _destination->detach(*this);
  } catch (...) {
    _closing = false;
    _closing.notify_all();
    throw;
  }
  _closing = false;
  _closing.notify_all();
  // A detached consumer stays closed even though the close barrier is over.
  _queue->wake();
}

void Consumer::finishSession() noexcept {
  try {
    close();
  } catch (const std::exception& error) {
    poco_error(_logger.get(), std::string("consumer teardown: ") + error.what());
  } catch (...) {
    poco_error(_logger.get(), "consumer teardown failed");
  }
  _closed = true;
  _session = nullptr;
  if (_durable) _queue->pending -= _transacted.size() + _pendingAcks;
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
  _pendingAcks = 0;
}

void Consumer::flushPendingAcks() {
  while (_inFlightHead) {
    acknowledge(*_inFlightHead);
    unlink(*_inFlightHead);
  }
}
}  // namespace tiny_mq
