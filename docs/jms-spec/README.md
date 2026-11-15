# Jakarta Messaging в tiny-mq

Класс K2. Цель выпуска и границы профилей описаны в [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md).
Нормативная редакция — [Jakarta Messaging 3.1 Final Release](https://jakarta.ee/specifications/messaging/3.1/),
включая [текст](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html)
и [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).
SDD 70–78 описывают предлагаемый объём. Текущее поведение проверяется по исходникам,
[базовому аудиту](../reviews/2026-11-15-jakarta-baseline.md) и [OpenSpec](../../openspec/README.md).

## Карта покрытия

| Jakarta Messaging 3.1 | Обязательство tiny-mq | SDD и состояние начальной реализации |
|---|---|---|
| 1–2 · поддержка Java SE, API, домены, потоки и request/reply | Classic, simplified и legacy API, обе модели доставки, потоковые гарантии | [71](71-jakarta-destinations.md), [72](72-jakarta-delivery.md), [74](74-jakarta-java-se-provider.md). In-process частично, Java отсутствует. |
| 3 · тело, заголовки, свойства и selector | Шесть тел, преобразования, конверт на границах, стандартная грамматика, сторонние сообщения | [70](70-jakarta-message-contract.md), [72](72-jakarta-delivery.md), [74](74-jakarta-java-se-provider.md). C++ частично. |
| 4 · PTP и pub/sub | Конкурирующая доставка, четыре вида подписок, temporary, browse и requestors | [71](71-jakarta-destinations.md). Очередь и топик есть, расширенные сценарии частично или отсутствуют. |
| 5 · administered objects | Factory и Destination через JNDI, сериализация, конфигурация | [74](74-jakarta-java-se-provider.md). Java/JNDI отсутствуют. |
| 6 · connection, session, порядок, ack, tx | Метаданные, auth, threading, close, recover, атомарность, отдельная XA модель | [72](72-jakarta-delivery.md), [73](73-jakarta-broker-transport.md), [74](74-jakarta-java-se-provider.md), [75](75-jakarta-xa-recovery.md). In-process частично. |
| 7 · send | Sync/async, приоритет, TTL, delivery delay, fluent API и заголовки | [70](70-jakarta-message-contract.md), [72](72-jakarta-delivery.md), [74](74-jakarta-java-se-provider.md). Sync C++ есть; async отсутствует; транзакционная задержка расходится с §7.9. |
| 8 · receive | Selector, delivery gate, timeout, nonblocking, receiveBody, listener, close | [71](71-jakarta-destinations.md), [72](72-jakarta-delivery.md), [74](74-jakarta-java-se-provider.md). Базовый C++ receive есть. |
| 9–10 · надёжность, наследование и ошибки | Профили delivery guarantees, запрет чужих domain методов, checked/runtime ошибки | [72](72-jakarta-delivery.md), [74](74-jakarta-java-se-provider.md), [77](77-jakarta-conformance-release.md). Отдельные C++ гарантии, Java отсутствует. |
| 11 · application server, XA | Опциональные для standalone возможности включены в полный профиль tiny-mq | [75](75-jakarta-xa-recovery.md), [76](76-jakarta-ee-integration.md). Отсутствуют. |
| 12 · Jakarta EE | Ограничения web/EJB, JTA и контейнерный JMSContext | [76](76-jakarta-ee-integration.md), [77](77-jakarta-conformance-release.md). Отсутствуют. |
| 13 · Jakarta Connectors | Рекомендованный адаптер и параметры MDB входят в полный профиль tiny-mq | [76](76-jakarta-ee-integration.md), [77](77-jakarta-conformance-release.md). Отсутствуют. |
| 14–15, приложение A | Примеры для smoke/сквозных тестов, история изменений помогает проверке миграции и 3.1 annotations | [74](74-jakarta-java-se-provider.md), [76](76-jakarta-ee-integration.md), [77](77-jakarta-conformance-release.md). |
| §1.1.4 · проектные решения | Wire, TLS/ACL, backpressure, ops, C++ клиент, DLQ и доступность по проектным контрактам | [72](72-jakarta-delivery.md), [73](73-jakarta-broker-transport.md), [78](78-jakarta-high-availability.md). DLQ есть локально. Сетевой стек и HA отсутствуют. |

Java SE и интеграция EE обеспечивают профиль Jakarta Messaging для Java. C++ и клиенты на других
языках используют общий сетевой контракт. [SDD 73](73-jakarta-broker-transport.md) задаёт
требования к транспорту, [SDD 77](77-jakarta-conformance-release.md) — критерии выпуска.
TCK проверяет профиль Jakarta Messaging для Java, а сквозной обмен Java ↔ C++ проверяет
межъязыковое взаимодействие. Матрица выпуска показывает применимость каждого нормативного пункта,
каждого метода Javadoc и доступные возможности по клиентам.

Проектные ограничения проверяются по [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md#ограничения).
SDD 77 проверяет производительность и зависимости поставки, SDD 78 — сохранность данных и
доступность при отказе узла. Абсолютные цели производительности, RTO и SLA задаются профилями испытаний.

В профиль включены XA, серверная доставка, EE-интеграция, адаптер, provider-native byte[]
correlation ID и административное переопределение параметров send. При выпуске матрица
отдельно указывает, какие опциональные JMSX properties поставляет провайдер. Обязательная
поддержка group properties и JMSXDeliveryCount проверяется в SDD 70 и 72.

## План реализации

| SDD | Статус |
|---|---|
| [70](70-jakarta-message-contract.md) | `clean`, [критика, раунд 3](../reviews/70-jakarta-message-contract.critique.md#раунд-3) |
| [71](71-jakarta-destinations.md) | drafted |
| [72](72-jakarta-delivery.md) | drafted |
| [73](73-jakarta-broker-transport.md) | drafted |
| [74](74-jakarta-java-se-provider.md) | drafted |
| [75](75-jakarta-xa-recovery.md) | drafted |
| [76](76-jakarta-ee-integration.md) | drafted |
| [77](77-jakarta-conformance-release.md) | drafted |
| [78](78-jakarta-high-availability.md) | drafted |

## Прежние постановки JMS 2.0

| Предшествующие постановки | Где продолжать |
|---|---|
| [10](10-message-headers.md), [11](11-object-message.md), [14](14-disable-msgid-timestamp.md), [30](30-selector-audit.md) | 70 |
| [25](25-no-local.md), [26](26-shared-consumers.md), [27](27-queue-browser.md), [31](31-request-reply.md) | 71 |
| [12](12-delivery-mode-priority-ttl.md), [13](13-delivery-delay.md), [20](20-message-listener.md), [21](21-completion-listener.md), [22](22-dups-ok-acknowledge.md), [23](23-recover.md), [24](24-redelivery-and-dlq.md), [28](28-receive-no-wait.md), [33](33-mixed-ack-modes.md), [44](44-message-expiration-sweep.md), [45](45-priority-ordering.md), [46](46-persist-redelivery-state.md) | 72 |
| [40](40-wire-protocol.md), [41](41-authn-authz-tls.md), [42](42-flow-control.md), [43](43-admin-introspection.md), [50](50-protocol-choice.md), [60](60-cpp-client.md) | 73 |
| [01](01-connection-layer.md), [02](02-jmscontext.md), [03](03-client-id-and-lifecycle.md), [61](61-java-client.md) | 74 и 76 |
| [32](32-xa-transactions.md) | 75 |
