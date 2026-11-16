#include "Destination.h"
#include "Connection.h"
#include "DestinationHash.h"
#include "Exceptions.h"
#include "Session.h"
#include "DeliveryClock.h"
#include "LocalTransactionJournal.h"
#include <Poco/File.h>
#include <Poco/Format.h>
#include <Poco/Timestamp.h>
#include <bit>
#include <cstring>
#include <filesystem>
#include <cmath>

namespace tiny_mq {
namespace {
Message::Ptr makeMessageShell(Message::Type type) {
  switch (type) {
    case Message::UNDEFINED: return std::make_shared<Message>();
    case Message::TEXT_MESSAGE: return std::make_shared<TextMessage>();
    case Message::BYTES_MESSAGE: return std::make_shared<BytesMessage>();
    case Message::MAP_MESSAGE: return std::make_shared<MapMessage>();
    case Message::STREAM_MESSAGE: return std::make_shared<StreamMessage>();
    case Message::OBJECT_MESSAGE: return std::make_shared<ObjectMessage>();
    default: throw Poco::DataFormatException("invalid stored message type");
  }
}
}  // namespace

void DeliveryState::enqueue(Message::Ptr message) {
  const bool special = message->_publication || message->_recoveryBatch;
  insert(std::move(message), special);
}

void DeliveryState::insert(Message::Ptr message, bool special) {
  std::vector<std::shared_ptr<Waiter>> notify;
  {
    std::lock_guard lock(_mutex);
    if (_closed) return;
    const auto band = std::clamp(message->jmsHeaders.priority, 0, QueueT::kBands - 1);
    const auto& added = _bands[band].emplace_back(std::move(message));
    _nonEmpty |= 1u << band;
    _special += special;
    if (!_listeners.empty()) notifyListeners();
    if (!_waiters.empty() && added->jmsHeaders.expiration != 0 && added->isExpired(DeliveryClock::now())) return;
    for (const auto& waiter : _waiters) {
      if (!waiter->signalled && (!waiter->selector || waiter->selector->matches(*added))) {
        notify.push_back(waiter);
        waiter->signalled = true;
      }
    }
  }
  for (const auto& waiter : notify) waiter->condition.notify_one();
}

Message::Ptr DeliveryState::take(const Selector* selector,
                                     const std::function<bool(const Message&)>* check, bool& incompatible) {
  if (!selector && !check && !_special) {
    if (!_nonEmpty) return {};
    const auto band = std::bit_width(_nonEmpty) - 1;
    auto& bandQueue = _bands[band];
    auto message = std::move(bandQueue.front());
    bandQueue.pop_front();
    if (bandQueue.empty()) _nonEmpty &= ~(1u << band);
    return message;
  }
  for (unsigned available = _nonEmpty; available != 0;) {
    const auto band = std::bit_width(available) - 1;
    auto& messages = _bands[band];
    auto match = messages.begin();
    while (match != messages.end() &&
           (!(*match)->isPublished() || (selector && !selector->matches(**match)) ||
            ((*match)->_recoveryBatch && followsRecoveredDelivery(**match, selector)))) ++match;
    if (match != messages.end()) {
      if (check && !(*match)->isExpired(DeliveryClock::now()) && !(*check)(**match)) {
        incompatible = true;
        return {};
      }
      auto message = std::move(*match);
      if (match == messages.begin()) messages.pop_front();
      else messages.erase(match);
      if (messages.empty()) _nonEmpty &= ~(1u << band);
      if (message->_publication || message->_recoveryBatch) --_special;
      message->_publication.reset();
      return message;
    }
    available &= ~(1u << band);
  }
  return {};
}

bool DeliveryState::followsRecoveredDelivery(const Message& message, const Selector* selector) const {
  const auto now = DeliveryClock::now();
  if (message.isExpired(now)) return false;
  for (unsigned available = _nonEmpty; available != 0; available &= available - 1) {
    const auto band = std::countr_zero(available);
    for (const auto& prior : _bands[band]) {
      if (prior->_recoveryBatch == message._recoveryBatch && prior->_receiveSequence < message._receiveSequence &&
          prior->isPublished() && !prior->isExpired(now) && (!selector || selector->matches(*prior))) {
        return true;
      }
    }
  }
  return false;
}

void DeliveryState::wake() {
  std::lock_guard lock(_mutex);
  notifyWaiters();
}

void DeliveryState::notifyWaiters() {
  if (!_listeners.empty()) notifyListeners();
  for (const auto& waiter : _waiters) {
    waiter->signalled = true;
    waiter->condition.notify_one();
  }
}

void DeliveryState::watch(const std::shared_ptr<std::atomic_uint64_t>& listener) {
  std::lock_guard lock(_mutex);
  _listeners.emplace_back(listener);
}

void DeliveryState::unwatch(const std::shared_ptr<std::atomic_uint64_t>& listener) {
  std::lock_guard lock(_mutex);
  std::erase_if(_listeners, [&](const auto& weak) {
    auto active = weak.lock();
    return !active || active == listener;
  });
}

void DeliveryState::notifyListeners() {
  for (const auto& weak : _listeners) {
    if (auto active = weak.lock()) {
      active->fetch_add(1, std::memory_order_release);
      active->notify_all();
    }
  }
}

Message::Ptr DeliveryState::claim(const Selector* selector, int64_t timeout,
                                   std::chrono::steady_clock::time_point& deadline,
                                   const std::atomic_bool& started, const std::atomic_bool& closing,
                                   const std::function<bool(const Message&)>* check, bool& incompatible) {
  using Clock = std::chrono::steady_clock;
  using Micros = std::chrono::microseconds;
  const auto armTimeout = [&] {
    if (timeout > 0 && deadline == Clock::time_point{}) {
      const auto now = Clock::now();
      const auto maxWait = std::chrono::duration_cast<Micros>(Clock::time_point::max() - now);
      deadline = now + std::min(maxWait, Micros(timeout));
    }
  };
  std::unique_lock lock(_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    armTimeout();
    lock.lock();
  }
  if (_closed || closing || (deadline != Clock::time_point{} && Clock::now() >= deadline)) return {};
  auto message = started ? take(selector, check, incompatible) : Message::Ptr{};
  if (message || incompatible || timeout == 0) {
    if (message && message->jmsHeaders.expiration != 0) armTimeout();
    return message;
  }
  armTimeout();

  const auto waiter = std::make_shared<Waiter>(selector);
  _waiters.push_back(waiter);
  const auto signalled = [&] { return _closed || closing || waiter->signalled; };
  try {
    while (!_closed && !closing) {
      waiter->signalled = false;
      if (timeout < 0) waiter->condition.wait(lock, signalled);
      else if (!waiter->condition.wait_until(lock, deadline, signalled)) break;
      if (!_closed && !closing && started) message = take(selector, check, incompatible);
      if (message || incompatible) break;
    }
  } catch (...) {
    std::erase(_waiters, waiter);
    throw;
  }
  std::erase(_waiters, waiter);
  return message;
}

std::vector<Message::Ptr> DeliveryState::snapshot(const Selector* selector) {
  std::lock_guard lock(_mutex);
  if (_closed) throw IllegalStateException("destination is deleted");
  const auto now = DeliveryClock::now();
  std::vector<Message::Ptr> messages;
  bool recovered = false;
  for (auto band = _bands.rbegin(); band != _bands.rend(); ++band) {
    for (const auto& message : *band) {
      if (message->isPublished() && !message->isExpired(now) && (!selector || selector->matches(*message))) {
        recovered |= message->_recoveryBatch != 0;
        messages.push_back(message->copy());
      }
    }
  }
  if (recovered) {
    // Walk the same eligible heads as claim, removing each from this snapshot.
    std::vector<Message::Ptr> ordered;
    ordered.reserve(messages.size());
    while (!messages.empty()) {
      auto next = std::find_if(messages.begin(), messages.end(), [&](const auto& candidate) {
        if (!candidate->_recoveryBatch) return true;
        return std::none_of(messages.begin(), messages.end(), [&](const auto& prior) {
          return prior->_recoveryBatch == candidate->_recoveryBatch && prior->_receiveSequence < candidate->_receiveSequence;
        });
      });
      ordered.push_back(std::move(*next));
      messages.erase(next);
    }
    return ordered;
  }
  return messages;
}

void DeliveryState::close() {
  std::vector<std::shared_ptr<Waiter>> waiters;
  {
    std::lock_guard lock(_mutex);
    _closed = true;
    notifyListeners();
    _listeners.clear();
    waiters.swap(_waiters);
    for (auto& band : _bands) band.clear();
    _nonEmpty = 0;
    _special = 0;
  }
  for (const auto& waiter : waiters) waiter->condition.notify_one();
}

Destination::Destination(Exchange& exchange, destination::Type type, std::string name, Poco::Path path,
                         Poco::UUID owner)
    : _exchange(exchange), _type(type), _name(std::move(name)), _path(std::move(path)), _owner(owner),
      _uri(destination::TypeName(type) + "://" + _name), _hash(destination::hash(type, _name)),
      _logger(Poco::Logger::get("tiny_mq.destination")) {
  if (isQueueFamily()) _queue = std::make_shared<DeliveryState>();
  if (!_owner.isNull()) return;

  Poco::File(_path).createDirectories();
  Poco::UUIDGenerator::defaultGenerator().createRandom().copyTo(_storageEpoch.data());
  // Keep the historical queue storage identity and transaction log layout.
  static const Poco::UUID storageNamespace("a3b4c5d6-e7f8-4a5b-8c9d-0e1f2a3b4c5d");
  auto storageId = Poco::UUIDGenerator::defaultGenerator().createFromName(storageNamespace, _uri);
  if (isQueueFamily()) {
    _storage = std::make_shared<Storage>(storageId, _path);
    _storage->start();
    replay(_queue, *_storage);
  }
}

Destination::~Destination() {
  try {
    shutdown();
  } catch (const std::exception& error) {
    poco_error(_logger.get(), std::string("destination teardown: ") + error.what());
  } catch (...) {
    poco_error(_logger.get(), "destination teardown failed");
  }
}

destination::Type Destination::type() const { return _type; }
bool Destination::isQueueFamily() const {
  return _type == destination::Queue || _type == destination::TemporaryQueue;
}
bool Destination::isTopicFamily() const { return !isQueueFamily(); }
const std::string& Destination::name() const { return _name; }
std::string Destination::typeName() const { return destination::TypeName(_type); }
const std::string& Destination::uri() const { return _uri; }
size_t Destination::hash() const { return _hash; }

void Destination::checkBroker(const Connection& connection) const {
  if (&connection._exchange.get() != &_exchange) throw std::invalid_argument("destination belongs to another Exchange");
}

void Destination::checkOwner(const Poco::UUID& owner) const {
  checkLive();
  if (!_owner.isNull() && owner != _owner) throw IllegalStateException("temporary destination belongs to another connection");
}

void Destination::checkLive() const {
  if (_deleted.load(std::memory_order_acquire)) {
    throw IllegalStateException("destination is deleted");
  }
}

Consumer::Ptr Destination::createConsumer(Session& session, std::shared_ptr<Selector> selector, bool noLocal) {
  checkBroker(session.connection());
  checkOwner(session.connection()._identity);
  if (isQueueFamily()) {
    if (noLocal) throw std::invalid_argument("noLocal is only supported for topics");
    auto consumer = Consumer::Ptr(new Consumer(*this, session, _queue, _storage, std::move(selector)));
    std::lock_guard lock(_mutex);
    checkLive();
    ++_activeConsumers;
    return consumer;
  }
  auto sub = std::make_shared<Subscription>();
  sub->selector = std::move(selector);
  sub->noLocal = noLocal;
  if (noLocal) sub->connectionId = session.connection()._identity;
  return attach(session, sub);
}

Consumer::Ptr Destination::attach(Session& session, const std::shared_ptr<Subscription>& subscription, bool requireActive) {
  auto consumer = Consumer::Ptr(new Consumer(*this, session, subscription->deliveries,
                                             subscription->storage, {}, subscription->storage != nullptr));
  std::lock_guard lock(_mutex);
  checkOwner(session.connection()._identity);
  if (requireActive && subscription->active == 0) return {};
  if (!subscription->storage && subscription->active == 0) _subscriptions.push_back(subscription);
  if (subscription->legacy) subscription->legacyConsumer = consumer;
  ++subscription->active;
  ++_activeConsumers;
  return consumer;
}

void Destination::detach(Consumer& consumer) {
  std::shared_ptr<Subscription> removed;
  {
    std::lock_guard lock(_mutex);
    if (consumer._closed) return;
    consumer._closed = true;
    --_activeConsumers;
    if (isQueueFamily()) return;
    const auto it = std::find_if(_subscriptions.begin(), _subscriptions.end(), [&](const auto& sub) {
      return sub->deliveries == consumer._queue;
    });
    if (it == _subscriptions.end()) return;
    auto& sub = *it;
    if (--sub->active == 0 && !sub->storage) {
      removed = std::move(sub);
      _subscriptions.erase(it);
    }
  }
  if (removed) {
    retire(removed, false);
    if (removed->sharedIdentity) _exchange.forgetShared(*removed->sharedIdentity, *removed);
  }
}

Consumer::Ptr Destination::createDurableConsumer(Session& session, const std::string& clientId,
                                                  const std::string& name, std::shared_ptr<Selector> selector) {
  if (_type != destination::Topic) throw Poco::RuntimeException("legacy durable subscribers require a permanent topic");
  checkBroker(session.connection());
  std::lock_guard legacyLock(_legacyMutex);
  const auto key = std::make_pair(clientId, name);
  auto found = _legacy.find(key);
  std::shared_ptr<Subscription> sub;
  if (found == _legacy.end()) {
    sub = std::make_shared<Subscription>();
    sub->legacy = true;
    sub->selector = selector;
    const auto directory = clientId.empty() ? "durable-" + name : "durable-" + clientId + "-" + name;
    sub->path = Poco::Path(_path).append(directory).makeDirectory();
    Poco::File(sub->path).createDirectories();
    static const Poco::UUID storageNamespace("b5c6d7e8-f9a0-4b5c-9d0e-1f2a3b4c5d6e");
    const auto storageName = clientId.empty() ? _uri + "/" + name : _uri + "/" + clientId + "/" + name;
    auto id = Poco::UUIDGenerator::defaultGenerator().createFromName(storageNamespace, storageName);
    sub->storage = std::make_shared<Storage>(id, sub->path);
    sub->storage->start();
    replay(sub->deliveries, *sub->storage);
    {
      std::lock_guard lock(_mutex);
      _subscriptions.push_back(sub);
    }
    _legacy.emplace(key, sub);
  } else {
    sub = found->second;
    std::lock_guard lock(_mutex);
    if (sub->active != 0) throw Poco::RuntimeException("legacy durable subscription already has a consumer");
    sub->selector = std::move(selector);
  }
  return attach(session, sub);
}

void Destination::deleteSubscription(const std::string& clientId, const std::string& name) {
  std::lock_guard legacyLock(_legacyMutex);
  const auto it = _legacy.find(std::make_pair(clientId, name));
  if (it == _legacy.end()) return;
  auto sub = it->second;
  if (auto consumer = sub->legacyConsumer.lock()) consumer->close();
  {
    std::lock_guard lock(_mutex);
    std::erase(_subscriptions, sub);
  }
  _legacy.erase(it);
  retire(sub, true);
}

void Destination::retire(const std::shared_ptr<Subscription>& subscription, bool removeFiles) {
  const auto& deliveries = subscription->deliveries;
  {
    std::lock_guard lock(deliveries->storageMutex);
    deliveries->retired = true;
    deliveries->close();
    _scheduler.cancel(*deliveries);
    if (subscription->storage) subscription->storage->stop();
  }
  if (removeFiles) {
    std::error_code error;
    std::filesystem::remove_all(subscription->path.toString(), error);
    if (error) poco_warning(_logger.get(), "could not remove retired subscription storage: " + error.message());
  }
}

void Destination::deleteTemporary(const Poco::UUID& owner) {
  {
    std::lock_guard lock(_mutex);
    checkOwner(owner);
    if (_activeConsumers != 0) throw IllegalStateException("temporary destination has active consumers");
    _deleted.store(true, std::memory_order_release);
  }
  shutdown();
}

void Destination::shutdown() {
  _deleted.store(true, std::memory_order_release);
  _scheduler.stop();
  if (_queue) {
    std::lock_guard storageLock(_queue->storageMutex);
    _queue->close();
    _queue->retired = true;
    if (_storage) _storage->stop();
  }
  std::list<std::shared_ptr<Subscription>> subscriptions;
  {
    std::lock_guard lock(_mutex);
    subscriptions.swap(_subscriptions);
  }
  for (const auto& sub : subscriptions) retire(sub, false);
}

void TemporaryDestination::remove(Connection& connection) {
  checkBroker(connection);
  _exchange.removeTemporary(*this, connection._identity);
}

Poco::UUID Destination::storageKey(int64_t sequence) const {
  // The original message UUID survives serialization. The record index has
  // a separate ID so re-sending one message cannot alias two persistent claims.
  auto bytes = _storageEpoch;
  std::memcpy(bytes.data() + 8, &sequence, sizeof(sequence));
  Poco::UUID key;
  key.copyFrom(bytes.data());
  return key;
}

void Destination::persist(Storage& storage, Message& message) {
  message.refreshCachedStorageBytes();
  const auto record = storage.appendDelivery(message._storageId, message._cachedStorageBytes);
  message._storageTomId = record.tomId;
  message._storageOffset = record.offset;
  message._cachedStorageBytes.clear();
}

Message::Ptr Destination::decodeStored(const std::vector<char>& bytes) {
  if (bytes.empty()) throw Poco::DataFormatException("empty stored delivery");
  auto message = makeMessageShell(static_cast<Message::Type>(static_cast<uint8_t>(bytes.front())));
  message->fromBytes(BytesVector(bytes.begin() + 1, bytes.end()));
  return message;
}

void Destination::replay(const std::shared_ptr<DeliveryState>& queue, Storage& storage) {
  auto& journal = _exchange._journal;
  journal.sweepAttempts(storage);
  auto records = storage.scan();
  std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.first.tomId, lhs.first.offset) < std::tie(rhs.first.tomId, rhs.first.offset);
  });
  for (const auto& [record, data] : records) {
    auto message = decodeStored(data);
    message->_storageId.copyFrom(record.header.uuid.data());
    message->_storageTomId = record.tomId;
    message->_storageOffset = record.offset;
    message->_storedAttempt = record.header.attempt != 0;
    if (message->isExpired(DeliveryClock::now())) {
      if (!storage.removeDelivery(record)) throw Poco::IOException("cannot remove expired delivery");
      continue;
    }
    retry(queue, &storage, std::move(message), true);
  }
}

