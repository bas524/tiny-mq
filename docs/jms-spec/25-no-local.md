# NoLocal

## JMS reference
- JMS 2.0 § 8.3 `Session.createConsumer(Topic, String, boolean noLocal)`.

## Current state in tiny-mq
- Not implemented. Topic subscribers always receive messages produced by their own connection.

## Proposed API
```cpp
Consumer::Ptr Session::createConsumer(const Destination::Ptr& topic,
                                     const std::string& selector,
                                     bool noLocal);
```
Also a `noLocal` flag on the durable variants.

## Semantics
- Topic-family only. If `noLocal=true`, the consumer must not receive messages sent by producers belonging to the **same Connection** (JMS 2.0 — Connection granularity, not Session).
- Requires carrying the origin-connection id on each in-flight message; filter in `Destination` topic fan-out loop.

## Persistence / wire implications
- Non-persistent filter — origin-connection id lives only in memory; after broker restart a durable subscriber has no way to replay the filter, so it receives everything (documented behaviour).

## Dependencies
- 01 (Connection layer) — connection id must exist before this means anything.

## Test plan
- `NoLocalTest`: same-connection publisher+subscriber with `noLocal=true` → subscriber sees nothing.
- `NoLocalTest`: cross-connection delivery unaffected.

## Open questions
- None.
