# 72 Доставка, порядок, квитирование и локальные транзакции

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.1
Review Date: 2026-11-15
Examples: `DeliveryContractTest.SendClockMatrix` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

Producer.cpp вычисляет задержку в транзакции от commit и ставит 0 без задержки; актуальная норма вычисляет JMSDeliveryTime от send. Connection.cpp ведёт _started, но семантику каждого receive и callback нужно подтвердить. RecoverTest и DlqTest фиксируют локальные результаты; атомарность между назначениями и рестарт требуют fault injection.

## Outcome

Сессия выполняет send/receive и ack по контракту 3.1. Проверки показывают границу видимости,
исход после сбоя, порядок callbacks и идемпотентный переход в DLQ там, где это расширение включено.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[70](70-jakarta-message-contract.md), [71](71-jakarta-destinations.md)

## Architecture

Сессия владеет последовательностью доставки и единым журналом входов и выходов транзакции. Время send берётся из согласованного источника; commit задаёт видимость. Признаки редоставки и результат фиксации наблюдаются через конверт и квитанцию. Для неблокирующей доставки и часов тесты управляют моментами времени и завершения.

### Persistence / wire implications

Persisted deliveryTime не используется как внутреннее поле backoff. Старая запись v0x02 читается с прежним смыслом для уже принятых записей; решение о миграции и формате журнала принимается до изменений данных.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `DeliveryClock` | SDD 72 |
| E2 | type | `DeliveryAttempt` | SDD 72 |
| E3 | type | `SessionDispatch` | SDD 72 |
| E4 | type | `LocalTransactionJournal` | SDD 72 |
| E5 | type | `SendCompletion` | SDD 72 |
| E6 | storage-field | `nextRedeliveryAt` | SDD 72 |
| E7 | test-suite | `DeliveryContractTest` | SDD 72 |

## Acceptance Criteria

### Requirement: Send clock, expiration и приоритет

Провайдер SHALL рассчитывать абсолютные delivery time и expiration от send, включая transacted send; TTL=0 даёт expiration=0. До commit и delivery time сообщение недоступно, expired-записи обрабатываются по TTL. Приоритет 0–9 учитывается в пределах правил порядка §6.2.9.

ID: `REQ-72-1`
Источник: `repo:docs/jms-spec/13-delivery-delay.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.4.9–3.4.13, 7.6–7.9.

#### Scenario: SendClockMatrix

- **WHEN** контролируемые часы разделяют send, срок TTL, срок delay и commit, включая delay=0, обе стороны commit и persistent replay
- **THEN** заголовки отражают время send, после commit просроченные записи пропущены, доступная запись не получает новую задержку
- Test: `DeliveryContractTest.SendClockMatrix`

### Requirement: Синхронное получение и lifecycle

Провайдер SHALL поддержать receive, receive(timeout), receiveNoWait и close согласно режиму доставки. stop приостанавливает доставку, start возобновляет её; close разблокирует ожидающее получение. receive(0) и receiveNoWait имеют разное поведение.

ID: `REQ-72-2`
Источник: `repo:docs/jms-spec/28-receive-no-wait.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.1.4–6.1.7, 6.2.5, 8.4–8.8.

#### Scenario: ReceiveLifecycle

- **WHEN** пустая, stopped и ready очередь прогоняются с контролируемым тайм-аутом, start/stop и закрытием в другом потоке
- **THEN** нет ранней выдачи при stopped, без ожидания возвращается null, blocking-вызов завершается при close согласно Javadoc
- Test: `DeliveryContractTest.ReceiveLifecycle`

### Requirement: Listener и граница сессии

Провайдер SHALL сериализовать MessageListener для одной сессии, соблюдать исключения потоковой модели для start/stop/close, и применять семантику исключения listener по режиму ack, включая Transaction rollback по решению приложения.

ID: `REQ-72-3`
Источник: `repo:docs/jms-spec/20-message-listener.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.5–6.2.6, 6.2.13, 8.7–8.8.

#### Scenario: ListenerModeMatrix

- **WHEN** два consumer одной сессии конкурируют, listener бросает runtime-ошибку в каждом режиме, закрытие вызывается из допустимых потоков
- **THEN** callbacks не перекрываются, ack/redelivery и закрытие следуют каждой ветви стандарта
- Test: `DeliveryContractTest.ListenerModeMatrix`

### Requirement: Завершение асинхронной отправки

Провайдер SHALL отличать исключение при вызове от поздней ошибки; для принятых async send вызвать ровно один callback в порядке отправок для данного MessageProducer или JMSContext. Сессия выполняет не более одного callback одновременно, вызывая его на другом потоке, чем вызов send. Callback, пытающийся закрыть свой producer/session/connection или вызвать commit/rollback своей сессии, получает IllegalStateException либо runtime-аналог. close/commit/rollback дожидаются завершения предусмотренных async send и callbacks; смешанные sends сохраняют правила порядка.

ID: `REQ-72-4`
Источник: `repo:docs/jms-spec/21-completion-listener.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 7.2–7.3.

#### Scenario: AsyncSendBarriers

- **WHEN** два producer одной сессии смешивают sync/async, поздний и немедленный отказы вводятся отдельно, callback пробует запрещённый повторный вход и удерживается перед close/commit/rollback
- **THEN** проверяются порядок, эксклюзивность на сессию, поток callback, отсутствие callback при ошибке вызова, видимость заголовков и барьеры завершения
- Test: `DeliveryContractTest.AsyncSendBarriers`

