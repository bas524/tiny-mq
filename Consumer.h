//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__CONSUMER_H_
#define TINY_MQ__CONSUMER_H_

#include <memory>
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include "Message.h"
#include "ConcurrentLinearStorage.h"
#include "TransactionBuffer.h"
#include "Selector.h"

namespace tiny_mq {
class Destination;
class Producer;
class Session;
class Consumer {
  Poco::UUID _uuid;
  Poco::Path _path;
  std::reference_wrapper<Destination> _destination;
  std::shared_ptr<QueueT> _queue;
  QueueT::consumer_token_t _token;
  std::reference_wrapper<Poco::Logger> _logger;
  std::reference_wrapper<Session> _session;
  std::shared_ptr<QueueT> _transactQueue;
  std::shared_ptr<linear_storage::ConcurrentLinearStorage> _storage;
  std::shared_ptr<TransactionBuffer> _transactionBuffer;
  std::shared_ptr<Selector> _selector;  // nullptr = no filter (match all)
  // DUPS_OK_ACKNOWLEDGE: storage removals accumulate here and flush in batches.
  std::vector<linear_storage::Record> _pendingAcks;

 public:
  using Ptr = std::shared_ptr<Consumer>;
  const Poco::UUID &id() const;
  Message::Ptr recv(int64_t usec_timeout = 10000000);
  moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t getProducerToken();
  void acknowledgeOn(const Message &message);
  const Session &session() const;
  virtual ~Consumer();

 private:
  explicit Consumer(Destination &destination, Session &session, std::shared_ptr<QueueT> queue, const Poco::UUID &uuid, Poco::Path path, std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage, std::shared_ptr<TransactionBuffer> transactionBuffer, std::shared_ptr<Selector> selector = nullptr);
  Message::Ptr preparePush(int64_t number, const Producer &producer, const Message &message);
  void push(int64_t number, const Producer &producer, const Message &message);
  void commit();
  void rollback();
  void flushPendingAcks();
  QueueT &transactQueue() const;
  Destination &destination() const;
  
  friend class Destination;
  friend class Session;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__CONSUMER_H_
