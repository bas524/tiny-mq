//
// Created by Alexander Bychuk on 10.11.2021.
//

#include "Producer.h"
#include "Destination.h"
#include "Exceptions.h"
#include "Session.h"
#include "TransactionBuffer.h"
#include "LogTracer.h"
#include <Poco/File.h>
#include <Poco/Timestamp.h>
#include <stdexcept>
#include <vector>

namespace tiny_mq {
Producer::Producer(Destination& destination, Session& session)
    : _uuid(session.createRandomUUID()),
      _destination(destination.shared_from_this()),
      _logger(Poco::Logger::get("tiny_mq.producer")),
      _session(&session) {
  if (session.acknowledgeMode() == Session::SESSION_TRANSACTED) {
    _transactQueue = std::make_unique<std::deque<Message::Ptr>>();
  }
}

Producer::~Producer() = default;

void Producer::ensureOpen() const {
  if (_closed) throw IllegalStateException("producer is closed");
  _destination->checkLive();
}

const Poco::UUID &Producer::id() const { return _uuid; }

void Producer::setDisableMessageID(bool value) { _disableMessageID = value; }
bool Producer::isDisableMessageID() const { return _disableMessageID; }
void Producer::setDisableMessageTimestamp(bool value) { _disableMessageTimestamp = value; }
bool Producer::isDisableMessageTimestamp() const { return _disableMessageTimestamp; }

/*static*/ void Producer::applyOptions(Message &message, const SendOptions &opts, bool transactional) {
  // opts override values set on the message; applied before persistence/enqueue.
  message.reliability = opts.deliveryMode;
  message.jmsHeaders.priority = opts.priority;
  const int64_t nowMs = Poco::Timestamp().epochMicroseconds() / 1000;
  // timeToLive == 0 means no expiration; else absolute expiry timestamp.
  message.jmsHeaders.expiration = opts.timeToLive > 0 ? nowMs + opts.timeToLive : 0;
  if (transactional) {
    // JMS 2.0 §7.8 (spec 13): for a transactional send, the delay clock starts at
    // commit, not send. Leave deliveryTime unresolved and stash the raw delay;
    // Producer::commit() resolves it to an absolute deliveryTime once the
    // transaction actually commits (rollback simply discards the message).
    message.jmsHeaders.deliveryTime = 0;
    message._pendingDeliveryDelay = opts.deliveryDelay;
  } else {
    message.jmsHeaders.deliveryTime = opts.deliveryDelay > 0 ? nowMs + opts.deliveryDelay : 0;
    message._pendingDeliveryDelay = 0;
  }
}

namespace {
// Ingress-side validation only. This rejects the common case (caller/API
// misuse) up front, but it is not the only — nor even the primary — line of
// defense: jmsHeaders.deliveryTime is a public field that bypasses this
// entirely (plain send() without setDefault()), and a replayed storage
// record bypasses it too. Both of those are re-checked/clamped at the point
// they actually enter the scheduler (DeliveryScheduler::enqueueOrSchedule,
// Destination.cpp's deliveryTimeFromStorageBytes) using the same
// kMaxFutureHeaderMs bound — see spec 13 review round 2, B4. Even that inner
// defense is defense-in-depth, not the sole guard: DeliveryScheduler::run
// caps how long it ever sleeps for, so no value that slips past validation
// can overflow the wait_until conversion or spin the worker.
void validateSendOptions(const SendOptions &opts) {
  if (opts.priority < 0 || opts.priority > 9) {
    throw std::invalid_argument("priority must be in range 0..9");
  }
  if (opts.timeToLive < 0 || opts.deliveryDelay < 0) {
    throw std::invalid_argument("timeToLive and deliveryDelay must be non-negative");
  }
  if (opts.timeToLive > kMaxFutureHeaderMs || opts.deliveryDelay > kMaxFutureHeaderMs) {
    throw std::invalid_argument("timeToLive and deliveryDelay must not exceed 10 years (ms)");
  }
}
}  // namespace

void Producer::setDefault(const SendOptions &opts) {
  ensureOpen();
  validateSendOptions(opts);
  _default = opts;
}

void Producer::send(const Message &message) {
  TRACE(_logger);
  ensureOpen();
  if (_default) applyOptions(const_cast<Message&>(message), *_default, !transactionId().empty());
  dispatch(message);
}

void Producer::send(const Message &message, const SendOptions &opts) {
  TRACE(_logger);
  ensureOpen();
  validateSendOptions(opts);
  applyOptions(const_cast<Message&>(message), opts, !transactionId().empty());
  dispatch(message);
}

void Producer::dispatch(const Message &message) {
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
  _destination->save(*this, message);
  poco_information(_logger.get(),
                   Poco::format("producer[%s] send message[%s] to %s", _uuid.toString(), message.uuid.toString(), _destination->uri()));
}
const std::string& Producer::transactionId() const {
  if (!_session) throw IllegalStateException("producer session is closed");
  return _session->transactionId();
}

void Producer::prepareCommit(int64_t now) {
  if (!_transactQueue || _transactQueue->empty()) return;
  _destination->checkLive();
  for (auto& message : *_transactQueue) {
    if (message->_pendingDeliveryDelay == 0) continue;
    message->jmsHeaders.deliveryTime = now + message->_pendingDeliveryDelay;
    message->_pendingDeliveryDelay = 0;
    if (_destination->_transactionBuffer && message->isPersistent()) {
      _destination->_transactionBuffer->patchDeliveryTime(message->_storageId, message->jmsHeaders.deliveryTime);
      message->patchCachedDeliveryTime(message->jmsHeaders.deliveryTime);
    }
  }
}

void Producer::commit(const std::string& transactionId) {
  if (!_transactQueue || _transactQueue->empty()) return;
  if (_destination->_transactionBuffer) _destination->_transactionBuffer->commitTransaction(transactionId);
  while (!_transactQueue->empty()) {
    _destination->deliverCommitted(*this, _transactQueue->front());
    _transactQueue->pop_front();
  }
}

void Producer::rollback(const std::string& transactionId) {
  if (!_transactQueue || _transactQueue->empty()) return;
  if (_destination->_transactionBuffer) _destination->_transactionBuffer->rollbackTransaction(transactionId);
  _transactQueue->clear();
}
}  // namespace tiny_mq
