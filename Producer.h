//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__PRODUCER_H_
#define TINY_MQ__PRODUCER_H_

#include <memory>
#include <Poco/Logger.h>
#include "Message.h"
#include "BlockingConcurrentQueueHeader.h"

namespace tiny_mq {
class Destination;
class Session;
class Producer {
  Poco::UUID _uuid;
  std::reference_wrapper<Destination> _destination;
  std::unique_ptr<moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t> _token;
  std::reference_wrapper<Poco::Logger> _logger;
  std::reference_wrapper<Session> _session;
  std::shared_ptr<QueueT> _transactQueue;
  bool _disableMessageID{false};
  bool _disableMessageTimestamp{false};

 public:
  using Ptr = std::shared_ptr<Producer>;
  void send(const Message &message);
  const std::string &transactionId() const;
  virtual ~Producer();
  const Poco::UUID &id() const;

  // JMS 2.0 § 7.5 — hints to skip provider-assigned MessageID / Timestamp.
  void setDisableMessageID(bool value);
  bool isDisableMessageID() const;
  void setDisableMessageTimestamp(bool value);
  bool isDisableMessageTimestamp() const;

 private:
  explicit Producer(Destination &destination,
                    Session &session,
                    const Poco::UUID &uuid,
                    std::unique_ptr<moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t> token);
  const moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t &token() const;
  void commit(const std::string& transactionId);
  void rollback(const std::string& transactionId);
  QueueT &transactQueue() const;
  Destination &destination() const;

  friend class Destination;
  friend class Consumer;
  friend class Session;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__PRODUCER_H_
