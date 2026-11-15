# 70 Сообщения, свойства и селекторы

Класс K1. Предложение к обсуждению и критике; критерии ниже описывают будущую реализацию.

Version: 0.3
Review Date: 2026-11-15
Examples: `MessageContractTest.CrossLanguageEnvelope` в критериях приёмки.
Machine Readability: Markdown, ID требований, Scenario, Test и таблица Entity inventory.
Cross References: [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md), [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), [Javadoc 3.1](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/package-summary.html).

## Problem

Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`
Компонент: `tiny-mq`

[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) восстанавливает цель и определяет границы работы этой постановки. Закреплённый
репозиторный источник у каждого требования ведёт к редакции исходного плана или прежней
постановки. Применимое внешнее нормативное основание указано по разделу.

### Current state

Message.cpp, MessageProperty.cpp и Selector.cpp имеют собственные C++-реализации. Наборы MessageHeadersTest, ObjectMessageTest и SelectorTest покрывают только существующее представление и часть правил.

## Outcome

Java-клиент получает те же значения, типы и признаки отсутствия после сериализации, хранения
и доставки. Селектор совпадает по нормативной грамматике и таблице значений независимо от точки входа.

## Constraints

- Базовая редакция — Jakarta Messaging 3.1 Final Release и Javadoc этой редакции.
- Действующие ограничения [ADR-0001–0008](../../arch/README.md) применяются до принятия нового ADR.
  При использовании текущего persistent формата держать парными запись/чтение и представление заголовков.
- Уточнения совместимости Java SE, Jakarta EE и расширений проекта фиксируются в [карте покрытия](README.md).

### Dependencies

—

## Architecture

Конверт сообщения различает отсутствие значения, явный null, пустое тело и тип значения. Кодеки сохраняют семантику конверта между C++, хранилищем и Java. Компилятор селектора принимает стандартную грамматику, выдаёт неизменяемый предикат или типизированную ошибку; результат вычисления — TRUE, FALSE или UNKNOWN.

C++ реализует конверт, хранение и вычисление селектора. Изолированный Java-модуль реализует
адаптацию конверта и объекты шести видов `jakarta.jms.Message` для последующего использования в
[SDD 74](74-jakarta-java-se-provider.md). Java-модуль проверяется на Java SE с официальным API 3.1
и стандартной библиотекой, без подключения к брокеру. Локальный тест принимает типизированные
эталонные записи и проверяет C++ и Java независимыми проверками. Протокол, Java-клиент с сетью
и сквозной тест процессов поставляются по [SDD 73](73-jakarta-broker-transport.md), SDD 74 и
[SDD 77](77-jakarta-conformance-release.md).

Вызывающий код передаёт конверту рассчитанные значения TTL и delay ([REQ-72-1](72-jakarta-delivery.md#requirement-send-clock-expiration-и-приоритет))
и метаданные попытки доставки ([REQ-72-5](72-jakarta-delivery.md#requirement-квитирование-recover-и-receivebody)).
В SDD 70 эти входы задаёт fixture. Метод `acknowledge()` у полученного Java-объекта делегирует
подтверждение сессии через внедрённый callback, а поведение квитирования проверяется по SDD 72.
Чтение переданного провайдеру стороннего `Message` и назначение его заголовков через публичный API
проверяются в [REQ-74-6](74-jakarta-java-se-provider.md#requirement-исключения-и-сторонние-сообщения)
по §3.12 Jakarta Messaging.

### Persistence / wire implications

Представление v0x02 должно читаться после обновления. Для изменения записи или кодека нужны
версия, тесты старых данных и явно поддерживаемый путь отката. Сериализованное Java-тело
непрозрачно для брокера. Имеющийся C++ `className` сохраняется в read/replay и round trip
как необязательные метаданные тела для клиента. Он не выставляется как JMS-заголовок или
свойство и не влияет на стандартный селектор.

## Entity inventory

| ID | Kind | Name | Note |
|---|---|---|---|
| E1 | type | `MessageEnvelope` | SDD 70 |
| E2 | type | `MessageBody` | SDD 70 |
| E3 | type | `PropertyValue` | SDD 70 |
| E4 | type | `CompiledSelector` | SDD 70 |
| E5 | type | `SelectorTruth` | SDD 70 |
| E6 | header | `JMSDeliveryTime` | SDD 70 |
| E7 | property | `JMSXDeliveryCount` | Выставляется провайдером при доставке |
| E8 | test-suite | `MessageContractTest` | SDD 70 |
| E9 | property | `JMSXGroupID` | Свойство клиента |
| E10 | property | `JMSXGroupSeq` | Свойство клиента |
| E11 | type | `CorrelationId` | Строка, байты или отсутствие |
| E12 | header | `JMSRedelivered` | Выставляется провайдером при доставке |
| E13 | storage-field | `className` | Метаданные непрозрачного тела C++ |
| E14 | type | `JavaMessageAdapter` | Локальная адаптация конверта и вызов подтверждения |
| E15 | type | `TinyMessage` | Java `Message` |
| E16 | type | `TinyTextMessage` | Java `TextMessage` |
| E17 | type | `TinyBytesMessage` | Java `BytesMessage` |
| E18 | type | `TinyMapMessage` | Java `MapMessage` |
| E19 | type | `TinyStreamMessage` | Java `StreamMessage` |
| E20 | type | `TinyObjectMessage` | Java `ObjectMessage` |
| E21 | test-suite | `NfrGateTest` | Идентификатор отдельной проверки бюджета |

## Acceptance Criteria

### Requirement: Конверт пересекает границу языка

Конверт SHALL сохранить пользовательские `JMSCorrelationID`, `JMSReplyTo`, `JMSType` и
значения `JMSDestination`, `JMSDeliveryMode`, `JMSExpiration`, `JMSDeliveryTime`,
`JMSPriority`, `JMSMessageID`, `JMSTimestamp`, рассчитанные провайдером. Для send
переданные клиентом значения полей провайдера заменяются. `JMSMessageID` имеет префикс
`ID:` и уникален в пределах установки брокера. Сетевую проверку ID до и после рестарта
выполняет [REQ-74-3](74-jakarta-java-se-provider.md#requirement-классический-жизненный-цикл).
Обычный `JMSTimestamp` лежит в пределах
вызова send. tiny-mq принимает каждую подсказку отключения независимо: с
`setDisableMessageID(true)` ID равен `null`, с `setDisableMessageTimestamp(true)`
timestamp равен 0. Входы времени и значения для теста задаются явно. Заголовок
`JMSRedelivered` и свойство `JMSXDeliveryCount` переносятся из метаданных доставки.
Первая доставка имеет счётчик 1 и `JMSRedelivered=false`. При `JMSRedelivered=true`
счётчик как минимум 2. Значения клиентского сообщения не определяют метаданные доставки.

ID: `REQ-70-1`
Источник: `repo:docs/jms-spec/10-message-headers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.1–3.5, 3.12.

