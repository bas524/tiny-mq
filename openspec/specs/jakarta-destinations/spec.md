# Jakarta Destinations

## Purpose

Локальный C++ контракт очередей, топиков, подписок, просмотра и временных
назначений Jakarta Messaging 3.1. Определяет выборку и владение записями,
восстановление persistent backlog, request/reply и бюджет доставки.

## Requirements

### Requirement: Конкурирующие получатели очереди
Очередь SHALL атомарно выдавать доступную запись одному подходящему получателю за попытку, оставляя несовпавшие с его селектором записи доступными другим сессиям. Сессия держит claim до разрешения по режиму квитирования. Закрытие consumer прекращает его выборки, сохраняя claim в живой сессии. Rollback, recover и закрытие сессии освобождают неподтверждённые записи по применимому режиму. После перезапуска доступны неподтверждённые persistent-записи. Квитирование исключает persistent-запись из последующего восстановления.

ID: `REQ-71-1`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#майлстоны-последовательность-по-зависимостям`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1.2, 6.2.9, 6.2.10, 8.2, 8.8.

#### Scenario: SelectiveQueueClaim

- **WHEN** два consumer разных сессий конкурируют с перекрывающимися селекторами при persistent и non-persistent отправке, затем выполняются закрытие consumer, rollback, recover, закрытие сессии и перезапуск
- **THEN** совпавшая запись выдаётся одному consumer, unmatched остаётся доступной, закрытие consumer сохраняет claim до разрешения сессией, а после перезапуска восстанавливаются только неподтверждённые persistent-записи
- Test: `DestinationContractTest.SelectiveQueueClaim`

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

### Requirement: Ключи и замена durable
Реестр подписок SHALL использовать пару (client ID или его отсутствие, имя) и требовать client ID для unshared durable. Shared и unshared durable с одинаковым ключом конфликтуют даже без активных consumer. Shared nondurable использует отдельное пространство ключей и отклоняет открытие активной подписки с другим топиком или селектором. При сравнении селекторов удаляются крайние пробелы, пустое выражение означает отсутствие фильтра. Эквивалентное открытие сохраняет backlog, а активная unshared durable допускает только одного consumer. Изменение топика, нормализованного селектора или применимого noLocal заменяет durable-определение и сбрасывает backlog, если отсутствуют активные consumer и неподтверждённые доставки. Такие же условия действуют для unsubscribe по ключу текущего соединения. Неизвестный ключ даёт ошибку. Имена принимают от 1 до 128 кодовых точек UTF-8, соответствующих `Character.isJavaIdentifierPart(int)` в Java 21, а также `.` и `-` в любой позиции. Ошибки длины, UTF-8 или состава имени выявляются до обращения к хранилищу. Legacy backlog доступен через прежний C++ API и изолирован от нового ключа с тем же именем.

ID: `REQ-71-3`
Источник: `repo:docs/jms-spec/26-shared-consumers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.4, 6.1.2, 8.3.3–8.3.4; Javadoc Session.unsubscribe.

#### Scenario: DurableIdentityMatrix

- **WHEN** проверяются ключи с client ID и без него, конфликт видов, отдельная замена топика, селектора или noLocal, запреты при активном consumer или pending claim, пограничные имена, рестарт и unsubscribe shared durable
- **THEN** эквивалентное открытие сохраняет backlog, допустимая замена его сбрасывает, недопустимые ключи и операции отклоняются, а прежний API читает legacy-записи отдельно от нового ключа. Исторический бинарник читает копию legacy backlog, подготовленную кандидатом
- Test: `DestinationContractTest.DurableIdentityMatrix`

### Requirement: NoLocal по типам подписки
Локальный C++ API SHALL применять noLocal по идентичности соединения у unshared nondurable топика. У unshared durable фильтр также сравнивает client ID, включая публикации в отсутствие consumer. Сравнение выполняется при send или публикации на commit. Запрос unshared durable без client ID отклоняется при любом значении noLocal. Shared API не принимает параметр noLocal.

ID: `REQ-71-4`
Источник: `repo:docs/jms-spec/25-no-local.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.2, 8.3.1, 8.3.3–8.3.4, A.3.20.

#### Scenario: NoLocalMatrix

- **WHEN** публикуют своё и другое соединения, включая commit, повторное использование client ID offline durable и попытку открыть durable с noLocal без client ID
- **THEN** unshared nondurable фильтрует отправителя по соединению, unshared durable также по client ID, shared принимает подходящую локальную публикацию, а durable без client ID отклоняется
- Test: `DestinationContractTest.NoLocalMatrix`

