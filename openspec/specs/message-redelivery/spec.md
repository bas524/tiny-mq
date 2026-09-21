# Message Redelivery

## Purpose

Что происходит с сообщением, которое консьюмер получил, но не подтвердил: повторная
доставка после `Session::recover()` / `Session::rollback()` / закрытия сессии, счётчик
`JMSXDeliveryCount` и флаг `JMSRedelivered`, лимит повторов, экспоненциальный backoff и
перенос в dead-letter queue по `RedeliveryPolicy` destination'а. Единица применения политики —
очередь консьюмера/подписки (как у ActiveMQ Classic, Artemis, Qpid). Источник: спека 24
(`docs/jms-spec/24-redelivery-and-dlq.md`); поведение `recover()` из спеки 23 будет
дописано бэкфиллом.

## Requirements

### Requirement: Redelivery counter and flag on recover and rollback
The broker SHALL increment `jmsHeaders.deliveryCount` and set `jmsHeaders.redelivered = true`
on every redelivery triggered by `Session::recover()` (non-transacted acknowledge modes) or
`Session::rollback()` (SESSION_TRANSACTED). The first delivery SHALL NOT touch the counter
(inherited from spec 23). A rollback reached from the `~Session()` teardown path
(`sessionClosing == true`) SHALL still increment the counter and check the redelivery limit,
but SHALL NOT apply backoff — the message is requeued onto the destination's queue
immediately. `Consumer::recover()` and `Consumer::rollback()` SHALL share a single
`Consumer::redeliver` path that performs the increment.

#### Scenario: RollbackIncrementsDeliveryCount

- **WHEN** a persistent message is received in a SESSION_TRANSACTED session, rolled back, and received again
- **THEN** the second receipt has `deliveryCount == 1` and `redelivered == true`; a second rollback yields `deliveryCount == 2`
- Test: `DlqTest.RollbackIncrementsDeliveryCount`

#### Scenario: RecoverIncrementsDeliveryCount

- **WHEN** a persistent message is received in a CLIENT_ACKNOWLEDGE session, recovered via `Session::recover()`, and received again
- **THEN** the second receipt has `deliveryCount == 1`; a repeat yields `deliveryCount == 2`
- Test: `DlqTest.RecoverIncrementsDeliveryCount`

#### Scenario: CountMonotonicAcrossRecoverAndRollbackPaths

- **WHEN** two persistent messages share one queue, one cycled through `recover()` in a CLIENT_ACK session and the other through `rollback()` in a transacted session
- **THEN** each message's `deliveryCount` rises by exactly 1 per cycle, independently of the other
- Test: `DlqTest.CountMonotonicAcrossRecoverAndRollbackPaths`

#### Scenario: SessionCloseRequeuesWithoutBackoff

- **WHEN** a SESSION_TRANSACTED session with `backoffMs = 5000`, `maxRedeliveries = 6` receives a message without commit and the session is closed
- **THEN** a new origin consumer receives the message within 100 ms with `redelivered == true`, `deliveryCount == 1`, `deliveryTime == 0` (backoff suppressed on the session-closing path)
- Test: `DlqTest.SessionCloseRequeuesWithoutBackoff`

### Requirement: Redelivery limit routes to dead-letter
When `maxRedeliveries >= 0` and, after incrementing, `deliveryCount > maxRedeliveries`,
the broker SHALL NOT requeue the message; it SHALL call `Destination::deadLetter` with
reason `"maxRedeliveries exceeded"` and remove the origin storage record (as on ack).
When `maxRedeliveries < 0` the broker SHALL impose no limit (unlimited redelivery). The
limit check SHALL also run on the `~Session()` teardown rollback path.

#### Scenario: LandsInDlqAfterMaxRedeliveries

- **WHEN** a persistent TextMessage is sent to a queue with `maxRedeliveries = 2`, `backoffMs = 0` and a DLQ configured, and the consumer rolls back three times
- **THEN** the origin queue is empty (`recv` returns nullptr) and the DLQ holds one copy with the same `messageId`/body, `JMSXDeadLetterReason == "maxRedeliveries exceeded"`, `deliveryCount == 3`, `redelivered == true`, `deliveryTime == 0`
- Test: `DlqTest.LandsInDlqAfterMaxRedeliveries`

#### Scenario: NullDlqDropsMessage

- **WHEN** `deadLetterQueue = nullptr` and `maxRedeliveries = 1`, and the consumer rolls back twice
- **THEN** the origin queue is empty, the message appears nowhere, and (for a persistent origin) after `_exchange.reset()` and recreation the origin is still empty — the storage record was removed
- Test: `DlqTest.NullDlqDropsMessage`

#### Scenario: UnlimitedRedeliveriesWhenNegative

