# Message expiration sweep

## JMS reference
- JMS 2.0 § 3.4.9 `JMSExpiration`.

## Current state in tiny-mq
- No expiration field, no sweep. Messages live forever (until consumed or explicitly deleted).

## Proposed API
- No public API; internal behaviour triggered by `headers.expiration`.

## Semantics
- On `Consumer::recv` (pop path): check `expiration`; if expired, drop and continue — never deliver.
- Background sweeper in `ConcurrentLinearStorage` worker scans for expired persistent records and removes them to reclaim disk.
- Expired durable-subscriber messages removed from that subscriber's storage too.

## Persistence / wire implications
- Requires `headers.expiration` (spec 10).
- Sweeper cadence configurable (default 1 s); must not starve normal I/O.

## Dependencies
- 10 (headers), 12 (TTL populates expiration).

## Test plan
- `ExpirationTest`: TTL=100ms, sleep 200ms, `recv` returns next message (expired one dropped).
- `ExpirationTest`: expired messages disappear from `ConcurrentLinearStorage` within one sweep interval.

## Open questions
- Should expired-and-dropped messages route to DLQ? JMS says no; keep them silent by default.
