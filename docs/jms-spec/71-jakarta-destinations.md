# 71 Очереди, топики, подписки и временные назначения

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.2
Review Date: 2026-11-16
Examples: `DestinationContractTest.SelectiveQueueClaim` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

Destination и Session хранят подписки и фильтры. Текущий протокол PriorityQueueT рассчитан
на одного читателя очереди согласно [ревью SDD 45](../reviews/45-priority-ordering.review.md).
[ADR-0005](../../arch/0005-session-threading-model.md) сохраняет потоковую модель сессии.
Старые SDD 25–28 и 31 описывают незакрытые возможности. Исторический локальный C++ API
обслуживает durable по ключу с назначением и допускает отсутствие client ID.

## Outcome

Клиент может параллельно читать очередь, выбрать вид topic-подписки, просмотреть очередь или
получить ответ через временное назначение. Тесты различают claim, browse и копию подписки.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[70](70-jakarta-message-contract.md)

## Architecture

[ADR-0009](../../arch/0009-destination-claim-and-ownership.md) задаёт атомарную выборку
между независимыми сессиями. Очередь хранит готовую запись до первого подходящего claim.
Освобождение claim повторно открывает запись для подходящих сессий, с тем же
правилом упорядочивания и учётом политики повторной доставки. Запись вне доступного
набора при действующем claim, задержке или истечении срока.

Каждая подходящая подписка в момент публикации получает отдельную копию. Момент публикации
для обычной отправки — принятие send, для транзакционной — публикация при commit
(§6.2.7). Отложенная доступность копии сохраняет состав подписок, выбранный при публикации.
Порядок вычисления и передачи JMSDeliveryTime, полные правила квитирования, сквозное
восстановление и транспорт покрывают [SDD 72](72-jakarta-delivery.md) и [SDD 73](73-jakarta-broker-transport.md).
SDD 71 поставляет локальное C++ ядро. [SDD 74](74-jakarta-java-se-provider.md) связывает его
с Java API и отображает исключения. Browser формирует снимок доступных записей целиком
в одной согласованной точке. Временное назначение связано с неизменной идентичностью
соединения, даже если другое соединение выбирает тот же client ID.

### Persistence / wire implications

Один реестр брокера различает durable по паре (client ID или его отсутствие, имя)
и shared nondurable по той же паре в отдельном пространстве ключей. В durable
записываются ключ, вид, URI топика, нормализованный вход селектора и noLocal.
Нормализация отождествляет null, пустой и состоящий только из пробелов селектор
и удаляет крайние пробелы у остальных. Текст внутри выражения сохраняется.
Реестр и persistent backlog доступны после перезапуска до первого consumer.
Невалидная метаинформация вызывает диагностируемый отказ, оставляя прежние данные
для восстановления. Замена фиксирует новый набор настроек и пустой backlog так,
что после прерванной операции доступна целостная старая либо новая подписка.
Ключи файловых записей однозначно кодируются, а имена клиента не используются
как сегменты пути для новых данных.

Существующие legacy-директории с данными остаются нетронутыми. При первом обращении
старого API на исходном топике прежний адрес (топик, старый client ID либо пусто,
имя) открывается как legacy-подписка с прежним backlog. Старый формат без метаданных
невозможно автоматически восстановить до первого обращения. Legacy-ключ
изолирован от новых ключей, доступен по адресу прежнего API и не переименовывает
старые файлы. Исторические topic-scoped перегрузки C++ продолжают адресовать legacy,
новые перегрузки по правилам 3.1 используют новый реестр. На откате legacy-файлы
читаются старой сборкой, а записи нового реестра сохраняются отдельной копией.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `DestinationIdentity` | SDD 71 |
| E2 | type | `SubscriptionIdentity` | SDD 71 |
| E3 | type | `SubscriptionKind` | SDD 71 |
| E4 | type | `DeliveryClaim` | SDD 71 |
| E5 | type | `QueueBrowser` | SDD 71 |
| E6 | type | `TemporaryDestination` | SDD 71 |
| E7 | method | `Session::unsubscribe` | SDD 71 |
| E8 | test-suite | `DestinationContractTest` | SDD 71 |
| E9 | storage-field | `key` | Client ID и имя в durable-реестре |
| E10 | storage-field | `kind` | Shared или unshared для durable |
| E11 | storage-field | `topicUri` | Идентичность топика |
| E12 | storage-field | `selector` | Нормализованный вход селектора |
| E13 | storage-field | `noLocal` | Фильтр unshared durable |
| E14 | method | `Session::createSharedConsumer` | Shared nondurable |
| E15 | method | `Session::createSharedDurableConsumer` | Shared durable |
| E16 | method | `Session::createQueueBrowser` | Снимок очереди по селектору |
| E17 | method | `Session::createTemporaryQueue` | Временная очередь соединения |
| E18 | method | `Session::createTemporaryTopic` | Временный топик соединения |
| E19 | test-suite | `NfrGateTest` | Измерение горячих путей |

## Acceptance Criteria

### Requirement: Конкурирующие получатели очереди

