# Connection / ConnectionFactory

## JMS reference
- JMS 2.0 § 6 "Connection", § 4 "ConnectionFactory".

## Current state in tiny-mq
- No `Connection` type. `Session` is constructed directly with `Exchange&` (`Session.h:88`).
- `TemporaryQueue`/`TemporaryTopic` lifetime is implicit — tied to the `Session` that created them rather than to a connection.

## Proposed API
```cpp
class ConnectionFactory {
 public:
  static ConnectionFactory inProcess(Exchange&);
  static ConnectionFactory network(std::string_view uri);
  Connection::Ptr createConnection();
};

class Connection {
 public:
  using Ptr = std::shared_ptr<Connection>;
  Session::Ptr createSession(Session::AcknowledgeMode);
  void setClientID(std::string);
  const std::string& clientID() const;
  void start();
  void stop();
  void close();
  void setExceptionListener(std::function<void(const std::exception&)>);
};
```

## Semantics
- A `Connection` owns its sessions, its `ClientID`, and the set of temporary destinations created under it.
- Message delivery (to listeners) is paused until `start()`; `stop()` halts delivery but allows sends.
- `close()` cascades: closes sessions, deletes temp destinations, flushes persistent state.
- Durable subscription identity is `(clientID, subscriptionName)` — this reshapes `Destination::_durableSubs` keys.

## Persistence / wire implications
- Durable-sub keying now requires `clientID` to be stable across reconnects — document it clearly.
- In-process and network factories share the same `Connection` interface.

## Dependencies
- Blocks: 02 (JMSContext), 03 (lifecycle), 25 (NoLocal), 31 (request/reply), all of Phase E.

## Test plan
- `ConnectionTest`: start/stop gates listener delivery; `recv` still works during stop (spec-allowed).
- `ConnectionTest`: close cascades to sessions and temp destinations.
- `ConnectionTest`: durable sub keyed by clientID; mismatched clientID cannot reattach.

## Open questions
- Keep a zero-overhead in-process path for current tests? Yes — in-process factory should not introduce threading changes.
