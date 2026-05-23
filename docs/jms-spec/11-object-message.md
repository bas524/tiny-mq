# ObjectMessage

## JMS reference
- JMS 2.0 § 3.11.4 `ObjectMessage`.

## Current state in tiny-mq
- Message hierarchy: `TextMessage`, `BytesMessage`, `StreamMessage`, `MapMessage`. No `ObjectMessage`.

## Proposed API
New class `ObjectMessage : public Message` parallel to `BytesMessage`:
```cpp
class ObjectMessage : public Message {
 public:
  using Ptr = std::shared_ptr<ObjectMessage>;
  Type type() const override { return OBJECT_MESSAGE; }
  void setBody(BytesVector serialized, std::string className);
  const BytesVector& body() const;
  const std::string& className() const;
  // ... toBytes/fromBytes/toJSON/copy/clearData/dataAsBytes overrides
};
```
Add `OBJECT_MESSAGE = 5` to `Message::Type`. Add `Session::createObjectMessage(BytesVector, std::string className)`.

## Semantics
- Broker is **payload-opaque** — no reflection, no class loading. Client-side libraries are responsible for serializing/deserializing the object.
- `className` is a hint stored as header (or dedicated field) for client-side routing/dispatch.

## Persistence / wire implications
- Same storage path as `BytesMessage`; type byte distinguishes them.

## Dependencies
- 10 (headers) — `className` may be stored as a standard header field.

## Test plan
- `ObjectMessageTest`: round-trip body + className; large payload; empty payload.

## Open questions
- Store `className` inside the body envelope or as a reserved header? Header is simpler for selector access.
