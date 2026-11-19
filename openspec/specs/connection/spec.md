# Connection

## Purpose

`Connection` / `ConnectionFactory` / `ConnectionMetaData` (спеки 01, 03): жизненный цикл
соединения, `clientID`, метаданные провайдера и перечень JMSX-свойств, которые брокер
понимает. Здесь пока только вклад спеки 24; остальное — бэкфилл садовника.

## Requirements

### Requirement: JMSX property names advertised
`ConnectionMetaData::jmsxPropertyNames` SHALL contain exactly `JMSXDeliveryCount` and
`JMSXDeadLetterReason` — the JMS-defined delivery count property (JMS 2.0 § 3.5.9) and the
provider-specific dead-letter reason property introduced by spec 24.

#### Scenario: JmsxPropertyNamesListed

- **WHEN** `ConnectionMetaData::jmsxPropertyNames` is read
- **THEN** it contains exactly `JMSXDeliveryCount` and `JMSXDeadLetterReason`
- Test: `DlqTest.JmsxPropertyNamesListed`

### Requirement: Синхронное получение и lifecycle
Локальная C++ Connection SHALL начинаться в stopped. Start разрешает получение, stop блокирует новые
выдачи и ждёт завершения выполняющихся listener. Операции повторяемы. Send разрешён в обоих
состояниях. Ожидание receive с конечным тайм-аутом продолжается и в stopped; бюджеты ожидания
положительны и измеряются steady clock. C++ методы receive() и receive(0) ждут до сообщения
или закрытия, receive(timeout) использует миллисекунды при timeout > 0, отрицательный timeout
отклоняется, receiveNoWait сразу возвращает пустой результат при отсутствии доступной записи.
Существующий recv(usec_timeout) сохраняет микросекунды и прежние значения: 0 — без ожидания,
отрицательное — бессрочно. Он подчиняется тому же барьеру доставки. Существующие клиенты,
использующие получение без start, должны вызвать start. При close незавершённое получение
возвращает пустой результат, если запись ещё не была передана приложению. Consumer close ждёт
выхода блокированного receive и не влияет на уже полученные квитирования или транзакцию.
Закрытые объекты отклоняют новые операции. При вызове receive у consumer в сессии с активной
асинхронной доставкой применяется IllegalStateException.

ID: `REQ-72-2`
Источник: `repo:docs/jms-spec/28-receive-no-wait.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.1.4–6.1.7, 6.2.5, 8.4–8.8.

#### Scenario: ReceiveLifecycle

- **WHEN** пустая, stopped и ready очередь прогоняются с контролируемым тайм-аутом, start/stop и закрытием в другом потоке
- **THEN** нет ранней выдачи при stopped, без ожидания возвращается null, blocking-вызов завершается при close согласно Javadoc
- Test: `DeliveryContractTest.ReceiveLifecycle`