### Requirement: Квитирование, recover и receiveBody

Провайдер SHALL применить AUTO_ACKNOWLEDGE, CLIENT_ACKNOWLEDGE и DUPS_OK_ACKNOWLEDGE с их границами. CLIENT подтверждает доставленное всей сессии. recover восстанавливает неподтверждённое с признаками redelivery; receiveBody с несовместимым типом соблюдает особые правила режима без ложного увеличения счётчика.

ID: `REQ-72-5`
Источник: `repo:docs/jms-spec/23-recover.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.4.7, 3.5.11, 6.2.10–6.2.13, 8.6.

#### Scenario: AcknowledgeAndRecoverMatrix

- **WHEN** два consumer, fault injection между доставкой и ack, recover и несовместимый receiveBody проверяются в каждом режиме
- **THEN** JMSXDeliveryCount начинается с 1, redelivery и исключения точны, подтверждения другой сессии не меняются
- Test: `DeliveryContractTest.AcknowledgeAndRecoverMatrix`

### Requirement: Атомарная локальная транзакция

Провайдер SHALL атомарно подтверждать потреблённое и публиковать отправленное в пределах сессии; rollback/close активной транзакции удаляет неподтверждённые выходы и возвращает входы. После сбоя состояние однозначно соответствует одному принятому решению фиксации.

ID: `REQ-72-6`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.7, 6.2.15, 9.1.

#### Scenario: AtomicConsumeAndSend

- **WHEN** транзакция читает очередь и отправляет в две цели; процесс останавливается до и после границы устойчивого commit
- **THEN** восстановление не выдаёт частичный commit, rollback даёт повтор входа без выходов, committed результат доступен целиком
- Test: `DeliveryContractTest.AtomicConsumeAndSend`

### Requirement: Доставка после отказа и политика DLQ

Провайдер SHALL выполнять гарантии для persistent очереди/durable подписки и ограничения non-persistent с учётом ack и допустимых повторов. tiny-mq сохраняет состояние redelivery и своей политики backoff/DLQ для persistent записей; стандартный профиль не применяет лимит DLQ по умолчанию.

ID: `REQ-72-7`
Источник: `repo:openspec/specs/message-redelivery/spec.md@9f7983c0054636a6686f9928831c8c53dfbd3679#requirements`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.5.11, 6.2.9–6.2.11, 7.7, 9.1; расширение tiny-mq.

#### Scenario: CrashAndDeadLetterMatrix

- **WHEN** после ack, до ack и во время переноса в DLQ брокер принудительно останавливается, затем читает старые и новые записи
- **THEN** нет потери зафиксированного сообщения в пределах гарантии, ack не переигран, redelivery не подменяет исходный JMSDeliveryTime, DLQ-маршрут идемпотентен
- Test: `DeliveryContractTest.CrashAndDeadLetterMatrix`

### Requirement: Порядок отправок одной сессии

Провайдер SHALL сохранять порядок отправок одной сессии в одну цель, включая разные producer этой сессии, с оговорками §6.2.9 о priority, delivery time, режиме доставки и атомарности транзакции. Между сессиями и назначениями общий порядок не гарантируется.

ID: `REQ-72-8`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#сквозные-требования-на-всех-майлстонах`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.9, 7.6.

#### Scenario: SessionSendOrder

- **WHEN** два producer одной сессии отправляют в одну цель последовательность с одинаковыми priority, режимом и временем доступности, затем отдельно проверяются допустимые исключения.
- **THEN** порядок для однородной последовательности совпадает с порядком send, перестановки ограничены ветвями §6.2.9.
- Test: `DeliveryContractTest.SessionSendOrder`

### Requirement: Бюджет горячего пути доставки

Изменённые ack, storage и delivery paths SHALL укладываться в +5% CPU time/op на сопоставимом
ABBA прогоне с девятью повторами в RelWithDebInfo. Сравниваются медианы в режимах доставки,
квитирования и транзакций, которые затрагивает изменение.

ID: `REQ-72-9`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#сквозные-требования-на-всех-майлстонах`

#### Scenario: HotPathBudget

- **WHEN** рецензент повторяет сопоставимые бенчмарки на одной машине с main и веткой.
- **THEN** журнал содержит медианы по каждому режиму и показывает соблюдение порога.
- Test: `DeliveryContractTest.HotPathBudget`

### Test plan

Набор `DeliveryContractTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `DeliveryContractTest.SendClockMatrix` — REQ-72-1.
- `DeliveryContractTest.ReceiveLifecycle` — REQ-72-2.
- `DeliveryContractTest.ListenerModeMatrix` — REQ-72-3.
- `DeliveryContractTest.AsyncSendBarriers` — REQ-72-4.
- `DeliveryContractTest.AcknowledgeAndRecoverMatrix` — REQ-72-5.
- `DeliveryContractTest.AtomicConsumeAndSend` — REQ-72-6.
- `DeliveryContractTest.CrashAndDeadLetterMatrix` — REQ-72-7.
- `DeliveryContractTest.SessionSendOrder` — REQ-72-8.
- `DeliveryContractTest.HotPathBudget` — REQ-72-9.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Невозможно обеспечить атомарность успешной фиксации по нескольким назначениям или требуется переход на необратимый формат. Перед изменением основания времени синхронизировать решение со спекой 13 и ADR-0008.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для изменённых горячих путей действует `REQ-72-9`.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/72/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/72/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/72/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
