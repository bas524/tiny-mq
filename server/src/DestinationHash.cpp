//
// Created by Alexander Bychuk on 08.02.2022.
//

#include "DestinationHash.h"
#include <Poco/Hash.h>
#include <Poco/Format.h>
namespace tiny_mq {
std::size_t destination::hash(destination::Type type, const std::string& destinationName) {
  return Poco::hash(Poco::format("%s://%s", destination::TypeName(type), destinationName));
}
}  // namespace tiny_mq