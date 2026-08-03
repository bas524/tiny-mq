# Architecture Decision Records (ADR)

Здесь фиксируются значимые технические и архитектурные решения по проекту tiny-mq.
Каждое решение — отдельный нумерованный файл в формате
[MADR](https://adr.github.io/madr/) (облегчённый вариант).

## Зачем

tiny-mq — переосмысление [ivk-jsc/broker](https://github.com/ivk-jsc/broker) с
целью получить брокер сообщений, поведенчески соответствующий спецификации
Java Message Service (JMS 2.0), плюс нативную C++ клиентскую библиотеку.
Решения, которые формируют облик системы, документируются здесь, чтобы их можно
было пересмотреть осознанно.

## Статусы

- `proposed` — предложено, не принято окончательно
- `accepted` — принято, действует
- `superseded by ADR-XXXX` — заменено другим решением
- `deprecated` — больше не актуально

## Список

| ADR | Заголовок | Статус |
|-----|-----------|--------|
| [0001](0001-wire-protocol-protobuf.md) | Wire-протокол: свой бинарный на Protocol Buffers | accepted |
| [0002](0002-tcp-transport-and-framing.md) | TCP-транспорт и кадрирование сообщений | accepted |
| [0003](0003-connection-object-model.md) | Введение Connection / ConnectionFactory в объектную модель | accepted |
| [0004](0004-build-targets-layout.md) | Разделение на CMake-таргеты: core / protocol / server / client | accepted |
| [0005](0005-session-threading-model.md) | Потоковая модель сессии (thread-affinity по JMS) | accepted |
| [0006](0006-destructors-must-not-throw.md) | Деструкторы не выпускают исключений | accepted |
| [0007](0007-delivery-scheduler-per-destination-lazy-thread.md) | Delivery delay — таймер на Destination, ленивый старт потока | accepted |

## Шаблон нового ADR

```markdown
# ADR-XXXX: <короткий заголовок решения>

- Статус: proposed
- Дата: YYYY-MM-DD
- Контекст: <что заставляет принимать решение>

## Решение
<что именно решено>

## Рассмотренные альтернативы
<варианты и почему отклонены>

## Последствия
<плюсы, минусы, что это за собой влечёт>
```
