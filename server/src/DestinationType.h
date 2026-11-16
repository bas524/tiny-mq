//
// Created by Alexander Bychuk on 04.11.2021.
//

#ifndef TINY_MQ__DESTINATIONTYPE_H_
#define TINY_MQ__DESTINATIONTYPE_H_

#include <string>

namespace tiny_mq {
namespace destination {
enum Type { Queue = 1, Topic = 2, TemporaryQueue = 3, TemporaryTopic = 4 };
inline std::string TypeName(Type type) {
  switch (type) {
    case destination::Queue:
      return {"queue"};
    case destination::Topic:
      return {"topic"};
    case destination::TemporaryQueue:
      return {"temp-queue"};
    case destination::TemporaryTopic:
      return {"temp-topic"};
  }
  return {};
}
}  // namespace destination
}  // namespace tiny_mq
#endif  // TINY_MQ__DESTINATIONTYPE_H_
