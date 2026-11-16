//
// Created by Alexander Bychuk on 03.11.2022.
//

#include "Session.h"
#include "Connection.h"
#include "Exceptions.h"
#include "LogTracer.h"
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
  for (const auto& [id, producer] : _producers) {
    try {
      producer->rollback(_suffix);
    } catch (const std::exception& error) {
      poco_error(_logger.get(), std::string("producer teardown: ") + error.what());
    } catch (...) {
      poco_error(_logger.get(), "producer teardown failed");
    }
    producer->_closed = true;
    producer->_session = nullptr;
  }
  for (const auto& [id, consumer] : _consumers) consumer->finishSession();
}

Destination::Ptr Session::createDestination(destination::Type type, const std::string& name) {
  return connection().exchange().create(type, name);
}

Consumer::Ptr Session::remember(Consumer::Ptr consumer) {
  try {
    _consumers.emplace(consumer->id(), consumer);
  } catch (...) {
    consumer->finishSession();
    throw;
  }
  return consumer;
}

void Session::pruneClosedConsumers() {
  for (auto it = _consumers.begin(); it != _consumers.end();) {
    if (it->second->_closed && !it->second->hasClaims()) {
      it->second->_session = nullptr;
      it = _consumers.erase(it);
    } else {
      ++it;
    }
  }
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr& destination) {
  return createConsumer(*destination);
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr& destination, const std::string& selector) {
  return createConsumer(*destination, selector);
}

Consumer::Ptr Session::createConsumer(Destination& destination) {
  return remember(destination.createConsumer(*this));
}

Consumer::Ptr Session::createConsumer(Destination& destination, const std::string& selector) {
  const auto expression = Poco::trim(selector);
  return remember(destination.createConsumer(*this, expression.empty() ? nullptr : Selector::parse(expression)));
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr& destination, const std::string& selector, bool noLocal) {
  const auto expression = Poco::trim(selector);
  return remember(destination->createConsumer(*this, expression.empty() ? nullptr : Selector::parse(expression), noLocal));
}

void Session::deleteConsumer(const Poco::UUID& id) {
  auto it = _consumers.find(id);
  if (it == _consumers.end()) return;
  it->second->close();
  pruneClosedConsumers();
}

Producer::Ptr Session::createProducer(const Destination::Ptr& destination) {
  return createProducer(*destination);
}

Producer::Ptr Session::createProducer(Destination& destination) {
  destination.checkBroker(connection());
  destination.checkLive();
  auto producer = Producer::Ptr(new Producer(destination, *this));
  _producers.emplace(producer->id(), producer);
  return producer;
}

void Session::deleteProducer(const Poco::UUID& id) {
  const auto it = _producers.find(id);
  if (it == _producers.end()) return;
  it->second->_closed = true;
  if (it->second->_transactQueue && !it->second->_transactQueue->empty()) return;
  it->second->_session = nullptr;
  _producers.erase(it);
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr& destination, const std::string& name) {
  return createDurableConsumer(*destination, name);
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr& destination, const std::string& name,
                                             const std::string& selector) {
  return createDurableConsumer(*destination, name, selector);
}

Consumer::Ptr Session::createDurableConsumer(Destination& destination, const std::string& name) {
  return createDurableConsumer(destination, name, {});
}

Consumer::Ptr Session::createDurableConsumer(Destination& destination, const std::string& name,
                                             const std::string& selector) {
  const auto expression = Poco::trim(selector);
  return remember(destination.createDurableConsumer(*this, connection().clientID(), name,
                                                     expression.empty() ? nullptr : Selector::parse(expression)));
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr& topic, const std::string& name,
                                             const std::string& selector, bool noLocal) {
  return remember(connection().exchange().subscribe(*this, *topic, SubscriptionKind::UnsharedDurable,
                                                    name, selector, noLocal));
}

Consumer::Ptr Session::createSharedConsumer(const Destination::Ptr& topic, const std::string& name,
                                            const std::string& selector) {
  return remember(connection().exchange().subscribe(*this, *topic, SubscriptionKind::Shared,
                                                    name, selector, false));
}