- **WHEN** `maxRedeliveries = -1` and the consumer rolls back ten consecutive times
- **THEN** the message is still receivable with `deliveryCount == 10` and no dead-lettering occurs
- Test: `DlqTest.UnlimitedRedeliveriesWhenNegative`

#### Scenario: DeadLetteringOnSessionCloseRollback

- **WHEN** a SESSION_TRANSACTED session with `maxRedeliveries = 0` receives a message without commit and the session is closed
- **THEN** the message lands in the DLQ (the teardown rollback counts toward the limit)
- Test: `DlqTest.DeadLetteringOnSessionCloseRollback`

### Requirement: Dead-letter copy preserves payload and headers
A dead-letter copy SHALL preserve the message body, `messageId`, `timestamp`, `expiration`,
`priority`, `replyTo`, `correlationId`, `type`, all user properties, and the final
`deliveryCount`/`redelivered` (the value that exceeded the limit). The broker SHALL add the
string property `JMSXDeadLetterReason` and SHALL reset `deliveryTime` to 0 so the copy is
visible immediately. The copy SHALL be persistent if and only if the origin is persistent;
a persistent copy SHALL survive a broker restart and SHALL be deliverable to a DLQ consumer
created after the dead-lettering. The origin storage record SHALL be removed from the
consumer's own storage (the subscription's storage for a durable subscriber).

#### Scenario: DlqPreservesHeadersAndProperties

- **WHEN** a message carrying `correlationId`, `replyTo`, `type`, `priority = 7` and int/string user properties is dead-lettered
- **THEN** all of those are preserved on the DLQ copy
- Test: `DlqTest.DlqPreservesHeadersAndProperties`

#### Scenario: DlqCopySurvivesRestart

- **WHEN** a persistent message is dead-lettered and the exchange is reset and recreated
- **THEN** the DLQ consumer receives the copy and the origin consumer does not
- Test: `DlqTest.DlqCopySurvivesRestart`

#### Scenario: DlqConsumerCreatedAfterDeadLettering

- **WHEN** a DLQ consumer is created after the redelivery limit is exceeded
- **THEN** it receives the dead-lettered copy (replayed from the DLQ storage / `_defaultConsumer`)
- Test: `DlqTest.DlqConsumerCreatedAfterDeadLettering`

#### Scenario: DurableSubscriberDeadLetterRemovesFromDurableStorage

- **WHEN** a durable subscriber `(clientID, name)` with `maxRedeliveries = 0` and a DLQ configured receives a persistent message and recovers
- **THEN** a copy lands in the DLQ; after reset and recreation the reconnected durable subscriber has no message (its subscription storage is cleared) while the DLQ consumer receives the copy
- Test: `DlqTest.DurableSubscriberDeadLetterRemovesFromDurableStorage`

### Requirement: Exponential backoff delays redelivery
When `backoffMs > 0` and the redelivery is from a live session (not the `~Session()` teardown
path), the broker SHALL make the redelivery visible no earlier than
`now + min(backoffMs * 2^(deliveryCount-1), maxBackoffMs)` by setting `jmsHeaders.deliveryTime`
and routing through `Destination::enqueueOrSchedule` (spec 13). When `backoffMs == 0` or on
the session-closing path, the broker SHALL requeue immediately with `deliveryTime` reset to 0.
The serialized byte cache SHALL be refreshed after these header mutations (ADR-0008).

#### Scenario: BackoffDelaysRedelivery

- **WHEN** `backoffMs = 200`, `maxBackoffMs = 60000` and a rollback occurs
- **THEN** `recv(50ms)` returns nullptr, `recv(1s)` returns the message with `redelivered` and `deliveryTime >= t_rollback + 200ms`; a second rollback is visible no earlier than 400 ms
- Test: `DlqTest.BackoffDelaysRedelivery`

#### Scenario: BackoffCappedByMaxBackoff

- **WHEN** `backoffMs = 200`, `maxBackoffMs = 300` and the third redelivery (uncapped 800 ms) occurs
- **THEN** it becomes visible within `maxBackoffMs` plus tolerance (capped at 300 ms)
- Test: `DlqTest.BackoffCappedByMaxBackoff`

#### Scenario: BackoffUpdatesCachedBytesForPersistent

- **WHEN** a persistent message is redelivered with `backoffMs = 100` and received again
- **THEN** the received message carries the updated `deliveryTime` and `deliveryCount` (the fast-path `recv` reads the refreshed cache — ADR-0008)
- Test: `DlqTest.BackoffUpdatesCachedBytesForPersistent`

