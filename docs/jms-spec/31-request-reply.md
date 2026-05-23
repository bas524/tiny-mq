# Request / reply pattern

## JMS reference
- JMS 2.0 § 6.2 `TemporaryQueue`, § 3.4.5 `JMSReplyTo`, § 3.4.6 `JMSCorrelationID`.

## Current state in tiny-mq
- `TemporaryQueue`/`TemporaryTopic` destination types exist in `destination::Type`.
- No standard headers `JMSReplyTo`/`JMSCorrelationID` → no idiomatic way to implement request/reply.

## Proposed API
- No new types; becomes implementable purely on top of spec 10 (headers).
- Optional helper:
```cpp
class Requestor {
 public:
  Requestor(Session&, Destination& target);
  Message::Ptr request(const Message& msg, int64_t timeoutUs);
};
```

## Semantics
- Client creates a `TemporaryQueue`, sets it as `replyTo`, sends request, blocks on `recv` from the temp queue.
- Responder echoes `JMSMessageID` of request into `JMSCorrelationID` of reply and sends to `replyTo`.
- Temporary destinations must be tied to the **Connection** (deleted on connection close) — requires spec 01.

## Persistence / wire implications
- None beyond headers.

## Dependencies
- 10 (headers), 01 (Connection).

## Test plan
- `RequestReplyTest`: request/reply round-trip; correlation id matches; timeout returns nullptr.

## Open questions
- Provide the `Requestor` helper or leave the pattern to clients? Include it — it's small and documents the idiom.
