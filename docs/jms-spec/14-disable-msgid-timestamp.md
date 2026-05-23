# Disable MessageID / Disable Timestamp

## JMS reference
- JMS 2.0 § 7.5 `MessageProducer.setDisableMessageID`, `setDisableMessageTimestamp`.

## Current state in tiny-mq
- Both always set (uuid always generated, no timestamp yet).

## Proposed API
```cpp
void Producer::setDisableMessageID(bool);
void Producer::setDisableMessageTimestamp(bool);
```

## Semantics
- Hint to the provider; tiny-mq MAY still assign values internally but must not expose them in outgoing message headers when disabled (they become empty string / 0 on the wire and in storage).
- Useful for high-throughput producers where the extra cost matters.

## Persistence / wire implications
- When disabled, skip writing the header bytes (smaller record size).

## Dependencies
- 10 (headers).

## Test plan
- `DisableMsgIdTest`: when disabled, received message has empty `messageId`.
- `DisableTimestampTest`: when disabled, `timestamp == 0` on the receiver.

## Open questions
- None.
