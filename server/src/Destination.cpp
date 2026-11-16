#include "Destination.h"
#include "Connection.h"
#include "DestinationHash.h"
#include "Exceptions.h"
#include "Session.h"
#include "TransactionBuffer.h"
#include <Poco/File.h>
#include <Poco/Format.h>
#include <Poco/Timestamp.h>
#include <bit>
#include <cstring>
#include <filesystem>

namespace tiny_mq {
namespace {
int64_t nowMs() { return Poco::Timestamp().epochMicroseconds() / 1000; }

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
  std::vector<std::shared_ptr<Waiter>> notify;
  {
    std::lock_guard lock(_mutex);
    if (_closed) return;
    const auto band = std::clamp(message->jmsHeaders.priority, 0, QueueT::kBands - 1);
    const auto& added = _bands[band].emplace_back(std::move(message));
    _nonEmpty |= 1u << band;
    if (!_waiters.empty() && added->jmsHeaders.expiration != 0 && added->isExpired(nowMs())) return;
    for (const auto& waiter : _waiters) {
      if (!waiter->signalled && (!waiter->selector || waiter->selector->matches(*added))) {
        notify.push_back(waiter);
        waiter->signalled = true;
      }
    }
  }
  for (const auto& waiter : notify) waiter->condition.notify_one();
}

Message::Ptr DeliveryState::take(const Selector* selector) {
  if (!selector) {
    if (_nonEmpty == 0) return {};
    const auto band = std::bit_width(_nonEmpty) - 1;
    auto& messages = _bands[band];
    auto message = std::move(messages.front());
    messages.pop_front();
    if (messages.empty()) _nonEmpty &= ~(1u << band);
    return message;
  }

  for (unsigned available = _nonEmpty; available != 0;) {
    const auto band = std::bit_width(available) - 1;
    auto& messages = _bands[band];
    auto match = std::find_if(messages.begin(), messages.end(), [selector](const auto& message) {
      return selector->matches(*message);
    });
    if (match != messages.end()) {
      auto message = std::move(*match);
      if (match == messages.begin()) messages.pop_front();
      else messages.erase(match);
      if (messages.empty()) _nonEmpty &= ~(1u << band);
      return message;
    }
    available &= ~(1u << band);
  }
  return {};
}

Message::Ptr DeliveryState::claim(const Selector* selector, int64_t& timeout) {
  std::unique_lock lock(_mutex);
  if (_closed) return {};
  auto message = take(selector);
  if (message || timeout == 0) return message;

  const auto waiter = std::make_shared<Waiter>(selector);
  _waiters.push_back(waiter);
  using Clock = std::chrono::steady_clock;
  constexpr auto longestWait = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::days(365));
  const auto end = timeout < 0 ? Clock::time_point{} :
      Clock::now() + std::min(longestWait, std::chrono::microseconds(timeout));
  const auto signalled = [&] { return _closed || waiter->signalled; };
  while (!_closed) {
    waiter->signalled = false;
    if (timeout < 0) waiter->condition.wait(lock, signalled);
    else if (!waiter->condition.wait_until(lock, end, signalled)) break;
    if (_closed) break;
    message = take(selector);
    if (message) break;
  }
  std::erase(_waiters, waiter);
  if (timeout > 0) {
    timeout = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(end - Clock::now()).count());
  }
  return message;
}

std::vector<Message::Ptr> DeliveryState::snapshot(const Selector* selector) {
  std::lock_guard lock(_mutex);
  if (_closed) throw IllegalStateException("destination is deleted");
  const auto now = nowMs();
  std::vector<Message::Ptr> messages;
  for (auto band = _bands.rbegin(); band != _bands.rend(); ++band) {
    for (const auto& message : *band) {
      if (!message->isExpired(now) && (!selector || selector->matches(*message))) {
        messages.push_back(message->copy());
      }
    }
  }
  return messages;
}

void DeliveryState::close() {
  std::vector<std::shared_ptr<Waiter>> waiters;
  {
    std::lock_guard lock(_mutex);
    _closed = true;
    waiters.swap(_waiters);
    for (auto& band : _bands) band.clear();
    _nonEmpty = 0;
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
    _transactionBuffer = std::make_unique<TransactionBuffer>(_path, _storage);
    _transactionBuffer->recover();
    _storage->start();
    replay(_queue, *_storage, _scheduler);
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
    replay(sub->deliveries, *sub->storage, _scheduler);
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

void Destination::replay(const std::shared_ptr<DeliveryState>& queue, Storage& storage, DeliveryScheduler& scheduler) {
  auto records = storage.scan();
  std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.first.tomId, lhs.first.offset) < std::tie(rhs.first.tomId, rhs.first.offset);
  });
  for (const auto& [record, data] : records) {
    if (data.empty()) throw Poco::DataFormatException("empty stored delivery");
    auto message = makeMessageShell(static_cast<Message::Type>(static_cast<uint8_t>(data.front())));
    message->fromBytes(BytesVector(data.begin() + 1, data.end()));
    message->_storageId.copyFrom(record.header.uuid.data());
    message->_storageTomId = record.tomId;
    message->_storageOffset = record.offset;
    if (message->isExpired(nowMs())) {
      storage.removeAsync(record);
      continue;
    }
    if (message->jmsHeaders.deliveryTime < 0) {
      poco_warning(Poco::Logger::get("tiny_mq.destination"), "negative stored JMSDeliveryTime, delivering immediately");
      message->jmsHeaders.deliveryTime = 0;
    }
    scheduler.enqueueOrSchedule(queue, std::move(message));
  }
}

