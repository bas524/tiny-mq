# Per-send DeliveryMode / Priority / TTL

## JMS reference
- JMS 2.0 § 7.6 `MessageProducer.send(Message, int deliveryMode, int priority, long timeToLive)`.

## Current state in tiny-mq
- `Producer::send(const Message&)` (`Producer.h:26`). Reliability is baked into the message at `Session::createXxxMessage`.
- No per-send priority, no TTL.

## Proposed API
```cpp
struct SendOptions {
  Message::Reliability deliveryMode = Message::NOT_PERSISTENT;
  int     priority   = 4;
  int64_t timeToLive = 0;          // 0 = never expire
  int64_t deliveryDelay = 0;       // see spec 13
};
void Producer::send(const Message& message);                   // uses producer defaults
void Producer::send(const Message& message, SendOptions opts); // per-send override
void Producer::setDefault(SendOptions);
```

## Semantics
- `opts` overrides values set on the message; applied immediately before persistence/enqueue.
- `timeToLive == 0` means no expiration; else `headers.expiration = now_ms + timeToLive`.
- Priority 0..9; 0..3 low, 4 normal (default), 5..9 expedited.

## Persistence / wire implications
- Writes `headers.priority` and `headers.expiration` set by this call.

## Dependencies
- 10 (headers), 44 (expiration sweep), 45 (priority ordering) — 12 is the ingress; those consume the metadata.

## Test plan
- `SendOptionsTest`: per-send override beats producer default; default persists across sends.
- `SendOptionsTest`: TTL sets correct expiration; priority propagates through storage and selector.

## Open questions
- None.
