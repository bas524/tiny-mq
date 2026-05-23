# Delivery delay (JMS 2.0)

## JMS reference
- JMS 2.0 § 7.8 `MessageProducer.setDeliveryDelay`.

## Current state in tiny-mq
- Not implemented. No scheduled delivery mechanism.

## Proposed API
- Field on `SendOptions` (see 12) and on `Message::Headers::deliveryTime`.
- `Destination` holds a scheduled-delivery min-heap keyed by `deliveryTime`.

## Semantics
- Message becomes visible to consumers only once `now_ms >= deliveryTime`.
- Must survive restart when `deliveryMode == PERSISTENT` — storage keeps the record; on replay the scheduler re-inserts the timer.
- Transactional sends: the delay timer starts at **commit time**, not send time.

## Persistence / wire implications
- `headers.deliveryTime` persisted alongside other headers.
- A lightweight timer thread per `Destination` (or a shared broker timer) fires visibility transitions.

## Dependencies
- 10 (headers), 12 (per-send options).

## Test plan
- `DeliveryDelayTest`: message invisible until timer fires.
- `DeliveryDelayTest`: persistent delayed message survives restart.
- `DeliveryDelayTest`: transactional commit starts the clock, rollback discards.

## Open questions
- Single global timer vs per-destination timer? Start with per-destination for isolation.
