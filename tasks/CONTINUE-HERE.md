# CONTINUE HERE — точка возобновления работы

> Снимок состояния для продолжения после перезапуска Claude / новой сессии.
> Обновлено: 2026-07-28.

## Как читать проект (порядок)

1. `CLAUDE.md` — сборка/тесты/перф-требование.
2. `tasks/UNIFIED-PLAN.md` — главный план: карта спека↔майлстон↔статус, протокол.
3. `docs/jms-spec/` — детальные спеки фич JMS 2.0 (авторитетный каталог).
4. `arch/` — ADR (решения: протокол, транспорт, объектная модель, таргеты, потоки).

## Решение по протоколу (зафиксировано)

STOMP 1.2 (v1) → свой protobuf (v2, вместо AMQP); admin — gRPC/HTTP.
См. [ADR-0001](../arch/0001-wire-protocol-protobuf.md).

## Команды

```
cmake --preset user-debug                                   # конфигурирование
cmake --build --preset debug --parallel                     # сборка (-Werror)
./cmake-build-debug/tiny_mq --gtest_filter='PriorityOrderingTest.*'   # набор тестов
./cmake-build-debug/tiny_mq --gtest_filter='-*Bench*'                 # весь сьют

cmake --preset user-release && cmake --build --preset release --parallel
./cmake-build-releasewithdebuginfo/tiny_mq --gbench --benchmark_min_time=1.2s
```
Примечания:
- `--gtest_filter=` **поддерживается** (прежнее утверждение в этом файле было неверным).
- ⚠ Бинарь **без аргументов падает в SIGSEGV** (`main.cpp:207` разыменовывает `argv[1]`
  при `argc==1`) — всегда передавай `--gtest_filter=` или `--gbench`.
- Перф мерить только на release, интерливированными прогонами main-vs-ветка (main —
  в отдельном `git worktree`), `--benchmark_repetitions=7..9`, сравнение по `cpu_mean_ns`.
  Сравнивать два бенча внутри одной ветки бессмысленно — так стоимость фичи не измеряется.
  Эталон — `benchmarks/baseline.md`.

## Состояние (что уже сделано)

**Тесты: 87/87 зелёные. Перф без регрессии относительно baseline.**

Baseline перфа (items/sec, для сравнения после изменений горячего пути):
- AutoAck_NonPersistent ≈ 760k–860k (шумит)
- AutoAck_Persistent ≈ 61k
- ClientAck_Persistent ≈ 59–60k
- Transacted_NonPersistent ≈ 40k
- Transacted_Persistent ≈ 14.3k
- Topic_AutoAck_NonPersistent ≈ 670k

Сделано по майлстону **M0** (см. UNIFIED-PLAN):
- ✅ 01 Connection/ConnectionFactory — `Connection.{h,cpp}`, `ConnectionFactory.{h,cpp}`;
  конструктор `Session` приватный, `friend Connection`; `createSession` → `Session&`.
- ✅ 33 Mixed ack modes — тест `SimpleTest.testTwoSessionsDifferentAckModesSameDestination`.
- ✅ 10 JMS-заголовки — модель `Message::Headers`, заполнение messageId/timestamp
  в `Producer::send`, формат хранения v0x02.
- ✅ 11 ObjectMessage — `ObjectMessage.{h,cpp}`, `OBJECT_MESSAGE=5`,
  `Session::createObjectMessage`, replay в `Destination::makeMessageShell`.
- ✅ 14 Disable MessageID/Timestamp — флаги на `Producer`.
- ✅ 22 DUPS_OK_ACKNOWLEDGE — enum=4, батч-флаш storage-удалений в `Consumer`.
- ✅ Заглушка `LinearStorageTest.testProducerConsumer` заменена реальным
  `testAppendReadRemoveLifecycle`.
- ✅ **Баг хранилища исправлен:** `Storage::remove` теперь делает `_index->erase(uuid)`
  (раньше оставлял устаревшую запись в индексе до перезапуска). uuid берётся из
  `Record.header.uuid`; ack-сайты в `Consumer` заполняют его из `message.uuid`,
  fallback — чтение заголовка с диска по offset.

Деструкторы `Consumer`/`Session`: `rollback()` обёрнут в try-catch (сделано ранее).

