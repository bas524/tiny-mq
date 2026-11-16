//
// Spec 24 (redelivery + DLQ).
//

#ifndef TINY_MQ__REDELIVERYPOLICY_H_
#define TINY_MQ__REDELIVERYPOLICY_H_

#include <cstdint>
#include <memory>

namespace tiny_mq {
class Destination;

// Per-destination redelivery/DLQ policy (JMS-provider-specific; mirrors the
// ActiveMQ Classic RedeliveryPolicy behavioral model). See
// docs/jms-spec/24-redelivery-and-dlq.md.
struct RedeliveryPolicy {
  int32_t maxRedeliveries = 6;      // redeliveries beyond the first delivery; < 0 = unlimited
  int64_t backoffMs = 0;            // delay before the 1st redelivery; 0 = immediate
  int64_t maxBackoffMs = 60000;     // exponential backoff ceiling
  std::shared_ptr<Destination> deadLetterQueue;  // nullptr = drop the message (with a warning)
};
}  // namespace tiny_mq

#endif  // TINY_MQ__REDELIVERYPOLICY_H_
