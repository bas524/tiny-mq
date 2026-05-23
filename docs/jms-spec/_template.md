# <Feature name>

## JMS reference
- JMS 2.0 § `<section>`, interfaces/methods covered.

## Current state in tiny-mq
- What exists today (file:line citations).
- What is missing.

## Proposed API
- Public C++ signatures to add or change (header-level).

## Semantics
- Behaviour, ordering, concurrency, error conditions.
- Interaction with acknowledge modes, transactions, durable subscribers.

## Persistence / wire implications
- New fields serialised into `Message::toBytes()` or storage?
- New protocol frames or control messages?

## Dependencies
- Other specs that must land first.

## Test plan
- Unit tests to add under `tests/` (follow existing `*Test.{h,cpp}` pattern).

## Open questions
