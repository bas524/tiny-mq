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
    : _exchange(exchange), _logger(Poco::Logger::get("tiny_mq.connection")) {
  TRACE(_logger);
}

Connection::~Connection() {
  TRACE(_logger);
  close();
}

Session &Connection::createSession(Session::AcknowledgeMode mode) {
  TRACE(_logger);
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  ensureOpen();
  _sessions.emplace_back(std::unique_ptr<Session>(new Session(*this, mode)));
  return *_sessions.back();
}

void Connection::start() {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  ensureOpen();
  _started = true;
}

void Connection::stop() {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  ensureOpen();
  _started = false;
}

bool Connection::isStarted() const {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  return _started;
}

void Connection::setClientID(const std::string &clientID) {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
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
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  ensureOpen();
  _exceptionListener = std::move(listener);
}

ConnectionMetaData Connection::metadata() const { return ConnectionMetaData{}; }

void Connection::onException(const std::exception &ex) {
  ExceptionListener listener;
  {
    Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
    listener = _exceptionListener;
  }
  if (listener) {
    listener(ex);
  }
}

void Connection::close() {
  TRACE(_logger);
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  if (_closed) return;  // idempotent
  _closed = true;
  _sessions.clear();
  _started = false;
}

bool Connection::isClosed() const {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  return _closed;
}

void Connection::ensureOpen() const {
  if (_closed) {
    throw IllegalStateException("connection is closed");
  }
}

Exchange &Connection::exchange() { return _exchange.get(); }

}  // namespace tiny_mq
