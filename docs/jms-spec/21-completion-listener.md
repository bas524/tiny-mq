# CompletionListener (async send, JMS 2.0)

## JMS reference
- JMS 2.0 § 7.3 `MessageProducer.send(Message, CompletionListener)`.

## Current state in tiny-mq
- `Producer::send(const Message&)` (`Producer.h:26`) is synchronous.

## Proposed API
```cpp
using CompletionListener = std::function<void(const Message&, std::exception_ptr)>;
void Producer::send(const Message& msg, CompletionListener cb);
void Producer::send(const Message& msg, SendOptions opts, CompletionListener cb);
```

## Semantics
- Returns immediately; listener is called on completion (success → `exception_ptr == nullptr`).
- Listener runs on a producer-side worker thread (reuse existing `ConcurrentLinearStorage` worker idiom).
- Ordering: messages sent from the same producer are **completed in send order**, even with async completion.
- Interaction with transactions: banned under `SESSION_TRANSACTED` (spec requires `IllegalStateException`).

## Persistence / wire implications
- None beyond existing send path.

## Dependencies
- 12 (SendOptions).

## Test plan
- `AsyncSendTest`: listener fires once per send; N async sends complete in order.
- `AsyncSendTest`: throws when called on transacted session.

## Open questions
- Per-producer dedicated thread vs pool? Start with shared pool + producer ordering token.
