# XA transactions

## JMS reference
- JMS 2.0 § 17 "XA Interfaces": `XAConnection`, `XASession`, mapping to JTA `XAResource`.

## Current state in tiny-mq
- Local transactions only via `TransactionBuffer` (`TransactionBuffer.h`). No two-phase commit, no distributed xid, no `prepare`.

## Proposed API
```cpp
class XaSession : public Session {
 public:
  void start(Xid xid, int flags);
  int  prepare(Xid xid);          // XA_OK or XA_RDONLY
  void commit(Xid xid, bool onePhase);
  void rollback(Xid xid);
  void end(Xid xid, int flags);
  std::vector<Xid> recover();
};
```

## Semantics
- `TransactionBuffer` gains an xid-keyed lookup. Messages staged during `start..end` are committed only when an external transaction manager drives `prepare`+`commit`.
- `prepare` must be durable — on crash, `recover` returns all prepared-but-not-committed xids.
- Heuristic outcomes (`XAER_HEURCOM`/`HEURRB`) intentionally out of scope for v1.

## Persistence / wire implications
- Add a "prepared" marker in `TransactionBuffer` storage; survive restart.
- Wire protocol needs XA frames if used cross-process (but XA over network is usually driven by a TM via JCA, not the raw wire).

## Dependencies
- All of Phase A; ideally Phase D (Connection).

## Decision point
This feature is large and heavy. **Recommendation: defer past v1**; document here so the extension surface is understood, but do not block the rest of the plan on it.

## Test plan
- `XATest`: happy-path two-phase commit across two `XaSession` instances.
- `XATest`: crash between `prepare` and `commit` — `recover` returns xid, resume to commit.

## Open questions
- Adopt an XA library (e.g. Poco's XA plumbing if any) or hand-roll?
