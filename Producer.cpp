//
// Created by Alexander Bychuk on 10.11.2021.
//

#include "Producer.h"
#include "Destination.h"
#include "Session.h"
#include "LogTracer.h"
#include <Poco/File.h>
#include <Poco/Timestamp.h>

namespace tiny_mq {
Producer::Producer(Destination &destination,
                   Session &session,
                   const Poco::UUID &uuid,
                   std::unique_ptr<moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t> token)
    : _uuid(uuid),
      _destination(destination),
      _token(std::move(token)),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.producer.%s", _uuid.toString()))),
      _session(session) {
  TRACE(_logger);
  if (_session.get().acknowledgeMode() == Session::AcknowledgeMode::SESSION_TRANSACTED) {
    _transactQueue = std::make_shared<QueueT>();
  }
  poco_information(_logger.get(), Poco::format("create producer[%s] for %s", _uuid.toString(), _destination.get().uri()));
}
Producer::~Producer() {
  TRACE(_logger);
  if (_transactQueue) {
    rollback(_session.get().transactionId());
  }
  poco_information(_logger.get(), Poco::format("destroy producer[%s] for %s", _uuid.toString(), _destination.get().uri()));
}
const Poco::UUID &Producer::id() const { return _uuid; }

void Producer::setDisableMessageID(bool value) { _disableMessageID = value; }
bool Producer::isDisableMessageID() const { return _disableMessageID; }
void Producer::setDisableMessageTimestamp(bool value) { _disableMessageTimestamp = value; }
bool Producer::isDisableMessageTimestamp() const { return _disableMessageTimestamp; }

void Producer::send(const Message &message) {
  TRACE(_logger);
  // JMS providers assign MessageID and Timestamp at send time unless disabled.
  // Message is const at the API boundary but its headers are mutable metadata
  // populated by the provider — match the JMS contract by filling them here.
  auto& mut = const_cast<Message&>(message);
  if (_disableMessageID) {
    mut.jmsHeaders.messageId.clear();
  } else if (mut.jmsHeaders.messageId.empty()) {
    mut.jmsHeaders.messageId = "ID:" + mut.uuid.toString();
  }
  if (_disableMessageTimestamp) {
    mut.jmsHeaders.timestamp = 0;
  } else if (mut.jmsHeaders.timestamp == 0) {
    mut.jmsHeaders.timestamp = Poco::Timestamp().epochMicroseconds() / 1000;
  }
  _destination.get().save(*this, message);
  poco_information(_logger.get(),
                   Poco::format("producer[%s] send message[%s] to %s", _uuid.toString(), message.uuid.toString(), _destination.get().uri()));
}
const std::string &Producer::transactionId() const { return _session.get().transactionId(); }
const moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t &Producer::token() const {
  return *_token;
}

void Producer::commit(const std::string& transactionId) {
  TRACE(_logger);
  if (_transactQueue) {
    // Persist all buffered message data to storage (single batch dispatch).
    _destination.get().commitTransaction(transactionId);

    // Deliver staged messages directly to consumer queues; move each ptr to
    // avoid an extra copy (deliverCommitted takes ownership).
    Message::Ptr message;
    while (_transactQueue->try_dequeue(message)) {
      if (message) {
        poco_trace(_logger.get(),
                   Poco::format("commit message[%?d][%s] to %s://%s",
                                message->number(),
                                message->uuid.toString(),
                                _destination.get().typeName(),
                                _destination.get().name()));
        _destination.get().deliverCommitted(std::move(message));
      }
    }
    // Queue drained; reuse the existing object.
  }
}

void Producer::rollback(const std::string& transactionId) {
  TRACE(_logger);
  if (_transactQueue) {
    _destination.get().rollbackTransaction(transactionId);
    Message::Ptr message;
    while (_transactQueue->try_dequeue(message)) {
      poco_trace(_logger.get(),
                 Poco::format("rollback discarding message[%s]",
                              message ? message->uuid.toString() : "null"));
    }
    // Queue drained; reuse the existing object.
  }
}

QueueT &Producer::transactQueue() const {
  return *_transactQueue;
}

Destination &Producer::destination() const
{
  return _destination.get();
}

}  // namespace tiny_mq
