//
// Connection implementation.
//

#include "Connection.h"
#include "Exceptions.h"
#include "Exchange.h"
#include "LogTracer.h"
#include <Poco/Format.h>

namespace tiny_mq {

Connection::Connection(Exchange &exchange)
    : _exchange(exchange), _logger(Poco::Logger::get("tiny_mq.connection")),
      _identity(Poco::UUIDGenerator::defaultGenerator().createRandom()) {
  TRACE(_logger);
}

Connection::~Connection() {
  TRACE(_logger);
  try {
    close();
  } catch (const std::exception& error) {
    poco_error(_logger.get(), std::string("connection teardown: ") + error.what());
  } catch (...) {
    poco_error(_logger.get(), "connection teardown failed");
  }
}

Session &Connection::createSession(Session::AcknowledgeMode mode) {
  TRACE(_logger);
  std::lock_guard lock(_mutex);
  ensureOpen();
  if (_closing) throw IllegalStateException("connection is closing");
  _sessions.emplace_back(std::unique_ptr<Session>(new Session(*this, mode)));
  return *_sessions.back();
}

void Connection::start() {
  std::lock_guard lock(_mutex);
  ensureOpen();
  if (_closing) throw IllegalStateException("connection is closing");
  if (_activeStops) {
    _restartRequested = true;
    return;
  }
  _started = true;
  wakeDelivery();
}

void Connection::stop() {
  if (inOwnCallback() && !SessionDispatch::inCompletion) {
    throw IllegalStateException("cannot stop connection from its message listener");
  }
  std::unique_lock lock(_mutex);
  ensureOpen();
  _started = false;
  _restartRequested = false;
  ++_activeStops;
  _changed.wait(lock, [&] { return !_activeListeners; });
  if (--_activeStops == 0 && _restartRequested && !_closing) {
    _started = true;
    wakeDelivery();
  }
}

bool Connection::isStarted() const { return _started.load(std::memory_order_acquire); }

bool Connection::inOwnCallback() const {
  const auto* dispatch = SessionDispatch::current;
  return dispatch && &dispatch->_session.connection() == this;
}

bool Connection::enterListener() {
  std::lock_guard lock(_mutex);
  if (!_started || _closing) return false;
  ++_activeListeners;
  return true;
}

void Connection::leaveListener() {
  std::lock_guard lock(_mutex);
  --_activeListeners;
  _changed.notify_all();
}

void Connection::watch(const std::shared_ptr<DeliveryState>& queue) {
  std::lock_guard lock(_mutex);
  std::erase_if(_watchedQueues, [](const auto& weak) { return weak.expired(); });
  if (std::none_of(_watchedQueues.begin(), _watchedQueues.end(),
                   [&](const auto& weak) { return weak.lock() == queue; })) _watchedQueues.emplace_back(queue);
}

void Connection::wakeDelivery() {
  for (const auto& weak : _watchedQueues) {
    if (auto queue = weak.lock()) queue->wake();
  }
  for (const auto& session : _sessions) {
    auto& dispatch = session->_dispatch;
    std::lock_guard lock(dispatch._mutex);
    dispatch.signalListener();
  }
}

void Connection::setClientID(const std::string &clientID) {
  std::lock_guard lock(_mutex);
  ensureOpen();
  if (!_clientID.empty()) {
    throw IllegalStateException("clientID is already set");
  }
  if (!_sessions.empty() || _started) {
    throw IllegalStateException("clientID must be set before the connection is used");
  }
  _clientID = clientID;
}

const std::string &Connection::clientID() const { return _clientID; }

void Connection::setExceptionListener(ExceptionListener listener) {
  std::lock_guard lock(_mutex);
  ensureOpen();
  _exceptionListener = std::move(listener);
}

ConnectionMetaData Connection::metadata() const { return ConnectionMetaData{}; }

void Connection::onException(const std::exception &ex) {
  ExceptionListener listener;
  {
    std::lock_guard lock(_mutex);
    listener = _exceptionListener;
  }
  if (listener) {
    listener(ex);
  }
}

void Connection::close() {
  TRACE(_logger);
  if (inOwnCallback()) throw IllegalStateException("cannot close connection from its callback");
  {
    std::unique_lock lock(_mutex);
    if (_closing) {
      _changed.wait(lock, [&] { return _closed; });
      return;
    }
    _closing = true;
    _started = false;
  }

  std::exception_ptr failure;
  for (const auto& session : _sessions) {
    try {
      session->close();
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  try {
    _exchange.get().removeTemporary(_identity);
  } catch (...) {
    if (!failure) failure = std::current_exception();
  }

  {
    std::lock_guard lock(_mutex);
    _closed = true;
    _changed.notify_all();
  }
  if (failure) std::rethrow_exception(failure);
}

bool Connection::isClosed() const {
  std::lock_guard lock(_mutex);
  return _closed;
}

void Connection::ensureOpen() const {
  if (_closed || (_closing && !inOwnCallback())) {
    throw IllegalStateException("connection is closed");
  }
}

Exchange &Connection::exchange() { return _exchange.get(); }

}  // namespace tiny_mq
