# 75 XA и устойчивое восстановление транзакций

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.1
Review Date: 2026-11-15
Examples: `XaRecoveryTest.XaSurfaceAndOwnership` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

Текущий TransactionBuffer реализует локальную очередь изменений; старый проектный план откладывал XA. XA API, recovery и совместное решение с внешним RM отсутствуют.

## Outcome

Менеджер транзакций завершает общую транзакцию брокера и другого ресурса после перезапуска
брокера, опираясь на список подготовленных Xid и однозначный outcome.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[74](74-jakarta-java-se-provider.md)

## Architecture

XAResource представляет один RM с документированной идентичностью. Брокер хранит ветвь Xid и состояние ассоциации, готовности и решения. Журнал prepared и outcome переживает перезапуск; recovery перечисляет незавершённые ветви в соответствии с флагами.

### Persistence / wire implications

Двоичный Xid сохраняет formatId, gtrid и bqual. Для stage и outcome фиксируются допустимые переходы, повтор, ошибки протокола XA и восстановление при потерянном ответе. Обратимая миграция не теряет prepared branches.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `XaBranch` | SDD 75 |
| E2 | type | `XaBranchJournal` | SDD 75 |
| E3 | type | `TinyXaConnectionFactory` | SDD 75 |
| E4 | type | `TinyXaSession` | SDD 75 |
| E5 | type | `TinyXaJmsContext` | SDD 75 |
| E6 | type | `TinyXaResource` | SDD 75 |
| E7 | type | `RmIdentity` | SDD 75 |
| E8 | test-suite | `XaRecoveryTest` | SDD 75 |

## Acceptance Criteria

### Requirement: Поверхность XA

Профиль tiny-mq SHALL предоставить XAConnectionFactory/XAConnection/XASession/XAJMSContext и legacy XA factories/sessions, связать их с XAResource и передать управление commit/rollback менеджеру транзакций. Ограничения прямых commit/rollback отражаются предусмотренными исключениями.

ID: `REQ-75-1`
Источник: `repo:docs/jms-spec/32-xa-transactions.md@9f7983c0054636a6686f9928831c8c53dfbd3679#jms-reference`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.8, 11.3.1–11.3.5; Javadoc XAResource.

#### Scenario: XaSurfaceAndOwnership

- **WHEN** менеджер получает ресурсы по classic, simplified и legacy путям и пытается выполнить прямой commit/rollback
- **THEN** одна и та же ветвь управляется TM, запрещённые вызовы имеют правильные типы исключений
- Test: `XaRecoveryTest.XaSurfaceAndOwnership`

### Requirement: Протокол XA

Ресурс SHALL реализовать start/end с флагами join/resume/suspend, prepare/read-only, 1PC, 2PC commit, rollback, recovery scan, timeout, isSameRM и forget с предусмотренными XA error codes.

ID: `REQ-75-2`
Источник: `repo:docs/jms-spec/32-xa-transactions.md@9f7983c0054636a6686f9928831c8c53dfbd3679#decision-point`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 11.3.5; Javadoc XAResource.

#### Scenario: XaTransitionMatrix

- **WHEN** таблица покрывает разрешённые и неверные Xid/переходы, timeout и повторный запрос при потере ответа
- **THEN** видимость и коды XA соответствуют протоколу, повтор не создаёт второй эффект
- Test: `XaRecoveryTest.XaTransitionMatrix`

### Requirement: Две устойчивые ветви

Профиль SHALL восстанавливать prepared ветви после принудительного рестарта и завершать 2PC по решению TM вместе с независимым durable RM. Подготовленные сообщения не видны потребителю до commit; rollback возвращает вход и убирает выход.

ID: `REQ-75-3`
Источник: `repo:docs/jms-spec/32-xa-transactions.md@9f7983c0054636a6686f9928831c8c53dfbd3679#decision-point`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 11.3, 12.3; Javadoc XAResource.

#### Scenario: TwoResourceCrashRecovery

- **WHEN** TM вводит отказ между prepare и решением, затем recovery полного и прерванного scan, с commit и rollback отдельными сценариями
- **THEN** список recovery содержит нужные Xid, обе ветви приходят к решению TM без частичного применения
- Test: `XaRecoveryTest.TwoResourceCrashRecovery`

### Requirement: Защита от несовместимого отката

Проект SHALL блокировать откат к сборке без чтения XA-журнала при незавершённой ветви или неподдерживаемом формате и выдавать диагностику без изменения журнала.

ID: `REQ-75-4`
Источник: `repo:tasks/M5-robustness-and-ops.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m5-3-p1-восстановление-персистентности-при-рестарте`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § Локальная гарантия данных; §11.3.

#### Scenario: XaDowngradeGuard

- **WHEN** сборка после prepare пытается открыть журнал в режиме downgrade до и после завершения всех ветвей
- **THEN** несовместимое открытие безопасно отклонено, штатный откат подтверждён отдельным прогоном
- Test: `XaRecoveryTest.XaDowngradeGuard`

### Test plan

Набор `XaRecoveryTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `XaRecoveryTest.XaSurfaceAndOwnership` — REQ-75-1.
- `XaRecoveryTest.XaTransitionMatrix` — REQ-75-2.
- `XaRecoveryTest.TwoResourceCrashRecovery` — REQ-75-3.
- `XaRecoveryTest.XaDowngradeGuard` — REQ-75-4.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- До выбора durability boundary, retention prepared и происхождения RM identity XA не переводится в доступную возможность. Необратимые эвристические исходы и откат на версию без чтения Xid требуют решения Owner.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/75/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/75/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/75/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