Очередь SHALL поддержать несколько активных consumer и выдавать сообщение лишь одному подходящему получателю за попытку. Выборка не меняет состояние чужих unmatched-записей. Сессия держит полученную запись до её разрешения по режиму квитирования. Закрытие только consumer прекращает его выборки, сохраняя неподтверждённый claim в живой сессии до разрешения этой сессией. Rollback, recover и закрытие сессии освобождают неподтверждённое по применимому режиму [SDD 72](72-jakarta-delivery.md). После перезапуска брокер восстанавливает неподтверждённую persistent запись, а ранее подтверждённая запись больше не доступна.

ID: `REQ-71-1`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#майлстоны-последовательность-по-зависимостям`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1.2, 6.2.9, 8.2, 8.4.8.

#### Scenario: SelectiveQueueClaim

- **WHEN** два consumer на разных сессиях под конкуренцией читают перекрывающиеся фильтры один откатывает claim, а затем матрица проверяет закрытие consumer, завершение сессии, recover и восстановление persistent записи
- **THEN** ни один claim не принадлежит одновременно двум получателям, unmatched доступно другому rollback и recover делают запись доступной, а закрытый consumer не снимает claim раньше разрешения его сессией; после рестарта доступна неподтверждённая persistent запись
- Test: `DestinationContractTest.SelectiveQueueClaim`

### Requirement: Независимая доставка топика

Топик SHALL направлять публикацию отдельно в каждую подходящую действующую в момент публикации подписку; unshared имеет один consumer, shared отдаёт каждую копию одному из участников; закрытие последнего consumer удаляет nondurable-подписку и её очередь. Durable-подписка держит persistent backlog без активных consumer и после перезапуска. Публикации non-persistent могут доставляться в живом процессе без гарантии сохранения после отказа. Shared имеет одинаковые правила уникального claim при активных участниках.

ID: `REQ-71-2`
Источник: `repo:docs/jms-spec/26-shared-consumers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.2, 8.3.1–8.3.4.

#### Scenario: SubscriptionFanout

- **WHEN** топик имеет unshared, shared и offline durable группы, часть сообщений приходит при commit или с задержкой, а последняя nondurable закрывается и создаётся заново
- **THEN** подтверждение одной копии не влияет на другие группы, shared не дублирует копии, nondurable не наследует старый backlog
- Test: `DestinationContractTest.SubscriptionFanout`

### Requirement: Ключи и замена durable

Провайдер SHALL требовать client ID для unshared durable и считать его необязательным для shared. Активные конфликтующие определения отклоняются; допустимое изменение неактивного определения заменяет подписку и её backlog по правилам спецификации. Shared и unshared durable с одинаковой парой конфликтуют даже в неактивном состоянии. Shared nondurable хранится отдельно, а при открытии с отличающимися топиком или селектором действующая shared nondurable выдаёт ошибку. Для стандартного unsubscribe нужны существующий ключ вызывающего соединения и отсутствие активных consumer и неподтверждённых доставок. Замена определения подчиняется тем же условиям удаления старого. Имя принимает буквы и десятичные цифры Unicode, `_`, `.` и `-` в любой позиции от 1 до 128 кодовых точек UTF-8. Пустое, длинное, невалидное UTF-8 или имя с другими символами отклоняется до обращения к хранилищу.

ID: `REQ-71-3`
Источник: `repo:docs/jms-spec/26-shared-consumers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.4, 6.1.2, 8.3.3–8.3.4; Javadoc Session.unsubscribe.

#### Scenario: DurableIdentityMatrix

- **WHEN** матрица меняет топик, селектор и noLocal где применимо, проверяет shared/unshared, активную или неподтверждённую подписку, отсутствие client ID, пограничные имена, повторное открытие после перезапуска и unsubscribe
- **THEN** ключи и ошибки соответствуют виду подписки, эквивалентное повторное открытие сохраняет backlog, а допустимая замена сбрасывает backlog. Старый backlog открывается через прежний C++ API и остаётся доступен старой сборке на копии данных. Новый ключ с тем же именем не получает legacy-записи
- Test: `DestinationContractTest.DurableIdentityMatrix`

### Requirement: NoLocal по типам подписки

Провайдер SHALL фильтровать публикации того же connection для unshared nondurable, а для unshared durable также публикации с тем же client ID. При запросе noLocal для durable без client ID стандартный API возвращает ошибку до создания подписки. Стандартные shared API не имеют параметра noLocal. Сравнение выполняется с идентичностью отправителя в момент публикации, в том числе у offline durable.

