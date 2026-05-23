# Standard JMS message headers

## JMS reference
- JMS 2.0 § 3.4 "Message Headers"; fields `JMSMessageID`, `JMSTimestamp`, `JMSExpiration`, `JMSPriority`, `JMSDeliveryMode`, `JMSRedelivered`, `JMSReplyTo`, `JMSCorrelationID`, `JMSType`, `JMSDeliveryTime`, and the provider-set property `JMSXDeliveryCount`.

## Current state in tiny-mq
- `Message` (`Message.h:22`) carries only `uuid`, `_number`, `Properties`, and a `Reliability` flag.
- No timestamp, expiration, priority, reply-to, correlation id, or redelivery counter.
- `JMSMessageID` is effectively `uuid`, but is not exposed as the JMS-style `"ID:..."` string.

## Proposed API
Extend `Message`:
```cpp
struct Headers {
  std::string    messageId;         // "ID:<uuid>"
  int64_t        timestamp = 0;     // set by Producer::send unless disabled
  int64_t        expiration = 0;    // 0 = never; absolute ms since epoch
  int            priority = 4;      // 0..9, default 4
  Reliability    deliveryMode = NOT_PERSISTENT;
  bool           redelivered = false;
  std::string    replyTo;           // destination URI
  std::string    correlationId;
  std::string    type;
  int64_t        deliveryTime = 0;  // JMS 2.0, absolute ms
  int            deliveryCount = 0; // JMSXDeliveryCount
};
Headers& headers();
const Headers& headers() const;
```
Keep existing `reliability` field as a view over `headers().deliveryMode` during migration.

## Semantics
- `Producer::send` sets `messageId`, `timestamp`, `deliveryMode`, `priority`, `expiration` (from TTL) unless explicitly disabled (see spec 14).
- `redelivered` set true on requeue paths (recover, rollback-induced redelivery, DLQ promotion precursor).
- `deliveryCount` starts at 1 on first delivery attempt, increments on each redelivery.

## Persistence / wire implications
- Serialise headers before properties in `toBytes()`; bump a storage format version byte.
- `ConcurrentLinearStorage` records must survive format upgrades — include a magic/version prefix check on read.

## Dependencies
- None — foundational. Blocks 12, 23, 24, 31, and the wire protocol (40).

## Test plan
- `HeadersTest`: round-trip each header through `toBytes`/`fromBytes`.
- `HeadersTest`: default-priority assignment, timestamp monotonicity under concurrent producers.
- `HeadersTest`: storage format backward-compat — read a pre-header record and fail loud (no silent corruption).

## Open questions
- Keep Poco JSON property serialisation or migrate to a binary header block?
- How strictly to enforce `messageId` uniqueness on read-back after restart?
