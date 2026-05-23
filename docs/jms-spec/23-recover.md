# Session.recover()

## JMS reference
- JMS 2.0 § 8.4.8 `Session.recover()`.

## Current state in tiny-mq
- No equivalent. There is no tracking of "in-flight, not yet acked" messages per consumer.

## Proposed API
```cpp
void Session::recover();
```

## Semantics
- Valid only for non-transacted sessions (`AUTO_`, `CLIENT_`, `DUPS_OK_`, `INDIVIDUAL_`).
- Stops delivery, requeues every message the session has received but not acknowledged, with `headers.redelivered = true` and `headers.deliveryCount++`.
- Ordering is redelivered in original order.

## Persistence / wire implications
- Requires per-consumer "in-flight" set (map of message-id → storage location). Already partially present via `_storageTomId`/`_storageOffset` on `Message`.

## Dependencies
- 10 (headers for redelivered + deliveryCount).

## Test plan
- `RecoverTest` (CLIENT_ACK): receive 3, ack 1, recover, expect 2 redelivered with `redelivered=true`.
- `RecoverTest` rejects call on transacted session (must use `rollback`).

## Open questions
- Interaction with durable subscribers: redelivered messages should not be re-persisted to durable storage (already stored).
