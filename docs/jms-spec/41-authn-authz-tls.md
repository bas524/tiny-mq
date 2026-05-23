# Authentication, authorization, TLS

## JMS reference
- JMS 2.0 § 6.1 `createConnection(String userName, String password)`. Authorization is provider-defined.

## Current state in tiny-mq
- None.

## Proposed API
- `ConnectionFactory::createConnection(user, password)` overload.
- `Exchange`-level ACL store: `(principal, destination, op)` → allow/deny. `op` ∈ {`send`, `consume`, `browse`, `manage`}.

## Semantics
- AuthN: SASL-PLAIN over TLS minimum; SCRAM-SHA-256 recommended. Pluggable backend (file, LDAP later).
- AuthZ: checked at `Session::createProducer`/`createConsumer`/`createBrowser`. Deny → throw JMS `JMSSecurityException`.
- TLS: mTLS optional; server certs mandatory for any non-loopback bind.

## Persistence / wire implications
- Credential store separate from message storage.
- Audit log of auth decisions — optional, behind a flag.

## Dependencies
- 01 (Connection), 40 (wire protocol — auth lives inside protocol handshake).

## Test plan
- `AuthTest`: wrong password → connect fails; wrong ACL → send/consume fails.
- `TlsTest`: plaintext client against TLS-only bind is rejected.

## Open questions
- Bundle Poco's NetSSL vs OpenSSL directly? Poco — already a dependency.
