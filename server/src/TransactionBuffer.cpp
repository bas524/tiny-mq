#include "TransactionBuffer.h"

namespace tiny_mq {
void TransactionBuffer::stage(Producer& producer, Message::Ptr message) {
  const auto priority = message->jmsHeaders.priority;
  _sends.push_back({&producer, std::move(message), priority});
}
}
