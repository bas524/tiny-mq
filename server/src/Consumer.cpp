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
      _durable(durable),
      _countClaims(durable || session.acknowledgeMode() == Session::DUPS_OK_ACKNOWLEDGE) {}

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
  return receiveImpl<false, false>(usec_timeout, nullptr);
}

ReceiveBodyResult Consumer::receiveChecked(const std::function<bool(const Message&)>& predicate, int64_t timeout) {
  if (!predicate) throw std::invalid_argument("receive body predicate is empty");
  ReceiveBodyResult result;
  result.message = receiveImpl<false, true>(timeout, &predicate, &result.disposition);
  return result;
}

template<bool listener, bool checked>
Message::Ptr Consumer::receiveImpl(int64_t timeout, const std::function<bool(const Message&)>* predicate,
                                   ReceiveBodyDisposition* disposition) {
  struct ReceiveGuard {
    std::atomic_uint32_t* active;
    ~ReceiveGuard() {
      if (active) {
        active->store(0, std::memory_order_release);
        active->notify_one();
      }
    }
  } guard{nullptr};

  if constexpr (!listener) {
    uint32_t idle = 0;
    if (!_receiving.compare_exchange_strong(idle, 1, std::memory_order_seq_cst)) {
      if (receiveSealed()) throw IllegalStateException("consumer is closed");
      throw IllegalStateException("receive already active");
    }
    guard.active = &_receiving;
    // Sequentially consistent admission pairs with close's seal and active
    // load. At least one side observes the other before touching the session.
    if (_closeState.load(std::memory_order_seq_cst) != CloseState::open) {
      throw IllegalStateException("consumer is closed");
    }
    if (_session->_closing.load(std::memory_order_acquire)) [[unlikely]] _session->ensureOpen();
    if (_session->_dispatch.hasListeners()) throw IllegalStateException("session has message listeners");
  } else if (receiveSealed()) {
    return {};
  }

  auto& session = *_session;
  const auto mode = session.acknowledgeMode();
  const bool dups = mode == Session::DUPS_OK_ACKNOWLEDGE;
  const bool automatic = mode == Session::AUTO_ACKNOWLEDGE;
  if (dups && _pendingAcks >= 100) [[unlikely]] flushPendingAcks();
  const auto* readyCheck = checked && (automatic || dups) ? predicate : nullptr;
  auto& started = session.connection()._started;
  bool incompatible = false;
  std::chrono::steady_clock::time_point deadline;
  for (;;) {
    auto message = _queue->claim(*this, timeout, deadline, started, readyCheck, incompatible);
    if constexpr (checked) {
      if (incompatible) {
        *disposition = ReceiveBodyDisposition::incompatible;
        return {};
      }
    }
    if (!message) return {};
    const bool expired = message->jmsHeaders.expiration != 0 && message->isExpired(DeliveryClock::now());
    try {
      if (hasStorage(*message)) [[unlikely]] {
        if (!expired) {
          _destination->registerAttempt(*_storage, *message);
          LocalTransactionJournal::point("ack", "before-ack");
        }
        if (expired || (automatic && !listener)) acknowledgeStored(*message);
      }
      if (!expired && !automatic) {
        message->_receiveSequence = session._receiveSequence++;
        if (mode == Session::SESSION_TRANSACTED) {
          _transacted.push_back(message);
          if (_durable) ++_queue->pending;
        } else {
          track(message);
        }
      }
    } catch (...) {
      _queue->enqueue(std::move(message));
      throw;
    }
    if (!expired) {
      if constexpr (checked) {
        if (!readyCheck && !(*predicate)(*message)) {
          *disposition = ReceiveBodyDisposition::incompatible;
          return {};
        }
        *disposition = ReceiveBodyDisposition::compatible;
      }
      return message;
    }
  }
}

void Consumer::setMessageListener(MessageListener listener) {
  if (receiveSealed()) throw IllegalStateException("consumer is closed");
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
  auto message = receiveImpl<true, false>(0, nullptr);
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
  else if (mode == Session::DUPS_OK_ACKNOWLEDGE && receiveSealed()) flushPendingAcks();
  return true;
}

linear_storage::Record Consumer::record(const Message& message) const {
  linear_storage::Record rec;
  rec.tomId = message._storageTomId;
  rec.offset = message._storageOffset;
  message._storageId.copyTo(rec.header.uuid.data());
  return rec;
}

void Consumer::acknowledgeStored(const Message& message) {
  std::lock_guard lock(_queue->storageMutex);
  if (_queue->retired) return;
  if (!_storage->removeDelivery(record(message))) throw Poco::IOException("cannot acknowledge stored delivery");
  LocalTransactionJournal::point("ack", "after-ack");
}

void Consumer::track(const Message::Ptr& message) noexcept {
  auto* previous = _inFlightTail;
  message->_inFlightLink.prev = previous;
  message->_inFlightLink.owner = this;
  auto& link = previous ? previous->_inFlightLink.next : _inFlightHead;
  link = message;
  _inFlightTail = message.get();
  if (_countClaims) [[unlikely]] {
    ++_pendingAcks;
    if (_durable) ++_queue->pending;
  }
}

void Consumer::acknowledgeOn(const Message& message) {
  if (!_session) [[unlikely]] throw IllegalStateException("consumer session is closed");
  if (message._inFlightLink.owner != this) return;
  acknowledge(message);
  unlink(message);
}

void Consumer::unlink(const Message& message) {
  auto* prev = message._inFlightLink.prev;
  auto* next = message._inFlightLink.next.get();
  auto& link = prev ? prev->_inFlightLink.next : _inFlightHead;
  link.swap(message._inFlightLink.next);
  auto self = std::move(message._inFlightLink.next);  // Pin the bare Message& through unlink.
  if (next) next->_inFlightLink.prev = prev;
  else _inFlightTail = prev;
  message._inFlightLink.prev = nullptr;
  message._inFlightLink.owner = nullptr;
  if (_countClaims) [[unlikely]] {
    --_pendingAcks;
    if (_durable) --_queue->pending;
  }
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
  auto state = CloseState::open;
  if (!_closeState.compare_exchange_strong(state, CloseState::closing, std::memory_order_seq_cst)) {
    if (self) return;
    _closeState.wait(CloseState::closing);
    if (!isClosed()) close();
    else _callbackActive.wait(true);
    return;
  }
  _queue->wake();
  if (_receiving.load(std::memory_order_seq_cst) != 0) {
    _receiving.wait(1, std::memory_order_acquire);
  }
  try {
    auto& dispatch = _session->_dispatch;
    dispatch.remove(*this);
    dispatch.wait(*this);
    if (_session->acknowledgeMode() == Session::DUPS_OK_ACKNOWLEDGE &&
        SessionDispatch::current != &dispatch) flushPendingAcks();
    _destination->detach(*this);
  } catch (...) {
    if (!isClosed()) _closeState.store(CloseState::open);
    _closeState.notify_all();
    throw;
  }
  _closeState.notify_all();
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
  _closeState.store(CloseState::closed);
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
