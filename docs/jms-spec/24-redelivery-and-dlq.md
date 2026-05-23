# Redelivery counter & Dead Letter Queue

## JMS reference
- JMS 2.0 § 3.5.7 `JMSXDeliveryCount` property.
- DLQ is provider-specific; the spec only mandates the counter.

## Current state in tiny-mq
- No redelivery counter, no DLQ. Rollback currently re-queues indefinitely.

## Proposed API
- Per-destination policy:
```cpp
struct RedeliveryPolicy {
  int     maxRedeliveries = 6;
  int64_t backoffMs       = 1000; // exponential
  Destination::Ptr deadLetterQueue; // nullptr = drop
};
void Destination::setRedeliveryPolicy(RedeliveryPolicy);
```

## Semantics
- Each redelivery increments `headers.deliveryCount`.
- When `deliveryCount > maxRedeliveries`, the message is removed from its origin and appended to the DLQ (or dropped). Original headers preserved; add `JMSXDeadLetterReason` property.
- Backoff applied by delaying visibility of the redelivered message (reuses scheduled-delivery plumbing from spec 13).

## Persistence / wire implications
- DLQ is just another `Destination` — no new storage types.
- Counter field persists on the message record.

## Dependencies
- 10 (headers), 13 (delivery delay for backoff), 23 (recover).

## Test plan
- `DlqTest`: message that always fails lands in DLQ after `maxRedeliveries` with preserved body.
- `DlqTest`: `deliveryCount` monotonic across recover/rollback paths.

## Open questions
- DLQ auto-creation naming convention: `DLQ.<origin-name>` vs explicit configuration? Propose configurable with sane default.
