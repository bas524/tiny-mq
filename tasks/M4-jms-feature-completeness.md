# M4 — Полнота JMS-функционала (P1)

Цель: довести поведение до соответствия JMS 2.0 за пределами базового
send/receive. Идёт после вертикального среза (M0–M3).

Зависит от: M2, M3.
Разблокирует: M6 (конформанс).

---

## M4-1 (P1) QueueBrowser
- Просмотр сообщений очереди без потребления (`createBrowser`, итерация,
  опц. selector).
- Протокол: команды Browse/BrowseNext. Acceptance: браузер видит, но не снимает.

## M4-2 (P1) Request/Reply
- `JMSReplyTo` + `JMSCorrelationID`; helper-обёртки (аналог
  `QueueRequestor`/`TopicRequestor`).
- Acceptance: синхронный запрос-ответ через temporary destination.

## M4-3 (P1) noLocal для топиков
- Подписчик не получает сообщения, отправленные собственным соединением.
- Acceptance: тест с одним соединением, producer+consumer на топике, noLocal=true.

## M4-4 (P1) Истечение и DLQ
- Принуждение expiration на доставке (связано с M0-3).
- Dead Letter Queue для просроченных/много раз переотправленных сообщений
  (порог `deliveryCount`).
- Acceptance: просроченное и «отравленное» сообщение уходят в DLQ.

## M4-5 (P1) Приоритетная доставка
- Очередь учитывает `JMSPriority` при выборе следующего сообщения (связано с
  M0-3).
- Acceptance: при смешанных приоритетах сначала доставляются высокоприоритетные.

## M4-6 (P1) ExceptionListener / асинхронные ошибки
- Доставка асинхронных ошибок соединения клиенту (разрыв, протокольная ошибка).
- Acceptance: при разрыве соединения вызывается ExceptionListener.

## M4-7 (P2) Connection/Session metadata
- `ConnectionMetaData`, `getJMSMajorVersion` и т.п.; поддержка clientID-правил
  (уникальность для durable).
- Acceptance: дубликат clientID для durable отклоняется.

## M4-8 (P2) Полная семантика durable по (clientID, name)
- Привести durable-подписки к ключу (clientID, subscriptionName) per JMS
  (сейчас только subscriptionName) — связано с ADR-0003.
- Acceptance: разные clientID с одинаковым именем подписки изолированы.