#### Scenario: CrossLanguageEnvelope

- **WHEN** эталонные векторы задают типы конверта, значения клиента и провайдера, четыре комбинации
  подсказок, первый и повторный приём. C++ сериализует и читает запись хранилища. Локальный
  Java-адаптер читает и записывает тот же типизированный контракт через независимый fixture
- **THEN** обе стороны по отдельности совпадают с ожидаемыми значениями и типами, включая null,
  границы timestamp, уникальность ID при двух обычных отправках и пары flag/count `false/1`, `true/2`.
  Совместимость настоящего wire и обмен между отдельными процессами проверяет
  `ReleaseConformanceTest.JavaAndCppRoundTrip` по SDD 77
- Test: `MessageContractTest.CrossLanguageEnvelope`

### Requirement: Тела и снимок отправки

Конверт SHALL поддержать Message без тела, TextMessage, BytesMessage, MapMessage,
StreamMessage и ObjectMessage. `ObjectMessage.setObject()` и инициализация с `Serializable`
снимают копию на вызове. Изменение объекта до вызова send не меняет уже заданное тело.
При ошибке сериализации Java возвращает `MessageFormatException`. Мутация исходного поля или
массива после завершения отправки не меняет переданный экземпляр. Setter копирует переданный
объект или массив, а запись null в `BytesMessage` даёт `NullPointerException` по §3.12.
Асинхронный доступ и момент завершения callback проверяются по REQ-72-4.