## M0 закрыт ✅ — спека 03 сделана

Спека 03 (`docs/jms-spec/03-client-id-and-lifecycle.md`) завершена:
1. ✅ `ConnectionMetaData` (`ConnectionMetaData.h`) + `Connection::metadata()`
   (доступен даже после close).
2. ✅ Идемпотентный `close()` (флаг `_closed`) + `tiny_mq::IllegalStateException`
   (`Exceptions.h`) на мутирующих операциях после close; `setClientID` теперь тоже
   бросает `IllegalStateException`. Тесты `LifecycleTest`/`ExceptionListenerTest`.
3. ✅ Ключ durable-подписки → **(clientID, subscriptionName)** через
   `Destination::durableKey()` (пустой clientID = legacy name-only ключ/layout,
   поэтому 7 старых `DurableSubscriberTest` прошли без изменений). Директория
   `durable-<clientID>-<name>` + clientID-scoped storage namespace. `Session`
   пробрасывает `connection().clientID()`. Новый тест
   `DurableSubscriberTest.testClientIdScopesDurableSubscription`.
   Спека 26 (shared durable) позже опирается на этот же ключ.

Тесты: **95/95 зелёные.** Перф без регрессии (AutoAck_Persistent ≈60.7k,
Topic_AutoAck_NonPersistent ≈669.6k).

## M1 в работе — семантика доставки

Прогресс M1 (см. UNIFIED-PLAN, по зависимостям):
- ✅ 12 per-send DeliveryMode/Priority/TTL — `SendOptions`, `Producer::send(msg,opts)`
  + `setDefault`. Ingress готов: заполняет `priority`/`expiration`/`deliveryTime`.
  `SendOptionsTest`. (ветка `spec-12-send-options`, коммит 9f85b47).
- ✅ 44 expiration sweep — drop протухших на recv-пути + фоновый sweeper по дедлайну
  через `Storage::scanPrefix` (43-байт префикс, чанк 4096/тик, курсор). `ExpirationTest`
  (4 теста). Кросс-модельное ревью approved (`docs/reviews/44-message-expiration-sweep.review.md`,
  раунд 2). Follow-up: m5/n1–n5.
- ✅ 45 priority ordering — `PriorityQueueT`: 10 бэндов, маска непустых бэндов,
  `LightweightSemaphore` вместо отдельной сигнальной очереди. `PriorityOrderingTest` ×5.
  Ревью (MiniMax-M3) + перф-гейт (deepseek-reasoner) approved, коммит `a26b5c5`.
  Перф к master: −2.2% / +2.5%. Отчёты — `docs/reviews/45-priority-ordering.{review,perf}.md`,
  документация — `docs/features/45-priority-ordering.md`.
- ⬜ 13 delivery delay → 23 Session.recover → 24 redelivery+DLQ → 25 noLocal
  → 28 receiveNoWait → 30 аудит грамматики Selector.

**Следующий шаг — 13 (delivery delay):** min-heap по `deliveryTime`, таймер commit-time
для транзакций. `deliveryTime` уже заполняется ingress-путём спеки 12 и round-trip'ится
через формат хранения `0x02`. Тесты по `docs/jms-spec/13-delivery-delay.md`.

**Урок спеки 45, применимый к 13:** это снова горячий путь delivery. Мерить перф только
на release и только main-vs-ветка (см. раздел «Команды»); проверять обе ветки доставки
(`save` и `deliverCommitted`). Дизайн из спеки может не проходить перф-требование —
у 45 прямолинейная реализация стоила −13%.

## Известные проблемы / долги

- `ConnectionFactory` пока без сетевого варианта (`network(uri)`); добавится в M4.
- Расхождение API M0-1 со спекой 01 (`Session&` vs `Session::Ptr`,
  ctor vs `inProcess`/`network`) — осознанное, см. конец UNIFIED-PLAN.

## Рабочие договорённости

- Перф-проверка после каждого значимого изменения горячего пути; регрессия >~5% — блокер.
- Сначала читать `docs/` перед архитектурными решениями (см. auto-memory).
- LSP в этом окружении сыпет ложными ошибками (нет vcpkg include path) — верить
  только сборке `ninja`, а не diagnostics.
