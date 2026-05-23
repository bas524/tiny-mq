# M0 — Доводка ядра и объектной модели JMS (P0)

Цель: привести in-process ядро к состоянию, на которое можно чисто отобразить
сетевой протокол. Это фундамент для M1–M3.

Зависит от: —
Разблокирует: M1, M2, M3.

---

## M0-1 (P0) Ввести `Connection` / `ConnectionFactory` — [x] СДЕЛАНО
**Самая приоритетная задача проекта.**
ADR: [0003](../arch/0003-connection-object-model.md).
Готово: `Connection` (владеет сессиями, clientID, start/stop, ExceptionListener),
`ConnectionFactory`, конструктор `Session` приватный (`friend Connection`),
все тесты переведены на `connection.createSession(...)`. 78/78 тестов зелёные,
перформанс без регрессии.
- Создать класс `Connection`: владеет своими `Session`, хранит `clientID`,
  состояние start/stop, ExceptionListener; метод `createSession(mode)`.
- Создать `ConnectionFactory` (серверная: над `Exchange`).
- Сделать конструктор `Session` приватным, `friend class Connection`
  (закрывает TODO `Session.h:88`).
- Перевести существующий код и тесты на `connection.createSession(...)`.
- Acceptance: все текущие тесты зелёные после рефакторинга; сессии создаются
  только через Connection.

## M0-2 (P0) Завершить начатые правки и TODO ядра — [x] СДЕЛАНО
- [x] try-catch в деструкторах `Consumer.cpp` / `Session.cpp`.
- [x] TODO `Session.h:86` снят (удалён вместе с рефакторингом конструктора).
- [x] Acceptance: `SimpleTest.testTwoSessionsDifferentAckModesSameDestination`
  — две сессии (TRANSACTED + AUTO) на одном destination работают независимо.

## M0-3 (P1) Принуждение JMS-семантики заголовков
Модель `Message::Headers` уже есть; нужно её *исполнять*:
- **Expiration**: не доставлять/удалять просроченные сообщения (`expiration > 0
  && now > expiration`).
- **Priority**: учитывать `priority` (0..9) в порядке доставки очереди.
- **DeliveryTime** (JMS 2.0): не доставлять раньше `deliveryTime`.
- Заполнять `messageId`, `timestamp` на стороне producer-пути, если не заданы.
- Acceptance: тесты на истечение, приоритетную доставку, отложенную доставку.

## M0-4 (P2) Redelivery / deliveryCount
- Инкремент `deliveryCount` и выставление `redelivered=true` при повторной
  доставке после rollback/recover/непод­тверждения.
- Заложить хук для DLQ (сам DLQ — M4).
- Acceptance: после rollback сообщение приходит с `redelivered=true` и
  возросшим `deliveryCount`.

## M0-5 (P2) Решить судьбу ObjectMessage
- JMS определяет 5 типов; есть Text/Bytes/Map/Stream. ObjectMessage завязан на
  Java-сериализацию и в C++ не имеет прямого аналога.
- Зафиксировать в ADR решение: не реализуем / реализуем как
  payload+content-type. (Кандидат на ADR-0006.)
- Acceptance: явное задокументированное решение, не «молчаливый пропуск».

## M0-6 (P1) Починить `tests/LinearStorageTest.cpp:113` — [x] СДЕЛАНО
- Заглушка `SUCCEED()` заменена реальным тестом `testAppendReadRemoveLifecycle`
  (append → read by uuid → remove → scan не содержит запись). TODO снят.
- **Баг исправлен:** `Storage::remove` (LinearStorage.cpp) теперь удаляет запись
  из in-memory индекса `_index` (`_index->erase(uuid)`). uuid берётся из
  переданного `Record.header.uuid`, а если он пуст (быстрый ack-путь) — читается
  с диска по offset. Чтобы не платить лишним чтением на горячем пути, ack-сайты
  в `Consumer::acknowledgeOn`/`commit` теперь заполняют `rec.header.uuid` из
  `message.uuid`. Тест `testAppendReadRemoveLifecycle` проверяет, что после
  remove `record(uuid)` отдаёт not-found и `scan()` пуст. Перф без регрессии.
