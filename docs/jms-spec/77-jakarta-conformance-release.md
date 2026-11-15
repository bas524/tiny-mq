# 77 Полнота покрытия, TCK, переносимость и выпуск

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.3
Review Date: 2026-11-15
Examples: `ReleaseConformanceTest.NormativeCoverage` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

В Git есть C++ GoogleTest, Release perf и ASan в CI, но нет Java тестов Jakarta, TCK-отчёта и манифеста конформанса.

## Outcome

Инженер выпуска повторяет проверку покрытия, Java TCK, обмен между Java и C++, испытания
под нагрузкой и при отказе узла. Манифест связывает результаты с конкретной поставкой и её зависимостями.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[73 Транспорт](73-jakarta-broker-transport.md), [74 Java SE](74-jakarta-java-se-provider.md),
[76 Jakarta EE](76-jakarta-ee-integration.md), [78 Высокая доступность](78-jakarta-high-availability.md).

## Architecture

Матрица привязывает каждый применимый нормативный пункт и обязательную Javadoc-операцию к исполнителю, тесту и редакции, распространяется с результатом выпуска. CI хранит точные команды и версии Java, брокера, контейнера, TM, TCK, platform image и свидетельства. Манифест ссылается на профили нагрузок и отказов, результаты измерений, граф зависимостей и минимальные версии компиляторов.

### Persistence / wire implications

Отчёт выпуска фиксирует версии wire, минимальные совместимые клиенты и store, пределы downgrade и сценарий восстановления backup. TCK комплектуется по правилам соответствующего выпуска и хранит только разрешённые для распространения материалы.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `ConformanceMatrix` | SDD 77 |
| E2 | type | `ReleaseManifest` | SDD 77 |
| E3 | test-suite | `JakartaMessagingTck` | SDD 77 |
| E4 | test-suite | `ReleaseConformanceTest` | SDD 77 |
| E5 | concept | `ReleasePerformanceProfile` | Нагрузки, среды, метрики и цели выпуска |
| E6 | concept | `DependencyInventory` | Учёт зависимостей по артефакту и профилю сборки |

## Acceptance Criteria

### Requirement: Полнота карты

Релиз SHALL иметь отчёт по каждому нормативному положению и обязательному публичному методу, включая отдельную применимость Java SE, EE, XA и расширений, выбранные optional возможности и причину каждой отметки N/A. ID, SDD, тест, результат, версия обязательны для применимых положений. Для каждого поддерживаемого клиента матрица указывает доступные операции, типы, гарантии и диапазон версий протокола.

ID: `REQ-77-1`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-5-p2-карта-соответствия-jms`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § гл. 1–13, Javadoc 3.1; гл. 14–15 и приложение A как примеры и история.

#### Scenario: NormativeCoverage

- **WHEN** проверка удаляет привязку одного MUST, добавляет пропущенный overload, ставит N/A без обоснования и убирает версию протокола у C++ клиента
- **THEN** каждый дефект блокирует полноту отчёта, отчёт без пропусков проходит
- Test: `ReleaseConformanceTest.NormativeCoverage`

### Requirement: TCK и публичное заявление

Проект SHALL прогнать официальный Messaging 3.1 TCK и signature checks на кандидатах с подходящими профилями. Перед публикацией claims Owner проверяет итог, правила совместимости и редакции; собственный suite идёт отдельным свидетельством.

ID: `REQ-77-2`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-1-p1-jms-conformance-test-suite`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § страница выпуска 3.1 и TCK.

#### Scenario: TckAndCoverage

- **WHEN** чистая повторяемая среда запускает TCK и signature, один сбой и отсутствие артефакта проверяются в CI
- **THEN** результат проверяем независимо, неподтверждённый профиль не получает claim
- Test: `ReleaseConformanceTest.TckAndCoverage`

### Requirement: Сквозной сценарий Java SE

Сборка SHALL выполнить INT-01-SE на отдельных JVM и брокере через JNDI, с rollback, commit, offline durable и перезапуском после фиксации, записывая трассу каждого наблюдаемого эффекта.

ID: `REQ-77-3`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-2-p2-интеграционные-end-to-end-тесты`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.7, 8.3, 9.1, 15.10.

#### Scenario: JavaSeRecoveryRoundTrip

- **WHEN** один тест запускает два приложения, разрывает первую транзакцию и перезапускает брокер после второй
- **THEN** у rollback нет видимого выхода, вход повторно доставлен, подтверждённый выход получает durable consumer
- Test: `ReleaseConformanceTest.JavaSeRecoveryRoundTrip`

### Requirement: Сквозной сценарий Jakarta EE

Сборка SHALL выполнить INT-01-EE в выбранном контейнере с MDB, injected context, TM, independent RM и брокером, тестируя commit и rollback с recovery.

ID: `REQ-77-4`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-2-p2-интеграционные-end-to-end-тесты`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 11–13; §1.1.5.

#### Scenario: JakartaEeXaRecovery

- **WHEN** процесс брокера принудительно завершается после prepare и запускается повторно
- **THEN** fixture повторяет исходы XA без потери входа при rollback и без частичной фиксации при commit
- Test: `ReleaseConformanceTest.JakartaEeXaRecovery`

### Requirement: Манифест и проверяемый откат

Кандидат SHALL содержать инструкции запуска и миграции, матрицу версий, манифест артефактов и тест восстановления на последнем поддерживаемом формате. Для профиля HA манифест связывает результаты [SDD 78](78-jakarta-high-availability.md) с заявленной топологией, пределами отказа и целями RTO/RPO. При downgrade сначала обрабатываются несовместимые данные и ветви XA по утверждённой процедуре.

