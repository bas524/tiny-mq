//
// Created by Alexander Bychuk on 04.11.2021.
//

#include "Exchange.h"
#include "Session.h"
#include "LogTracer.h"
#include <Poco/File.h>
#include "DestinationHash.h"
namespace tiny_mq {

Exchange::Exchange(Poco::Path path) : _path(std::move(path)), _logger(Poco::Logger::get("tiny_mq.exchange")) {
  TRACE(_logger);
  Poco::File dir(_path);
  dir.createDirectories();
}
Destination::Ptr Exchange::create(destination::Type type, const std::string& destinationName) {
  TRACE(_logger);
  Poco::ScopedLock<Poco::FastMutex> sl(_mutex);
  Poco::Path path = _path;
  auto hash = destination::hash(type, destinationName);
  auto itDest = _destinations.find(hash);
  if (itDest != _destinations.end()) {
    return itDest->second;
  } else {
    std::string hashString = std::to_string(hash);
    path.append(hashString).makeDirectory();
    auto destination = Destination::Ptr(new Destination(type, destinationName, std::move(path)));
    auto it = _destinations.emplace(destination->hash(), destination);
    if (it.second) {
      return it.first->second;
    }
  }
  return nullptr;
}
Exchange::~Exchange() {
  TRACE(_logger);
  _destinations.clear();
}
}  // namespace tiny_mq
