# Client ID, lifecycle, ExceptionListener, ConnectionMetaData

## JMS reference
- JMS 2.0 § 6.1 `ClientID`, § 6.2 lifecycle, § 6.4 `ExceptionListener`, § 6.5 `ConnectionMetaData`.

## Current state in tiny-mq
- No equivalents.

## Proposed API
- On `Connection` (see spec 01): `setClientID`, `start`, `stop`, `close`, `setExceptionListener`, `metadata()`.
- `ConnectionMetaData`: provider name/version, JMS major/minor, supported features.

## Semantics
- `setClientID` must be called before any other `Connection` method that would produce a durable side effect; once set, immutable.
- `ExceptionListener` invoked asynchronously on connection-level errors (network drop, auth failure); never on per-message errors.
- `close()` is idempotent; subsequent operations throw `IllegalStateException`.

## Persistence / wire implications
- `clientID` becomes part of the durable subscription key (see spec 01).

## Dependencies
- 01 (Connection).

## Test plan
- `LifecycleTest`: double-`setClientID` throws; `close` idempotent; operations after `close` throw.
- `ExceptionListenerTest`: simulated underlying failure fires listener.

## Open questions
- Expose `ConnectionMetaData` in-process too? Yes — it documents the provider.