Message::Ptr Destination::prepareCopy(const Producer& producer, Message::Ptr copy, int64_t number) {
  copy->_cachedStorageBytes.clear();
  copy->_storageTomId = std::numeric_limits<Poco::UInt32>::max();
  copy->_nextRedeliveryAt = 0;
  copy->_attemptSidecar = false;
  copy->_storedAttempt = false;
  copy->_recoveryBatch = 0;
  copy->_publication.reset();
  copy->_number = number;
  if (copy->uuid.isNull()) copy->uuid = producer._session.load(std::memory_order_acquire)->createRandomUUID();
  return copy;
}

void Destination::save(const Producer& producer, const Message& message, int64_t acceptedAt) {
  if (isTopicFamily()) {
    publish(producer, message, acceptedAt);
    return;
  }
  auto copy = prepareCopy(producer, message.copy(), _messageCounter.fetch_add(1, std::memory_order_relaxed));
  if (_storage && copy->isPersistent()) {
    copy->_storageId = storageKey(copy->_number);
    persist(*_storage, *copy);
  }
  if (copy->jmsHeaders.deliveryTime <= acceptedAt) _queue->insert(std::move(copy), false);
  else _scheduler.enqueueOrSchedule(_queue, std::move(copy));
}

bool Destination::matches(const Subscription& sub, const Producer& producer, const Message& message) const {
  if (sub.legacy && sub.active == 0 && !message.isPersistent()) return false;
  if (sub.noLocal) {
    const auto& connection = producer._session.load(std::memory_order_acquire)->connection();
    if (connection._identity == sub.connectionId ||
        (!sub.clientId.empty() && sub.clientId == connection.clientID())) return false;
  }
  return !sub.selector || sub.selector->matches(message);
}

