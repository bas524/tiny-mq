# receiveNoWait

## JMS reference
- JMS 2.0 § 8.5 `MessageConsumer.receiveNoWait()`.

## Current state in tiny-mq
- `Consumer::recv(int64_t usec_timeout = 10000000)` — always blocks for up to the timeout; no "return immediately" form.

## Proposed API
```cpp
Message::Ptr Consumer::recvNoWait();   // returns nullptr immediately if queue empty
```

## Semantics
- Non-blocking poll. No difference in ack semantics.

## Persistence / wire implications
- None.

## Dependencies
- None.

## Test plan
- `RecvNoWaitTest`: empty queue → returns `nullptr` within microseconds.
- `RecvNoWaitTest`: produced message → returns it.

## Open questions
- None.
