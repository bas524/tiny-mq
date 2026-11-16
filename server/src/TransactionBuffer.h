#ifndef TINY_MQ_TRANSACTION_BUFFER_H
#define TINY_MQ_TRANSACTION_BUFFER_H

#include "Producer.h"
#include <vector>

namespace tiny_mq {
// Internal transaction output sequence, shared by all producers of a session.
class TransactionBuffer {
  friend class Session;
  friend class Producer;
  struct Send {
    Producer* producer;
    Message::Ptr message;
    int32_t priority;
  };

  void stage(Producer& producer, Message::Ptr message);
  std::vector<Send> _sends;
};
}
#endif