void Destination::preparePublication(const Producer& producer, const Message::Ptr& message,
                                     std::vector<Publication>& outputs, int64_t now) {
  checkLive();
  if (message->isExpired(now)) return;
  auto stage = [&](const std::shared_ptr<DeliveryState>& queue, const std::shared_ptr<Storage>& storage,
                   Message::Ptr copy) {
    if (storage && copy->isPersistent()) {
      copy->_storageId = storageKey(copy->_number);
      copy->refreshCachedStorageBytes();
    }
    auto persistent = copy->isPersistent() ? storage : nullptr;
    const auto priority = copy->jmsHeaders.priority;
    outputs.push_back({this, queue, std::move(persistent), std::move(copy), priority});
  };
  if (isQueueFamily()) {
    stage(_queue, _storage, message);
  } else {
    std::lock_guard lock(_mutex);
    checkLive();
    const auto number = _messageCounter.load(std::memory_order_relaxed);
    _messageCounter.store(number + 1, std::memory_order_relaxed);
    for (const auto& sub : _subscriptions) {
      if (matches(*sub, producer, *message)) {
        stage(sub->deliveries, sub->storage, prepareCopy(producer, message->copy(), number));
      }
    }
  }
}

void Destination::publish(const Producer& producer, const Message& message, int64_t acceptedAt) {
  std::shared_ptr<Subscription> first;
  std::vector<std::shared_ptr<Subscription>> rest;
  int64_t number;
  {
    std::lock_guard lock(_mutex);
    checkLive();
    for (const auto& sub : _subscriptions) {
      if (!matches(*sub, producer, message)) continue;
      if (first) rest.push_back(sub);
      else first = sub;
    }
    if (!first) return;
    number = _messageCounter.load(std::memory_order_relaxed);
    _messageCounter.store(number + 1, std::memory_order_relaxed);
  }

  auto dispatch = [&](const Subscription& sub) {
    auto copy = prepareCopy(producer, message.copy(), number);
    if (sub.storage && copy->isPersistent()) {
      copy->_storageId = storageKey(copy->_number);
    }
    deliver(sub, std::move(copy), acceptedAt);
  };
  dispatch(*first);
  for (const auto& sub : rest) dispatch(*sub);
}

