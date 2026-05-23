# ADR-0003: Введение Connection / ConnectionFactory в объектную модель

- Статус: accepted
- Дата: 2026-05-23

## Контекст

JMS определяет иерархию объектов:

```
ConnectionFactory → Connection → Session → MessageProducer / MessageConsumer
```

В tiny-mq сейчас иерархия обрывается: `Session` создаётся напрямую из
`Exchange` (`Session(Exchange&, AcknowledgeMode)`), уровня `Connection` нет.
В коде это уже заложено как намерение — конструктор сессии помечен
`Session(/*connection,*/ Exchange &exchange, ...)` и есть TODO «hide to private
section». Без `Connection` невозможно корректно реализовать:

- client identifier (нужен для durable subscriptions по спецификации);
- start/stop доставки на уровне соединения;
- ExceptionListener;
- единую точку владения сетевым соединением и его сессиями;
- metadata соединения.

## Решение

Ввести два уровня:

- **`Connection`** — владелец одного клиентского сетевого соединения; создаёт и
  владеет своими `Session`; держит `clientID`, состояние start/stop,
  ExceptionListener. `Session` создаётся **только** через `Connection`
  (конструктор `Session` уходит в private, `friend class Connection`).
- **`ConnectionFactory`** — фабрика соединений; на стороне сервера ассоциирована
  с `Exchange`, на стороне клиента инкапсулирует адрес брокера и параметры
  транспорта.

`Exchange` остаётся корнем брокера (реестр `Destination`), но сессии теперь
подвешены под `Connection`, а не напрямую под `Exchange`.

Это решение закрывает существующие TODO в `Session.h:86` (разные режимы
квитирования для одного destination — естественно решается тем, что сессии
независимы и принадлежат разным соединениям) и `Session.h:88` (скрыть
конструктор).

## Рассмотренные альтернативы

- **Оставить Session корнем клиентского API** — отклонено: ломает соответствие
  JMS, негде хранить clientID и connection-level lifecycle.
- **Слить Connection и Session** — отклонено: JMS требует, чтобы одно соединение
  несло несколько сессий с независимыми транзакциями и режимами квитирования.

## Последствия

- Рефакторинг создания сессий по всему коду и тестам (`Session(exchange, mode)`
  → `connection.createSession(mode)`).
- На сервере одно TCP-соединение ↔ один `Connection`-объект; протокол получает
  команды Connect/CreateSession/CreateProducer/CreateConsumer, отражающие эту
  иерархию.
- Durable subscriptions начинают опираться на пару (clientID, subscriptionName),
  как требует JMS, а не только на subscriptionName.
