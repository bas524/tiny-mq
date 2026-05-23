# M3 — C++ клиентская библиотека (P0)

Цель: нативная C++ библиотека с JMS-подобным API поверх протокола M1; завершает
вертикальный срез (end-to-end send/receive по сети).
ADR: [0003](../arch/0003-connection-object-model.md),
[0004](../arch/0004-build-targets-layout.md),
[0005](../arch/0005-session-threading-model.md).

Зависит от: M1 (протокол), M2 (сервер для интеграции).
Разблокирует: M4, M6 (конформанс).

---

## M3-1 (P0) Транспорт и таргет клиента
- Таргет `tiny_mq_client` (ADR-0004), зависит от `tiny_mq_protocol`.
- TCP-клиент: подключение, кодек кадров, цикл чтения, корреляция по `request_id`.
- Acceptance: клиент подключается к серверу M2 и проходит handshake.

## M3-2 (P0) `ConnectionFactory` / `Connection`
- `ConnectionFactory(uri)` → `createConnection()`.
- `Connection`: `createSession(mode)`, `start()`, `stop()`, `close()`,
  `setClientID()`, `setExceptionListener()`.
- Acceptance: соединение/сессия создаются и закрываются корректно.

## M3-3 (P0) `Session` / `MessageProducer` / `MessageConsumer`
- Клиентские прокси, отражающие команды на сеть.
- Фабрики сообщений (`createTextMessage` и т.д.) на клиенте.
- Acceptance: producer.send() и consumer.receive() работают end-to-end.

## M3-4 (P0) Синхронный приём `receive()` / `receive(timeout)` / `receiveNoWait`
- Поток доставки на сессию кладёт входящие в очередь; `receive` снимает.
- Acceptance: блокирующий и таймаут-приём проходят.

## M3-5 (P0) Асинхронный `MessageListener` (onMessage)
- Регистрация листенера на consumer; вызовы сериализованы в рамках сессии
  (ADR-0005).
- Acceptance: листенер получает сообщения по порядку; нет гонок в одной сессии.

## M3-6 (P0) Квитирование и транзакции на клиенте
- `message.acknowledge()`, `session.commit()/rollback()/recover()`.
- Acceptance: все 4 режима квитирования работают по сети.

## M3-7 (P1) Selector, durable, temporary destinations на клиенте
- Передача selector-выражения и durable-имени; `unsubscribe`.
- `createTemporaryQueue/Topic` + жизненный цикл, привязанный к соединению.
- Acceptance: фильтрация по selector и offline-replay durable работают по сети.

## M3-8 (P1) CLI-утилита клиента
- Маленький `tiny_mq_cli` для ручной отправки/подписки (компенсирует отсутствие
  готовых сторонних клиентов для бинарного протокола — см. ADR-0001).
- Acceptance: можно из терминала отправить и принять сообщение.
