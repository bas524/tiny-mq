# Jakarta Message Contract — delta

## ADDED Requirements

### Requirement: Send clock, expiration и приоритет

Локальный C++ провайдер SHALL один раз принять время send и вычислить timestamp, JMSDeliveryTime = send + delay
и JMSExpiration = send + TTL, включая transacted send. При delay=0 delivery time равен времени send,
при TTL=0 expiration=0. Для активных сроков используются неотрицательные миллисекунды,
переполнение абсолютного срока обрабатывается насыщением на максимуме представления.
Подсказка отключения timestamp сохраняется по REQ-70-1; при её принятии timestamp=0,
но send clock для delay/TTL остаётся прежним. Приоритет 0–9 действует в пределах §6.2.9. Запись может быть получена после публикации
(send для обычной отправки, commit для транзакции), когда now >= deliveryTime и
(expiration == 0 или now < expiration). Для повтора дополнительно действует
сохранённый nextRedeliveryAt по REQ-72-7. Просроченные записи пропускаются при replay и выдаче.
Заголовки принятых отправок не пересчитываются на commit, replay или повторном получении.
Закреплённый источник 13 описывает исторический отсчёт delay от commit. Для новых отправок
оно заменяется отсчётом от send по §3.4.13 и §7.9. Исторические записи сохраняют ранее записанные заголовки.

ID: `REQ-72-1`
Источник: `repo:docs/jms-spec/13-delivery-delay.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.4.9–3.4.13, 7.6–7.9.

#### Scenario: SendClockMatrix

- **WHEN** тест разделяет send, точные границы TTL и delay, commit раньше и позже delay, задержку и TTL 0, а также replay старых и новых persistent записей; отдельно выполняет тот же живой путь для non-persistent, обычной отправки и commit
- **THEN** проверяются все три заголовка на стороне отправки и получения, отсутствие видимости до границ, исключение истёкшей записи и немедленная готовность после позднего commit
- Test: `DeliveryContractTest.SendClockMatrix`