ID: `REQ-71-4`
Источник: `repo:docs/jms-spec/25-no-local.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.2, 8.3.1, 8.3.3–8.3.4, A.3.20.

#### Scenario: NoLocalMatrix

- **WHEN** два подключения публикуют при активной unshared подписке, затем publisher повторно использует client ID ранее закрытого durable consumer; отдельная строка пробует включить durable noLocal без client ID
- **THEN** локальные сообщения отфильтрованы только в соответствующих случаях, чужие сообщения сохраняются, shared подписка принимает свои допустимые публикации, а запрос без client ID отвергается
- Test: `DestinationContractTest.NoLocalMatrix`

### Requirement: Неразрушающий просмотр

QueueBrowser SHALL перечислять разрешённые по selector сообщения без удаления и без изменения признаков доставки, соблюдая delivery time и документированную политику видимости конкурентных изменений. Вызов получения перечисления фиксирует снимок доступных, непросроченных и совпадающих по селектору записей. Снимок исключает записи с действующим claim и будущим временем доставки. Последующие send, claim, ack, rollback и истечение срока не меняют уже полученный снимок. Возвращаются независимые представления в порядке приоритета и стабильном порядке внутри приоритетной полосы. Браузер не регистрирует доставку в сессии.

ID: `REQ-71-5`
Источник: `repo:docs/jms-spec/27-queue-browser.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1.6; Javadoc QueueBrowser.

#### Scenario: BrowseWithoutClaim

- **WHEN** получение снимка с селектором пересекается с send/claim/ack, а очередь содержит claimed, просроченные и задержанные записи
- **THEN** снимок фиксирует только подходящие доступные записи, сохраняется при изменении очереди и не меняет доставку
- Test: `DestinationContractTest.BrowseWithoutClaim`

### Requirement: Временные назначения и reply

TemporaryQueue и TemporaryTopic SHALL разрешать отправку любому открытому соединению того же брокера, которое держит действительную ссылку, с дополнительной сетевой авторизацией по SDD 73. Получение, browse и явное удаление принадлежат соединению-владельцу. Удаление прекращается с ошибкой при активном consumer. Закрытие владельца автоматически удаляет назначения после закрытия его дочерних объектов. Прежняя ссылка не восстанавливается при повторном создании, а отложенные доставки удаляются вместе с назначением. Временный топик обслуживает nondurable-подписки. Локальный тест воспроизводит шаги request/reply через JMSReplyTo и JMSCorrelationID. Классы QueueRequestor и TopicRequestor относятся к Java API SDD 74.

ID: `REQ-71-6`
Источник: `repo:docs/jms-spec/31-request-reply.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 2.16, 4.1.5–4.1.7, 4.2.7, 4.2.10, 6.2.2.

#### Scenario: TemporaryReplyLifecycle

- **WHEN** для обоих типов другое соединение отправляет ответ в JMSReplyTo и пытается потреблять или удалить назначение, владелец проверяет delete с активным consumer, затем закрывает consumer и удаляет назначение либо закрывает соединение
- **THEN** владелец читает ответ с ожидаемой корреляцией, чужое потребление и удаление отклоняются, а после удаления либо закрытия владельца выданные ссылки непригодны
- Test: `DestinationContractTest.TemporaryReplyLifecycle`

### Requirement: Бюджет горячего пути очереди и топика

Расход CPU time/op на существующем пути с одним получателем SHALL вырасти не более
чем на 5% относительно чистого main до SDD 71. Проверяются одинаковые сборки
RelWithDebInfo и интерливированные прогоны на одной машине по [методике проекта](../../benchmarks/baseline.md).
Отдельно измеряется сценарий конкурирующего claim для двух consumer и fan-out на 1, 2 и 4 подписки
с отчётом CPU time на доставку, чтобы явно отслеживать стоимость новой семантики.

ID: `REQ-71-7`
Источник: `repo:benchmarks/baseline.md@0e44570528680e4d4f8cc0d68bbb07841e7fad78#методика`

#### Scenario: DestinationHotPathBudget

- **WHEN** шлюз сравнивает не менее семи повторов C++ бенчей AutoAck для неперсистентной очереди и топика и для persistent очереди с предыдущим main
- **THEN** для каждого пути CV не превышает 5%, рост CPU time/op не превышает 5%, а отчёт содержит команды, исходные измерения и оба новых профиля
- Test: `NfrGateTest.DestinationHotPathBudget`

## Test plan

Набор `DestinationContractTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `DestinationContractTest.SelectiveQueueClaim` — REQ-71-1.
- `DestinationContractTest.SubscriptionFanout` — REQ-71-2.
- `DestinationContractTest.DurableIdentityMatrix` — REQ-71-3.
- `DestinationContractTest.NoLocalMatrix` — REQ-71-4.
- `DestinationContractTest.BrowseWithoutClaim` — REQ-71-5.
- `DestinationContractTest.TemporaryReplyLifecycle` — REQ-71-6.
- `NfrGateTest.DestinationHotPathBudget` — REQ-71-7, отдельный runner шлюза NFR в `tests/`.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Перед откатом остановить запись, снять копию каталога брокера и отдельно сохранить новый
durable-реестр и его backlog. На проверочной копии старая сборка должна читать прежние
legacy-директории. Для новых подписок старая сборка использует состояние до обновления,
их новое состояние сохраняется для повторного запуска новой сборки. Подтверждённые
записи сохраняют исход своего квитирования. Незавершённые записи остаются в резервной копии.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Если атомарный claim, согласованный снимок, совместимость legacy-подписок или бюджет NFR
  требуют отступления от [ADR-0009](../../arch/0009-destination-claim-and-ownership.md),
  вернуться к архитектурному решению и повторить критику изменённой постановки.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/71/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/71/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/71/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
