# Protocol choice recommendation

## Question
Should tiny-mq adopt gRPC as its wire protocol? What is the best choice for the data plane?

## Short answer
- **Data plane v1: STOMP 1.2.**
- **Data plane v2: custom protobuf binary** (updated 2026-05-23 — was AMQP 1.0;
  changed to meet the project's hard performance requirement, see
  [ADR-0001](../../arch/0001-wire-protocol-protobuf.md)). AMQP 1.0 retained as a
  future option if broad interop is needed.
- **Admin/control plane: gRPC (or HTTP+JSON for v1 to avoid a new dependency).**
- **Do not use gRPC as the JMS data plane.**

## Options evaluated

| Option | Verdict | Reasoning |
|---|---|---|
| **gRPC (as JMS wire)** | ❌ No | HTTP/2 stream semantics fit unary request/response and server-streaming well, but a message broker needs long-lived, bidirectional, credit-based sessions with fine-grained per-frame types (SEND, RECEIVE, ACK, BEGIN/COMMIT/ABORT, FLOW, HEARTBEAT…). Modelling this as a gRPC bidi stream forces every frame into a single `oneof` inside a single RPC, HTTP/2 head-of-line blocking hurts multi-destination traffic on the same connection, back-pressure is coarse (gRPC flow control is per-stream window, not per-destination credit), and you inherit HTTP-centric TLS/auth/proxy assumptions. You also lose cheap binary framing — every frame carries HTTP/2 HEADERS overhead. |
| **STOMP 1.2** | ✅ v1 | Text framing, trivial implementation, clients in every language. Weak on binary efficiency and flow control, but "tiny-mq" implies both are acceptable. Keeps the learning curve for contributors near zero. |
| **AMQP 1.0** | ✅ v2 target | Designed for exactly this domain: sessions, links, credit-based flow control, transactions, standardised message format that maps 1:1 onto the message headers in spec 10. Heavier to implement; consider using `qpid-proton` as a codec. |
| **Custom binary + protobuf frames over TCP** | ⚠️ Possible | Keep protobuf's schema ergonomics for message shape, but handle framing yourself. Works — but you are rebuilding 80% of AMQP with none of the ecosystem. Pick this only if neither STOMP nor AMQP fits. |
| **gRPC (admin/control plane)** | ✅ Recommended | Perfect fit for short-lived admin RPCs (`ListDestinations`, `GetStats`, `PurgeQueue`). Strongly typed, streaming for `Drain`, TLS/auth out of the box. See spec 43. |

## Guidance for the implementation
- Phase E starts with STOMP 1.2 (`40-wire-protocol.md`) because it unblocks real clients fastest and lets us validate the JMS mapping of the new message headers (spec 10) against a spec-conformant client library.
- AMQP 1.0 is the v2 target once the broker has flow control (spec 42), proper headers, transactions, and enough operational maturity to justify the codec work.
- Admin gRPC is additive and can land at any time after spec 43.

## When to revisit
- If a strong user demand for "high-throughput, binary-native, single-protocol" surfaces, skip STOMP v2 and go straight to AMQP 1.0. The STOMP step is a convenience, not a dependency.
