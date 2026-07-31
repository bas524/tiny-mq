# 45 — Priority ordering on dequeue

Spec: [`docs/jms-spec/45-priority-ordering.md`](../jms-spec/45-priority-ordering.md).
Review: [`docs/reviews/45-priority-ordering.review.md`](../reviews/45-priority-ordering.review.md)
(approved, iteration 3). Perf gate:
[`docs/reviews/45-priority-ordering.perf.md`](../reviews/45-priority-ordering.perf.md)
(approved, iteration 3).

This document describes the **accepted implementation**, not the spec's original
proposal. Where the two diverge, the divergence is called out explicitly.

## What it does

Messages with a higher `JMSPriority` are delivered to a consumer ahead of
messages with a lower priority. The feature is purely internal — the public API
(`SendOptions`, `Producer::send`) is unchanged; only the per-destination queue
type changed from `moodycamel::BlockingConcurrentQueue<Message::Ptr>` to a
priority-aware `PriorityQueueT` (`Message.h`).

JMS 2.0 § 3.4.10 says a provider *should* deliver higher-priority messages
ahead of lower-priority ones but does **not** require strict priority ordering.
tiny-mq gives ordering by priority band, not a globally strict priority order
(see *Limitations*).

## Semantics

- **10 priority bands**, `0..9`. A consumer takes the highest-priority
  non-empty band first; within a band, delivery is strict FIFO.
- **Default priority is 4** (`Message::Headers::priority = 4`,
  `SendOptions::priority = 4`). A message sent without an explicit priority
  keeps its create-time value, which defaults to 4.
- **Out-of-range priority is rejected at the public API and clamped only as a
  defensive backstop.** This is a divergence from the spec, which described
  clamping uniformly:
  - `Producer::send(msg, opts)` and `Producer::setDefault(opts)` validate
    `0 <= priority <= 9` and throw `std::invalid_argument` otherwise
    (`Producer.cpp:55`, `Producer.cpp:76`).
  - `PriorityQueueT::enqueue` clamps the message's `jmsHeaders.priority` to
    `[0, 9]` with `std::clamp` before selecting a band (`Message.h:177`), so a
    message that reaches the queue by any other path cannot index out of bounds.
- **Restart / replay restores the band.** Persistent messages carry priority in
  the `0x02` wire payload at offset 51 (`Destination.cpp:94`,
  `priorityFromStorageBytes`). On replay the priority is read back and clamped
  before `enqueue`, so a restarted destination lays messages out into the same
  bands. Truncated records, legacy `0x01` records, or garbage all fall back to
  the default priority 4.
- **Both delivery paths honor bands.** The non-transactional path
  (`Destination::save` → `Consumer::push` → `PriorityQueueT::enqueue`) and the
  transactional path (`Producer::commit` → `Destination::deliverCommitted` →
  `PriorityQueueT::enqueue`, `Destination.cpp:390`) converge on the same
  `enqueue`, which is the single source of truth for band assignment. For the
  Queue family `deliverCommitted` enqueues exactly once; for the Topic family it
  copies per consumer.

## How to use

Set priority per send via `SendOptions` (spec 12), or set a producer default
applied by the no-argument `send()`. Both examples below are real call sites
from `tests/PriorityOrderingTest.cpp`.

```cpp
// Per-send override: priority is the 2nd SendOptions field
// (deliveryMode, priority, timeToLive, deliveryDelay).
TextMessage msg = session.createTextMessage(body, Message::NOT_PERSISTENT);
producer->send(msg, SendOptions{Message::NOT_PERSISTENT, /*priority=*/9, 0, 0});
```

```cpp
// Producer-wide default, applied by every plain send().
producer->setDefault(SendOptions{Message::NOT_PERSISTENT, 7, 0, 0});
producer->send(msg);   // routed to band 7
```

A consumer reads with the usual `recv()`; it observes all `p=9` messages before
any `p=0` message when both are queued, and FIFO order within a band.

## How it is built

The spec proposed "10 bands + poll 9→0" verbatim. The accepted implementation
differs, driven by the perf gate (rounds 1–3 in the perf report):

