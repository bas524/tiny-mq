//
// Created by Alexander Bychuk on 03.11.2022.
//

#include "Session.h"
#include "Connection.h"
#include "Exceptions.h"
#include "LogTracer.h"
#include "DeliveryClock.h"
#include <Poco/Format.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Timestamp.h>
#include <Poco/String.h>
#include <functional>

namespace tiny_mq {

Session::Session(Connection &connection, Session::AcknowledgeMode mode)
    : _mode(mode),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.session %s", getAcknowledgeModeName(mode)))),
      _connection(connection),
      _uuidGenerator(),
      _id(_uuidGenerator.createRandom().toString()) {
  TRACE(_logger);
  if (_mode == AcknowledgeMode::SESSION_TRANSACTED) {
    _suffix = createRandomUUID().toString();
  }
}
Session::~Session() {
  try {
    close();
  } catch (const std::exception& error) {
    poco_error(_logger.get(), std::string("session teardown: ") + error.what());
  } catch (...) {
    poco_error(_logger.get(), "session teardown failed");
  }
}

void Session::close() {
  if (SessionDispatch::current == &_dispatch) throw IllegalStateException("cannot close session from its callback");
  std::lock_guard lock(_closeMutex);
  if (_closed) return;
  _closing = true;
  _dispatch.shutdown();
  std::exception_ptr failure;
  for (const auto& [id, consumer] : _consumers) {
    try {
      consumer->close();
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  _transaction._sends.clear();
  try {
    returnClaims(_mode == SESSION_TRANSACTED);
  } catch (...) {
    if (!failure) failure = std::current_exception();
  }
  for (const auto& [id, producer] : _producers) {
    producer->_closed = true;
    producer->_session = nullptr;
  }
  for (const auto& [id, consumer] : _consumers) consumer->finishSession();
  _closed = true;
  if (failure) std::rethrow_exception(failure);
}

Destination::Ptr Session::createDestination(destination::Type type, const std::string& name) {
  ensureOpen();
  return connection().exchange().create(type, name);
}

Consumer::Ptr Session::remember(Consumer::Ptr consumer) {
  try {
    connection().watch(consumer->_queue);
    _consumers.emplace(consumer->id(), consumer);
  } catch (...) {
    consumer->finishSession();
    throw;
  }
  return consumer;
}

void Session::pruneClosedConsumers() {
  for (auto it = _consumers.begin(); it != _consumers.end();) {
    if (it->second->isClosed() && !it->second->hasClaims()) {
      it->second->_session = nullptr;
      it = _consumers.erase(it);
    } else {
      ++it;
    }
  }
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr& destination) {
  ensureOpen();
  return createConsumer(*destination);
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr& destination, const std::string& selector) {
  ensureOpen();
  return createConsumer(*destination, selector);
}

Consumer::Ptr Session::createConsumer(Destination& destination) {
  ensureOpen();
  return remember(destination.createConsumer(*this));
}

Consumer::Ptr Session::createConsumer(Destination& destination, const std::string& selector) {
  ensureOpen();
  const auto expression = Poco::trim(selector);
  return remember(destination.createConsumer(*this, expression.empty() ? nullptr : Selector::parse(expression)));
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr& destination, const std::string& selector, bool noLocal) {
  ensureOpen();
  const auto expression = Poco::trim(selector);
  return remember(destination->createConsumer(*this, expression.empty() ? nullptr : Selector::parse(expression), noLocal));
}

void Session::deleteConsumer(const Poco::UUID& id) {
  ensureOpen();
  auto it = _consumers.find(id);
  if (it == _consumers.end()) return;
  it->second->close();
  pruneClosedConsumers();
}

Producer::Ptr Session::createProducer(const Destination::Ptr& destination) {
  ensureOpen();
  return createProducer(*destination);
}

Producer::Ptr Session::createProducer(Destination& destination) {
  ensureOpen();
  destination.checkBroker(connection());
  destination.checkLive();
  auto producer = Producer::Ptr(new Producer(destination, *this));
  _producers.emplace(producer->id(), producer);
  return producer;
}

void Session::deleteProducer(const Poco::UUID& id) {
  ensureOpen();
  const auto it = _producers.find(id);
  if (it == _producers.end()) return;
  it->second->close();
  if (!it->second->_staged) {
    it->second->_session = nullptr;
    _producers.erase(it);
  }

}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr& destination, const std::string& name) {
  ensureOpen();
  return createDurableConsumer(*destination, name);
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr& destination, const std::string& name,
                                             const std::string& selector) {
  ensureOpen();
  return createDurableConsumer(*destination, name, selector);
}

Consumer::Ptr Session::createDurableConsumer(Destination& destination, const std::string& name) {
  ensureOpen();
  return createDurableConsumer(destination, name, {});
}

Consumer::Ptr Session::createDurableConsumer(Destination& destination, const std::string& name,
                                             const std::string& selector) {
  ensureOpen();
  const auto expression = Poco::trim(selector);
  return remember(destination.createDurableConsumer(*this, connection().clientID(), name,
                                                     expression.empty() ? nullptr : Selector::parse(expression)));
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr& topic, const std::string& name,
                                             const std::string& selector, bool noLocal) {
  ensureOpen();
  return remember(connection().exchange().subscribe(*this, *topic, SubscriptionKind::UnsharedDurable,
                                                    name, selector, noLocal));
}

Consumer::Ptr Session::createSharedConsumer(const Destination::Ptr& topic, const std::string& name,
                                            const std::string& selector) {
  ensureOpen();
  return remember(connection().exchange().subscribe(*this, *topic, SubscriptionKind::Shared,
                                                    name, selector, false));
}

Consumer::Ptr Session::createSharedDurableConsumer(const Destination::Ptr& topic, const std::string& name,
                                                   const std::string& selector) {
  ensureOpen();
  return remember(connection().exchange().subscribe(*this, *topic, SubscriptionKind::SharedDurable,
                                                    name, selector, false));
}

void Session::unsubscribe(Destination& destination, const std::string& name) {
  ensureOpen();
  destination.checkBroker(connection());
  destination.deleteSubscription(connection().clientID(), name);
  pruneClosedConsumers();
}

void Session::unsubscribe(const Destination::Ptr& destination, const std::string& name) {
  ensureOpen();
  unsubscribe(*destination, name);
}

void Session::unsubscribe(const std::string& name) {
  ensureOpen();
  connection().exchange().unsubscribe(connection().clientID(), name);
}

QueueBrowser Session::createQueueBrowser(const Destination::Ptr& queue, const std::string& selector) {
  ensureOpen();
  queue->checkBroker(connection());
  queue->checkOwner(connection()._identity);
  if (!queue->isQueueFamily()) throw std::invalid_argument("browser requires a queue");
  const auto expression = Poco::trim(selector);
  return QueueBrowser(queue, expression.empty() ? nullptr : Selector::parse(expression));
}

TemporaryDestination::Ptr Session::createTemporaryQueue() {
  ensureOpen();
  return connection().exchange().createTemporary(destination::TemporaryQueue, connection()._identity);
}

TemporaryDestination::Ptr Session::createTemporaryTopic() {
  ensureOpen();
  return connection().exchange().createTemporary(destination::TemporaryTopic, connection()._identity);
}

TextMessage Session::createTextMessage(std::string text, Message::Reliability reliability) {
  ensureOpen();
  return TextMessage(createRandomUUID(), std::move(text), reliability);
}

MapMessage Session::createMapMessage(Message::Reliability reliability) {
  ensureOpen();
  return {createRandomUUID(), reliability};
}

StreamMessage Session::createStreamMessage(Message::Reliability reliability) {
  ensureOpen();
  return {createRandomUUID(), reliability};
}

BytesMessage Session::createBytesMessage(BytesVector bytes, Message::Reliability reliability) {
  ensureOpen();
  return BytesMessage(createRandomUUID(), std::move(bytes), reliability);
}

BytesMessage Session::createBytesMessage(const int8_t *bytes, size_t size, Message::Reliability reliability) {
  ensureOpen();
  return BytesMessage(createRandomUUID(), bytes, size, reliability);
}

ObjectMessage Session::createObjectMessage(BytesVector body, std::string className, Message::Reliability reliability) {
  ensureOpen();
  return ObjectMessage(createRandomUUID(), std::move(body), std::move(className), reliability);
}

std::string Session::acknowledgeModeName() const {
  TRACE(_logger);
  return getAcknowledgeModeName(_mode);
}

std::string Session::getAcknowledgeModeName(Session::AcknowledgeMode mode) {
  switch (mode) {
    case AcknowledgeMode::AUTO_ACKNOWLEDGE:
      return "AUTO_ACKNOWLEDGE";
    case AcknowledgeMode::SESSION_TRANSACTED:
      return "SESSION_TRANSACTED";
    case AcknowledgeMode::CLIENT_ACKNOWLEDGE:
      return "CLIENT_ACKNOWLEDGE";
    case AcknowledgeMode::INDIVIDUAL_ACKNOWLEDGE:
      return "INDIVIDUAL_ACKNOWLEDGE";
    case AcknowledgeMode::DUPS_OK_ACKNOWLEDGE:
      return "DUPS_OK_ACKNOWLEDGE";
    default:
      return "INVALID_ACKNOWLEDGE_MODE";
  }
}

const std::string &Session::transactionId() const { return _suffix; }

Poco::UUID Session::createRandomUUID() const { return _uuidGenerator.createRandom(); }

const std::string &Session::id() { return _id; }

Connection &Session::connection() { return _connection.get(); }

void Session::commit() {
  ensureOpen();
  _dispatch.wait();
  if (_mode != SESSION_TRANSACTED) return;
  auto& exchange = connection().exchange();
  auto& journal = exchange._journal;
  std::lock_guard brokerLock(exchange._mutex);

  auto nextTransaction = createRandomUUID().toString();
  auto& sends = _transaction._sends;
  const auto now = sends.empty() ? 0 : DeliveryClock::now();
  const bool durableInputs = std::any_of(_consumers.begin(), _consumers.end(), [](const auto& item) {
    const auto& consumer = *item.second;
    return std::any_of(consumer._transacted.begin(), consumer._transacted.end(),
                       [&](const auto& message) { return consumer.hasStorage(*message); });
  });
  if (!durableInputs) {
    if (sends.empty()) {
      finishTransaction(std::move(nextTransaction));
      return;
    }
    const auto& target = *sends.front().producer->_destination;
    if (target.isQueueFamily() && std::all_of(sends.begin(), sends.end(), [&](const auto& send) {
          return send.producer->_destination.get() == &target && !send.message->isPersistent() &&
                 send.message->jmsHeaders.deliveryTime <= now && !send.message->isExpired(now);
        })) {
      target.checkLive();
      target._queue->enqueueBatch(sends);
      finishTransaction(std::move(nextTransaction));
      return;
    }
  }

  std::vector<Destination::Publication> outputs;
  outputs.reserve(sends.size());
  for (const auto& send : _transaction._sends) {
    send.producer->_destination->preparePublication(*send.producer, send.message, outputs, now);
  }

  std::vector<LocalTransactionJournal::Operation> operations;
  std::vector<std::shared_ptr<DeliveryState>> touched;
  for (const auto& [id, consumer] : _consumers) {
    if (consumer->_storage && !consumer->_transacted.empty()) touched.push_back(consumer->_queue);
  }
  for (const auto& output : outputs) {
    if (touched.empty() || touched.back() != output.queue) touched.push_back(output.queue);
  }
  std::sort(touched.begin(), touched.end());
  touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
  std::vector<std::unique_lock<std::mutex>> storageLocks;
  storageLocks.reserve(touched.size());
  for (const auto& queue : touched) {
    storageLocks.emplace_back(queue->storageMutex);
    if (queue->retired || queue->isClosed()) throw IllegalStateException("transaction destination is closed");
  }

  for (const auto& [id, consumer] : _consumers) {
    for (const auto& message : consumer->_transacted) {
      if (consumer->hasStorage(*message)) {
        auto* storage = consumer->_storage.get();
        operations.push_back({journal.address(*storage), message->_storageId, {}, storage, consumer->record(*message)});
      }
    }
  }
  const auto inputs = operations.size();
  for (auto& output : outputs) {
    if (output.storage) {
      operations.push_back({journal.address(*output.storage), output.message->_storageId,
                            std::move(output.message->_cachedStorageBytes), output.storage.get(), {}});
    }
  }

  const auto publishAt = outputs.empty() ? 0 : DeliveryClock::now();
  const bool immediateBatch = !outputs.empty() && std::all_of(outputs.begin(), outputs.end(), [&](const auto& output) {
    return output.queue == outputs.front().queue && output.message->jmsHeaders.deliveryTime <= publishAt;
  });
  auto published = outputs.empty() || immediateBatch ? nullptr : std::make_shared<std::atomic_bool>(false);
  std::vector<std::unique_lock<DestinationMutex>> visibilityLocks;
  visibilityLocks.reserve(touched.size());
  journal.decide(operations, "commit");
  try {
    auto stored = inputs;
    for (auto& output : outputs) {
      if (output.storage) {
        const auto& record = operations[stored++].applied;
        output.message->_storageTomId = record.tomId;
        output.message->_storageOffset = record.offset;
      }
      if (!immediateBatch) {
        output.message->_publication = published;
        output.destination->enqueueOrSchedule(output.queue, std::move(output.message), publishAt);
      }
    }
    if (immediateBatch) outputs.front().queue->enqueueBatch(outputs);
    if (published) {
      for (const auto& queue : touched) visibilityLocks.emplace_back(queue->_mutex);
      published->store(true, std::memory_order_release);
      visibilityLocks.clear();
      for (const auto& queue : touched) queue->wake();
    }
  } catch (const std::exception& error) {
    LocalTransactionJournal::stopAfterDecision(error.what());
  } catch (...) {
    LocalTransactionJournal::stopAfterDecision("commit publication failed");
  }
  storageLocks.clear();
  finishTransaction(std::move(nextTransaction));
}

void Session::finishTransaction(std::string nextId) {
  for (const auto& [id, consumer] : _consumers) {
    if (consumer->_durable) consumer->_queue->pending -= consumer->_transacted.size();
    consumer->_transacted.clear();
  }
  _transaction._sends.clear();
  _suffix = std::move(nextId);
  for (auto it = _producers.begin(); it != _producers.end();) {
    it->second->_staged = false;
    if (it->second->_closed) {
      it->second->_session = nullptr;
      it = _producers.erase(it);
    } else {
      ++it;
    }
  }
  pruneClosedConsumers();
}

void Session::returnClaims(bool transacted) {
  std::vector<std::pair<Consumer*, Message::Ptr>> claims;
  for (const auto& [id, consumer] : _consumers) {
    if (transacted) {
      for (const auto& message : consumer->_transacted) claims.emplace_back(consumer.get(), message);
    } else {
      for (auto message = consumer->_inFlightHead; message; message = message->_inFlightLink.next) {
        claims.emplace_back(consumer.get(), message);
      }
    }
  }
  std::sort(claims.begin(), claims.end(), [](const auto& left, const auto& right) {
    return left.second->_receiveSequence < right.second->_receiveSequence;
  });
  static std::atomic_uint64_t nextBatch{1};
  const auto batch = claims.size() > 1 ? nextBatch.fetch_add(1, std::memory_order_relaxed) : 0;
  for (const auto& [consumer, message] : claims) {
    consumer->redeliver(*message, batch);
    if (transacted) {
      consumer->_transacted.pop_front();
      if (consumer->_durable) --consumer->_queue->pending;
    } else {
      consumer->unlink(*message);
    }
  }
}

void Session::rollback() {
  ensureOpen();
  _dispatch.wait();
  if (_mode != SESSION_TRANSACTED) return;
  _transaction._sends.clear();
  returnClaims(true);
  finishTransaction(createRandomUUID().toString());
}

void Session::recover() {
  ensureOpen();
  if (_mode == SESSION_TRANSACTED) {
    throw IllegalStateException("Session.recover() is not valid on a transacted session");
  }
  returnClaims(false);
  pruneClosedConsumers();
}

void Session::acknowledge() {
  ensureOpen();
  if (_mode != CLIENT_ACKNOWLEDGE) return;
  for (const auto& [id, consumer] : _consumers) consumer->flushPendingAcks();
  pruneClosedConsumers();
}

}  // namespace tiny_mq
