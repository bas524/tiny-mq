# Connection — delta

## ADDED Requirements

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
