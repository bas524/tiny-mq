# JMSContext (JMS 2.0 simplified API)

## JMS reference
- JMS 2.0 § 12 "JMS simplified API".

## Current state in tiny-mq
- Not implemented.

## Proposed API
```cpp
class Context {
 public:
  using Ptr = std::shared_ptr<Context>;
  Producer::Ptr createProducer();                          // context-owned, reusable
  Consumer::Ptr createConsumer(const Destination::Ptr&);
  TextMessage   createTextMessage(std::string);
  // ... all message types
  void commit();
  void rollback();
  void close();
};
Context::Ptr ConnectionFactory::createContext(Session::AcknowledgeMode);
```

## Semantics
- Façade over `Connection + Session + Producer + Consumer`; no new messaging semantics.
- `Context` owns a hidden `Connection` and `Session`; lifecycle mirrors both.
- Producer-in-context is a fluent object: `ctx.createProducer().setPriority(9).send(dest, msg)`.

## Persistence / wire implications
- None — pure API shape.

## Dependencies
- 01 (Connection).

## Test plan
- `ContextTest`: parity with equivalent Connection+Session+Producer test (send/recv, transactions).

## Open questions
- None.