### Requirement: Redelivery order preserved without backoff
Among messages redelivered by a single `recover()`/`rollback()` call with `backoffMs == 0`,
the broker SHALL preserve the original receive order. With `backoffMs > 0`, order SHALL be
determined by `DeliveryScheduler` on `deliveryTime`, with ties broken by enqueue order.

#### Scenario: RedeliveryOrderPreservedWithoutBackoff

- **WHEN** three messages are received in a CLIENT_ACK session and `recover()` is called with `backoffMs == 0`
- **THEN** the redelivered messages arrive in the original receive order
- Test: `DlqTest.RedeliveryOrderPreservedWithoutBackoff`

### Requirement: Per-subscriber limit and DLQ for topics
For topic-family destinations the unit of the redelivery limit and DLQ SHALL be each
subscriber's own queue. A copy that exceeds the limit at one subscriber SHALL be
dead-lettered independently; other subscribers SHALL be unaffected and SHALL continue to
receive the message with their own `deliveryCount`. For a durable subscriber the origin
record SHALL be removed from that subscriber's own subscription storage, not the
destination's.

#### Scenario: TopicSubscriberDeadLetteredIndependently

- **WHEN** a Topic with `maxRedeliveries = 1` and a DLQ has two CLIENT_ACK subscribers A and B, one persistent message is sent, and A recovers twice
- **THEN** A's origin is empty with exactly one copy in the DLQ; B receives the message normally with `deliveryCount == 0`, and after B recovers once (`deliveryCount == 1`) the DLQ still holds exactly one copy
- Test: `DlqTest.TopicSubscriberDeadLetteredIndependently`

### Requirement: Dead-letter copy keeps expiration
The dead-letter copy SHALL preserve the origin `expiration`. An expired DLQ copy SHALL be
subject to the expiration rule of spec 44 on the DLQ consumer's recv path; dead-lettering
by limit neither substitutes for nor cancels expiration.

#### Scenario: DlqCopyKeepsExpiration

- **WHEN** `maxRedeliveries = 0`, TTL = 2 s, and a rollback occurs immediately after receipt
- **THEN** the DLQ consumer receives a copy with `expiration` equal to the origin's (while TTL is unexpired); a later recv from the DLQ after TTL expires returns nullptr (spec 44 rule)
- Test: `DlqTest.DlqCopyKeepsExpiration`

### Requirement: Default redelivery policy
Any destination (either family) without an explicit `setRedeliveryPolicy` call SHALL behave
as `RedeliveryPolicy{}`: `maxRedeliveries = 6`, `backoffMs = 0`, `maxBackoffMs = 60000`,
`deadLetterQueue = nullptr` — the 7th redelivery drops the message with a warning log. This
is a change from the prior unlimited-redelivery behavior; the prior behavior SHALL remain
available via `setRedeliveryPolicy({.maxRedeliveries = -1})`.

#### Scenario: DefaultPolicyDropsAfterSixRedeliveries

- **WHEN** no `setRedeliveryPolicy` is called and the consumer rolls back seven consecutive times
- **THEN** the origin queue is empty (the default policy drops after six redeliveries)
- Test: `DlqTest.DefaultPolicyDropsAfterSixRedeliveries`

### Requirement: Counter and backoff not persisted across restart
`deliveryCount` and the backoff `deliveryTime` SHALL NOT be written to the origin storage
record on redelivery. After a broker restart, replay SHALL yield `deliveryCount == 0`,
`redelivered == false`, `deliveryTime == 0`, and the redelivery limit SHALL be recounted
from zero.

#### Scenario: RestartResetsCounterAndLimit

- **WHEN** `maxRedeliveries = 2`, `backoffMs = 5000`, a persistent message is rolled back twice (`deliveryCount == 2`, scheduled) and the exchange is reset and recreated
- **THEN** recv from the origin within 100 ms returns the message with `deliveryCount == 0`, `redelivered == false`, `deliveryTime == 0`; three further rollbacks are needed before the DLQ
- Test: `DlqTest.RestartResetsCounterAndLimit`

### Requirement: Redelivery policy assignment validation
`Destination::setRedeliveryPolicy` SHALL throw `Poco::InvalidArgumentException` (leaving the
policy unchanged) when `deadLetterQueue` is non-null and either equals the destination
itself or is not queue-family. The policy SHALL be applicable to both destination families
(a Topic may point at a queue-family DLQ).

#### Scenario: SetPolicyRejectsSelfOrTopicAsDlq

- **WHEN** `setRedeliveryPolicy` is called with the DLQ equal to the queue itself, or with a topic as the DLQ
- **THEN** it throws `Poco::InvalidArgumentException`; calling it on a Topic with a queue-family DLQ is accepted
- Test: `DlqTest.SetPolicyRejectsSelfOrTopicAsDlq`
