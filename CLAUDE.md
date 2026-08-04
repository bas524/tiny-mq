# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **Новая сессия — начни с [START-HERE.md](START-HERE.md).** Там: текущее состояние,
> следующий шаг (спека 45), AEF-harness (`.claude/agents`, `.claude/skills`, событийная
> цепочка) и **актуальные команды сборки через CMake-пресеты**. Раздел «Build & test»
> ниже описывает старый `ninja`-путь — verified команды этой сессии в START-HERE.md.

## Project

tiny-mq is a C++20 message broker. This project is an attempt to rethink the project https://github.com/ivk-jsc/broker from the perspective of simplifying message storage and processing. Dependencies are managed through vcpkg (Poco, GTest, benchmark, parallel-hashmap, span).

## Build & test

Build uses CMake + Ninja with the vcpkg toolchain. The build directory `cmake-build-debug` is preconfigured:

```
cd cmake-build-debug && ninja
```

If reconfiguring from scratch, pass:
```
-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -G Ninja
```

Without the vcpkg toolchain file, CMake will not find the dependencies — do not fall back to plain `cmake --build` outside this setup.

Tests are compiled into the single `tiny_mq` executable (GTest is linked directly, gated by `ENABLE_TESTS`, default ON). Run all tests:
```
./cmake-build-debug/tiny_mq
```
Run a single test via GTest filter:
```
./cmake-build-debug/tiny_mq --gtest_filter=DurableSubscriberTest.*
```

Compiler warnings are treated as errors (`-Wall -Werror -Wextra -Wshadow` on Clang/GCC). Fix warnings rather than suppressing them.

## Performance is a hard requirement

High performance is a core project requirement, not an afterthought. **Check perf after every significant change** — any edit to a hot path (routing, delivery, message (de)serialization, storage, the future network codec/reactor, acknowledge/transaction paths).

Run the benchmarks:
```
./cmake-build-debug/tiny_mq --gbench [--benchmark_filter=<regex>]
```
Benchmarks live in `tests/BenchmarkTest.cpp`. Record a baseline and compare against it; treat a significant regression (rule of thumb: > ~5% throughput/latency without justification) as a blocker, not something to defer. When adding functionality on a hot path that existing benchmarks don't cover, add a benchmark for it.

## Architecture

Top-down ownership:

- `Exchange` — broker root; owns `Destination`s keyed by `DestinationHash`.
- `Session` — a client session; owns its `Consumer`s and `Producer`s and drives acknowledge/commit/rollback.
- `Destination` — a Queue or Topic; owns routing and persistence. Destination family dictates delivery semantics:
  - **Queue family** (Queue, TemporaryQueue): deliver to the first available consumer only.
  - **Topic family** (Topic, TemporaryTopic): deliver to all subscribed consumers.
- `Consumer` / `Producer` — messaging endpoints bound to a Session and a Destination.
- `ConcurrentLinearStorage` — persistent UUID-keyed append-log with an async worker thread; used for durable message storage. Ack removes the entry.
- `TransactionBuffer` — per-session write-ahead buffer for `SESSION_TRANSACTED` mode; drained on commit, discarded on rollback.
- `Selector` — JMS-style message selector expressions evaluated against `MessageProperty`.

Acknowledge modes mirror JMS: `AUTO_ACKNOWLEDGE`, `CLIENT_ACKNOWLEDGE`, `INDIVIDUAL_ACKNOWLEDGE`, `SESSION_TRANSACTED`.

Persistence is driven by the `Message::PERSISTENT` flag. Non-persistent messages never touch storage.

**Destructors must not throw ([ADR-0006](arch/0006-destructors-must-not-throw.md)).** Destructors here routinely do real work — stopping the storage worker, rolling back a transaction — and a destructor is implicitly `noexcept`, so an escaping exception is an immediate `std::terminate`, not a catchable error. Wrap anything that can throw in try-catch and log it. This defect has recurred three times; treat it as an invariant, not a style preference.

**Message headers and `_cachedStorageBytes` change together ([ADR-0008](arch/0008-header-and-cached-bytes-invariant.md)).** A persistent message carries its headers twice: as parsed `jmsHeaders` fields and as the serialized `0x02` record cached by `Consumer::preparePush`. On delivery the cache wins — `Consumer::recv`'s fast path calls `fromBytes` on it and overwrites the fields. So editing a header after serialization is silently lost unless you also patch the cache (`Message::patchCachedDeliveryTime` and friends). This bug occurred three times in three different places during spec 13; test the **persistent** case, because a non-persistent message never exposes it.

### Durable subscribers (topics only)

`Session::createDurableConsumer(topic, name)` allocates a dedicated `ConcurrentLinearStorage` at `topic-path/durable-<name>/`. Offline persistent messages accumulate there and replay on reconnect. `Session::unsubscribe(topic, name)` deletes that directory.

Two delivery paths feed durable storage:
- Non-transactional: `Destination::save` writes as messages arrive.
- Transactional: `Destination::deliverCommitted` runs post-commit so uncommitted messages never leak to durable subscribers.

When modifying routing or persistence, verify both paths — a change that only touches `save` will silently break transactional delivery (and vice versa).

## Message types

`Message` is the base; subclasses `TextMessage`, `BytesMessage`, `StreamMessage`, `MapMessage` each define their own JSON (de)serialization. When adding a property type, update `MessageProperty` and the visitor in `PocoAnyVisitor.h` together — they must stay in sync.