- **Bitmask of non-empty bands** (`std::atomic<uint16_t> _nonEmpty`) instead of
  a linear 9→0 scan. A bit is set after the band `enqueue` is visible and before
  the semaphore is signaled, so the mask can lag toward "looks non-empty but is
  drained" (benign — cleared on a miss) but never toward "empty while a message
  is present". A full 9→0 scan remains as a safety net.
- **`moodycamel::LightweightSemaphore`** instead of a separate signaling queue.
  Round 1 used a second `BlockingConcurrentQueue<int32_t>` for wakeup; that
  doubled the per-message queue bookkeeping and cost ~13% on the hot path. The
  semaphore (`signal()` = `fetch_add(1, release)`, `wait()` = acquire CAS) is
  the same primitive `BlockingConcurrentQueue` uses internally, so it provides
  the release-acquire pairing for free — no explicit fence between band
  `enqueue` and `signal` is needed.
- **Token return** (`dequeueFromBandsOrReturnToken`): if a semaphore acquire
  succeeds but the band scan finds nothing, the token is handed back via
  `signal()` and the call returns `false`. `Consumer::recv` treats that as "no
  message" and is simply re-invoked.

## Limitations and caveats

Read this section carefully — it is more important than the rest of the doc.

- **Band order, not global strict order.** JMS 2.0 § 3.4.10 does not require
  strict priority ordering, and tiny-mq does not provide it. Two messages in
  different bands are ordered by band; the guarantee is "all of band N before
  any of band N−1", not a heap over individual messages.
- **Cost.** Multi-band polling costs roughly **6 ns per message (~4%)** in the
  default uniform case (`Priority_Uniform` vs `AutoAck` on the same branch, perf
  report round 3). Against `master` the change is performance-neutral —
  **−2.2% (Queue) / +2.5% (Topic)**, within run-to-run noise (paired t-test
  insignificant). Round 1's +13% regression was caused by the second signaling
  queue and is gone.
- **Correctness rests on the invariant "exactly one consumer per
  `PriorityQueueT`."** This is guaranteed today by ADR-0005 plus
  `Destination::createConsumer`: the Queue family holds a single consumer on its
  `_queue` (the default consumer is dropped when a user consumer is created);
  the Topic family gives each consumer its own `QueueT`. The proof that token
  return cannot livelock (and that a woken consumer always finds its message)
  depends on this single-consumer property — with one consumer,
  `dequeueFromBands` always succeeds after a successful `wait()`, so the token
  return path is never reached. **Spec 26 (shared consumers), which places
  several consumers on one subscription, must re-derive this analysis before it
  can reuse `PriorityQueueT`.** This is the single most important line in the
  document.
- **Durable-subscriber replay is not separately tested for priority**
  (review finding F3, accepted as out of scope). The durable topic replay path
  calls the same shared `Destination::replayFromStorage` →
  `priorityFromStorageBytes`, so it is covered transitively by the queue-family
  restart test; there is no dedicated `DurableSubscriberTest` case asserting
  band order after replay.

## Verifiability

Test plan coverage — every item maps to a GTest in
`tests/PriorityOrderingTest.cpp` (5 tests, all passing on clean storage):

| Test plan item | Test |
| --- | --- |
| (a) interleaved `p=0`/`p=9` — receiver sees all `p=9` before `p=0` | `PriorityOrderingTest.testInterleavedPriority` |
| (b) uniform-priority workload remains FIFO | `PriorityOrderingTest.testUniformPriorityIsFIFO` |
| (b′) adjacent-priority interleave preserves per-band FIFO | `PriorityOrderingTest.testAdjacentPriorityInterleavePreservesBandFIFO` |
| (c) restart preserves priority banding | `PriorityOrderingTest.testRestartPreservesBanding` |
| transactional path also respects bands (bonus, covers `deliverCommitted`) | `PriorityOrderingTest.testTransactionalPriorityOrdering` |

Run them with:

```
./cmake-build-debug/tiny_mq --gtest_filter='PriorityOrderingTest.*'
```

The (b′) case is the strongest discriminator: 5×`p=4` + 5×`p=5` interleaved
checks both inter-band ordering (all `p=5` first) **and** intra-band FIFO. A
plain FIFO queue fails the first; a banded queue without per-band FIFO fails the
second; only the full design passes both.
