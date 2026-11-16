#ifndef TINY_MQ__CONSUMER_H_
#define TINY_MQ__CONSUMER_H_

#include <deque>
#include "ConcurrentLinearStorage.h"
#include "Message.h"
#include "Selector.h"

namespace tiny_mq {
class DeliveryState;
class Destination;
class Session;

class Consumer {
 public:
  using Ptr = std::shared_ptr<Consumer>;
  const Poco::UUID& id() const;
  Message::Ptr recv(int64_t usec_timeout = 10000000);
  QueueT::producer_token_t getProducerToken();
  void acknowledgeOn(const Message& message);
  const Session& session() const;
  virtual ~Consumer();

 private:
  Consumer(Destination& destination, Session& session, std::shared_ptr<DeliveryState> queue,
           std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage,
           std::shared_ptr<Selector> selector = {}, bool durable = false);
  void commit();
  void rollback(bool sessionClosing);
  void recover(bool sessionClosing = false);
  void redeliver(Message& message, bool sessionClosing);
  void close();
  void finishSession() noexcept;
  void clearInFlight() noexcept;
  void flushPendingAcks();
  bool hasClaims() const;
  linear_storage::Record record(const Message& message) const;
  void acknowledge(const Message& message);

  Poco::UUID _uuid;
  std::shared_ptr<Destination> _destination;
  Session* _session;
  std::shared_ptr<DeliveryState> _queue;
  std::shared_ptr<linear_storage::ConcurrentLinearStorage> _storage;
  std::shared_ptr<Selector> _selector;
  std::reference_wrapper<Poco::Logger> _logger;
  std::deque<Message::Ptr> _transacted;
  std::vector<linear_storage::Record> _pendingAcks;
  // ADR-0005 gives the owning session exclusive access to these delivery links.
  Message::Ptr _inFlightHead;
  Message* _inFlightTail{nullptr};
  bool _closed{false};
  bool _durable;

  friend class Destination;
  friend class Session;
};
}  // namespace tiny_mq
#endif
