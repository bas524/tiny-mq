#ifndef TINY_MQ__DESTINATION_H_
#define TINY_MQ__DESTINATION_H_

#include <memory>
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include "Producer.h"
#include "Consumer.h"
#include "Selector.h"
#include "DestinationType.h"
#include "HashHelper.h"
#include "ConcurrentQueueHeader.h"
#include "ConcurrentLinearStorage.h"
#include "TransactionBuffer.h"

namespace tiny_mq {
class Session;
class Destination {

  // Per-subscription state for a named durable topic subscriber.
  struct DurableSubState {
    std::string name;
    Poco::Path path;
    std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage;
    Poco::UUID activeConsumerUuid;  // null UUID = subscriber is currently offline
    std::shared_ptr<Selector> selector;  // nullptr = match all
  };

  destination::Type _type;
  std::string _name;
  Poco::Path _path;
  std::atomic_int64_t _messageCounter{0};
  phmap::parallel_node_hash_map<Poco::UUID, Consumer::Ptr> _consumers;
  phmap::parallel_node_hash_map<Poco::UUID, Producer::Ptr> _producers;
  std::shared_ptr<QueueT> _queue;
  Consumer::Ptr _defaultConsumer;
  std::reference_wrapper<Poco::Logger> _logger;
  std::string _uri;
  size_t _hash;
  std::shared_ptr<linear_storage::ConcurrentLinearStorage> _storage;
  std::shared_ptr<TransactionBuffer> _transactionBuffer;

  // Durable subscription registry (topic-family only)
  phmap::parallel_node_hash_map<std::string, DurableSubState> _durableSubs;
  // Reverse lookup: consumer UUID -> subscription name (durable consumers only)
  phmap::parallel_node_hash_map<Poco::UUID, std::string> _consumerToSubName;

 public:
  using Ptr = std::shared_ptr<Destination>;
  virtual ~Destination();

  destination::Type type() const;
  bool isQueueFamily() const;
  bool isTopicFamily() const;
  const std::string &name() const;
  std::string typeName() const;
  const std::string &uri() const;
  size_t hash() const;

 private:
  Destination(destination::Type type, std::string name, Poco::Path path);
  Producer::Ptr createProducer(Session &session);
  void deleteProducer(const Poco::UUID &id);
  Consumer::Ptr createConsumer(Session &session, std::shared_ptr<Selector> selector = nullptr);
  void deleteConsumer(const Poco::UUID &id);
  Consumer::Ptr createDefaultConsumer(Session &session);

  // Create or resume a named durable subscription (topic-family only).
  // Throws Poco::RuntimeException if called on a queue, or if the subscription
  // already has an active consumer attached.
  Consumer::Ptr createDurableConsumer(Session &session, const std::string &subscriptionName,
                                      std::shared_ptr<Selector> selector = nullptr);

  // Permanently remove a named durable subscription.  The active consumer (if
  // any) is disconnected first and all buffered messages are discarded.
  void deleteSubscription(const std::string &subscriptionName);

  void save(const Producer &producer, const Message &message);

  // Deliver already-committed messages directly to consumer queues (no re-persistence).
  // Takes ownership of the message ptr — moves it into the queue for queue-family
  // destinations, copies for topic-family (one copy per subscriber).
  void deliverCommitted(Message::Ptr message);

  // Replay non-deleted records from a storage instance into a queue.
  static void replayFromStorage(QueueT &queue, linear_storage::ConcurrentLinearStorage &storage);

  // Replay non-deleted messages from the destination's own storage (restart path).
  void replayStoredMessages(QueueT &queue) const;

  // Serialise message to [type-byte][toBytes()] and append to an offline durable
  // sub's storage.  Selector matching must be performed by the caller.
  static void persistToOfflineSub(DurableSubState &sub, const Message &message);

  // Transaction buffer access
  TransactionBuffer::Ptr getTransactionBuffer() const;

  // Transaction operations
  void commitTransaction(const std::string& transactionId);
  void rollbackTransaction(const std::string& transactionId);

  friend class Exchange;
  friend class Producer;
  friend class Consumer;
  friend class Session;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__DESTINATION_H_