void Destination::deliver(const Subscription& subscription, Message::Ptr message, int64_t acceptedAt) {
  auto publish = [&] {
    if (message->jmsHeaders.deliveryTime <= acceptedAt) subscription.deliveries->insert(std::move(message), false);
    else _scheduler.enqueueOrSchedule(subscription.deliveries, std::move(message));
  };
  if (subscription.storage && message->isPersistent()) {
    std::lock_guard lock(subscription.deliveries->storageMutex);
    if (subscription.deliveries->retired) return;
    persist(*subscription.storage, *message);
    publish();
  } else {
    publish();
  }
}

void Destination::enqueueOrSchedule(const std::shared_ptr<DeliveryState>& queue, Message::Ptr message) {
  enqueueOrSchedule(queue, std::move(message), DeliveryClock::now());
}

void Destination::enqueueOrSchedule(const std::shared_ptr<DeliveryState>& queue, Message::Ptr message, int64_t now) {
  const auto readyAt = std::max(message->jmsHeaders.deliveryTime, message->_nextRedeliveryAt);
  if (readyAt <= now) queue->enqueue(std::move(message));
  else _scheduler.enqueueOrSchedule(queue, std::move(message));
}

void Destination::setRedeliveryPolicy(RedeliveryPolicy policy) {
  if (policy.deadLetterQueue && (policy.deadLetterQueue.get() == this || !policy.deadLetterQueue->isQueueFamily() ||
                                 &policy.deadLetterQueue->_exchange != &_exchange)) {
    throw Poco::InvalidArgumentException("dead letter destination must be a different queue");
  }
  if (policy.backoffMs < 0 || policy.maxBackoffMs < 0) {
    throw Poco::InvalidArgumentException("redelivery backoff must be non-negative");
  }
  _redeliveryPolicy = std::move(policy);
}