Consumer::Ptr Session::createSharedDurableConsumer(const Destination::Ptr& topic, const std::string& name,
                                                   const std::string& selector) {
  return remember(connection().exchange().subscribe(*this, *topic, SubscriptionKind::SharedDurable,
                                                    name, selector, false));
}

void Session::unsubscribe(Destination& destination, const std::string& name) {
  destination.checkBroker(connection());
  destination.deleteSubscription(connection().clientID(), name);
  pruneClosedConsumers();
}

void Session::unsubscribe(const Destination::Ptr& destination, const std::string& name) {
  unsubscribe(*destination, name);
}

void Session::unsubscribe(const std::string& name) {
  connection().exchange().unsubscribe(connection().clientID(), name);
}

QueueBrowser Session::createQueueBrowser(const Destination::Ptr& queue, const std::string& selector) {
  queue->checkBroker(connection());
  queue->checkOwner(connection()._identity);
  if (!queue->isQueueFamily()) throw std::invalid_argument("browser requires a queue");
  const auto expression = Poco::trim(selector);
  return QueueBrowser(queue, expression.empty() ? nullptr : Selector::parse(expression));
}

TemporaryDestination::Ptr Session::createTemporaryQueue() {
  return connection().exchange().createTemporary(destination::TemporaryQueue, connection()._identity);
}

TemporaryDestination::Ptr Session::createTemporaryTopic() {
  return connection().exchange().createTemporary(destination::TemporaryTopic, connection()._identity);
}

TextMessage Session::createTextMessage(std::string text, Message::Reliability reliability) {
  return TextMessage(createRandomUUID(), std::move(text), reliability);
}

MapMessage Session::createMapMessage(Message::Reliability reliability) { return {createRandomUUID(), reliability}; }

StreamMessage Session::createStreamMessage(Message::Reliability reliability) { return {createRandomUUID(), reliability}; }

BytesMessage Session::createBytesMessage(BytesVector bytes, Message::Reliability reliability) {
  return BytesMessage(createRandomUUID(), std::move(bytes), reliability);
}

BytesMessage Session::createBytesMessage(const int8_t *bytes, size_t size, Message::Reliability reliability) {
  return BytesMessage(createRandomUUID(), bytes, size, reliability);
}

ObjectMessage Session::createObjectMessage(BytesVector body, std::string className, Message::Reliability reliability) {
  return ObjectMessage(createRandomUUID(), std::move(body), std::move(className), reliability);
}

Session::AcknowledgeMode Session::acknowledgeMode() const { return _mode; }

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
  if (_mode != SESSION_TRANSACTED) return;
  const auto clock = Poco::Timestamp().epochMicroseconds() / 1000;
  for (const auto& [id, producer] : _producers) producer->prepareCommit(clock);
  for (const auto& [id, producer] : _producers) producer->commit(_suffix);
  for (const auto& [id, consumer] : _consumers) consumer->commit();
  _suffix = createRandomUUID().toString();
  for (auto it = _producers.begin(); it != _producers.end();) {
    if (it->second->_closed) {
      it->second->_session = nullptr;
      it = _producers.erase(it);
    } else {
      ++it;
    }
  }
  pruneClosedConsumers();
}

void Session::rollback() { rollback(false); }

void Session::rollback(bool sessionClosing) {
  if (_mode != SESSION_TRANSACTED) return;
  for (const auto& [id, producer] : _producers) producer->rollback(_suffix);
  for (const auto& [id, consumer] : _consumers) consumer->rollback(sessionClosing);
  _suffix = createRandomUUID().toString();
  pruneClosedConsumers();
}

void Session::recover() {
  if (_mode == SESSION_TRANSACTED) {
    throw IllegalStateException("Session.recover() is not valid on a transacted session");
  }
  for (const auto& [id, consumer] : _consumers) consumer->recover();
  pruneClosedConsumers();
}

}  // namespace tiny_mq
