# Selector grammar audit

## JMS reference
- JMS 2.0 § 3.8.1 "Message Selector Syntax" — SQL92-subset grammar: comparison, arithmetic, `BETWEEN`, `LIKE`/`ESCAPE`, `IN`, `IS [NOT] NULL`, `AND`/`OR`/`NOT`, header references (`JMSPriority`, `JMSMessageID`, …), property references.

## Current state in tiny-mq
- `Selector::parse` (`Selector.h:36`) exists and claims "JMS SQL-92"; implementation detail hidden in `Impl`.
- Tests: `tests/SelectorTest.cpp` — coverage unknown until audited.

## Proposed action
1. **Audit** — enumerate every grammar production in the JMS spec and add a failing unit test for each one not yet covered.
2. **Extend** the parser/evaluator to make them pass. Grammar items likely missing:
   - `LIKE 'pat%'` with `ESCAPE '\\'`
   - `IN ('a', 'b', 'c')`
   - `BETWEEN x AND y`
   - `IS NULL` / `IS NOT NULL`
   - Arithmetic on numeric properties (`+ - * /`, unary `-`)
   - Header references: `JMSPriority`, `JMSDeliveryMode`, `JMSType`, `JMSCorrelationID`, `JMSMessageID`, `JMSTimestamp`, `JMSExpiration`.
   - Implicit `null = null → UNKNOWN` three-valued logic.

## Semantics
- Three-valued logic (TRUE/FALSE/UNKNOWN); UNKNOWN treated as non-match.
- Type coercion per JMS table (byte↔short↔int↔long, float↔double, no bool↔numeric).

## Persistence / wire implications
- None; selector is client-side per consumer.

## Dependencies
- 10 (headers) — header references only meaningful once headers exist.

## Test plan
- `SelectorGrammarTest`: one parameterised test per production (LIKE variants, escape, arithmetic, NULL logic, BETWEEN edge cases, IN with mixed types → reject).
- `SelectorHeaderTest`: select on `JMSPriority > 5`, `JMSType = 'foo'`.

## Open questions
- Is the existing `Selector::Impl` a hand-written recursive descent or a library? Choose extension strategy only after reading `Selector.cpp`.
