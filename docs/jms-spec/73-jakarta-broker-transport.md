# 73 Сетевой брокер, транспорт, безопасность и C++ клиент

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.2
Review Date: 2026-11-15
Examples: `WireContractTest.CapabilityNegotiation` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

CMake собирает tiny_mq вокруг in-process API; сетевой фабрики, протокольных команд и отдельного клиента в дереве нет. ADR-0001/0002 фиксируют STOMP 1.2 v1 и protobuf v2.

## Outcome

Оператор поднимает отдельный брокер, клиент подключается по объявленной версии, а ошибки
транспорта, отказ в доступе и подтверждение брокером различимы снаружи. Java, C++ и независимые
клиенты используют один опубликованный сетевой контракт.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[72](72-jakarta-delivery.md)

## Architecture

Транспортное подключение владеет своими session/producer/consumer handles. Кодек проверяет frame до вызова брокера; capability negotiation выбирает совместимое отображение. Server dispatch передаёт операции в ядро с connection/session scope и различает транспортный receipt, брокерское подтверждение и итог транзакции.

### Persistence / wire implications

База v1 — STOMP 1.2. Расширения для типизированного конверта, многооперационных сессий, start/stop, ack, browse, durable и async completion документируются и версионируются. v2 protobuf опционален в истории проекта; условие необходимости — v1 не удовлетворяет утверждённой семантике или измеренному бюджету. XA и recovery имеют отдельный контракт 75.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `BrokerServer` | SDD 73 |
| E2 | type | `WireCodec` | SDD 73 |
| E3 | type | `BrokerDispatcher` | SDD 73 |
| E4 | type | `RemoteSession` | SDD 73 |
| E5 | type | `CapabilitySet` | SDD 73 |
| E6 | type | `CppClient` | SDD 73 |
| E7 | type | `AuthorizationPolicy` | SDD 73 |
| E8 | type | `BrokerMetrics` | SDD 73 |
| E9 | test-suite | `WireContractTest` | SDD 73 |
| E10 | config | `DestinationDeliveryOverrides` | Четыре опциональные настройки |

## Acceptance Criteria

### Requirement: Совместимый handshake и кадрирование

Брокер SHALL согласовать версию и обязательные возможности, отклонить неизвестную обязательную возможность и разобрать фрагментированные и объединённые кадры с лимитами размера и состоянием heartbeat.

ID: `REQ-73-1`
Источник: `repo:docs/jms-spec/40-wire-protocol.md@9f7983c0054636a6686f9928831c8c53dfbd3679#decision`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § ADR-0001, ADR-0002; §1.1.4 оставляет wire проекту.

#### Scenario: CapabilityNegotiation

- **WHEN** сырой STOMP 1.2 клиент, проектный клиент и специально повреждённые кадры открывают отдельные подключения
- **THEN** стандартный STOMP сценарий работает; несовместимость и нарушение frame закрываются предсказуемо без воздействия на соседнюю сессию
- Test: `WireContractTest.CapabilityNegotiation`

### Requirement: Сетевые команды и итог операции

Брокер SHALL поддержать типизированный конверт, жизненный цикл сессии и назначения, selector, browse,
подписки, send/receive, квитирование и транзакции через документированный сетевой контракт.
Контракт определяет сообщения, состояния, версию, ошибки и возможности, чтобы независимый клиент
мог реализовать эти операции. Ответ несёт correlation ID и класс результата. При обрыве после
передачи команды клиент видит неопределённый исход, если брокер не может его доказать.

ID: `REQ-73-2`
Источник: `repo:docs/jms-spec/40-wire-protocol.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics-stomp-12-mapping`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § §§4, 6–10, локальное отображение.

#### Scenario: SessionAndReceiptIsolation

- **WHEN** независимый тестовый клиент по опубликованному контракту открывает две сессии,
  выполняет send, ack и commit, а обрыв вводится до и после устойчивой записи
- **THEN** результат прикреплён к правильному ID, квитанция сообщения и итог транзакции различимы,
  клиент сообщает неопределённый исход, если результат команды неизвестен
- Test: `WireContractTest.SessionAndReceiptIsolation`

### Requirement: TLS, проверка личности и прав

Удалённый режим SHALL проверять личность, сертификат и полномочие для send, consume, browse, создания/удаления и admin. Ошибки не раскрывают credentials или содержимое запрещённого назначения.

