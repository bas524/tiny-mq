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

## Следующий шаг — доделать M0: спека 03

Файл спеки: `docs/jms-spec/03-client-id-and-lifecycle.md` (+ `01`).
Уже есть в `Connection`: `clientID`, `start/stop`, `setExceptionListener`, `close`.
**Осталось:**
1. `ConnectionMetaData` — provider name/version, JMS major/minor, supported features;
   метод `Connection::metadata()`. (Лёгкое.)
2. Идемпотентный `close()` + бросать `IllegalStateException` на операции после close.
   (Сейчас `close()` просто чистит сессии; повторные вызовы безопасны, но
   операции после close не запрещены — нужно ввести флаг `_closed` и проверки.)
3. **Главное и инвазивное:** ключ durable-подписки → **(clientID, subscriptionName)**
   вместо только `subscriptionName`. Трогает `Destination` (`_durableSubs`,
   `createDurableConsumer`, `deleteSubscription`/`unsubscribe`) и `Session`
   durable-методы — им нужен доступ к `clientID` через `Session::connection().clientID()`.
   Делать аккуратно: прогнать `DurableSubscriberTest` (7 тестов) — они сейчас не
   задают clientID, поэтому понадобится дефолтный clientID или обновление тестов.
   Спека 26 (shared durable) позже опирается на этот же ключ.

После 03 → M0 закрыт, переходить к M1 (см. UNIFIED-PLAN): 12/44/45/13/23/24/25/28/30.

## Известные проблемы / долги

- `ConnectionFactory` пока без сетевого варианта (`network(uri)`); добавится в M4.
- Расхождение API M0-1 со спекой 01 (`Session&` vs `Session::Ptr`,
  ctor vs `inProcess`/`network`) — осознанное, см. конец UNIFIED-PLAN.

## Рабочие договорённости

- Перф-проверка после каждого значимого изменения горячего пути; регрессия >~5% — блокер.
- Сначала читать `docs/` перед архитектурными решениями (см. auto-memory).
- LSP в этом окружении сыпет ложными ошибками (нет vcpkg include path) — верить
  только сборке `ninja`, а не diagnostics.
