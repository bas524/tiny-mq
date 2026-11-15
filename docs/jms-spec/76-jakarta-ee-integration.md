# 76 Jakarta EE, JTA, MDB и адаптер Jakarta Connectors

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.1
Review Date: 2026-11-15
Examples: `EeContractTest.ServerSessionPoolDelivery` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

Контейнера, RAR и Java-модуля в Git нет. В план полного профиля включены выбранные проектом возможности глав 11–13.

## Outcome

Приложение в целевом EE-контейнере получает управляемый JMSContext и обрабатывает сообщение
через MDB. Контейнер управляет соединениями, доставкой и транзакцией до и после сбоя.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[75](75-jakarta-xa-recovery.md)

## Architecture

Java-провайдер, серверный consumer, адаптер и контейнер имеют разные жизненные циклы. JTA владеет транзакцией, контейнер владеет потоками и scope injected JMSContext, broker хранит порядок и исход. Адаптер связывает managed connection, ActivationSpec и endpoint без ручного управления из приложения.

### Persistence / wire implications

XA и local outcomes переиспользуют 72/75. Конфигурация activation отображает стандартные имена; secrets передаются конфигурацией контейнера, не сохраняются в проверочные артефакты.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `ConnectionConsumer` | SDD 76 |
| E2 | type | `ServerSessionBridge` | SDD 76 |
| E3 | type | `TinyResourceAdapter` | SDD 76 |
| E4 | type | `TinyActivationSpec` | SDD 76 |
| E5 | type | `TinyManagedConnectionFactory` | SDD 76 |
| E6 | type | `ManagedContextBridge` | SDD 76 |
| E7 | type | `EndpointDelivery` | SDD 76 |
| E8 | test-suite | `EeContractTest` | SDD 76 |

## Acceptance Criteria

### Requirement: ConnectionConsumer и пул сессий

Профиль SHALL поддержать серверные методы создания consumer, включая предусмотренные durable/shared варианты, ServerSession и ServerSessionPool, Session.run и Session MessageListener. Не более maxMessages передаётся сессии за назначение, доставка внутри каждой сессии последовательна.

ID: `REQ-76-1`
Источник: `repo:docs/jms-spec/61-java-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#interfaces-to-implement`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 11.2, 11.4; Javadoc.

#### Scenario: ServerSessionPoolDelivery

- **WHEN** контейнер предоставляет пул с двумя сессиями, ограничением maxMessages и выборкой selector
- **THEN** параллелизм наблюдается между сессиями, порядок и transaction ownership сохраняются внутри сессии
- Test: `EeContractTest.ServerSessionPoolDelivery`

### Requirement: Контейнерные ограничения и JTA

EE профиль SHALL различать application client, web и Enterprise Beans среды, соблюдать один активный session где требуется, запретные операции и фактическую матрицу поведения выбранного ack mode с активной и отсутствующей JTA. Async send в web/EJB явно отклоняется как политика проекта при разрешённой сигнатуре ошибки.

ID: `REQ-76-2`
Источник: `repo:docs/jms-spec/32-xa-transactions.md@9f7983c0054636a6686f9928831c8c53dfbd3679#jms-reference`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 7.3.5, 12.2–12.3.

#### Scenario: ContainerRestrictionsMatrix

- **WHEN** таблица параметров createSession/createContext, forbidden методов, async send и commit прогоняется в применимых средах
- **THEN** JTA управляет фиксацией, вне JTA работают предписанные правила, исключения различают обязательные и выбранные проектом ветви
- Test: `EeContractTest.ContainerRestrictionsMatrix`

### Requirement: Внедрение и именование

Полный профиль SHALL обеспечить внедрение JMSContext с JMSConnectionFactory, JMSPasswordCredential, JMSSessionMode, DefaultJMSConnectionFactory, а также resource definitions factory и destination, включая повторяемые аннотации 3.1. Одинаковые наборы аннотаций делят underlying context в области JTA/request; завершение области его закрывает.

ID: `REQ-76-3`
Источник: `repo:docs/jms-spec/02-jmscontext.md@9f7983c0054636a6686f9928831c8c53dfbd3679#jms-reference`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 12.4, A.1; Javadoc аннотаций.

#### Scenario: InjectionAndResourceDefinitions

- **WHEN** fixture определяет несколько ресурсов, использует повторяемые определения и два injected context с совпадающими ключами, меняет транзакцию и запрос
- **THEN** lookup и инъекция разрешаются, scope и reuse наблюдаемы, каждый запрещённый метод injected context даёт IllegalStateRuntimeException
- Test: `EeContractTest.InjectionAndResourceDefinitions`

### Requirement: Адаптер и MDB

Выбранный профиль SHALL поставлять RAR с outbound pooling и inbound endpoint activation/deactivation и recovery. ActivationSpec поддерживает destinationLookup, connectionFactoryLookup, destinationType, messageSelector, acknowledgeMode, subscriptionDurability, clientId и subscriptionName с их применимостью.

ID: `REQ-76-4`
Источник: `repo:docs/jms-spec/61-java-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#purpose`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 13, 13.1; Jakarta Connectors и Enterprise Beans по применимости.

#### Scenario: MdbXaActivation

- **WHEN** контейнер развёртывает RAR, активирует queue MDB и durable topic MDB, вводит сбой prepared XA, затем деактивирует endpoint
- **THEN** конфигурация проверена до доставки, recovery возобновляет решение TM, после деактивации новые callbacks не приходят
- Test: `EeContractTest.MdbXaActivation`

### Test plan

Набор `EeContractTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `EeContractTest.ServerSessionPoolDelivery` — REQ-76-1.
- `EeContractTest.ContainerRestrictionsMatrix` — REQ-76-2.
- `EeContractTest.InjectionAndResourceDefinitions` — REQ-76-3.
- `EeContractTest.MdbXaActivation` — REQ-76-4.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- До испытаний зафиксировать конкретный Full Platform контейнер и поддерживаемую версию Java. Если контейнер сам владеет частью требований, записать интерфейс, версию и воспроизводимый тест интеграции.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/76/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/76/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/76/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
