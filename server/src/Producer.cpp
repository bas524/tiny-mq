#include "Producer.h"
#include "DeliveryClock.h"
#include "Destination.h"
#include "Exceptions.h"
#include "Session.h"
#include <cstdlib>

namespace tiny_mq {
namespace {
void validate(const SendOptions& options) {
  if (options.priority < 0 || options.priority > 9) {
    throw std::invalid_argument("priority must be in range 0..9");
  }
  if (options.timeToLive < 0 || options.deliveryDelay < 0) {
    throw std::invalid_argument("timeToLive and deliveryDelay must be non-negative");
  }
  if (options.deliveryMode != Message::PERSISTENT && options.deliveryMode != Message::NOT_PERSISTENT) {
    throw std::invalid_argument("invalid delivery mode");
  }
}
}

Producer::Producer(Destination& destination, Session& session)
    : _uuid(session.createRandomUUID()),
      _destination(destination.shared_from_this()),
      _session(&session) {}

Producer::~Producer() = default;

Session& Producer::ensureOpen() const {
  if (_closed) throw IllegalStateException("producer is closed");
  auto* session = _session.load(std::memory_order_acquire);
  if (!session) throw IllegalStateException("producer session is closed");
  session->ensureOpen();
  _destination->checkLive();
  return *session;
}

const Poco::UUID& Producer::id() const { return _uuid; }

void Producer::setDisableMessageID(bool value) { ensureOpen(); _disableMessageID = value; }
bool Producer::isDisableMessageID() const { return _disableMessageID; }
void Producer::setDisableMessageTimestamp(bool value) { ensureOpen(); _disableMessageTimestamp = value; }
bool Producer::isDisableMessageTimestamp() const { return _disableMessageTimestamp; }

void Producer::setDefault(const SendOptions& options) {
  ensureOpen();
  validate(options);
  _default = options;
}

void Producer::send(const Message& message) {
  auto& session = ensureOpen();
  if (!_default) validate(SendOptions{message.reliability, message.jmsHeaders.priority});
  dispatch(session, message, _default ? &*_default : nullptr);
}

void Producer::send(const Message& message, const SendOptions& options) {
  auto& session = ensureOpen();
  validate(options);
  dispatch(session, message, &options);
}

void Producer::sendAsync(const Message& message, CompletionListener listener) {
  accept(message, _default ? &*_default : nullptr, std::move(listener));
}

void Producer::sendAsync(const Message& message, const SendOptions& options, CompletionListener listener) {
  accept(message, &options, std::move(listener));
}

void Producer::accept(const Message& message, const SendOptions* options, CompletionListener listener) {
  auto& session = ensureOpen();
  if (!listener) throw std::invalid_argument("completion listener is empty");
  validate(options ? *options : SendOptions{message.reliability, message.jmsHeaders.priority});
  auto result = std::make_shared<SendCompletion>();
  auto& executor = session._dispatch;
  const auto completion = executor.reserve(*this, [result, listener = std::move(listener)] { listener(*result); });
  try {
    LocalTransactionJournal::point("send", "accepted");
#ifdef ENABLE_TESTS
    if (const auto* injected = std::getenv("TINY_MQ_TEST_SEND_ERROR"); injected && *injected) {
      throw Poco::IOException("injected failure after send acceptance");
    }
#endif
    dispatch(session, message, options, result.get());
  } catch (...) {
    result->headers.reset();
    result->error = std::current_exception();
  }
  executor.complete(completion);
}

void Producer::dispatch(Session& session, const Message& message, const SendOptions* options, SendCompletion* completion) {
  // Provider headers are assigned on the accepted send, using one clock sample.
  const auto now = DeliveryClock::now();
  const auto& outgoing = message;
  auto& headers = outgoing.jmsHeaders;
  if (options) {
    outgoing.reliability = options->deliveryMode;
    headers.priority = options->priority;
  }
  headers.expiration = options && options->timeToLive != 0 ? DeliveryClock::add(now, options->timeToLive) : 0;
  headers.deliveryTime = options ? DeliveryClock::add(now, options->deliveryDelay) : now;
  headers.timestamp = _disableMessageTimestamp ? 0 : now;
  if (_disableMessageID) headers.messageId.clear();
  else if (headers.messageId.empty()) headers.messageId = "ID:" + outgoing.uuid.toString();
  headers.redelivered = false;
  headers.deliveryCount = 0;
  outgoing._cachedStorageBytes.clear();
  if (completion) completion->headers = headers;
  if (session.acknowledgeMode() == Session::SESSION_TRANSACTED) {
    auto draft = message.copy();
    if (_destination->isQueueFamily()) {
      const auto number = _destination->_messageCounter.fetch_add(1, std::memory_order_relaxed);
      draft = _destination->prepareCopy(*this, std::move(draft), number);
    }
    session._transaction.stage(*this, std::move(draft));
    _staged = true;
  } else {
    _destination->save(*this, message, now);
  }
}

void Producer::close() {
  auto* session = _session.load(std::memory_order_acquire);
  if (!session) return;
  auto& executor = session->_dispatch;
  if (SessionDispatch::current == &executor && SessionDispatch::inCompletion) {
    std::lock_guard lock(executor._mutex);
    if (executor._activeProducer == this || std::any_of(executor._completions.begin(), executor._completions.end(),
          [&](const auto& completion) { return completion->producer == this; })) {
      throw IllegalStateException("producer close would wait on its session completion thread");
    }
  }
  _closed = true;
  executor.wait(this);
}

const std::string& Producer::transactionId() const {
  return ensureOpen().transactionId();
}
}
