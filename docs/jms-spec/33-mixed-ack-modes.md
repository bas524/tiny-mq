# Mixed ack modes per session

## JMS reference
- Resolves the TODO at `Session.h:86`: "Need to allow creating sessions with different ack modes for the same destination."

## Current state in tiny-mq
- Ack mode is a single `_mode` field on `Session` (`Session.h:91`). All consumers of that session share it.
- JMS sessions are themselves single-ack-mode — but the TODO signals that tiny-mq currently over-constrains the relationship between Session and Destination.

## Proposed API
- Clean resolution: accept the JMS constraint (one ack mode per session) and instead allow **multiple sessions** from the same Connection to attach consumers to the same Destination with different ack modes. This requires spec 01 (Connection) and spec 03 (Connection lifecycle).
- Remove the TODO once Connection layer lands.

## Semantics
- Destinations must key per-consumer state by (sessionId, consumerId), never by destination alone.
- Verify `Destination._consumers` (`Destination.h:33`) already keyed by consumer UUID; confirm no hidden session-wide assumptions in ack/commit paths.

## Persistence / wire implications
- None.

## Dependencies
- 01 (Connection).

## Test plan
- `MixedAckTest`: one connection, two sessions (AUTO + CLIENT), both consume the same queue; independent ack state.

## Open questions
- None.
