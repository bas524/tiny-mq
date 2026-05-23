# DUPS_OK_ACKNOWLEDGE

## JMS reference
- JMS 2.0 § 8.4.11 `DUPS_OK_ACKNOWLEDGE`.

## Current state in tiny-mq
- `Session::AcknowledgeMode` (`Session.h:20`) lists `SESSION_TRANSACTED`, `AUTO_ACKNOWLEDGE`, `CLIENT_ACKNOWLEDGE`, `INDIVIDUAL_ACKNOWLEDGE` — `DUPS_OK_ACKNOWLEDGE` is absent.

## Proposed API
- Add `DUPS_OK_ACKNOWLEDGE = 4` to the enum.
- No new method surface.

## Semantics
- Lazy, batched acknowledgement. Consumer accumulates ack intents and flushes periodically (N messages or T ms) to the broker.
- On crash before flush, client may see duplicates — which is exactly the mode's contract.
- Useful for high-throughput topic subscribers that tolerate duplicates.

## Persistence / wire implications
- Storage `delete` calls are batched; record multiple message ids in one storage op if `ConcurrentLinearStorage` supports it, else one-by-one at flush time.

## Dependencies
- None beyond current ack plumbing.

## Test plan
- `DupsOkAckTest`: verify batching — individual `recv` calls do not immediately delete from storage.
- `DupsOkAckTest`: simulated crash mid-batch exposes duplicates on replay (expected behaviour).

## Open questions
- Batch size and flush interval defaults? Propose 100 messages / 1 second.
