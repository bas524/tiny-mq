# QueueBrowser

## JMS reference
- JMS 2.0 § 8.13 `QueueBrowser`, `Session.createBrowser(Queue [, selector])`.

## Current state in tiny-mq
- No browser. `Destination::replayFromStorage` (`Destination.h:86`) walks storage records — exactly the primitive a browser needs, but it's private.

## Proposed API
```cpp
class QueueBrowser {
 public:
  using Ptr = std::shared_ptr<QueueBrowser>;
  class Iterator { ...snapshot cursor... };
  Iterator begin();
  Iterator end();
};
QueueBrowser::Ptr Session::createBrowser(const Destination::Ptr& queue,
                                         const std::string& selector = "");
```

## Semantics
- Queue-family only (spec forbids browsing topics).
- **Snapshot read** — enumerates messages without consuming them. Does not mutate queue state, never triggers redelivery counters.
- Iteration may race with in-flight sends/acks; the iterator must be tolerant of records disappearing mid-iteration (return only what is still present).

## Persistence / wire implications
- Reuse existing `ConcurrentLinearStorage` read iterator; add a public read-only iterator type if not already exposed.

## Dependencies
- 30 (selector audit — browser with selector must use the full grammar).

## Test plan
- `QueueBrowserTest`: browse does not consume; subsequent `recv` returns all browsed messages.
- `QueueBrowserTest`: selector filters browsed set.
- `QueueBrowserTest`: concurrent ack during browse does not crash/skip.

## Open questions
- None.
