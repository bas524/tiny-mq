//
// Connection — owner of a single client connection's Sessions.
// Mirrors the JMS object model: ConnectionFactory -> Connection -> Session.
//

#ifndef TINY_MQ__CONNECTION_H_
#define TINY_MQ__CONNECTION_H_

#include "Session.h"
#include <Poco/Logger.h>
#include <Poco/Mutex.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tiny_mq {
class Exchange;

class Connection {
 public:
  using Ptr = std::shared_ptr<Connection>;
  using ExceptionListener = std::function<void(const std::exception &)>;

  explicit Connection(Exchange &exchange);
  ~Connection();
  Connection(const Connection &) = delete;
  Connection(Connection &&) = delete;
  Connection &operator=(const Connection &) = delete;
  Connection &operator=(Connection &&) = delete;

  // Sessions are owned by the connection and live until close()/destruction.
  Session &createSession(Session::AcknowledgeMode mode = Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);

  // JMS delivery gate. Stored here for the in-process core; network push
  // delivery honours it on the server side (see tasks M2-5).
  void start();
  void stop();
  bool isStarted() const;

  // JMS requires clientID to be set before any other action and only once.
  void setClientID(const std::string &clientID);
  const std::string &clientID() const;

  void setExceptionListener(ExceptionListener listener);
  void onException(const std::exception &ex);

  void close();

  Exchange &exchange();

 private:
  std::reference_wrapper<Exchange> _exchange;
  std::reference_wrapper<Poco::Logger> _logger;
  std::string _clientID;
  bool _started{false};
  ExceptionListener _exceptionListener;
  std::vector<std::unique_ptr<Session>> _sessions;
  mutable Poco::FastMutex _mutex;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__CONNECTION_H_
