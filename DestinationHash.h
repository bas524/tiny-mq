//
// Created by Alexander Bychuk on 08.02.2022.
//

#ifndef TINY_MQ__DESTINATIONHASH_H_
#define TINY_MQ__DESTINATIONHASH_H_

#include <Poco/Hash.h>
#include "DestinationType.h"
namespace tiny_mq {
namespace destination {
std::size_t hash(destination::Type, const std::string& destinationName);
}
}  // namespace tiny_mq
#endif  // TINY_MQ__DESTINATIONHASH_H_
