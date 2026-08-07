//
// Created by Alexander Bychuk on 03.11.2022.
//

#include "Session.h"
#include "Connection.h"
#include "Exceptions.h"
#include "LogTracer.h"
#include <Poco/Format.h>
#include <Poco/UUIDGenerator.h>
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
    rollback();
  } catch (...) {
    poco_error(_logger.get(), "rollback() threw in destructor — ignoring");
  }
  for (auto &cons : _consumers) {
    cons.second->destination().deleteConsumer(cons.second->id());
  }
  for (auto &prod : _producers) {
    prod.second->destination().deleteProducer(prod.second->id());
  }
}

Destination::Ptr Session::createDestination(destination::Type type, const std::string &destinationName) {
  TRACE(_logger);
  auto destination = _connection.get().exchange().create(type, destinationName);
  if (destination) {
    destination->createDefaultConsumer(*this);
    _destinations.emplace(destination->hash(), destination);
    poco_debug(_logger.get(), Poco::format("create destination %s://%s in session[%s]", destination->typeName(), destination->name(), _suffix));
  }
  return destination;
}

Consumer::Ptr Session::createConsumer(const Destination::Ptr &destination) { return createConsumer(*destination); }
Consumer::Ptr Session::createConsumer(const Destination::Ptr &destination, const std::string &selector) {
  return createConsumer(*destination, selector);
}

void Session::deleteConsumer(const Poco::UUID &id) {
  TRACE(_logger);
  auto it = _consumers.find(id);
  if (it != _consumers.end()) {
    it->second->destination().deleteConsumer(id);
    _consumers.erase(it);
  }
}

Producer::Ptr Session::createProducer(const Destination::Ptr &destination) { return createProducer(*destination); }

void Session::deleteProducer(const Poco::UUID &id) {
  TRACE(_logger);
  auto it = _producers.find(id);
  if (it != _producers.end()) {
    it->second->destination().deleteProducer(id);
    _producers.erase(it);
  }
}

Consumer::Ptr Session::createConsumer(Destination &destination) {
  TRACE(_logger);
  auto consumer = destination.createConsumer(*this);
  if (consumer) {
    _consumers.emplace(consumer->id(), consumer);
    poco_debug(_logger.get(),
               Poco::format(
                   "create consumer[%s] in session[%s] for %s://%s", consumer->id().toString(), _suffix, destination.typeName(), destination.name()));
  }
  return consumer;
}

Consumer::Ptr Session::createConsumer(Destination &destination, const std::string &selectorExpr) {
  TRACE(_logger);
  // Parse (and validate) the selector expression up front — throws InvalidSelectorException on error.
  auto selector = Selector::parse(selectorExpr);
  auto consumer = destination.createConsumer(*this, std::move(selector));
  if (consumer) {
    _consumers.emplace(consumer->id(), consumer);
    poco_debug(_logger.get(),
               Poco::format("create consumer[%s] in session[%s] for %s://%s with selector [%s]",
                            consumer->id().toString(), _suffix, destination.typeName(), destination.name(), selectorExpr));
  }
  return consumer;
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr &destination, const std::string &subscriptionName) {
  return createDurableConsumer(*destination, subscriptionName);
}

Consumer::Ptr Session::createDurableConsumer(const Destination::Ptr &destination, const std::string &subscriptionName,
                                              const std::string &selectorExpr) {
  return createDurableConsumer(*destination, subscriptionName, selectorExpr);
}

Consumer::Ptr Session::createDurableConsumer(Destination &destination, const std::string &subscriptionName) {
  TRACE(_logger);
  auto consumer = destination.createDurableConsumer(*this, connection().clientID(), subscriptionName);
  if (consumer) {
    _consumers.emplace(consumer->id(), consumer);
    poco_debug(_logger.get(),
               Poco::format("create durable consumer[%s] in session[%s] for %s://%s subscription='%s'",
                            consumer->id().toString(), _suffix, destination.typeName(), destination.name(), subscriptionName));
  }
  return consumer;
}

Consumer::Ptr Session::createDurableConsumer(Destination &destination, const std::string &subscriptionName,
                                              const std::string &selectorExpr) {
  TRACE(_logger);
  auto selector = Selector::parse(selectorExpr);
  auto consumer = destination.createDurableConsumer(*this, connection().clientID(), subscriptionName, std::move(selector));
  if (consumer) {
    _consumers.emplace(consumer->id(), consumer);
    poco_debug(_logger.get(),
               Poco::format("create durable consumer[%s] in session[%s] for %s://%s subscription='%s' selector=[%s]",
                            consumer->id().toString(), _suffix, destination.typeName(), destination.name(),
                            subscriptionName, selectorExpr));
  }
  return consumer;
}

void Session::unsubscribe(Destination &destination, const std::string &subscriptionName) {
  TRACE(_logger);
  destination.deleteSubscription(connection().clientID(), subscriptionName);
  poco_debug(_logger.get(),
             Poco::format("unsubscribed '%s' from %s://%s in session[%s]",
                          subscriptionName, destination.typeName(), destination.name(), _suffix));
}

void Session::unsubscribe(const Destination::Ptr &destination, const std::string &subscriptionName) {
  unsubscribe(*destination, subscriptionName);
}

Producer::Ptr Session::createProducer(Destination &destination) {
  TRACE(_logger);
  auto producer = destination.createProducer(*this);
  if (producer) {
    _producers.emplace(producer->id(), producer);
    poco_debug(_logger.get(),
               Poco::format(
                   "create producer[%s] in session[%s] for %s://%s", producer->id().toString(), _suffix, destination.typeName(), destination.name()));
  }
  return producer;
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
  TRACE(_logger);
  // Save the current transaction ID before clearing — producers need it to
  // identify which buffered messages to commit in the TransactionBuffer.
  // _suffix is cleared first so that any deliveries triggered inside commit()
  // are treated as non-transactional (go straight to consumer queues).
  std::string oldSuffix = _suffix;
  _suffix.clear();
  for (auto &producer : _producers) {
    producer.second->commit(oldSuffix);
  }
  for (auto &consumer : _consumers) {
    consumer.second->commit();
  }
  _suffix = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
}

void Session::rollback() {
  TRACE(_logger);
  std::string oldSuffix = _suffix;
  _suffix.clear();
  for (auto &producer : _producers) {
    producer.second->rollback(oldSuffix);
  }
  for (auto &consumer : _consumers) {
    consumer.second->rollback();
  }
  _suffix = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
}

void Session::recover() {
  TRACE(_logger);
  if (_mode == AcknowledgeMode::SESSION_TRANSACTED) {
    // JMS 2.0 § 8.4.8: recover() is illegal on a transacted session — the
    // application must call rollback() instead. Mirrors jakarta.jms's
    // IllegalStateException for this exact case.
    throw IllegalStateException("Session.recover() is not valid on a transacted session; use rollback() instead");
  }
  for (auto &consumer : _consumers) {
    consumer.second->recover();
  }
}

}  // namespace tiny_mq
