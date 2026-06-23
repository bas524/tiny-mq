# CONTINUE HERE — точка возобновления работы

> Снимок состояния для продолжения после перезапуска Claude / новой сессии.
> Обновлено: 2026-05-23.

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
cd cmake-build-debug && ninja                       # сборка (-Werror)
./cmake-build-debug/tiny_mq --gtest                 # все тесты
./cmake-build-debug/tiny_mq --gbench --benchmark_min_time=1.0s   # бенчмарки
```
Примечание: `--gtest_filter=` этим бинарём НЕ обрабатывается корректно — гонять
весь сьют и грепать вывод. Перф мерить с `min_time=1.0s` (короткие прогоны шумят).

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
- ⬜ 44 expiration sweep → 45 priority ordering → 13 delivery delay →
  23 Session.recover → 24 redelivery+DLQ → 25 noLocal → 28 receiveNoWait →
  30 аудит грамматики Selector.

**Следующий шаг — 44 (expiration sweep):** на recv-пути отбрасывать сообщения с
`jmsHeaders.expiration != 0 && now >= expiration` + фоновый sweeper в хранилище.
Метаданные expiration уже проставляются спекой 12 (TTL) — 44 их потребитель.
Тесты по `docs/jms-spec/44-*.md`.

## Известные проблемы / долги

- `ConnectionFactory` пока без сетевого варианта (`network(uri)`); добавится в M4.
- Расхождение API M0-1 со спекой 01 (`Session&` vs `Session::Ptr`,
  ctor vs `inProcess`/`network`) — осознанное, см. конец UNIFIED-PLAN.

## Рабочие договорённости

- Перф-проверка после каждого значимого изменения горячего пути; регрессия >~5% — блокер.
- Сначала читать `docs/` перед архитектурными решениями (см. auto-memory).
- LSP в этом окружении сыпет ложными ошибками (нет vcpkg include path) — верить
  только сборке `ninja`, а не diagnostics.
