# Priority ordering on dequeue

## JMS reference
- JMS 2.0 § 3.4.10 `JMSPriority` — providers should deliver higher-priority messages ahead of lower-priority ones, but strict priority order is not required.

## Current state in tiny-mq
- `QueueT = moodycamel::BlockingConcurrentQueue<Message::Ptr>` — strict FIFO, no priority awareness.

## Proposed API
- No public surface change; internal queue structure changes.

## Semantics
- Partition the queue into **10 priority bands** (0..9). A consumer polls bands 9→0, taking the first non-empty band. Within a band, FIFO.
- Alternative: single priority-queue with heap ordering. Discarded because moodycamel's MPMC lock-freedom is hard to keep under a heap.
- Must preserve ordering guarantees for the common case of uniform priority (= FIFO).

## Persistence / wire implications
- Storage already persists the message; priority is just a header. Replay path must restore messages into the correct band on restart.

## Dependencies
- 10 (headers).

## Test plan
- `PriorityOrderingTest`: interleaved send of p=0 and p=9; receiver sees all p=9 before p=0.
- `PriorityOrderingTest`: uniform-priority workload remains FIFO.
- `PriorityOrderingTest`: restart preserves priority banding.

## Open questions
- Does multi-band polling add unacceptable latency to the default (uniform) case? Benchmark before merging.