RedeliveryPolicy Destination::redeliveryPolicy() const { return _redeliveryPolicy; }

void Destination::registerAttempt(Storage& storage, Message& message) {
  auto& journal = _exchange._journal;
  const auto& policy = _redeliveryPolicy;
  LocalTransactionJournal::DeliveryAttempt attempt;
  attempt.count = std::max(message.jmsHeaders.deliveryCount, message.jmsHeaders.redelivered ? 1 : 0);
  if (!message._attemptSidecar && policy.maxRedeliveries < 0 && !policy.backoffMs && !policy.deadLetterQueue) {
    linear_storage::Record record;
    record.tomId = message._storageTomId;
    record.offset = message._storageOffset;
    if (journal.markInlineAttempt(storage, record)) return;
  }
  attempt.limit = policy.maxRedeliveries;
  attempt.backoff = policy.backoffMs;
  attempt.maxBackoff = policy.maxBackoffMs;
  if (policy.deadLetterQueue && policy.deadLetterQueue->_storage) {
    attempt.deadLetter = journal.address(*policy.deadLetterQueue->_storage);
  }
  journal.saveAttempt(storage, message._storageId, attempt);
  message._attemptSidecar = true;
}

void Destination::retry(const std::shared_ptr<DeliveryState>& queue, Storage* storage,
                         Message::Ptr message, bool restoring) {
  using Journal = LocalTransactionJournal;
  auto& journal = _exchange._journal;
  auto target = _redeliveryPolicy.deadLetterQueue;
  Journal::DeliveryAttempt state;
  if (restoring) {
    auto saved = journal.attempt(*storage, message->_storageId);
    if (saved) {
      message->_attemptSidecar = true;
      state = std::move(*saved);
    } else if (message->_storedAttempt) {
      state.count = std::max(message->jmsHeaders.deliveryCount, message->jmsHeaders.redelivered ? 1 : 0);
    } else {
      enqueueOrSchedule(queue, std::move(message));
      return;
    }
    if (state.phase == Journal::DeliveryAttempt::scheduled) {
      message->jmsHeaders.redelivered = true;
      message->jmsHeaders.deliveryCount = state.count;
      message->_nextRedeliveryAt = state.nextRedeliveryAt;
      enqueueOrSchedule(queue, std::move(message));
      return;
    }
    target.reset();
    if (state.deadLetter) {
      for (const auto& [hash, destination] : _exchange._destinations) {
        if (destination->_storage && journal.address(*destination->_storage).path == state.deadLetter->path) {
          target = destination;
          break;
        }
      }
    }
  } else {
    state.count = std::max(message->jmsHeaders.deliveryCount, message->jmsHeaders.redelivered ? 1 : 0);
    state.limit = _redeliveryPolicy.maxRedeliveries;
    state.backoff = _redeliveryPolicy.backoffMs;
    state.maxBackoff = _redeliveryPolicy.maxBackoffMs;
    if (target && target->_storage) state.deadLetter = journal.address(*target->_storage);
  }
  const auto now = DeliveryClock::now();
  const bool expired = message->isExpired(now);
  if (expired || state.phase == Journal::DeliveryAttempt::terminal ||
      (state.limit >= 0 && state.count >= state.limit)) {
    if (storage && !expired && state.phase != Journal::DeliveryAttempt::terminal) {
      state.phase = Journal::DeliveryAttempt::terminal;
      journal.saveAttempt(*storage, message->_storageId, state);
    }
    std::vector<Journal::Operation> operations;
    if (storage) operations.push_back({journal.address(*storage), message->_storageId, {}, storage, {}});
    if (!expired && (target || state.deadLetter)) {
      message->jmsHeaders.deliveryCount = state.count < INT32_MAX ? state.count + 1 : state.count;
      message->jmsHeaders.redelivered = true;
      message->_nextRedeliveryAt = 0;
      message->_recoveryBatch = 0;
      message->_storageTomId = std::numeric_limits<Poco::UInt32>::max();
      message->setStringProperty("JMSXDeadLetterReason", "maxRedeliveries exceeded");
      if (storage && state.deadLetter) {
        message->_storageId = Poco::UUIDGenerator::defaultGenerator().createRandom();
        message->refreshCachedStorageBytes();
        operations.push_back({*state.deadLetter, message->_storageId, std::move(message->_cachedStorageBytes),
                              target ? target->_storage.get() : nullptr, {}});
      }
      journal.decide(operations, "dlq");
      if (target) {
        if (operations.size() == 2) {
          message->_storageTomId = operations.back().applied.tomId;
          message->_storageOffset = operations.back().applied.offset;
        }
        try {
          target->enqueueOrSchedule(target->_queue, std::move(message));
        } catch (...) {
          Journal::stopAfterDecision("DLQ publication failed");
        }
      }
    } else {
      journal.decide(operations, "dlq");
    }
    return;
  }
  if (state.count < INT32_MAX) ++state.count;
  state.phase = Journal::DeliveryAttempt::scheduled;
  const auto delay = std::ldexp(static_cast<long double>(state.backoff), std::min(state.count - 1, 63));
  const auto bounded = static_cast<int64_t>(std::min(delay, static_cast<long double>(state.maxBackoff)));
  state.nextRedeliveryAt = bounded > 0 ? DeliveryClock::add(now, bounded) : 0;
  if (storage) {
    journal.saveAttempt(*storage, message->_storageId, state);
    message->_attemptSidecar = true;
    Journal::point("retry", "scheduled");
  }
  message->jmsHeaders.redelivered = true;
  message->jmsHeaders.deliveryCount = state.count;
  message->_nextRedeliveryAt = state.nextRedeliveryAt;
  enqueueOrSchedule(queue, std::move(message));
}
}  // namespace tiny_mq
