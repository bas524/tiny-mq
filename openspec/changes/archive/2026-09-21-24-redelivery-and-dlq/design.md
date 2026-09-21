# Design — Redelivery counter & Dead Letter Queue

Accepted implementation (spec 24). Signatures are from the working tree
(`RedeliveryPolicy.h`, `Destination.{h,cpp}`, `Consumer.{h,cpp}`, `Session.{h,cpp}`,
`ConnectionMetaData.h`).

## Public API

```cpp
// RedeliveryPolicy.h
namespace tiny_mq {
struct RedeliveryPolicy {
  int32_t                          maxRedeliveries = 6;      // redeliveries beyond the first delivery; < 0 = unlimited
  int64_t                          backoffMs       = 0;      // delay before the 1st redelivery; 0 = immediate
  int64_t                          maxBackoffMs    = 60000;  // exponential backoff ceiling
  std::shared_ptr<Destination>     deadLetterQueue;         // nullptr = drop the message (with a warning)
};
}
```

```cpp
// Destination.h — the only public mutators on Destination
void            setRedeliveryPolicy(RedeliveryPolicy policy);
RedeliveryPolicy redeliveryPolicy() const;
```

`setRedeliveryPolicy` applies to both destination families. It throws
`Poco::InvalidArgumentException` (leaving the policy unchanged) when
`deadLetterQueue` is non-null and either `== this` or not queue-family. Threading is
as for `Destination` in general (ADR-0005): call before consumers start or from the
destination-affinity thread; concurrent policy change during an in-flight
`recover()`/`rollback()` is unsupported and untested.

## Internal redelivery path

`Consumer::recover()` and `Consumer::rollback(bool)` both route through the new private
`Consumer::redeliver(Message::Ptr message, bool sessionClosing)`:

1. Set `jmsHeaders.redelivered = true`, `++jmsHeaders.deliveryCount`.
2. Read `policy = _destination.get().redeliveryPolicy()`.
3. **Limit:** if `policy.maxRedeliveries >= 0 && deliveryCount > policy.maxRedeliveries`
   → `_destination.get().deadLetter(message, "maxRedeliveries exceeded")`, then (for a
   persistent origin) remove the origin storage record via `_storage->removeAsync(rec)`
   — fast path on `message->_storageTomId`, else `_storage->record(uuid)` lookup. Return.
   Order matters for at-least-once: the DLQ append (synchronous, inside `deadLetter`)
   completes *before* the origin record is removed, so a crash between them leaves a
   duplicate, never a loss.
4. **Backoff:** `applyBackoff = !sessionClosing && policy.backoffMs > 0`. If applied,
   `delayMs = min(backoffMs * 2^(deliveryCount-1), maxBackoffMs)` (computed in `double`
   to avoid overflow on unlimited policies), and `deliveryTime = now + delayMs`; route
   via `_destination.get().enqueueOrSchedule(_queue, ...)` (spec 13 scheduler). If not
   applied, `deliveryTime = 0` (clears any stale backoff `deliveryTime` from a prior
   round) and `_queue->enqueue(...)` directly.
5. `message->refreshCachedStorageBytes()` — keeps the serialized cache in sync with the
   header mutations (ADR-0008); a no-op for non-persistent messages.

`sessionClosing == true` is passed only from the `~Session()` teardown path:
`~Session()` → `Session::rollback(true)` → `Consumer::rollback(true)` →
`redeliver(msg, true)`. The public `Session::rollback()` delegates with `false`. On the
session-closing path the counter still increments and the limit is still checked (so a
close-time rollback can dead-letter), but backoff is suppressed — the message goes
straight back onto the destination's queue (ActiveMQ Classic's redelivery-on-close model).

## Dead-letter copy

`Destination::deadLetter(Message::Ptr message, std::string reason) noexcept`
(private, ADR-0006): wrapped in `try { ... } catch (const std::exception&) {} catch (...) {}`;
any failure is logged as `poco_error` and the copy is lost (the spec permits this on the
`~Consumer()`/`~Session()` path).

- `copy = message->copy()` — preserves body, `messageId`, `timestamp`, `expiration`,
  `priority`, `replyTo`, `correlationId`, `type`, all user properties, and the final
  `deliveryCount`/`redelivered` (the value that exceeded the limit).
- `copy->jmsHeaders.deliveryTime = 0` — visible immediately in the DLQ.
- `copy->setStringProperty("JMSXDeadLetterReason", reason)`.
- Persistent iff origin persistent: `copy->toBytes()` prefixed with the type byte is
  appended synchronously to `dlq->_storage` (`ConcurrentLinearStorage::append` returns
  after the worker executes it), so a persistent DLQ copy survives restart.
- If a DLQ consumer is already attached (`dlq->_queue`), the copy is also enqueued for
  immediate delivery; a consumer created later replays the persistent copy from storage.
- `deadLetterQueue == nullptr` → `poco_warning` with `uuid`, origin URI, `deliveryCount`;
  message dropped. Origin record removal is still the caller's job (it runs unconditionally
  in `redeliver`'s limit branch).

`deadLetter` does NOT touch the origin record — `Consumer::redeliver` removes it from the
consumer's own storage (`Consumer::_storage`: the destination's storage for a queue, the
subscription's storage for a durable subscriber), as on ack.

## Per-subscriber unit (topics)

For topic-family destinations the limit/DLQ unit is each subscriber's own `Consumer::_queue`
(allocated per consumer in `createConsumer`/`createDurableConsumer`, not shared). A copy
exceeding the limit at one subscriber is dead-lettered independently; other subscribers are
unaffected. For a durable subscriber the origin record is removed from that subscription's
storage, and the durable key `(clientID, name)` / directory layout is untouched.

## Connection metadata

```cpp
// ConnectionMetaData.h
std::vector<std::string> jmsxPropertyNames{"JMSXDeliveryCount", "JMSXDeadLetterReason"};
```

## Defaults / behavior change

`RedeliveryPolicy{}` (no `setRedeliveryPolicy` call) → `maxRedeliveries = 6`, `backoffMs = 0`,
`maxBackoffMs = 60000`, `deadLetterQueue = nullptr`. The 7th redelivery drops the message
with a warning. This is a deliberate change from the prior unlimited-redelivery behavior;
the prior behavior is still available via `setRedeliveryPolicy({.maxRedeliveries = -1})`.

## What this does NOT do (per SDD Open questions)

- **`deliveryCount` / backoff `deliveryTime` are not persisted** to the origin record on
  redelivery. After a broker restart, replay yields `deliveryCount == 0`,
  `redelivered == false`, `deliveryTime == 0`, and the limit is recounted from zero.
  Persisting the counter is deferred to an M5 spec (design ready: positional `PATCH_AT`
  on the existing `0x02` record, no format change).
- **No DLQ auto-creation** (`DLQ.<origin>`); the DLQ is supplied explicitly. Deferred to
  spec 43 (admin/introspection), which would give `Destination` access to `Exchange`.
- **No `JMSXOriginalDestination`** on the DLQ copy (not required by JMS).
- Wire format `0x02`, `Message::toBytes`/`fromBytes`/`patchCachedDeliveryTime`, and the
  durable-branch routing (`Destination::save`, `Destination::deliverCommitted`) are
  unchanged — verified green by `PersistentTransactionTest`, `DurableSubscriberTest`,
  `RecoverTest`.
