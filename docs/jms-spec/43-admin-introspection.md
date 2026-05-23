# Admin / introspection

## JMS reference
- Not covered by JMS; needed for operability.

## Current state in tiny-mq
- None.

## Proposed API
- **gRPC service** (this is where gRPC is the right tool):
```proto
service Admin {
  rpc ListDestinations(Empty) returns (DestinationList);
  rpc CreateDestination(DestinationSpec) returns (Empty);
  rpc DeleteDestination(DestinationName) returns (Empty);
  rpc GetStats(DestinationName) returns (DestinationStats);  // depth, consumer count, bytes
  rpc ListSubscriptions(DestinationName) returns (SubscriptionList);
  rpc DeleteSubscription(SubId) returns (Empty);
  rpc PurgeQueue(DestinationName) returns (PurgeResult);
  rpc Drain(DestinationName) returns (stream DrainProgress);
}
```
- Alternative: HTTP+JSON via Poco's `HTTPServer` — lower dependency cost.

## Semantics
- Read-only calls require `manage:read`; mutating calls require `manage:write` (see spec 41).
- `PurgeQueue` and `DeleteSubscription` are destructive — must be audit-logged.

## Persistence / wire implications
- No new persisted state; reads aggregate existing counters, mutations call existing `Exchange`/`Destination` methods.

## Dependencies
- 41 (auth/authz).

## Test plan
- `AdminTest`: each RPC via a client; authz enforced; stats match actual state.

## Open questions
- gRPC vs HTTP+JSON for v1? Pick **HTTP+JSON** initially (no new dependency), migrate to gRPC once the schema stabilises.