### Requirement: Неразрушающий просмотр
QueueBrowser SHALL возвращать при каждом вызове `enumerate()` согласованный снимок доступных, непросроченных записей, совпавших с селектором. Записи с действующим claim и будущим delivery time исключаются. Снимок состоит из независимых копий в порядке приоритета и FIFO внутри полосы. Последующие send, claim, ack, rollback или истечение срока не меняют этот снимок. Просмотр не регистрирует доставку в сессии и не меняет флаги доставки.

ID: `REQ-71-5`
Источник: `repo:docs/jms-spec/27-queue-browser.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1.6; Javadoc QueueBrowser.

#### Scenario: BrowseWithoutClaim

- **WHEN** браузер снимает снимок по селектору на фоне send, claim и ack при наличии claimed, задержанных и просроченных записей
- **THEN** снимок содержит допустимый упорядоченный набор с исходными флагами, остаётся прежним после recover, ack и истечения срока и оставляет сообщения доступными обычным consumer
- Test: `DestinationContractTest.BrowseWithoutClaim`

### Requirement: Временные назначения и reply
TemporaryQueue и TemporaryTopic SHALL принимать отправку из открытого соединения того же брокера с действительной ссылкой. Только владелец может потреблять, просматривать временную очередь и явно удалять назначение. Явное удаление при активном consumer завершается ошибкой. Закрытие соединения удаляет его временные назначения после закрытия дочерних объектов. Удаление отменяет отложенные доставки и делает ранее выданные ссылки недействительными. Временный топик поддерживает nondurable-подписки. Локальный request/reply использует JMSReplyTo для выбора назначения и JMSCorrelationID для сопоставления ответа с запросом.

ID: `REQ-71-6`
Источник: `repo:docs/jms-spec/31-request-reply.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 2.16, 4.1.5–4.1.7, 4.2.7, 4.2.10, 6.2.2.

#### Scenario: TemporaryReplyLifecycle

- **WHEN** для очереди и топика другое соединение отвечает через JMSReplyTo, пробует чужое потребление и удаление, а владелец проверяет запрет удаления при активном consumer и обе ветви завершения жизненного цикла
- **THEN** владелец получает ответ с ожидаемым JMSCorrelationID, чужие операции и отправка из другого брокера отклоняются, а явное удаление или закрытие владельца освобождает отложенную копию и делает прежнюю ссылку непригодной
- Test: `DestinationContractTest.TemporaryReplyLifecycle`

### Requirement: Бюджет горячего пути очереди и топика
Рост медианы CPU time/op для AutoAck с одним получателем SHALL составлять не более 5% относительно чистого main до SDD 71 для non-persistent очереди, non-persistent топика и persistent очереди. Сравниваются одинаковые сборки RelWithDebInfo на одной машине, ABBA, не менее семи повторов на сборку и CV CPU не выше 5% для каждой пары путь/сборка. Методика закреплена в [baseline.md](../../../benchmarks/baseline.md). Persistent путь измеряется с `MeasureProcessCPUTime` в обеих сборках, включая worker хранилища. В эталоне меняется только регистрация persistent-бенчмарка, а два однопоточных non-persistent бенчмарка сохраняют прежнюю регистрацию. Receive в эталоне оставляет persistent-запись, а в кандидате синхронно помечает её удалённой до возврата. В отчёте сохраняются команды, исходные CPU и стеновые измерения и эта разница семантики. Дополнительно измеряются конкурирующий claim двух consumer и fan-out на 1, 2 и 4 подписки с CPU time на доставку.

ID: `REQ-71-7`
Источник: `repo:benchmarks/baseline.md@0e44570528680e4d4f8cc0d68bbb07841e7fad78#методика`

#### Scenario: DestinationHotPathBudget

- **WHEN** на одной Release VM шлюз выполняет девять повторов на сборку в порядке ABBA отдельно для каждого из трёх AutoAck путей с минимальным временем 1,2 с и прогревом 2 с, затем измеряет дополнительные профили кандидата
- **THEN** для каждого пути рост CPU time/op и CV укладываются в 5%. Независимый пересчёт итоговой серии дал −12,39%, −9,95% и −75,92% соответственно для non-persistent очереди, non-persistent топика и persistent очереди, при максимальном CV 3,26%. Команды и сырьё перечислены в [отчёте NFR](../../../docs/reviews/71-jakarta-destinations.nfr.md#итоговый-github-вердикт-итерация-3)
- Test: `NfrGateTest.DestinationHotPathBudget`