ID: `REQ-73-3`
Источник: `repo:docs/jms-spec/41-authn-authz-tls.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.1.1, 10.3; локальная политика.

#### Scenario: TlsAndAuthorization

- **WHEN** проверяются ошибочные сертификаты, отсутствие credentials, чужой temporary/handle и операция вне ACL
- **THEN** операции отклонены по политике, secrets отсутствуют в ответе и логах, авторизованный клиент успешно работает
- Test: `WireContractTest.TlsAndAuthorization`

### Requirement: Ограничения ресурсов

Транспорт SHALL ограничить входные кадры, накопленные неподтверждённые данные, in-flight и буфер медленного consumer по задокументированным пределам. Перегрузка имеет ограниченное ожидание или типизированный отказ и не исчерпывает ресурсы соседней сессии.

ID: `REQ-73-4`
Источник: `repo:docs/jms-spec/42-flow-control.md@9f7983c0054636a6686f9928831c8c53dfbd3679#proposed-design`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § §1.1.4 и §4.2.9; локальная политика.

#### Scenario: BoundedBackpressure

- **WHEN** медленный consumer удерживает credit, второй клиент работает, неподдерживаемый размер и heartbeat timeout вводятся отдельно
- **THEN** лимиты наблюдаемы, вторая сессия продолжает работать, ресурсы первого соединения освобождаются
- Test: `WireContractTest.BoundedBackpressure`

### Requirement: C++ клиент и совместимость конфигурации

Проект SHALL предоставить C++ библиотеку с in-process и network входами, едиными типами конверта и документированным соответствием ошибок. Существующий API продолжает собираться на прежнем fixture либо меняется через утверждённую миграцию.

ID: `REQ-73-5`
Источник: `repo:docs/jms-spec/60-cpp-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#scope-split`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § ADR-0003, ADR-0004; расширение tiny-mq.

#### Scenario: CppClientBackendParity

- **WHEN** один тестовый C++ сценарий запускает оба backend, транспорт теряет соединение во время операции
- **THEN** данные и наблюдаемая семантика совпадают для общих операций, сетевой исход доступен через ошибку, старый fixture собирается
- Test: `WireContractTest.CppClientBackendParity`

### Requirement: Эксплуатация и сетевой бюджет

Проект SHALL дать оператору конфигурацию назначения, health, счётчики backlog/ack/reject/recovery и защищённую интроспекцию. Для сетевого пути до изменения формируется baseline, после — ABBA с девятью повторами и порогом +5% CPU time/op к принятой базовой реализации.

ID: `REQ-73-6`
Источник: `repo:docs/jms-spec/43-admin-introspection.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § §1.1.4; локальные ADR и НФТ.

#### Scenario: OperationalProfile

- **WHEN** standalone broker поднимается с тестовой конфигурацией, затем его перезапускают под нагрузкой
- **THEN** оператор видит состояния без раскрытия данных, новые маршруты имеют воспроизводимую линию сравнения, существующий горячий путь в бюджете
- Test: `WireContractTest.OperationalProfile`

### Requirement: Административные параметры доставки

Брокер SHALL поддерживать выбранную опциональную возможность §3.4.12. Переназначение delivery
mode, priority, TTL и delay задаётся отдельно для назначения. Если правило активно, фактические
заголовки на стороне send и receive отражают значения правила и часы из 72. Правило без
указанного параметра сохраняет действующее значение клиентского send.

ID: `REQ-73-7`
Источник: `repo:docs/jms-spec/43-admin-introspection.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание — §3.4.12 [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html).

#### Scenario: AdministrativeSendOverrides

- **WHEN** оператор задаёт по одному и все четыре параметра, клиенты посылают сообщения с конфликтующими и незаданными значениями, выполняют транзакционные и обычные отправки.
- **THEN** обе стороны видят выбранные mode, priority, expiration и deliveryTime. Время и гарантия доставки следуют фактическим параметрам.
- Test: `WireContractTest.AdministrativeSendOverrides`

### Test plan

Набор `WireContractTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `WireContractTest.CapabilityNegotiation` — REQ-73-1.
- `WireContractTest.SessionAndReceiptIsolation` — REQ-73-2.
- `WireContractTest.TlsAndAuthorization` — REQ-73-3.
- `WireContractTest.BoundedBackpressure` — REQ-73-4.
- `WireContractTest.CppClientBackendParity` — REQ-73-5.
- `WireContractTest.OperationalProfile` — REQ-73-6.
- `WireContractTest.AdministrativeSendOverrides` — REQ-73-7.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Нужна смена ADR-0001/0002, или STOMP-отображение теряет обязательную операцию либо тип. До работы с реальными credentials должны быть проверены TLS и изоляция соединений.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/73/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/73/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/73/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
