//
// Created by Alexander Bychuk on 10.11.2021.
//

#include "Producer.h"
#include "Destination.h"
#include "Session.h"
#include "LogTracer.h"
#include <Poco/File.h>
#include <Poco/Timestamp.h>
#include <stdexcept>
#include <vector>

namespace tiny_mq {
Producer::Producer(Destination &destination,
                   Session &session,
                   const Poco::UUID &uuid,
                   std::unique_ptr<QueueT::producer_token_t> token)
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
  validateSendOptions(opts);
  _default = opts;
}

void Producer::send(const Message &message) {
  TRACE(_logger);
  // With explicit producer defaults, plain send applies them; otherwise the
  // message keeps its create-time reliability/priority (backward compatible).
  if (_default) {
    // Same predicate Consumer::push uses (producer.transactionId().empty()) —
    // not acknowledgeMode() == SESSION_TRANSACTED. Session::commit()/rollback()
    // deliberately clear transactionId() (Session's _suffix) for the duration
    // of their own execution, so a send() issued from inside a commit/rollback
    // callback must be classified the same way here as it will be by push(),
    // or a delayed message resolved here as "transactional" would be pushed by
    // push() through the non-transactional branch with deliveryTime left at 0
    // (unresolved) — i.e. the delay silently vanishes. See spec 13 review B1.
    bool transactional = !transactionId().empty();
    applyOptions(const_cast<Message &>(message), *_default, transactional);
  }
  dispatch(message);
}

void Producer::send(const Message &message, const SendOptions &opts) {
  TRACE(_logger);
  validateSendOptions(opts);
  // See the comment in send(const Message&) above: must match Consumer::push's
  // predicate exactly.
  bool transactional = !transactionId().empty();
  applyOptions(const_cast<Message &>(message), opts, transactional);
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
  _destination.get().save(*this, message);
  poco_information(_logger.get(),
                   Poco::format("producer[%s] send message[%s] to %s", _uuid.toString(), message.uuid.toString(), _destination.get().uri()));
}
const std::string &Producer::transactionId() const { return _session.get().transactionId(); }
const QueueT::producer_token_t &Producer::token() const {
  return *_token;
}

void Producer::commit(const std::string& transactionId) {
  TRACE(_logger);
  if (_transactQueue) {
    // Resolve any pending delivery delay to an absolute deliveryTime now — this is
    // the moment the JMS 2.0 §7.8 clock actually starts for a transactional send
    // (spec 13). Patch already-buffered persistent bytes *before* commitTransaction
    // flushes them, so the persisted deliveryTime matches what was just resolved.
    const int64_t nowMs = Poco::Timestamp().epochMicroseconds() / 1000;
    std::vector<Message::Ptr> staged;
    Message::Ptr message;
    while (_transactQueue->try_dequeue(message)) {
      if (!message) continue;
      if (message->_pendingDeliveryDelay > 0) {
        message->jmsHeaders.deliveryTime = nowMs + message->_pendingDeliveryDelay;
        message->_pendingDeliveryDelay = 0;
        if (message->isPersistent()) {
          // Two independent copies of the pre-commit (deliveryTime == 0) bytes
          // exist and both must be patched: the TransactionBuffer's buffered
          // copy (flushed to storage by commitTransaction() below) and this
          // Message's own _cachedStorageBytes (read by Consumer::recv()'s fast
          // path instead of storage). Missing either one hands the application
          // JMSDeliveryTime == 0 on a message that really was delayed — spec 13
          // review B2.
          _destination.get().patchPendingDeliveryTime(message->uuid, message->jmsHeaders.deliveryTime);
          message->patchCachedDeliveryTime(message->jmsHeaders.deliveryTime);
        }
      }
      staged.push_back(std::move(message));
    }

    // Persist all buffered message data to storage (single batch dispatch).
    _destination.get().commitTransaction(transactionId);

    // Deliver staged messages directly to consumer queues; move each ptr to
    // avoid an extra copy (deliverCommitted takes ownership).
    for (auto& msg : staged) {
      poco_trace(_logger.get(),
                 Poco::format("commit message[%?d][%s] to %s://%s",
                              msg->number(),
                              msg->uuid.toString(),
                              _destination.get().typeName(),
                              _destination.get().name()));
      _destination.get().deliverCommitted(std::move(msg));
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
