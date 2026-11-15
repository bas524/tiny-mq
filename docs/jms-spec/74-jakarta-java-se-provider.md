# 74 Java SE провайдер Jakarta Messaging 3.1

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.2
Review Date: 2026-11-15
Examples: `JavaProviderTest.ApiSurfaceAndJavaBaseline` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

В репозитории отсутствуют Java-модуль и JNDI fixture. Старый 61-java-client.md описывает JMS 2.0 и STOMP. Брокер ещё не предоставляет capabilities, нужные Java API.

## Outcome

Java SE приложение собирается с официальным jakarta.jms 3.1, получает фабрику и назначение
через JNDI и выполняет сценарии classic, simplified и legacy через один провайдер.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

[73](73-jakarta-broker-transport.md)

## Architecture

Java клиент реализует API jakarta.jms 3.1 с классами провайдера за интерфейсами. Один transport connection обслуживает отдельные состояния сессий. Общая модель конверта, lifecycle и исключений переиспользуется во всех API. JNDI возвращает конфигурацию, соединение открывается при явной операции клиента.

### Persistence / wire implications

Клиент проверяет capabilities до первой операции, связанной с конкретным расширением wire. При смене версии проверяются serializable/referenceable конфигурации и отказ несовместимой версии.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | API | `jakarta.jms` | SDD 74 |
| E2 | type | `TinyConnectionFactory` | SDD 74 |
| E3 | type | `TinyConnection` | SDD 74 |
| E4 | type | `TinySession` | SDD 74 |
| E5 | type | `TinyJmsContext` | SDD 74 |
| E6 | type | `TinyMessageProducer` | SDD 74 |
| E7 | type | `TinyMessageConsumer` | SDD 74 |
| E8 | type | `TinyNamingProvider` | SDD 74 |
| E9 | type | `JmsErrorMapper` | SDD 74 |
| E10 | test-suite | `JavaProviderTest` | SDD 74 |

## Acceptance Criteria

### Requirement: Подписи и полнота API

Поставляемый Java SE клиент SHALL реализовать применимые classic, simplified и legacy интерфейсы и перегрузки jakarta.jms 3.1. Все обязательные методы имеют проверенную семантику; provider JAR не подменяет официальный API JAR. Рабочий минимум Java SE 11 подтверждается отдельным прогоном.

ID: `REQ-74-1`
Источник: `repo:docs/jms-spec/61-java-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#interfaces-to-implement`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 1.1.5, 2.7–2.14, 4, 6–8; Javadoc 3.1.

#### Scenario: ApiSurfaceAndJavaBaseline

- **WHEN** собирается клиент, импортирующий только jakarta.jms, signature check сравнивает классы, smoke запускается на Java 11
- **THEN** интерфейсы и обязательные операции доступны, ни одна ветвь обязательного метода не оставлена заглушкой
- Test: `JavaProviderTest.ApiSurfaceAndJavaBaseline`

### Requirement: Администрируемые объекты и обнаружение

Factory и Destination SHALL поддержать требования JNDI к административной конфигурации, serializable/referenceable представлению и одновременному использованию; поддерживаются queue/topic и соответствующие legacy factories, получение объекта не открывает сеть.

ID: `REQ-74-2`
Источник: `repo:docs/jms-spec/61-java-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#purpose`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 2.3, 5.1–5.3, 6.1.1, 6.1.3; Javadoc.

#### Scenario: AdministeredObjectsAndNaming

- **WHEN** несколько потоков получают factory и назначения JNDI, сериализуют, восстанавливают и запускают два подключения
- **THEN** провайдер восстанавливает настройки подключения и типы объектов; соединение появляется при вызове factory и ошибки аутентификации типизированы
- Test: `JavaProviderTest.AdministeredObjectsAndNaming`

### Requirement: Классический жизненный цикл

Connection SHALL соблюдать правила уникальности и момента установки clientID, metadata, ExceptionListener и start/stop/close. `ConnectionMetaData.getJMSXPropertyNames()` объявляет поддерживаемые имена, включая `JMSXDeliveryCount`, `JMSXGroupID` и `JMSXGroupSeq`. Выдача `JMSMessageID` соответствует уникальности в пределах установки после рестарта. Session, Producer, Consumer, Browser и временные цели закрываются в порядке зависимости и возвращают классы исключений, предусмотренные контрактом.