ID: `REQ-70-2`
Источник: `repo:docs/jms-spec/11-object-message.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.10–3.12.

#### Scenario: BodyRoundTrip

- **WHEN** fixture проходит шесть форм тела и допустимые null/пустые значения. Для ObjectMessage
  приложение присваивает изменяемый сериализуемый объект, меняет его до передачи конверта,
  затем отдельно меняет тело после передачи. Проверяются мутация переданного byte[],
  запись null в BytesMessage, ошибка сериализации и C++ round trip байтов с присутствующим и
  отсутствующим `className`. Fixture повторно создаёт доставленный объект из сохранённого
  конверта после того, как клиент изменил тело и свойства первой копии
- **THEN** для каждого тела сохраняются исходные вид и значения. Десериализованное тело
  ObjectMessage равно значению при `setObject()`. На некорректный сериализуемый объект возвращается
  `MessageFormatException`, для null в BytesMessage — `NullPointerException`. Брокер сохраняет
  непрозрачные байты и метаданные `className`. При повторном создании сохранены исходные тело и
  свойства, а признаки доставки поступают от провайдера по REQ-70-1
- Test: `MessageContractTest.BodyRoundTrip`

### Requirement: Режим тела и преобразования

Java-объекты SHALL выполнять правила read-only, `reset`, `clearBody` и таблицы преобразований
§3.11 и Javadoc. После `MessageFormatException` или `NumberFormatException` при чтении из
StreamMessage или BytesMessage позиция курсора сохраняется. `getBody()` применяет правила
относимости к классу, включая `null` для пустого тела, и сбрасывает BytesMessage до и после чтения.
Для MapMessage допустимы типы §3.11.3 и `byte[]` с копированием ввода. Вложенный изменяемый
объект, отличный от `byte[]`, отклоняется с `MessageFormatException` по Javadoc `MapMessage.setObject()`.
Для StreamMessage `getBody()` выдаёт `MessageFormatException`, а `isBodyAssignableTo()` возвращает
`false`. При ошибке десериализации ObjectMessage `getBody()` и `getObject()` выдают
`MessageFormatException`, а `isBodyAssignableTo()` возвращает `false`.

ID: `REQ-70-3`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`

