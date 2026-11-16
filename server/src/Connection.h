//
// Connection — owner of a single client connection's Sessions.
// Mirrors the JMS object model: ConnectionFactory -> Connection -> Session.
//

#ifndef TINY_MQ__CONNECTION_H_
#define TINY_MQ__CONNECTION_H_

#include "ConnectionMetaData.h"
#include "Session.h"
#include <Poco/Logger.h>
#include <condition_variable>
#include <mutex>
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

  // Delivery gate shared by the synchronous receive and listener paths.
  void start();
  void stop();
  bool isStarted() const;

  // JMS requires clientID to be set before any other action and only once.
  void setClientID(const std::string &clientID);
  const std::string &clientID() const;

  void setExceptionListener(ExceptionListener listener);
  void onException(const std::exception &ex);

  // Provider/JMS description (JMS 2.0 § 6.5). Allowed at any time, including
  // after close() — it is purely informational.
  ConnectionMetaData metadata() const;

  // Idempotent: a second call is a no-op. After close(), any operation that
  // would mutate the connection throws IllegalStateException.
  void close();
  bool isClosed() const;

  Exchange &exchange();

 private:
  // Throws IllegalStateException if the connection is closed. Callers must hold
  // _mutex (it reads _closed without locking).
  void ensureOpen() const;

  std::reference_wrapper<Exchange> _exchange;
  std::reference_wrapper<Poco::Logger> _logger;
  const Poco::UUID _identity;
  std::string _clientID;
  void watch(const std::shared_ptr<DeliveryState>& queue);
  void wakeDelivery();
  bool enterListener();
  void leaveListener();
  bool inOwnCallback() const;

  std::atomic_bool _started{false};
  bool _closing{false};
  size_t _activeListeners = 0;
  size_t _activeStops = 0;
  bool _restartRequested = false;
  bool _closed{false};
  ExceptionListener _exceptionListener;
  std::vector<std::unique_ptr<Session>> _sessions;
  mutable std::mutex _mutex;
  std::condition_variable _changed;
  std::vector<std::weak_ptr<DeliveryState>> _watchedQueues;

  friend class Destination;
  friend class Consumer;
  friend class SessionDispatch;
  friend class Exchange;
  friend class QueueBrowser;
  friend class Session;
  friend class TemporaryDestination;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__CONNECTION_H_
