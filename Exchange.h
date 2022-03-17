//
// Created by Alexander Bychuk on 04.11.2021.
//

#ifndef TINY_MQ__EXCHANGE_H_
#define TINY_MQ__EXCHANGE_H_

#include <Poco/Path.h>
#include "DestinationType.h"
#include "Destination.h"
#include "parallel_hashmap/phmap.h"
namespace tiny_mq {
class Exchange {
  phmap::parallel_node_hash_map<size_t, Destination::Ptr> _destinations;
  Poco::Path _path;
  Poco::Logger& _logger;

 public:
  explicit Exchange(Poco::Path path);
  virtual ~Exchange();
  Destination::Ptr create(destination::Type type, const std::string& destinationName);
};
}  // namespace tiny_mq
#endif  // TINY_MQ__EXCHANGE_H_
