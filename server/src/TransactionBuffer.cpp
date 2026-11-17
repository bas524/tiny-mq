#include "TransactionBuffer.h"
#include <algorithm>

namespace tiny_mq {
void TransactionBuffer::stage(Producer& producer, Message::Ptr message) {
  const auto& draft = *message;
  const auto priority = draft.jmsHeaders.priority;
  _sends.push_back({&producer, std::move(message), priority});
  _hasPersistentSends |= draft.isPersistent();
  _oneProducer &= _sends.front().producer == &producer;
  _latestDelivery = std::max(_latestDelivery, draft.jmsHeaders.deliveryTime);
  const auto expiresAt = draft.jmsHeaders.expiration;
  if (expiresAt != 0 && (_earliestExpiration == 0 || expiresAt < _earliestExpiration)) {
    _earliestExpiration = expiresAt;
  }
}

void TransactionBuffer::clear() noexcept {
  _sends.clear();
  _latestDelivery = 0;
  _earliestExpiration = 0;
  _hasPersistentSends = false;
  _oneProducer = true;
}
}