void Destination::save(const Producer& producer, const Message& message) {
  const auto& transaction = producer.transactionId();
  if (isTopicFamily() && transaction.empty()) {
    publish(producer, message);
    return;
  }

  auto copy = message.copy();
  copy->_cachedStorageBytes.clear();
  copy->_storageTomId = std::numeric_limits<Poco::UInt32>::max();
  if (isQueueFamily()) copy->_number = _messageCounter.fetch_add(1, std::memory_order_relaxed);
  if (copy->uuid.isNull()) copy->uuid = producer._session->createRandomUUID();
  if (!transaction.empty()) {
    auto& staged = *producer._transactQueue;
    staged.push_back(copy);
    try {
      if (_transactionBuffer && copy->isPersistent()) {
        copy->_storageId = storageKey(copy->_number);
        copy->refreshCachedStorageBytes();
        _transactionBuffer->addMessage(transaction, copy->_storageId, copy->_cachedStorageBytes);
      }
    } catch (...) {
      staged.pop_back();
      throw;
    }
    return;
  }
  if (_storage && copy->isPersistent()) {
    copy->_storageId = storageKey(copy->_number);
    persist(*_storage, *copy);
  }
  enqueueOrSchedule(_queue, std::move(copy));
}

void Destination::deliverCommitted(const Producer& producer, Message::Ptr message) {
  checkLive();
  if (isTopicFamily()) {
    publish(producer, *message);
    return;
  }
  if (_storage && message->isPersistent()) {
    const auto record = _storage->record(message->_storageId);
    if (record.tomId == std::numeric_limits<Poco::UInt32>::max()) {
      if (message->isExpired(nowMs())) return;
      throw Poco::IOException("cannot find a committed delivery");
    }
    message->_storageTomId = record.tomId;
    message->_storageOffset = record.offset;
    message->_cachedStorageBytes.clear();
  }
  enqueueOrSchedule(_queue, std::move(message));
}

void Destination::publish(const Producer& producer, const Message& message) {
  std::shared_ptr<Subscription> first;
  std::vector<std::shared_ptr<Subscription>> rest;
  int64_t number;
  {
    std::lock_guard lock(_mutex);
    checkLive();
    for (const auto& sub : _subscriptions) {
      if (sub->legacy && sub->active == 0 && !message.isPersistent()) continue;
      if (sub->noLocal) {
        const auto& connection = producer._session->connection();
        if (connection._identity == sub->connectionId ||
            (!sub->clientId.empty() && sub->clientId == connection.clientID())) continue;
      }
      if (sub->selector && !sub->selector->matches(message)) continue;
      if (first) rest.push_back(sub);
      else first = sub;
    }
    if (!first) return;
    number = _messageCounter.load(std::memory_order_relaxed);
    _messageCounter.store(number + 1, std::memory_order_relaxed);
  }

  auto dispatch = [&](const Subscription& sub) {
    auto copy = message.copy();
    copy->_cachedStorageBytes.clear();
    copy->_storageTomId = std::numeric_limits<Poco::UInt32>::max();
    copy->_number = number;
    if (copy->uuid.isNull()) copy->uuid = producer._session->createRandomUUID();
    if (sub.storage && copy->isPersistent()) {
      copy->_storageId = sub.legacy ? copy->uuid : storageKey(copy->_number);
    }
    deliver(sub, std::move(copy));
  };
  dispatch(*first);
  for (const auto& sub : rest) dispatch(*sub);
}

void Destination::deliver(const Subscription& subscription, Message::Ptr message) {
  if (subscription.storage && message->isPersistent()) {
    std::lock_guard lock(subscription.deliveries->storageMutex);
    if (subscription.deliveries->retired) return;
    persist(*subscription.storage, *message);
    enqueueOrSchedule(subscription.deliveries, std::move(message));
  } else {
    enqueueOrSchedule(subscription.deliveries, std::move(message));
  }
}

void Destination::enqueueOrSchedule(const std::shared_ptr<DeliveryState>& queue, Message::Ptr message) {
  if (message->jmsHeaders.deliveryTime == 0) queue->enqueue(std::move(message));
  else _scheduler.enqueueOrSchedule(queue, std::move(message));
}

void Destination::setRedeliveryPolicy(RedeliveryPolicy policy) {
  if (policy.deadLetterQueue && (policy.deadLetterQueue.get() == this || !policy.deadLetterQueue->isQueueFamily())) {
    throw Poco::InvalidArgumentException("dead letter destination must be a different queue");
  }
  _redeliveryPolicy = std::move(policy);
}

RedeliveryPolicy Destination::redeliveryPolicy() const { return _redeliveryPolicy; }

void Destination::deadLetter(Message::Ptr message, std::string reason) noexcept {
  try {
    auto dlq = _redeliveryPolicy.deadLetterQueue;
    if (!dlq) {
      poco_warning(_logger.get(), "dead-letter: no DLQ configured for " + _uri);
      return;
    }
    dlq->checkLive();
    message->jmsHeaders.deliveryTime = 0;
    message->setStringProperty("JMSXDeadLetterReason", std::move(reason));
    message->_storageTomId = std::numeric_limits<Poco::UInt32>::max();
    if (dlq->_storage && message->isPersistent()) {
      message->_storageId = dlq->storageKey(dlq->_messageCounter++);
      persist(*dlq->_storage, *message);
    }
    dlq->enqueueOrSchedule(dlq->_queue, std::move(message));
  } catch (const std::exception& error) {
    poco_error(_logger.get(), std::string("dead-letter: ") + error.what());
  } catch (...) {
    poco_error(_logger.get(), "dead-letter failed");
  }
}
}  // namespace tiny_mq