ID: `REQ-74-3`
Источник: `repo:docs/jms-spec/03-client-id-and-lifecycle.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.1.2–6.1.7, 6.2, 10.2–10.3.

#### Scenario: ClassicLifecycleAndMetadata

- **WHEN** повторяется одинаковый clientID, соединение останавливается с активным callback, выполняется cascading close и удалённый failure. Клиент читает метаданные и отправляет обычные сообщения до и после рестарта того же брокера
- **THEN** результаты жизненного цикла соответствуют контракту объектов. В метаданных есть все три указанных JMSX-имени, обычные ID имеют префикс `ID:` и различаются до и после рестарта
- Test: `JavaProviderTest.ClassicLifecycleAndMetadata`

### Requirement: JMSContext и цепочки JMSProducer

JMSContext SHALL поддержать создание отдельной сессии на общем подключении, auto-start, независимое закрытие context, chainable setters, typed send/receiveBody, стандартные значения параметров и runtime-исключения.

ID: `REQ-74-4`
Источник: `repo:docs/jms-spec/02-jmscontext.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2, 7.2–7.3, 7.10, 8.4–8.8, 10.

#### Scenario: SimplifiedContextMatrix

- **WHEN** два контекста одного подключения в разных режимах, disable auto-start, смешанные типы тела и закрытие одного context
- **THEN** второй context сохраняет доступ, auto-start соответствует настройке, неверное тело и ack дают ожидаемый результат по матрице
- Test: `JavaProviderTest.SimplifiedContextMatrix`

### Requirement: Legacy domain API

Провайдер SHALL реализовать QueueConnectionFactory/QueueConnection/QueueSession/QueueSender/QueueReceiver и TopicConnectionFactory/TopicConnection/TopicSession/TopicPublisher/TopicSubscriber, включая Requestor и ограничения унаследованных методов на чужом домене.

ID: `REQ-74-5`
Источник: `repo:docs/jms-spec/61-java-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#interfaces-to-implement`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 4.1–4.2, 9.2; Javadoc.

#### Scenario: LegacyDomainMatrix

- **WHEN** legacy клиенты выполняют PTP и pub/sub в одном запуске и вызывают все forbidden операции из таблицы §9.2
- **THEN** операции своего домена успешны, чужие вызовы дают IllegalStateException
- Test: `JavaProviderTest.LegacyDomainMatrix`

### Requirement: Исключения и сторонние сообщения

Провайдер SHALL преобразовать ошибки в точные JMSException/JMSRuntimeException и подклассы по методу, сохраняя errorCode и cause/linked exception где предусмотрено. При создании consumer или browser неверный селектор даёт `InvalidSelectorException` в checked API и `InvalidSelectorRuntimeException` в simplified API. Совместимая сторонняя реализация Message отправляется через стандартный контракт.

ID: `REQ-74-6`
Источник: `repo:docs/jms-spec/61-java-client.md@9f7983c0054636a6686f9928831c8c53dfbd3679#interfaces-to-implement`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.12, 10, Javadoc 3.1.

#### Scenario: ExceptionsAndForeignMessages

- **WHEN** матрица создаёт ошибки selector, destination, security, format, closed, allocation и transaction, плюс сторонний Message
- **THEN** checked и simplified API возвращают объявленные классы, включая обе ошибки селектора. Причинные данные сохраняются без секретов, сторонний конверт читается
- Test: `JavaProviderTest.ExceptionsAndForeignMessages`

### Requirement: Приоритет настроек отправителя

Провайдер SHALL в classic и legacy API применять настройки конкретного send поверх дефолтов
producer для delivery mode, priority и TTL. В simplified API свойства и клиентские заголовки,
установленные на JMSProducer, имеют приоритет над соответствующими значениями входного Message.
Send с телом использует ту же конфигурацию. Проверяются unset, override и clearProperties.

ID: `REQ-74-7`
Источник: `repo:docs/jms-spec/02-jmscontext.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание — §§7.4–7.6 [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html).

#### Scenario: ProducerOverrides

- **WHEN** один и тот же Message и typed send используют conflicting defaults, per-send параметры и свойства с одинаковыми именами.
- **THEN** фактические значения сообщений соответствуют матрице источников и сброса, настройки другого producer не изменились.
- Test: `JavaProviderTest.ProducerOverrides`

### Test plan

Набор `JavaProviderTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Для Java и интеграционных сценариев сохраняется тот же
устойчивый ID `Suite.Case` в коде теста под `tests/` и в отчёте runner. Матрица сценария
приводит в отчёте каждый вход и исход отдельной записью, включая негативные ветви.

- `JavaProviderTest.ApiSurfaceAndJavaBaseline` — REQ-74-1.
- `JavaProviderTest.AdministeredObjectsAndNaming` — REQ-74-2.
- `JavaProviderTest.ClassicLifecycleAndMetadata` — REQ-74-3.
- `JavaProviderTest.SimplifiedContextMatrix` — REQ-74-4.
- `JavaProviderTest.LegacyDomainMatrix` — REQ-74-5.
- `JavaProviderTest.ExceptionsAndForeignMessages` — REQ-74-6.
- `JavaProviderTest.ProducerOverrides` — REQ-74-7.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Wire не различает обязательные эффекты API, Java baseline не соответствует 3.1 либо стандартная операция требует vendor binding в коде приложения. Нужен согласованный способ хранения настроек JNDI без raw credentials.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/74/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/74/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/74/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

Alexander B
