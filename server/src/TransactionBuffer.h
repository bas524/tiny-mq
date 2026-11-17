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
  void clear() noexcept;
  std::vector<Send> _sends;
  // Bounds over the staged copies, compared with the commit time.
  int64_t _latestDelivery = 0;
  int64_t _earliestExpiration = 0;
  bool _hasPersistentSends = false;
  bool _oneProducer = true;
};
}
#endif
