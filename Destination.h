//
// Created by Alexander Bychuk on 04.11.2021.
//

#ifndef TINY_MQ__DESTINATION_H_
#define TINY_MQ__DESTINATION_H_

#include <memory>
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include "Producer.h"
#include "Consumer.h"
#include "DestinationType.h"
#include "parallel_hashmap/phmap.h"
#include "ConcurrentQueueHeader.h"
namespace tiny_mq {
class Destination {
  destination::Type _type;
  std::string _name;
  Poco::Path _path;
  Poco::Path _sent;
  std::atomic_int64_t _messageCounter{0};
  phmap::parallel_node_hash_map<std::string, Consumer::Ptr> _consumers;
  phmap::parallel_node_hash_map<std::string, Producer::Ptr> _producers;
  Poco::UUIDGenerator _uuidGenerator;
  std::shared_ptr<QueueT> _queue;
  Consumer::Ptr _defaultConsumer;
  Poco::Logger &_logger;
  std::string _uri;
  size_t _hash;

 public:
  using Ptr = std::shared_ptr<Destination>;
  virtual ~Destination();
  Producer::Ptr createProducer();
  Consumer::Ptr createConsumer();
  size_t consumersCount() const;
  size_t producersCount() const;
  destination::Type type() const;
  const std::string &name() const;
  std::string typeName() const;
  const std::string &uri() const;
  size_t hash() const;

 private:
  Destination(destination::Type type, std::string name, Poco::Path path);
  void save(const Producer &producer, const Message &message);
  friend class Exchange;
  friend class Producer;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__DESTINATION_H_
