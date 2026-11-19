# Jakarta Destinations — delta

## MODIFIED Requirements

### Requirement: Независимая доставка топика
Топик SHALL создавать независимую копию для каждой подходящей подписки, выбранной при обычном send или при подготовке публикации на commit,
а не при staging транзакционной отправки. Общая граница видимости транзакции
определена REQ-72-6; единица retry и DLQ — REQ-72-7. Задержка меняет время доступности копии и сохраняет выбранный состав подписок. У unshared один consumer, а в shared каждая копия доступна для claim одному участнику за попытку. Закрытие последнего consumer удаляет nondurable-подписку с её очередью. Durable сохраняет persistent backlog при отсутствии consumer и после перезапуска. Non-persistent копии стандартной durable-подписки могут накапливаться в памяти при отсутствии consumer до перезапуска. Legacy durable в таком состоянии пропускает non-persistent публикации.

ID: `REQ-71-2`
Источник: `repo:docs/jms-spec/26-shared-consumers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.2, 8.3.1–8.3.4.

#### Scenario: SubscriptionFanout

- **WHEN** send и commit публикуют persistent и non-persistent сообщения для unshared, shared и offline durable, включая задержку доставки, конкурирующих участников обеих shared-групп и повторное создание nondurable
- **THEN** квитирование одной подписки сохраняет копии остальных, каждая shared-группа получает одну копию на публикацию, offline durable восстанавливает persistent backlog, а новая nondurable начинает с пустой очередью
- Test: `DestinationContractTest.SubscriptionFanout`

#### Scenario: DurableSnapshotAtCommit

- **WHEN** durable-подписки с selectors copies = 2 и copies = 3 создаются после staging и до commit, а ещё одна matching-подписка после commit; матрица использует persistent, non-persistent и mixed сообщения
- **THEN** до commit копий нет; существующая к commit matching-подписка получает ровно одну копию, excluded и созданная после commit не получают её
- Test: `DeliveryContractTest.AtomicConsumeAndSend`
