//
// Created by Alexander Bychuk on 04.11.2021.
//

#include "Destination.h"
#include "Session.h"
#include "LogTracer.h"
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Exception.h>
#include <Poco/Format.h>
#include <Poco/Timestamp.h>
#include <algorithm>
#include <cstring>
#include "DestinationHash.h"
#include "TextMessage.h"
#include "BytesMessage.h"
#include "MapMessage.h"
#include "StreamMessage.h"
#include "ObjectMessage.h"

namespace tiny_mq {

Destination::Destination(destination::Type type, std::string name, Poco::Path path)
    : _type(type),
      _name(std::move(name)),
      _path(std::move(path)),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.destination.%s.%s", destination::TypeName(_type), _name))),
      _uri(Poco::format("%s://%s", destination::TypeName(_type), _name)),
      _hash(destination::hash(_type, _name)) {
  TRACE(_logger);
  Poco::File f(_path);
  f.createDirectories();
  // Deterministic storage UUID so the index file survives Exchange restarts.
  // Derived from the destination URI (type + name) so it's unique and stable.
  static const Poco::UUID kStorageNs("a3b4c5d6-e7f8-4a5b-8c9d-0e1f2a3b4c5d");
  Poco::UUID storageId = Poco::UUIDGenerator::defaultGenerator().createFromName(kStorageNs, _uri);
  _storage = std::make_shared<linear_storage::ConcurrentLinearStorage>(storageId, _path);
  _storage->start();

  _transactionBuffer = std::make_shared<TransactionBuffer>(_path, _storage);
  _transactionBuffer->recover();

  _scheduler = std::make_unique<DeliveryScheduler>();

  Poco::FileOutputStream fo(_path.toString() + "/meta.info");
  fo << "destination: " << _name << '\n';
  fo << "type: " << destination::TypeName(_type) << '\n';
  fo << "hash: " << _hash << '\n';
}

Consumer::Ptr Destination::createDefaultConsumer(Session& session) {
  if ((_type == destination::Queue || _type == destination::TemporaryQueue)) {
    if (_queue == nullptr) {
      _queue = std::make_shared<QueueT>();
    }
    _defaultConsumer = Consumer::Ptr(new Consumer(*this, session, _queue, session.createRandomUUID(), _path, _storage, _transactionBuffer));
    _consumers.emplace(_defaultConsumer->id(), _defaultConsumer);
  }
  return _defaultConsumer;
}

void Destination::save(const Producer& producer, const Message& message) {
  TRACE(_logger);
  int64_t number = _messageCounter++;
  bool queueFamily = isQueueFamily();
  for (auto& item : _consumers) {
    item.second->push(number, producer, message);
    if (queueFamily) break;
  }

  // Buffer persistent messages for offline durable subscriptions (topic-family only).
  // Transactional messages arrive here before commit, so we skip them — they will be
  // handled in deliverCommitted() once the producer commits.
  if (isTopicFamily() && message.isPersistent() && producer.transactionId().empty()) {
    for (auto& [subName, sub] : _durableSubs) {
      if (!sub.activeConsumerUuid.isNull()) continue;  // online — already handled above
      if (sub.selector && !sub.selector->matches(message)) continue;
      persistToOfflineSub(sub, message);
    }
  }
}

namespace {
// Extract JMSPriority from storage-format bytes for correct band placement on replay.
//
// Storage layout: [1 type-byte][toBytes() output]
// toBytes() 0x02 header offsets (relative to start of _cachedStorageBytes):
//   [0]  type byte (prefix, NOT part of toBytes)
//   [1]  magic (0x02)
//   [2..9]  int64_t number
//   [10..25] UUID (16 bytes)
//   [26] reliability
//   [27..34] timestamp (8 bytes)
//   [35..42] expiration (8 bytes)
//   [43..50] deliveryTime (8 bytes)
//   [51..54] priority (int32_t)  ← extracted here
int32_t priorityFromStorageBytes(const std::vector<char>& data) noexcept {
  constexpr size_t kOffset = 1   // type byte
                           + 1   // magic
                           + 8   // number
                           + 16  // uuid
                           + 1   // reliability
                           + 8   // timestamp
                           + 8   // expiration
                           + 8;  // deliveryTime  (total = 51)
  constexpr int32_t kDefault = 4;
  if (data.size() < kOffset + sizeof(int32_t)) return kDefault;
  if (static_cast<uint8_t>(data[1]) != 0x02) return kDefault;  // pre-header format: use default
  int32_t prio = kDefault;
  std::memcpy(&prio, data.data() + kOffset, sizeof(prio));
  return std::clamp(prio, int32_t{0}, int32_t{9});
}

// Extract JMSDeliveryTime (spec 13) from storage-format bytes so replay can decide
// whether a record is still pending — see the offset table above; deliveryTime is
// the 8 bytes immediately before priority, i.e. at offset 43.
int64_t deliveryTimeFromStorageBytes(const std::vector<char>& data) noexcept {
  constexpr size_t kOffset = 1 + 1 + 8 + 16 + 1 + 8 + 8;  // = 43
  constexpr size_t kSize = 8;
  if (data.size() < kOffset + kSize) return 0;
  if (static_cast<uint8_t>(data[1]) != 0x02) return 0;  // pre-header format: never delayed
  int64_t deliveryTime = 0;
  std::memcpy(&deliveryTime, data.data() + kOffset, kSize);
  // Defense-in-depth (spec 13 review round 2, B4): a raw 8-byte field read
  // straight out of a storage record is untrusted the moment it's out of
  // range of anything a legitimate send could ever have produced — file
  // corruption, an old/foreign record, or a future writer with a bug.
  // Treat it as garbage rather than let it reach the scheduler unchecked:
  // clamp to 0 (immediate delivery, matching "deliveryTime unset") and log,
  // so "message arrived immediately instead of on schedule" is diagnosable
  // instead of silently indistinguishable from a normal replay.
  if (deliveryTime < 0 || deliveryTime > Poco::Timestamp().epochMicroseconds() / 1000 + kMaxFutureHeaderMs) {
    poco_warning(Poco::Logger::get("tiny_mq.destination"),
                 Poco::format("deliveryTimeFromStorageBytes: out-of-range deliveryTime=%?d in "
                              "storage record — delivering immediately instead of scheduling",
                              deliveryTime));
    return 0;
  }
  return deliveryTime;
}

// Factory: create an empty shell message of the given type for replay
tiny_mq::Message::Ptr makeMessageShell(tiny_mq::Message::Type type) {
  switch (type) {
    case tiny_mq::Message::TEXT_MESSAGE:   return std::make_shared<tiny_mq::TextMessage>();
    case tiny_mq::Message::BYTES_MESSAGE:  return std::make_shared<tiny_mq::BytesMessage>();
    case tiny_mq::Message::MAP_MESSAGE:    return std::make_shared<tiny_mq::MapMessage>();
    case tiny_mq::Message::STREAM_MESSAGE: return std::make_shared<tiny_mq::StreamMessage>();
    case tiny_mq::Message::OBJECT_MESSAGE: return std::make_shared<tiny_mq::ObjectMessage>();
    default: return nullptr;
  }
}
}  // anonymous namespace

// Replay all non-deleted records from an arbitrary storage into a queue.
// Sets the cached-bytes and record-location fields so recv() and acknowledgeOn()
// can use the fast (no-lookup) path. A record whose deliveryTime has not yet
// elapsed is armed on the scheduler instead of being made immediately visible
// (JMS 2.0 § 7.8, spec 13) — a still-pending delay must survive both destination
// restart and durable-subscriber reconnect.
/*static*/ void Destination::replayFromStorage(std::shared_ptr<QueueT> queue,
                                               linear_storage::ConcurrentLinearStorage& storage,
                                               DeliveryScheduler& scheduler) {
  auto records = storage.scan();
  for (auto& [record, data] : records) {
    if (data.empty()) continue;
    auto msgType = static_cast<Message::Type>(static_cast<uint8_t>(data[0]));
    auto shell = makeMessageShell(msgType);
    if (!shell) continue;
    Poco::UUID uuid;
    uuid.copyFrom(record.header.uuid.data());
    shell->uuid = uuid;
    shell->reliability = Message::PERSISTENT;
    shell->_cachedStorageBytes = data;
    // Restore the original JMSPriority so the message lands in the correct band.
    // Priority is embedded in the 0x02 wire payload; extract it here so the
    // PriorityQueueT::enqueue() call below routes to the right band.
    shell->jmsHeaders.priority = priorityFromStorageBytes(data);
    shell->jmsHeaders.deliveryTime = deliveryTimeFromStorageBytes(data);
    // deliveryTimeFromStorageBytes clamps an out-of-range raw value to 0, but
    // _cachedStorageBytes above still holds the untouched raw record: a later
    // Consumer::recv() rehydrates jmsHeaders from that cache via fromBytes and
    // would silently revert this clamp (same defect class as spec 13 review
    // round 2's B2 / round 3's N15 — header and cached-bytes drifting apart).
    // Keep them in sync unconditionally; a no-op when nothing was clamped.
    shell->patchCachedDeliveryTime(shell->jmsHeaders.deliveryTime);
    shell->_storageTomId  = record.tomId;
    shell->_storageOffset = record.offset;
    scheduler.enqueueOrSchedule(queue, std::move(shell));
  }
}

void Destination::replayStoredMessages(std::shared_ptr<QueueT> queue) const {
  replayFromStorage(std::move(queue), *_storage, *_scheduler);
}

/*static*/ void Destination::persistToOfflineSub(DurableSubState& sub, const Message& message) {
  auto copy = message.copy();
  if (copy->uuid.isNull()) {
    copy->uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  }
  auto bytesData = copy->toBytes();
  std::vector<char> data;
  data.reserve(1 + bytesData.size());
  data.push_back(static_cast<char>(copy->type()));
  data.insert(data.end(), bytesData.begin(), bytesData.end());
  sub.storage->append(copy->uuid, data);
}

Consumer::Ptr Destination::createConsumer(Session& session, std::shared_ptr<Selector> selector) {
  TRACE(_logger);
  auto id = session.createRandomUUID();
  Consumer::Ptr consumer;
  switch (_type) {
    case destination::Queue:
    case destination::TemporaryQueue: {
      if (_defaultConsumer) {
        // Remove the default consumer from the map so save() routes through the
        // real consumer (which may have a selector).  Both consumers share _queue,
        // so any messages already enqueued by the default consumer are still visible.
        _consumers.erase(_defaultConsumer->id());
        _defaultConsumer.reset();
      }
      consumer = Consumer::Ptr(new Consumer(*this, session, _queue, id, _path, _storage, _transactionBuffer, selector));
      // Replay any messages committed in a previous Exchange lifetime
      if (_queue) replayStoredMessages(_queue);
    } break;
    case destination::Topic:
    case destination::TemporaryTopic:
      Poco::Path path(_path);
      path.append(id.toString()).makeDirectory();
      consumer = Consumer::Ptr(new Consumer(*this, session, std::make_shared<QueueT>(), id, path, _storage, _transactionBuffer, selector));
      break;
  }
  auto it = _consumers.emplace(id, std::move(consumer));
  if (it.second) {
    return it.first->second;
  }
  return nullptr;
}

Consumer::Ptr Destination::createDurableConsumer(Session& session, const std::string& clientID,
                                                  const std::string& subscriptionName,
                                                  std::shared_ptr<Selector> selector) {
  TRACE(_logger);
  if (!isTopicFamily()) {
    throw Poco::RuntimeException("Durable subscribers are only supported on topic destinations");
  }

  const std::string key = durableKey(clientID, subscriptionName);

  // Get or create the durable subscription state, keyed by (clientID, name).
  auto subIt = _durableSubs.find(key);
  if (subIt == _durableSubs.end()) {
    // New subscription — create dedicated storage under the topic path. The
    // directory and storage namespace are scoped by clientID so the same name
    // under different clients never collides on disk.
    const std::string dirName =
        clientID.empty() ? ("durable-" + subscriptionName)
                         : ("durable-" + clientID + "-" + subscriptionName);
    Poco::Path subPath(_path);
    subPath.append(dirName).makeDirectory();
    Poco::File(subPath).createDirectories();

    static const Poco::UUID kDurableNs("b5c6d7e8-f9a0-4b5c-9d0e-1f2a3b4c5d6e");
    const std::string storageName =
        clientID.empty() ? (_uri + "/" + subscriptionName)
                         : (_uri + "/" + clientID + "/" + subscriptionName);
    Poco::UUID storageId =
        Poco::UUIDGenerator::defaultGenerator().createFromName(kDurableNs, storageName);
    auto storage = std::make_shared<linear_storage::ConcurrentLinearStorage>(storageId, subPath);
    storage->start();

    DurableSubState state;
    state.name = subscriptionName;
    state.path = subPath;
    state.storage = std::move(storage);
    state.selector = selector;

    auto [it, _inserted] = _durableSubs.emplace(key, std::move(state));
    subIt = it;
  } else if (!subIt->second.activeConsumerUuid.isNull()) {
    throw Poco::RuntimeException(
        "Durable subscription '" + subscriptionName + "' already has an active consumer");
  }

  DurableSubState& sub = subIt->second;

  // Each reconnect may bring a new selector — update it
  sub.selector = selector;

  auto id = session.createRandomUUID();
  auto queue = std::make_shared<QueueT>();

  // Use the durable sub's own storage so persistent messages are tracked per-subscriber
  auto consumer = Consumer::Ptr(new Consumer(*this, session, queue, id, sub.path,
                                             sub.storage, _transactionBuffer, selector));

  // Replay any messages buffered while the subscriber was offline
  replayFromStorage(queue, *sub.storage, *_scheduler);

  auto [consIt, ok] = _consumers.emplace(id, consumer);
  if (!ok) return nullptr;

  sub.activeConsumerUuid = id;
  _consumerToSubName.emplace(id, key);  // composite (clientID,name) key for deleteConsumer lookup

  poco_information(_logger.get(),
                   Poco::format("durable consumer[%s] attached to subscription '%s' on %s",
                                id.toString(), subscriptionName, _uri));
  return consIt->second;
}

/*static*/ std::string Destination::durableKey(const std::string& clientID,
                                               const std::string& subscriptionName) {
  // Empty clientID -> legacy name-only key (keeps pre-clientID layout addressable).
  // \x1f (unit separator) cannot appear in JMS identifiers, so it cleanly
  // delimits the two parts of the composite key.
  if (clientID.empty()) return subscriptionName;
  return clientID + '\x1f' + subscriptionName;
}

void Destination::deleteSubscription(const std::string& clientID, const std::string& subscriptionName) {
  TRACE(_logger);
  auto subIt = _durableSubs.find(durableKey(clientID, subscriptionName));
  if (subIt == _durableSubs.end()) return;

  // Disconnect active consumer if present
  if (!subIt->second.activeConsumerUuid.isNull()) {
    deleteConsumer(subIt->second.activeConsumerUuid);
  }

  // Stop the dedicated storage worker, then wipe the directory so a future
  // re-subscription with the same name starts completely fresh.
  if (subIt->second.storage) {
    subIt->second.storage->stop();
  }
  try {
    Poco::File dir(subIt->second.path);
    if (dir.exists()) dir.remove(/*recursive=*/true);
  } catch (...) {}

  _durableSubs.erase(subIt);
  poco_information(_logger.get(),
                   Poco::format("unsubscribed durable subscription '%s' from %s",
                                subscriptionName, _uri));
}

void Destination::deleteConsumer(const Poco::UUID& id) {
  TRACE(_logger);
  auto it = _consumers.find(id);
  if (it != _consumers.end()) {
    _consumers.erase(it);
  }
  // If this is a durable consumer, mark its subscription as offline so future
  // messages are buffered in the subscription's persistent storage.
  auto subNameIt = _consumerToSubName.find(id);
  if (subNameIt != _consumerToSubName.end()) {
    auto subIt = _durableSubs.find(subNameIt->second);
    if (subIt != _durableSubs.end()) {
      subIt->second.activeConsumerUuid = Poco::UUID();  // reset to null = offline
    }
    _consumerToSubName.erase(subNameIt);
  }
}

Producer::Ptr Destination::createProducer(Session& session) {
  TRACE(_logger);
  using TokenType = QueueT::producer_token_t;
  std::unique_ptr<TokenType> token;
  if (isQueueFamily()) {
    token = std::make_unique<TokenType>(_consumers.begin()->second->getProducerToken());
  }
  auto id = session.createRandomUUID();
  auto producer = Producer::Ptr(new Producer(*this, session, id, std::move(token)));

  auto it = _producers.emplace(id, std::move(producer));
  if (it.second) {
    return it.first->second;
  }
  return nullptr;
}

void Destination::deleteProducer(const Poco::UUID& id) {
  TRACE(_logger);
  auto it = _producers.find(id);
  if (it != _producers.end()) {
    _producers.erase(it);
  }
}

destination::Type Destination::type() const { return _type; }

bool Destination::isQueueFamily() const {
  return (_type == destination::Queue) || (_type == destination::TemporaryQueue);
}

bool Destination::isTopicFamily() const {
  return (_type == destination::Topic) || (_type == destination::TemporaryTopic);
}

const std::string& Destination::name() const { return _name; }

std::string Destination::typeName() const { return destination::TypeName(_type); }

const std::string& Destination::uri() const { return _uri; }

Destination::~Destination() {
  TRACE(_logger);
  _consumers.clear();
  _producers.clear();
  if (_scheduler) {
    try {
      _scheduler->stop();
    } catch (const std::exception& e) {
      poco_error(_logger.get(), Poco::format("DeliveryScheduler::stop() threw — ignoring: %s", std::string(e.what())));
    } catch (...) {
      poco_error(_logger.get(), "DeliveryScheduler::stop() threw a non-std exception — ignoring");
    }
  }
}

size_t Destination::hash() const { return _hash; }

TransactionBuffer::Ptr Destination::getTransactionBuffer() const {
  return _transactionBuffer;
}

void Destination::commitTransaction(const std::string& transactionId) {
  TRACE(_logger);
  if (!transactionId.empty() && _transactionBuffer) {
    _transactionBuffer->commitTransaction(transactionId);
  }
}

void Destination::rollbackTransaction(const std::string& transactionId) {
  TRACE(_logger);
  if (!transactionId.empty() && _transactionBuffer) {
    _transactionBuffer->rollbackTransaction(transactionId);
  }
}

void Destination::enqueueOrSchedule(std::shared_ptr<QueueT> queue, Message::Ptr msg) {
  TRACE(_logger);
  _scheduler->enqueueOrSchedule(std::move(queue), std::move(msg));
}

void Destination::patchPendingDeliveryTime(const Poco::UUID& messageId, int64_t deliveryTime) {
  TRACE(_logger);
  if (_transactionBuffer) {
    _transactionBuffer->patchDeliveryTime(messageId, deliveryTime);
  }
}

void Destination::deliverCommitted(Message::Ptr message) {
  TRACE(_logger);
  bool queueFamily = isQueueFamily();
  for (auto& item : _consumers) {
    if (queueFamily) {
      enqueueOrSchedule(item.second->_queue, std::move(message));
      return;
    }
    enqueueOrSchedule(item.second->_queue, message->copy());
  }

  // After delivering to online consumers, buffer committed persistent messages
  // for any offline durable subscriptions on this topic.
  if (isTopicFamily() && message && message->isPersistent()) {
    for (auto& [subName, sub] : _durableSubs) {
      if (!sub.activeConsumerUuid.isNull()) continue;  // online — already received above
      if (sub.selector && !sub.selector->matches(*message)) continue;
      persistToOfflineSub(sub, *message);
    }
  }
}

}  // namespace tiny_mq
