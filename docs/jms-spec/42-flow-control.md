# Flow control

## JMS reference
- JMS spec itself is silent on wire-level flow control; it is a provider responsibility.

## Current state in tiny-mq
- In-process: `moodycamel::BlockingConcurrentQueue` provides back-pressure only via blocking on full bounds (currently unbounded).
- Remote: n/a (no wire yet).

## Proposed design
- **Producer-side back-pressure (in-process)**: `Destination` publishes a `pendingBytes` / `pendingMessages` counter. Once a soft cap is crossed, `Producer::send` blocks (or, with `CompletionListener`, defers) until drained below a low-watermark.
- **Remote (STOMP)**: leverage TCP back-pressure only in v1; document the limitation.
- **Remote (AMQP 1.0, v2)**: implement link credit properly — consumer grants `N` credits, producer may send at most `N` frames without renewal.

## Persistence / wire implications
- No new persisted state. Counters are in-memory.

## Dependencies
- 40 (wire protocol) for credit semantics.

## Test plan
- `FlowControlTest` (in-process): saturating producer blocks once soft cap reached; resumes after consumer drains.
- `FlowControlTest` (AMQP): credit exhaustion pauses sender; credit grant resumes.

## Open questions
- Per-destination caps vs global? Per-destination with a global fallback.
