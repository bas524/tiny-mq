# M1 — Определение protobuf-протокола (P0)

Цель: контракт «клиент ↔ брокер» в виде `.proto` + кодек кадрирования.
ADR: [0001](../arch/0001-wire-protocol-protobuf.md),
[0002](../arch/0002-tcp-transport-and-framing.md),
[0004](../arch/0004-build-targets-layout.md).

Зависит от: M0 (объектная модель, которую отображаем).
Разблокирует: M2, M3.

---

## M1-1 (P0) Подключить protobuf в сборку
- Добавить `protobuf` в vcpkg-манифест.
- Создать таргет `tiny_mq_protocol` (ADR-0004), подключить `protobuf_generate`.
- Acceptance: пустой `.proto` собирается и линкуется в отдельную библиотеку.

## M1-2 (P0) Спроектировать `Envelope` и кадрирование
- Корневое сообщение `Envelope { uint64 request_id; oneof body { ... } }`.
- Length-prefixed кодек: `[uint32 BE length][payload]`, лимит размера кадра,
  конечный автомат для частичных чтений (ADR-0002).
- Acceptance: round-trip кодек-тест (сериализация/десериализация потока кадров,
  включая разрезанные по границам буфера).

## M1-3 (P0) Команды управления соединением и сессией
- `Connect` / `Connected` (clientID, версия протокола, метаданные).
- `CreateSession` / `CloseSession` (ack-mode).
- `CreateProducer` / `CreateConsumer` / `Close*` (destination, selector,
  durable name, noLocal).
- `Disconnect`.
- Acceptance: proto компилируется; покрыто описанием семантики каждого поля.

## M1-4 (P0) Сообщения данных и квитирование
- `Send` (destination, сериализованное тело сообщения + заголовки + properties,
  deliveryMode, priority, ttl).
- `Deliver` (push сообщения потребителю, с messageId/redelivered/deliveryCount).
- `Ack` (modes: AUTO/CLIENT/INDIVIDUAL; диапазон или конкретный messageId).
- Маппинг тел сообщений: переиспользовать существующую сериализацию
  `Message::toBytes()` + поле `Message.Type`, чтобы не дублировать формат.
- Acceptance: все 4 типа сообщений проходят round-trip через proto.

## M1-5 (P0) Транзакции по сети
- `Commit` / `Rollback` / `Recover` для транзакционной сессии.
- Acceptance: семантика согласована с `TransactionBuffer` ядра.

## M1-6 (P1) Системные кадры
- `Heartbeat` (keepalive), `Error` (код + текст), `FlowControl`/credit
  (заготовка под M5).
- Acceptance: типы определены, даже если обработка появится в M5.

## M1-7 (P0) Версионирование протокола
- Согласование версии в `Connect`/`Connected`; политика обратной совместимости
  (reserved-поля, запрет переиспользования номеров).
- Acceptance: документ в `docs/` или комментарии в `.proto` с правилами эволюции.
