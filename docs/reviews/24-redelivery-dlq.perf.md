# Perf gate — Spec 24 (Redelivery counter & DLQ)

- **Stage:** perf (Specialist)
- **Model:** deepseek-v4-pro · **Role:** Specialist · **Autonomy:** R2
- **Spec:** `docs/jms-spec/24-redelivery-and-dlq.md`
- **Verdict:** `approved` — no hot-path regression
- **Date:** 2026-09-21

## Hot-path determination

The spec's new code lives entirely on the **redelivery** side of the
acknowledge/transaction path:

- `Consumer::rollback(bool)` / `Consumer::recover()` now route through the new
  `Consumer::redeliver()` (counter increment, DLQ limit check, optional backoff,
  `refreshCachedStorageBytes()`).
- `Destination::setRedeliveryPolicy` / `redeliveryPolicy` / `deadLetter`
  (new, private-`deadLetter` used only from `redeliver`).
- `Session::rollback(bool)` overload + `~Session()` passing `sessionClosing=true`.

These paths **are** part of ack/transaction, so per the perf-check procedure this
cannot be waved through as N/A — it must be measured. Crucially, however, none of
the *benchmarked* loops exercise them: the `Transacted_*` / `ClientAck_*` /
`AutoAck_*` round-trips measure send → recv → ack/commit only. The diff leaves
`Consumer::push`/`preparePush`/`recv`/`acknowledgeOn`/`commit`, `Destination::save`/
`deliverCommitted`/`enqueueOrSchedule`, `Producer::send`, and all storage code
byte-for-byte untouched (`git diff main -- '*.cpp' '*.h' CMakeLists.txt` =
the 8 Producer files only). The measurement therefore bounds *any* incidental
cost the new members/plumbing add to the shared code path.

## Methodology

- **Builds:** both sides compiled at `RelWithDebInfo` with `-Werror`, own builds
  (Standard 15), from a clean worktree for `main`.
  - branch (working tree, spec-24 edits): `cmake --preset user-release` →
    `cmake-build-releasewithdebuginfo/tiny_mq`.
  - main (`git worktree add ../tiny-mq-main main`, HEAD `662ed97`):
    `cmake -S ../tiny-mq-main -B …/cmake-build-releasewithdebuginfo
    -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=…/vcpkg/…`.
- **Comparison:** `main` vs branch measured in **separate git worktrees** (not two
  benches inside one tree), ABBA interleaving.
- **Harness:** `--gbench --benchmark_filter='Transacted|ClientAck|AutoAck'
  --benchmark_repetitions=7`; metric = **median `cpu_time`**; CV monitored.

## Results (clean cycle)

Cycle 2 (runs `perf-{main,branch}-{3,4}.log`) was run at load average ≈ 2.6 on
8 cores; all CVs ≤ 5.0% except two benchmarks (noted, deltas ~0). Median cpu_time,
main vs branch (negative Δ = branch faster):

| benchmark | main (ns) | branch (ns) | Δ% |
|---|---|---|---|
| AutoAck_NonPersistent_RoundTrip | 153.0 | 153.5 | +0.33 |
| AutoAck_Persistent_RoundTrip | 4154.0 | 4072.5 | −1.96 |
| ClientAck_Persistent_RoundTrip | 4282.0 | 4249.5 | −0.76 |
| ClientAck_Batch_NonPersistent/1 | 190.5 | 194.0 | +1.84 |
| ClientAck_Batch_NonPersistent/100 | 17076.5 | 17016.0 | −0.35 |
| ClientAck_Batch_NonPersistent/1000 | 180815.0 | 179732.0 | −0.60 |
| Transacted_NonPersistent_RoundTrip | 18471.0 | 18544.0 | +0.40 |
| Transacted_Persistent_RoundTrip | 53136.0 | 52924.5 | −0.40 |
| Transacted_Batch_NonPersistent/10 | 21182.5 | 21096.5 | −0.41 |
| Transacted_Batch_NonPersistent/100 | 44312.5 | 44966.0 | +1.47 |
| Transacted_Batch_NonPersistent/1000 | 268403.5 | 268343.0 | −0.02 |
| Transacted_Batch_Persistent/10 | 121784.0 | 121738.5 | −0.04 |
| Transacted_Batch_Persistent/100 | 769274.5 | 779552.5 | +1.34 |
| Transacted_Batch_Persistent/1000 | 7313563.0 | 7488783.5 | +2.40 |
| Topic_AutoAck_NonPersistent_RoundTrip | 156.5 | 158.5 | +1.28 |

Max regression (branch slower) = **+2.40%** (`Transacted_Batch_Persistent/1000`),
within noise and far below the 5% gate. The two CV outliers
(`Transacted_Batch_Persistent/10` main CV 11.0%, `AutoAck_Persistent` br CV 5.0%)
carry near-zero deltas (−0.04% and −1.96%) and do not change the verdict.

## Discarded cycle

The first ABBA cycle (`perf-{main,branch}-{1,2}.log`, runs at 13:13–13:20) was
recorded at load average ≈ 24 with a concurrent WindowServer spike. It produced
main-side CVs of 6–18% and internally inconsistent deltas (branch −8% to +5.6%
depending on benchmark) — a classic confounding signature. It is kept on disk for
transparency but **not** used for the verdict, per the CV > ~5% rule.

## Evidence

Own runs (Standard 15) — bench logs under `handoffs/24/logs/`:
`perf-main-{1,2,3,4}.log`, `perf-branch-{1,2,3,4}.log`; build logs
`perf-build-branch.log`, `perf-build-main.log`.