ID: `REQ-77-5`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-4-p2-документация`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 9.1; локальный релизный контракт.

#### Scenario: ReleaseManifestAndRestore

- **WHEN** сборка из чистого репозитория прогоняет Linux/macOS C++ проверки, Java/EE интеграцию,
  испытание HA, обновление и восстановление fixture storage
- **THEN** из манифеста воспроизводятся развёртывание, тесты, измерения при отказе и операция отката
  либо диагностированный запрет при несовместимом состоянии
- Test: `ReleaseConformanceTest.ReleaseManifestAndRestore`

### Requirement: Сквозное взаимодействие Java и C++

Сборка SHALL выполнить INT-01-INTEROP через отдельный брокер, стандартный Java API и нативный
сетевой клиент C++. Сквозная проверка подтверждает межъязыковой контракт и результаты приложения.

ID: `REQ-77-6`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-2-p2-интеграционные-end-to-end-тесты`
Источник: `repo:docs/jms-spec/60-cpp-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#scope-split`

#### Scenario: JavaAndCppRoundTrip

- **WHEN** Java-приложение посылает persistent-запрос с `JMSReplyTo` и типизированными свойствами,
  C++ процесс получает его по селектору, отправляет ответ, затем подтверждает запрос
- **THEN** Java получает ответ по адресу `JMSReplyTo` с `JMSCorrelationID`, равным `JMSMessageID` запроса.
  Проверяются значения и типы тела и свойств в обоих направлениях. Переоткрытый consumer
  с тем же селектором не получает подтверждённый запрос
- Test: `ReleaseConformanceTest.JavaAndCppRoundTrip`

### Requirement: Производительность выпуска

Кандидат SHALL укладываться в опубликованные цели для штатной и отказной нагрузки. Профиль задаёт
оборудование, топологию, объём сообщений, длительность, число клиентов, режимы persistence, ack и
транзакций. В отчёте есть throughput завершённых операций, p95/p99 сквозной задержки, CPU, пик
памяти, таймауты и ошибки. Во время переключения измеряется также длительность прерывания. Замеры
регрессии горячего пути сохраняют порог 5% CPU time/op и методику существующих SDD.

ID: `REQ-77-7`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#сквозные-требования-на-всех-майлстонах`
Источник: `repo:tasks/M6-conformance-and-docs.md@9f7983c0054636a6686f9928831c8c53dfbd3679#m6-3-p2-бенчмарки-сетевого-пути`

#### Scenario: PerformanceProfile

- **WHEN** одинаковый набор нагрузок проходит на кандидате и принятой базе, а в отдельном прогоне
  под нагрузкой теряется один узел профиля HA
- **THEN** отчёт показывает соблюдение применимых абсолютных порогов, бюджета времени переключения
  и относительного порога горячего пути. Ошибки и таймауты учитываются при оценке результата
- Test: `ReleaseConformanceTest.PerformanceProfile`

### Requirement: Состав сторонних зависимостей поставки

Манифест SHALL указывать фактические прямые и транзитивные зависимости брокера, клиентов и
адаптеров по профилям поставки, включая статически связанные и встроенные библиотеки. Зависимости сборки,
тестов и бенчмарков отмечаются отдельно. Для каждой поставляемой сторонней библиотеки записываются
назначение, область применения и причина, по которой штатные средства платформы или уже
используемые компоненты не покрывают требования по корректности, скорости или переносимости.
Для нативных компонентов в первую очередь оценивается стандартная библиотека C++.

ID: `REQ-77-8`
Источник: `repo:arch/0004-build-targets-layout.md@9f7983c0054636a6686f9928831c8c53dfbd3679#решение`

#### Scenario: DependencyFootprint

- **WHEN** артефакты собираются в профилях поставки с отключёнными тестами и бенчмарками,
  граф сборки, линковка и содержимое пакетов сверяются с инвентарём
- **THEN** все поставляемые зависимости учтены и обоснованы, инструменты тестов и бенчмарков
  отсутствуют в поставке, а минимальный заявленный компилятор собирает продукт
- Test: `ReleaseConformanceTest.DependencyFootprint`

### Test plan

Набор `ReleaseConformanceTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `ReleaseConformanceTest.NormativeCoverage` — REQ-77-1.
- `ReleaseConformanceTest.TckAndCoverage` — REQ-77-2.
- `ReleaseConformanceTest.JavaSeRecoveryRoundTrip` — REQ-77-3.
- `ReleaseConformanceTest.JakartaEeXaRecovery` — REQ-77-4.
- `ReleaseConformanceTest.ReleaseManifestAndRestore` — REQ-77-5.
- `ReleaseConformanceTest.JavaAndCppRoundTrip` — REQ-77-6.
- `ReleaseConformanceTest.PerformanceProfile` — REQ-77-7.
- `ReleaseConformanceTest.DependencyFootprint` — REQ-77-8.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Обязательный пункт без теста, неподтверждённая применимость, неуспешный TCK, несовместимый откат или расхождение повторного прогона с заявленным отчётом останавливают соответствующее заявление о совместимости.
- Для приёмки производительности и доступности нужны численные цели и эталонные профили.
  Непройденное испытание на отказ узла, неподтверждённый бюджет производительности
  или поставляемая зависимость без обоснования останавливают выпуск.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/77/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/77/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/77/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
