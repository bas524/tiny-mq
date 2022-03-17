//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__CONSUMER_H_
#define TINY_MQ__CONSUMER_H_

#include <memory>
#include <Poco/Path.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Logger.h>
#include "Message.h"
#include "parallel_hashmap/phmap.h"

namespace tiny_mq {
class Destination;

class Consumer {
  Poco::UUID _uuid;
  Poco::Path _path;
  Poco::Path _sent;
  Destination &_destination;
  phmap::parallel_node_hash_map<std::string, std::string> _messages;
  std::shared_ptr<QueueT> _queue;
  QueueT::consumer_token_t _token;
  std::atomic_bool _needToStop{false};
  Poco::UUIDGenerator _uuidGenerator;
  Poco::Logger &_logger;

 public:
  using Ptr = std::shared_ptr<Consumer>;
  const Poco::UUID &id() const;
  Message::Ptr recv(int64_t usec_timeout = 1);
  moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t getProducerToken();
  void acknowledgeOn(const Message &message);
  void stop();
  virtual ~Consumer();

 private:
  explicit Consumer(Destination &destination, std::shared_ptr<QueueT> queue, const Poco::UUID &uuid, Poco::Path path);
  Message::Ptr preparePush(int64_t number, const Message &message);
  void push(int64_t number, const QueueT::producer_token_t &token, const Message &message);
  void push(int64_t number, const Message &message);
  friend class Destination;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__CONSUMER_H_