Основание поведения — [Jakarta Messaging 3.1, §3.11](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#jakarta-messaging-message-body),
Javadoc Message, BytesMessage, [MapMessage](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/mapmessage) и StreamMessage.

#### Scenario: BodyAccessMatrix

- **WHEN** матрица проверяет разрешённые и запрещённые преобразования, EOF, read-only, reset,
  clearBody, несовместимый класс, пустое тело, чтение StreamMessage и ошибку десериализации.
  Для потока после ошибки выполняется повторное чтение допустимым методом. Для MapMessage
  матрица меняет переданный `byte[]` и пытается передать вложенный изменяемый объект
- **THEN** значения и классы исключений соответствуют контрактам, повторное чтение возвращает
  исходный элемент, clearBody сохраняет свойства и заголовки, а getBody/isBodyAssignableTo
  выдают перечисленные исходы. MapMessage сохраняет исходные байты и отвергает вложенный объект
- Test: `MessageContractTest.BodyAccessMatrix`

### Requirement: Типизированные свойства

Java-объекты SHALL принимать имена свойств по грамматике идентификаторов селектора с учётом
зарезервированных слов и пространств JMSX/JMS_. Java setter бросает `IllegalArgumentException`
для null, пустого или недопустимого имени. Для последней группы это явная политика tiny-mq.
Полученные свойства read-only до `clearProperties()`, попытка записи выдаёт
`MessageNotWriteableException`. Проверяются допустимые типы, преобразования, перечисление,
`propertyExists` и `clearProperties`. `JMSXDeliveryCount` — int на доставленном сообщении,
связь с повторной доставкой задаёт REQ-70-1. Список поддерживаемых JMSX в метаданных
соединения входит в [REQ-74-3](74-jakarta-java-se-provider.md#requirement-классический-жизненный-цикл).

ID: `REQ-70-4`
Источник: `repo:docs/jms-spec/10-message-headers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.5.1–3.5.11.

#### Scenario: PropertyAccessMatrix

- **WHEN** матрица пробует допустимые Java-идентификаторы, запрещённые слова, имя с дефисом,
  null, пустое имя, отсутствующее и null-значение, все типы и запрещённую конверсию.
  Затем она пытается менять полученные свойства до и после clearProperties
- **THEN** допустимое имя доступно селектору с учётом регистра, ошибки имён имеют тип
  `IllegalArgumentException`. Для отсутствующего и null-значения фиксируются исходы getter,
  propertyExists и перечисления, для каждого типа — результаты и ошибки преобразования.
  После clearProperties старые свойства исчезают, запись доступна, тело и заголовки сохранены
- Test: `MessageContractTest.PropertyAccessMatrix`

### Requirement: Компилятор и трёхзначное вычисление

Компилятор и вычислитель SHALL поддержать §3.8.1.1–3.8.1.3, включая приоритеты, типизированные
сравнения, арифметику, AND/OR/NOT, BETWEEN, IN, LIKE с ESCAPE, IS NULL и литералы. Селектор
видит только свойства и шесть разрешённых заголовков: JMSDeliveryMode, JMSPriority, JMSMessageID,
JMSTimestamp, JMSCorrelationID и JMSType. `JMSDeliveryMode` имеет вид строки `'PERSISTENT'`
или `'NON_PERSISTENT'`. Строковое свойство не преобразуется в число для сравнения.
Для exact/exact сохраняется точность Java long, при смешении exact и approximate применяется
числовое продвижение Java. Пустое или null-выражение соответствует отсутствию селектора,
совпадение даёт только TRUE. C++ `Selector::parse` использует существующий
`tiny_mq::InvalidSelectorException`. Создание consumer/browser подключается в SDD 71 и 74.
Преобразование исключения C++ в `jakarta.jms.InvalidSelectorException` для checked API
и `jakarta.jms.InvalidSelectorRuntimeException` для simplified API проверяет
[REQ-74-6](74-jakarta-java-se-provider.md#requirement-исключения-и-сторонние-сообщения).

ID: `REQ-70-5`
Источник: `repo:docs/jms-spec/30-selector-audit.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.8.

#### Scenario: SelectorGrammarMatrix

- **WHEN** матрица проверяет каждую продукцию, пустое выражение, отсутствующее свойство,
  строки delivery mode обоих режимов, `p > 1` для строкового `p="2"`, exact/exact
  на 2^53+1 и смешанное сравнение с double. Отдельно проверяется синтаксическая ошибка компиляции
- **THEN** выбор определяется типом свойства и SQL-логикой, `p > 1` в строковом случае
  даёт FALSE, оба строковых режима совпадают со своими строками, точные целые не округляются.
  Локальная проверка синтаксической ошибки наблюдает `tiny_mq::InvalidSelectorException`
- Test: `MessageContractTest.SelectorGrammarMatrix`

### Requirement: Совместимость горячего пути

Изменённый горячий путь SHALL укладываться в порог 5% CPU time/op относительно main
в RelWithDebInfo. Сравниваются медианы девяти повторов при одинаковых машине, сборке и входах,
сериями ABBA. Минимальный фильтр — `BenchmarkFixture/AutoAck_(NonPersistent|Persistent)_RoundTrip$`.
Для изменённого горячего пути селектора или сериализации, отсутствующего в этом фильтре,
добавляется профильный бенчмарк и снимается сопоставимая база. Сценарий выполняется НФТ-шлюзом.

ID: `REQ-70-6`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#сквозные-требования-на-всех-майлстонах`

Основание — [ограничения INT-01](../../requirements/intents/INT-01-jakarta-messaging.md#ограничения)
и [базовая методика проекта](../../benchmarks/baseline.md).

#### Scenario: HotPathBudget

- **WHEN** шлюз NFR запускает базовую и рабочую сборки со штатным фильтром и применимым
  профильным бенчмарком, сохраняет исходные результаты, команды и версии сборок
- **THEN** для каждого случая видны девять повторов и медиана CPU time/op.
  Регрессия более 5% останавливает допуск по НФТ
- Test: `NfrGateTest.HotPathBudget`

### Requirement: Свойства группы JMSX

Конверт SHALL принимать JMSXGroupID типа String и JMSXGroupSeq типа int от клиента и сохранять
их значения, типы и отсутствие при локальном обмене. Объявление этих имён в `ConnectionMetaData`
проверяет [REQ-74-3](74-jakarta-java-se-provider.md#requirement-классический-жизненный-цикл).
Дополнительная политика порядка групп может предоставляться как явно описанное расширение.

ID: `REQ-70-7`
Источник: `repo:docs/jms-spec/10-message-headers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание — §3.5.9 [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html).

#### Scenario: GroupProperties

- **WHEN** локальный fixture передаёт записи с обоими свойствами группы и без них
  через C++ конверт и Java-адаптер
- **THEN** оба runner наблюдают ожидаемые значения String/int, а отсутствие остаётся различимым
- Test: `MessageContractTest.GroupProperties`

### Requirement: Двоичный correlation ID

Профиль tiny-mq SHALL предоставлять поддержку provider-native byte[] correlation ID. Конверт
различает двоичный вариант, строковый вариант и отсутствие значения. Сеттер и геттер двоичного
значения защищают сохранённые байты от последующей мутации массива приложения. У пустого byte[]
сохраняется двоичный вид. При отсутствии ID оба Java getter возвращают null. Для установленного
ID обращение к getter другого представления даёт `MessageFormatException`. В селекторе
двоичный ID имеет NULL как выбранное проектное отображение, строковый остаётся строкой.
Отображение несовпадающего getter и двоичного ID в селекторе — политика tiny-mq. Использование
байтового ID помечено как непереносимое.

ID: `REQ-70-8`
Источник: `repo:docs/jms-spec/10-message-headers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание — §3.4.5 [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html).

#### Scenario: BinaryCorrelationId

- **WHEN** fixture проверяет строку, отсутствие, пустой массив и массив с нулевым и не-UTF-8
  байтом, меняет переданный и возвращённый массивы, вызывает обе формы getter и селектор
- **THEN** вариант и значения переживают C++ storage и локальную Java-адаптацию. Мутации
  не меняют сохранённые значения, чужой getter даёт `MessageFormatException`, двоичный ID
  соответствует `JMSCorrelationID IS NULL`. Реальный wire по этому контракту проверяется в SDD 73
- Test: `MessageContractTest.BinaryCorrelationId`

### Requirement: Совместимость записей v0x02

C++ хранилище SHALL читать записи v0x02 исходной редакции с исходными значениями заголовков,
типов и тел. Новое представление получает отдельную версию, если его нельзя записать в
v0x02 без потери значений. Совместимость отката проверяется на восстановленной копии.

ID: `REQ-70-9`
Источник: `repo:docs/jms-spec/10-message-headers.md@9f7983c0054636a6686f9928831c8c53dfbd3679#persistence-wire-implications`

#### Scenario: StorageV02Compatibility

- **WHEN** сборка читает эталонные записи v0x02, созданные исходной редакцией, затем сохраняет и
  перечитывает новые записи с отличающимися состояниями null, типа и двоичного ID
- **THEN** старые поля совпадают с исходным декодированием, новые записи сохраняют различимые
  состояния, а несовместимый старый читатель не получает файл нового формата при откате
- Test: `MessageContractTest.StorageV02Compatibility`

### Test plan

Набор `MessageContractTest` в `tests/`. Каждый тест в коде и логе указывает свой ID требования.
C++ сценарии выполняются GoogleTest. Локальный Java runner вызывает модуль без сетевых
компонентов. Если один критерий проверяется обоими runner, в отчёте отражаются оба
независимых результата. Эталонные ожидаемые значения задаются в fixture независимо от
кодеков реализации. `NfrGateTest.HotPathBudget` — результат отдельного прогона NFR-шлюза по
benchmark-фильтру, а не GoogleTest-проверка производительности.

- `MessageContractTest.CrossLanguageEnvelope` — REQ-70-1.
- `MessageContractTest.BodyRoundTrip` — REQ-70-2.
- `MessageContractTest.BodyAccessMatrix` — REQ-70-3.
- `MessageContractTest.PropertyAccessMatrix` — REQ-70-4.
- `MessageContractTest.SelectorGrammarMatrix` — REQ-70-5.
- `NfrGateTest.HotPathBudget` — REQ-70-6.
- `MessageContractTest.GroupProperties` — REQ-70-7.
- `MessageContractTest.BinaryCorrelationId` — REQ-70-8.
- `MessageContractTest.StorageV02Compatibility` — REQ-70-9.

## Rollback

До выпуска откатить код и зависимые изменения по цепочке INT-01 в порядке, обратном зависимостям.
Если использовался изменённый persistent формат или новая версия wire, перед откатом проверить
чтение старой версией на восстановленной копии и применить описанную для неё миграцию или restore.
Незавершённые принятые операции не выбрасываются ради запуска старой сборки.

## Autonomy Level

R2 в изолированной рабочей копии. Коммит, push, изменение сборочной среды и утверждение выпуска
выносятся на рубеж владельца согласно [PROJECT.md](../../PROJECT.md).

## Stop-conditions

- Нельзя сохранить тип или null между текущей записью и новым конвертом, или требуется расширить стандартную грамматику в переносимом Java API.
- Остановиться при необходимости поменять согласованный публичный контракт, расширить реестр сущностей
  или отказаться от исполняемого критерия. Для новых или изменённых горячих путей действуют критерии НФТ.
- До передачи исполнителю получить критический разбор актуальной постановки и решения по зависимым вопросам.

## Evidence

- `handoffs/current/70/logs/test-plan.log` — журнал перечисленных критериев с командами и средой.
- `handoffs/current/70/logs/trace-tests.log` — независимая сверка тестов и требований.
- `handoffs/current/70/logs/verification.log` — результаты применимых сборок, проверок формата и шлюзов;
  для замеров приложить исходные значения и команду запуска.

## Owner

- Alexander B
