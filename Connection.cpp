//
// Connection implementation.
//

#include "Connection.h"
#include "Exchange.h"
#include "LogTracer.h"
#include <Poco/Format.h>
#include <stdexcept>

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
  _sessions.emplace_back(std::unique_ptr<Session>(new Session(*this, mode)));
  return *_sessions.back();
}

void Connection::start() {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  _started = true;
}

void Connection::stop() {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  _started = false;
}

bool Connection::isStarted() const {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  return _started;
}

void Connection::setClientID(const std::string &clientID) {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  if (!_clientID.empty()) {
    throw std::logic_error("clientID is already set");
  }
  if (!_sessions.empty() || _started) {
    throw std::logic_error("clientID must be set before the connection is used");
  }
  _clientID = clientID;
}

const std::string &Connection::clientID() const { return _clientID; }

void Connection::setExceptionListener(ExceptionListener listener) {
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  _exceptionListener = std::move(listener);
}

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
  _sessions.clear();
  _started = false;
}

Exchange &Connection::exchange() { return _exchange.get(); }

}  // namespace tiny_mq
