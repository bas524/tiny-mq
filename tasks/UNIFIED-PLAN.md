# tiny-mq — объединённый план

Единый план, сводящий три источника воедино:

- **`docs/jms-spec/`** — детальный каталог фич JMS 2.0 (спеки 01–61), фазы A–F.
  Авторитетный источник по семантике каждой фичи.
- **`tasks/M*.md`** — майлстоны доставки (сборка, сервер, клиент, perf, конформанс).
- **`arch/*`** — архитектурные решения (ADR).

Цель проекта: брокер, поведенчески соответствующий **JMS 2.0**, с C++ и Java
клиентскими библиотеками. Производительность — жёсткое требование
([перф-дисциплина](README.md#сквозное-требование-производительность)).

## Решение по протоколу (зафиксировано)

- **v1 data plane: STOMP 1.2** — быстрый MVP, interop, отладка готовыми клиентами.
- **v2 data plane: свой protobuf** (замена AMQP 1.0 ради производительности).
- **admin plane: gRPC / HTTP+JSON** (спека 43), не как data plane.

Детали: [ADR-0001](../arch/0001-wire-protocol-protobuf.md),
[ADR-0002](../arch/0002-tcp-transport-and-framing.md). Спеки `docs/jms-spec/40,50`
обновлены (v2 = protobuf).

## Снимок состояния

**Готово:**
- Ядро in-process: Exchange/Destination/Session/Consumer/Producer, 4 режима
  квитирования, Selector, durable subscribers, транзакции, персистентность.
- Спека 01 (Connection/ConnectionFactory) — реализовано (M0-1).
- Спека 33 (mixed ack modes) — реализовано (M0-2).
- Спека 10 (JMS-заголовки) — **частично**: модель данных `Message::Headers` +
  сериализация + `MessageHeadersTest` есть; *принуждение* (expiration/priority/
  delivery-time) не сделано.
- try-catch в деструкторах Consumer/Session.

**Не готово:** всё остальное ниже.

## Карта: спека → майлстон → статус

| Спека (docs) | Фаза | Майлстон | Статус |
|---|---|---|---|
| 01 Connection/ConnectionFactory | D | M0 | ✅ done |
| 33 Mixed ack modes | B | M0 | ✅ done |
| 10 Message headers | A | M0 | ✅ done (модель + send-path messageId/timestamp + storage v0x02) |
| 11 ObjectMessage | A | M0 | ✅ done |
| 14 Disable MessageID/Timestamp | A | M0 | ✅ done |
| 22 DUPS_OK_ACKNOWLEDGE (5-й режим) | B | M0 | ✅ done (батч-флаш по порогу/teardown) |
| 03 ClientID/lifecycle/ExceptionListener | D | M0 | 🟡 partial (clientID/start/stop/exc-listener есть; нужен ConnectionMetaData, close-idempotency, durable-ключ (clientID,name)) |
| 12 Per-send DeliveryMode/Priority/TTL | A | M1 | ⬜ planned |
| 44 Expiration sweep | A | M1 | ⬜ planned |
| 45 Priority ordering | A | M1 | ⬜ planned |
| 13 Delivery delay | A | M1 | ⬜ planned |
| 23 Session.recover() | B | M1 | ⬜ planned |
| 24 Redelivery counter + DLQ | B | M1 | ⬜ planned |
| 25 NoLocal | B | M1 | ⬜ planned |
| 28 receiveNoWait | B | M1 | ⬜ planned |
| 30 Selector grammar audit | C | M1 | ⬜ planned |
| 20 MessageListener (async push) | B | M2 | ⬜ planned |
| 21 CompletionListener (async send) | D | M2 | ⬜ planned |
| 02 JMSContext (simplified API) | D | M2 | ⬜ planned |
| 26 Shared consumers | C | M2 | ⬜ planned |
| 27 QueueBrowser | C | M2 | ⬜ planned |
| 31 Request/reply (+Requestor) | C | M2 | ⬜ planned |
| 40 Wire protocol v1 (STOMP) + BrokerServer | E | M3 | ⬜ planned |
| 41 AuthN/AuthZ/TLS | E | M3 | ⬜ planned |
| 42 Flow control | E | M3 | ⬜ planned |
| 43 Admin/introspection | E | M3 | ⬜ planned |
| 60 C++ client (over STOMP) | F | M4 | ⬜ planned |
| 61 Java client (JMS provider, over STOMP) | F | M5 | ⬜ planned |
| 40-v2 Wire protocol v2 (protobuf) | E | M6 | ⬜ planned |
| 32 XA transactions | D | — | ⏸ deferred (рекомендация спеки 32) |

## Майлстоны (последовательность по зависимостям)

Критический путь к «работающему сетевому JMS-брокеру с клиентами» =
**M0 → M1 → M2 → M3 → M4 → M5**. v2-протокол (M6) — оптимизация после.

### M0 — Объектная модель и фундамент сообщений (in-process)
Спеки: 01 ✅, 33 ✅, 10 ✅, 11 ✅, 14 ✅, 22 ✅, 03 🟡.
Закрывает фундамент, на который опираются все остальные фазы.
- ✅ 10: модель `Message::Headers`, заполнение messageId/timestamp на send-пути
  (`Producer::send`), формат хранения v0x02 с версионированием.
- ✅ 11: `ObjectMessage` (broker payload-opaque, `OBJECT_MESSAGE=5`,
  self-contained `[className][body]` в dataAsBytes); `Session::createObjectMessage`.
- ✅ 14: `setDisableMessageID/Timestamp` на Producer.
- ✅ 22: `DUPS_OK_ACKNOWLEDGE` (enum=4) + батч-флаш storage-удалений по порогу
  (100) и на teardown консьюмера.
- 🟡 03 (осталось): `ConnectionMetaData`, идемпотентный `close()` +
  `IllegalStateException` после close, **ключ durable-подписки →
  (clientID, subscriptionName)** (сейчас только name). clientID/start/stop/
  ExceptionListener уже есть из M0-1. Это самый инвазивный пункт (трогает
  `Destination::_durableSubs`) — делать аккуратно с тестами durable.

### M1 — Семантика доставки (in-process)
Спеки: 12, 44, 45, 13, 23, 24, 25, 28, 30.
«Потребители» заголовков из M0.
- 12: per-send DeliveryMode/Priority/TTL (`SendOptions`).
- 44: expiration sweep (на recv-пути + фоновый sweeper в хранилище).
- 45: priority ordering (10 бэндов, polling 9→0; **бенчмаркать uniform-кейс**).
- 13: delivery delay (min-heap по deliveryTime; таймер commit-time для транзакций).
- 23: `Session.recover()` (per-consumer in-flight set).
- 24: redelivery counter + DLQ (RedeliveryPolicy, backoff через 13).
- 25: noLocal (origin-connection id, фильтр в topic fan-out; нужна 01).
- 28: `recvNoWait()`.
- 30: аудит грамматики Selector (LIKE/ESCAPE, IN, BETWEEN, NULL-логика,
  ссылки на заголовки, арифметика) — тест на каждую продукцию.

### M2 — Асинхронность и продвинутые потребители (in-process)
Спеки: 20, 21, 02, 26, 27, 31.
- 20: `MessageListener` (поток доставки **на сессию**, serial-гарантия).
- 21: `CompletionListener` (async send; запрет под TRANSACTED).
- 02: `JMSContext` (фасад над Connection+Session+Producer/Consumer).
- 26: shared (durable) consumers (`DurableSubState` → набор UUID).
- 27: `QueueBrowser` (snapshot-итератор, queue-only).
- 31: request/reply + `Requestor` (temp-destinations, привязанные к Connection).

### M3 — Сетевой протокол v1 (STOMP) + брокер-сервер
Спеки: 40 (STOMP), 41, 42, 43 + серверная инфраструктура.
- `BrokerServer`: реактор, акцептор, per-connection стейт-машина, STOMP-кодек.
- Push-доставка (переход от pull `recv` к push в сокет) — [ADR-0005](../arch/0005-session-threading-model.md).
- Заполнить `main.cpp` (сейчас пустой каркас), graceful shutdown, конфиг.
- 41: SASL-PLAIN/TLS, ACL на destination.
- 42: flow control (in-process watermark'и + TCP back-pressure для STOMP).
- 43: admin (HTTP+JSON для v1).
- См. также детальные шаги в [M2-broker-server.md](M2-broker-server.md).

### M4 — C++ клиентская библиотека (спека 60)
Сетевые `Connection/Session/Producer/Consumer` поверх STOMP; sync recv +
async MessageListener; reconnect; та же публичная сигнатура, что in-process
(`ConnectionFactory::network(uri)`). Детали: [M3-cpp-client-library.md](M3-cpp-client-library.md).

### M5 — Java клиент (спека 61) — настоящий JMS-провайдер
Реализация `jakarta.jms.*` поверх STOMP 1.2: ConnectionFactory/Connection/
Session/JMSContext/Producer/Consumer/QueueBrowser/все типы сообщений.
Цель — пройти применимое подмножество Jakarta Messaging TCK. Interop-тесты
Java↔C++.

### M6 — Протокол v2 (protobuf) + производительность + конформанс
- `.proto` схема `Envelope`, length-prefixed кодек, кодоген (vcpkg protobuf).
- `ProtobufCodec` в общий back-end рядом со `StompCodec`.
- Кредитный flow control (v2).
- Сетевые бенчмарки (расширить `BenchmarkTest` на сетевой путь), baseline.
- JMS conformance matrix (спека → тест → статус); e2e в CI.
- Детали: [M6-conformance-and-docs.md](M6-conformance-and-docs.md).

## Отложено

- **32 XA transactions** — большой и тяжёлый; по рекомендации самой спеки 32
  откладывается за пределы v1. Поверхность расширения задокументирована.

## Сквозные требования (на всех майлстонах)

- **Производительность** — перф-проверка после каждого значимого изменения
  горячего пути; baseline + сравнение; регрессия > ~5% — блокер
  ([детали](README.md#сквозное-требование-производительность)).
- **Сборка по таргетам** — core / protocol / server / client
  ([ADR-0004](../arch/0004-build-targets-layout.md)); кодек v2 — `ProtobufCodec`
  (не `AmqpCodec`).
- **Потоковая модель** — thread-affinity сессии, push-доставка на сервере
  ([ADR-0005](../arch/0005-session-threading-model.md)).
- **Каждая фича** сопровождается тестами по `Test plan` из своей спеки
  `docs/jms-spec/<NN>.md`.

## Замечание о расхождении API M0-1 со спекой 01

Реализация M0-1 немного отличается от предложенного в `docs/jms-spec/01`:
`Connection::createSession` возвращает `Session&` (а не `Session::Ptr`), фабрика —
`Connection(Exchange&)`/`ConnectionFactory(Exchange&)` (а не статические
`inProcess`/`network`). Это сознательный zero-overhead выбор для in-process пути
(спека 01 сама допускает его в Open questions). Сетевые фабрики
(`ConnectionFactory::network(uri)`) добавятся в M4 без изменения семантики;
выравнивание сигнатур — по необходимости тогда же.
