# C++ client library

## Purpose
A standalone C++ client library that speaks the tiny-mq wire protocol (spec 40) and exposes the same in-process API shape that the broker already provides (`Connection`, `Session`, `Producer`, `Consumer`, `Message`).

## Scope split
tiny-mq already has an **in-process** C++ API (the classes in `Session.h`, `Producer.h`, `Consumer.h`, `Message.h`). This spec covers the **remote** C++ client:

- Embeds no broker code.
- Depends only on: Poco (Net, NetSSL, Foundation), and the shared message-format headers.
- Same public API as in-process, so user code can switch between `ConnectionFactory::inProcess(...)` and `ConnectionFactory::network("tcp://host:port")` with no source changes.

## Directory layout
```
client-cpp/
├── include/tiny_mq/       # re-export of public headers (Connection, Session, …)
├── src/
│   ├── StompCodec.{h,cpp}      # v1 wire (spec 40)
│   ├── AmqpCodec.{h,cpp}       # v2 wire, behind a build flag
│   ├── NetworkConnection.cpp   # implements Connection over a socket
│   ├── NetworkSession.cpp
│   ├── NetworkProducer.cpp
│   └── NetworkConsumer.cpp
├── tests/                 # against a running broker (integration)
└── CMakeLists.txt         # exports a `tiny_mq::client` target
```

## Proposed API
No new public surface — `Connection`, `Session`, `Producer`, `Consumer`, `Message` are exactly the types from the broker's public headers. Only the factory differs:
```cpp
auto cf   = ConnectionFactory::network("tcp://broker:61613");
auto conn = cf.createConnection("user", "pass");
conn->setClientID("my-app-1");
conn->start();
auto s   = conn->createSession(Session::AUTO_ACKNOWLEDGE);
auto q   = s->createDestination(destination::QUEUE, "orders");
auto p   = s->createProducer(q);
p->send(s->createTextMessage("hello", Message::PERSISTENT));
```

## Semantics
- Thread model: one reader thread per `Connection` dispatches inbound frames to the right `Session`/`Consumer`; `send` is synchronous on the caller thread and serialised by a writer mutex (or a SPSC outbound queue).
- Reconnect: optional automatic reconnect with exponential backoff; on success, re-subscribes durable consumers and republishes the `clientID`. Non-durable state is lost (documented).
- Heartbeats: per the wire protocol's heartbeat spec.
- Errors surface via `ExceptionListener` (spec 03).

## Ack / transaction mapping
- `AUTO_ACKNOWLEDGE` → client sends ACK frame after `recv` returns.
- `CLIENT_ACKNOWLEDGE` / `INDIVIDUAL_ACKNOWLEDGE` → ACK on explicit `acknowledgeOn`.
- `SESSION_TRANSACTED` → client issues BEGIN/COMMIT/ABORT frames wrapping sends and acks.

## Dependencies
- 10 (headers — wire format must carry them)
- 01 (Connection)
- 40 (wire protocol)
- 41 (auth/TLS) — client must support SASL-PLAIN and TLS to connect to a secured broker.

## Test plan
- `ClientIntegrationTest`: boot broker on loopback, run client test against it (round-trip, ack modes, transactions, reconnect).
- Conformance matrix: every in-process test that can be rewritten against the network factory is run in both modes to prove API parity.

## Open questions
- Package name / vcpkg port? Publish `tiny-mq-client` separately from the broker.
- Shared vs static by default? Static for ease of embedding; shared available via CMake option.
