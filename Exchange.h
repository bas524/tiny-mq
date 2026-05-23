//
// Created by Alexander Bychuk on 04.11.2021.
//

#ifndef TINY_MQ__EXCHANGE_H_
#define TINY_MQ__EXCHANGE_H_

#include <Poco/Path.h>
#include <Poco/Mutex.h>
#include "DestinationType.h"
#include "Destination.h"
#include "parallel_hashmap/phmap.h"
namespace tiny_mq {

class Exchange {
  phmap::parallel_node_hash_map<size_t, Destination::Ptr> _destinations;
  Poco::Path _path;
  std::reference_wrapper<Poco::Logger> _logger;
  Poco::FastMutex _mutex;

 public:
  explicit Exchange(Poco::Path path);
  virtual ~Exchange();
  Exchange(const Exchange &) = delete;
  Exchange(Exchange &&) = delete;
  Exchange &operator=(const Exchange &) = delete;
  Exchange &operator=(Exchange &&) = delete;

 private:
  Destination::Ptr create(destination::Type type, const std::string &destinationName);
  friend class Session;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__EXCHANGE_H_
