# Wire protocol

## JMS reference
- Not part of the JMS spec proper — JMS is a client API, not a wire format. This spec fills the gap required for tiny-mq to function as a **remote** broker.

## Current state in tiny-mq
- In-process C++ library only. `main.cpp` boots a `Poco::Util::ServerApplication` shell but there is no listener or frame handling.

## Decision
- **v1 data plane: STOMP 1.2** — text framing, easy to implement, broad client support.
- **v2 data plane: custom protobuf binary** (updated 2026-05-23 — was AMQP 1.0).
  Length-prefixed `Envelope` frames, JMS-1:1 mapping, credit-based flow control.
  Chosen over AMQP to meet the hard performance requirement; AMQP 1.0 kept as a
  future interop option. See [ADR-0001](../../arch/0001-wire-protocol-protobuf.md)
  and [ADR-0002](../../arch/0002-tcp-transport-and-framing.md).
- **Admin plane: gRPC** (see spec 43).

Rationale in [`50-protocol-choice.md`](./50-protocol-choice.md).

## Proposed API
- New `BrokerServer` class that owns a TCP acceptor and per-connection state machines.
- Frame layer abstracted so STOMP today and AMQP later share the same `Connection`/`Session` back-end.

## Semantics (STOMP 1.2 mapping)
- STOMP `CONNECT` → `Connection::start` + clientID.
- `SEND` → `Producer::send` on the named destination.
- `SUBSCRIBE` → `Consumer` creation; `ack` header → ack mode.
- `ACK`/`NACK` → `Consumer::acknowledgeOn` / redelivery path.
- `BEGIN`/`COMMIT`/`ABORT` → `Session::commit`/`rollback`.
- `DISCONNECT` → `Connection::close`.
- Headers map: `priority` → `JMSPriority`, `expires` → `JMSExpiration`, `reply-to` → `JMSReplyTo`, `correlation-id` → `JMSCorrelationID`, `amq-msg-type` → `Message::Type`.

## Persistence / wire implications
- Each frame carries `Message::toBytes()` payload for non-STOMP-native types (BytesMessage, ObjectMessage); content-type header distinguishes.
- Heartbeats per STOMP spec.

## Dependencies
- 10 (headers), 01 (Connection), 41 (TLS/auth), 42 (flow control; mostly post-STOMP for AMQP v2).

## Test plan
- `StompWireTest`: round-trip via a real STOMP client library (e.g. `stompy` Python) in an integration test.
- `StompWireTest`: ack-mode matrix; selectors via `selector` header.

## Open questions
- Ship our own STOMP codec or pull a dependency? Keep it first-party — STOMP framing is trivial and we do not want to grow vcpkg surface.
