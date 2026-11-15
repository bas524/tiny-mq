# 71 Очереди, топики, подписки и временные назначения

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.1
Review Date: 2026-11-15
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

Destination и Session хранят подписки и фильтры. ADR-0005 допускает один активный consumer на очередь; старые SDD 25–28 и 31 описывают незакрытые возможности. Имя durable без client ID обслуживается историческим C++ API.

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

Назначение имеет тип и устойчивую идентичность. Очередь атомарно закрепляет подходящее сообщение за одним потребителем, не снимая чужие unmatched-сообщения. Для каждой topic-подписки сохраняется независимая единица доставки с зафиксированной ролью и фильтром. Browser читает без claim, temporary назначение связано с connection ID.

### Persistence / wire implications

Зафиксировать ключ подписки, topic, canonical selector и noLocal. Старые идентификаторы durable мигрируются явным отображением; несовпадение конфигураций и удаление активной подписки имеют отдельные исходы.

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

## Acceptance Criteria

### Requirement: Конкурирующие получатели очереди

Очередь SHALL поддержать несколько активных consumer и выдавать сообщение лишь одному подходящему получателю за попытку; selector одного consumer не удаляет и не блокирует сообщение для другого.

ID: `REQ-71-1`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#майлстоны-последовательность-по-зависимостям`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1.2, 8.2.

#### Scenario: SelectiveQueueClaim

- **WHEN** два consumer на разных сессиях под конкуренцией читают перекрывающиеся фильтры и один откатывает claim
- **THEN** ни один claim не принадлежит одновременно двум получателям, unmatched доступно другому и rollback возвращает claim
- Test: `DestinationContractTest.SelectiveQueueClaim`

### Requirement: Независимая доставка топика

Топик SHALL направлять публикацию отдельно в каждую подходящую существовавшую при send подписку; unshared имеет один consumer, shared отдаёт каждую копию одному из участников; закрытие последнего consumer удаляет nondurable-подписку.

ID: `REQ-71-2`
Источник: `repo:docs/jms-spec/26-shared-consumers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.2, 8.3.1–8.3.4.

#### Scenario: SubscriptionFanout

- **WHEN** топик имеет unshared, shared и offline durable группы, затем последняя nondurable закрывается и создаётся заново
- **THEN** подтверждение одной копии не влияет на другие группы, shared не дублирует копии, nondurable не наследует старый backlog
- Test: `DestinationContractTest.SubscriptionFanout`

### Requirement: Ключи и замена durable

Провайдер SHALL требовать client ID для unshared durable и считать его необязательным для shared. Активные конфликтующие определения отклоняются; допустимое изменение неактивного определения заменяет подписку и её backlog по правилам спецификации. Shared и unshared durable с одинаковой парой конфликтуют; shared nondurable хранится отдельно.

ID: `REQ-71-3`
Источник: `repo:docs/jms-spec/26-shared-consumers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.4, 6.1.2, 8.3.3–8.3.4; Javadoc Session.unsubscribe.

#### Scenario: DurableIdentityMatrix

- **WHEN** матрица меняет topic, selector и noLocal где применимо, проверяет активную подписку, пустой client ID, повторное открытие и unsubscribe
- **THEN** ключи, исключения и сохранность backlog соответствуют виду подписки; active/unacknowledged случай удаления проверяется отдельно
- Test: `DestinationContractTest.DurableIdentityMatrix`

### Requirement: NoLocal по типам подписки

Провайдер SHALL фильтровать публикации того же connection для unshared nondurable, а для unshared durable также публикации с тем же client ID. Стандартные shared API не имеют параметра noLocal.

ID: `REQ-71-4`
Источник: `repo:docs/jms-spec/25-no-local.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.2.2, 8.3.1, 8.3.3–8.3.4.

#### Scenario: NoLocalMatrix

- **WHEN** два подключения публикуют при активной unshared подписке, затем publisher повторно использует client ID ранее закрытого durable consumer
- **THEN** локальные сообщения отфильтрованы только в соответствующих случаях, чужие сообщения сохраняются, shared подписка принимает свои допустимые публикации
- Test: `DestinationContractTest.NoLocalMatrix`

### Requirement: Неразрушающий просмотр

QueueBrowser SHALL перечислять разрешённые по selector сообщения без удаления и без изменения признаков доставки, соблюдая delivery time и документированную политику видимости конкурентных изменений. Выбранная политика tiny-mq — снимок eligible-записей на начало перечисления.

ID: `REQ-71-5`
Источник: `repo:docs/jms-spec/27-queue-browser.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1.6; Javadoc QueueBrowser.

#### Scenario: BrowseWithoutClaim

- **WHEN** browse с selector совпадает с конкурентным send и содержит сообщение с невышедшей задержкой
- **THEN** выдаются подходящие записи, уже доступные в момент снимка; ack и claim при browse отсутствуют
- Test: `DestinationContractTest.BrowseWithoutClaim`

### Requirement: Временные назначения и reply

TemporaryQueue и TemporaryTopic SHALL принимать публикации от авторизованного отправителя, разрешать потребление только соединению-владельцу и удаляться по допустимому delete или close владельца; QueueRequestor/TopicRequestor используют временный reply destination. Корреляция переносится через стандартные заголовки.

ID: `REQ-71-6`
Источник: `repo:docs/jms-spec/31-request-reply.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 2.16, 4.1.5–4.1.7, 4.2.7, 4.2.10.

#### Scenario: TemporaryReplyLifecycle

- **WHEN** другое соединение отправляет ответ в JMSReplyTo, пытается потреблять из него, затем владелец закрывается
- **THEN** владелец читает ответ с ожидаемой корреляцией, чужое потребление отклонено, закрытое временное назначение непригодно
- Test: `DestinationContractTest.TemporaryReplyLifecycle`

### Test plan

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

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Для нескольких потребителей нужен пересмотр ADR-0005. До решения не изменять правила закрепления сообщения или старые долговечные ключи.
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
