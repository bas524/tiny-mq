# Redelivery counter & Dead Letter Queue

- **SDD:** `docs/jms-spec/24-redelivery-and-dlq.md` (JMS 2.0 § 3.5.9 `JMSXDeliveryCount`, § 3.4.7 `JMSRedelivered`, § 3.4.10 `JMSDeliveryTime`, § 4.4 transacted rollback, § 8.4.8 `recover()`)
- **Review:** `docs/reviews/24-redelivery-dlq.review.md` — `approved`, iteration 1 (22/22 `DlqTest`, 157/157 cpp-verify, closed-world drift E1–E15 clean)
- **Perf:** `docs/reviews/24-redelivery-dlq.perf.md` — `approved`; ABBA `RelWithDebInfo`, max regression +2.40% (`Transacted_Batch_Persistent/1000`), under the 5% gate
- **Class:** K2 — Engineering
- **Capabilities:** `message-redelivery` (new), `connection`

## Why

Before spec 24, an unacknowledged message redelivered forever: `Consumer::recover()`
(spec 23) incremented `deliveryCount` and set `redelivered`, but `Consumer::rollback()`
(SESSION_TRANSACTED) did neither, there was no redelivery limit, and no dead-letter
queue — a poison message spun indefinitely. Spec 24 closes the `rollback()` gap, adds a
per-destination `RedeliveryPolicy` (limit, exponential backoff, DLQ), and advertises the
JMSX property names the broker now understands.

## What changes

- New `RedeliveryPolicy` struct and `Destination::setRedeliveryPolicy`/`redeliveryPolicy`
  public API (the only public mutators on `Destination`).
- A shared private `Consumer::redeliver(Message::Ptr, bool sessionClosing)` path for
  `recover()` and `rollback()`: increments the counter, sets `redelivered`, checks the
  limit → `Destination::deadLetter`, or requeues (immediate or backoff-scheduled).
- `Destination::deadLetter` (private, `noexcept` per ADR-0006): copies the message to the
  DLQ preserving body/headers/final counter, adds `JMSXDeadLetterReason`, resets
  `deliveryTime`; removes nothing from origin (the caller does).
- `Session::rollback(bool sessionClosing)` private overload: `~Session()` passes `true`
  (counter + limit still apply, backoff suppressed); public `rollback()` passes `false`.
- `ConnectionMetaData::jmsxPropertyNames` now `{"JMSXDeliveryCount", "JMSXDeadLetterReason"}`.

## Scope notes

- Wire format `0x02` is unchanged; `deliveryCount`/backoff `deliveryTime` are NOT persisted
  to the origin record on redelivery (counter resets to 0 after restart — intentional,
  deferred to an M5 spec).
- DLQ is an ordinary queue-family `Destination` supplied explicitly; no auto-creation
  (deferred to spec 43).
- No new `MessageProperty` kind — `JMSXDeadLetterReason` is a plain string property.

## Discrepancies with the SDD

None material. The SDD's `Proposed API` writes the DLQ field as `Destination::Ptr
deadLetterQueue`; the code uses the equivalent `std::shared_ptr<Destination>` (the
`Destination::Ptr` alias). Defaults and semantics match the SDD as confirmed by review
§5.1–§5.10.
