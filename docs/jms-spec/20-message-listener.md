# MessageListener (async push delivery)

## JMS reference
- JMS 2.0 § 8.7 `MessageConsumer.setMessageListener`, `MessageListener.onMessage`.

## Current state in tiny-mq
- Only pull-based `Consumer::recv(int64_t usec_timeout)` (`Consumer.h:36`).

## Proposed API
```cpp
using MessageListener = std::function<void(Message::Ptr)>;
void Consumer::setMessageListener(MessageListener);
void Consumer::clearMessageListener();
```

## Semantics
- Once a listener is set, no concurrent `recv()` is permitted (throw).
- A dedicated delivery thread per session (not per consumer) drains queues and dispatches to the right listener — matches JMS "session serial" guarantee: a session dispatches one message at a time across all its consumers.
- Interaction with ack modes:
  - `AUTO_ACKNOWLEDGE`: ack after listener returns normally.
  - `CLIENT_ACKNOWLEDGE`/`INDIVIDUAL_ACKNOWLEDGE`: ack only when client calls `acknowledgeOn`.
  - `SESSION_TRANSACTED`: no auto-ack; awaits `commit`.
  - Exception from listener → redelivery path (requires spec 23/24).

## Persistence / wire implications
- None directly. Listener errors increment `JMSXDeliveryCount`.

## Dependencies
- 23 (recover) and 24 (redelivery) — listener-thrown exceptions plug into those flows.

## Test plan
- `MessageListenerTest`: set listener, produce N messages, assert all delivered in order.
- `MessageListenerTest`: session serial guarantee — two consumers on the same session never overlap.
- `MessageListenerTest`: listener exception under `AUTO_ACKNOWLEDGE` triggers redelivery.

## Open questions
- Thread pool per session vs single thread? Start with single thread to match JMS serial semantics.
