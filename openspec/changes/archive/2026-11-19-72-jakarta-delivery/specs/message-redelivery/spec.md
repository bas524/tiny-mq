# Message Redelivery — delta

## MODIFIED Requirements

### Requirement: Redelivery counter and flag on recover and rollback

Локальная C++ сессия SHALL выполнять следующие правила квитирования.
AUTO подтверждает успешный синхронный receive на границе передачи и listener после его
успешного возврата. CLIENT подтверждает все выданные неподтверждённые записи этой сессии;
жизненный цикл consumer не меняет принадлежность записи. DUPS_OK может группировать ack,
в том числе откладывать их до следующего receive или нормального закрытия; на сбое
неподтверждённая запись может вернуться. Recover в нетранзакционной сессии возвращает
только выданные неподтверждённые записи в соответствии с §6.2.10. Recover и rollback без
backoff сохраняют относительный порядок исходных доставок этой сессии среди записей,
которые пережили проверку TTL и остаются доступными. Правило охватывает разных consumer
одной сессии. Новая подходящая запись может попасть между ними по приоритету или наступившему
deliveryTime. Backoff сдвигает готовность только соответствующей записи.
Recover в транзакционной сессии даёт IllegalStateException.
Индивидуальное квитирование сохраняется как расширение C++.

Для будущего Java receiveBody Consumer::receiveChecked принимает предикат для Message
и тайм-аут с единицами и значениями recv. ReceiveBodyResult различает отсутствие записи,
совместимое тело с сообщением и несовместимое тело по ReceiveBodyDisposition до решения
о квитировании. Session::acknowledge квитирует выданные записи сессии в режиме CLIENT. При несовместимом теле AUTO/DUPS_OK сохраняют прежнюю позицию
в ready и прежнее значение счётчика, CLIENT/TRANSACTED учитывают неуспешную проверку
как полученную запись. Повтор для CLIENT появляется после recover, для TRANSACTED
после rollback. Java типизация и MessageFormatRuntimeException относятся к SDD 74.
JMSXDeliveryCount = счётчик повторов + 1 с насыщением на INT32_MAX и минимумом 2 для
JMSRedelivered=true. Внутренний счётчик повторов и старая сериализация сохраняют 0-based шкалу.
Экземпляры доставки в разных подписках ведут независимые значения.

ID: `REQ-72-5`
Источник: `repo:docs/jms-spec/23-recover.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.4.7, 3.5.11, 6.2.10–6.2.13, 8.6.

#### Scenario: AcknowledgeAndRecoverMatrix

- **WHEN** два consumer, fault injection между доставкой и ack, recover и несовместимый receiveBody проверяются в каждом режиме
- **THEN** JMSXDeliveryCount начинается с 1, redelivery и исключения точны, подтверждения другой
  сессии сохраняются, а recover через двух consumer сохраняет относительный порядок выживших
  записей и учитывает новую запись более высокого приоритета и истечение TTL
- Test: `DeliveryContractTest.AcknowledgeAndRecoverMatrix`

Условия persistence, явного backoff и close-повтора определяет REQ-72-7.
Публичный счётчик соответствует REQ-70-1; здесь задаются его переходы и насыщение.

#### Scenario: RollbackIncrementsDeliveryCount

- **WHEN** persistent вход получен в TRANSACTED и дважды возвращён rollback
- **THEN** внутренний 0-based deliveryCount равен 1, затем 2; redelivered=true
- Test: `DlqTest.RollbackIncrementsDeliveryCount`

#### Scenario: RecoverIncrementsDeliveryCount

- **WHEN** persistent вход получен в CLIENT и дважды возвращён recover
- **THEN** внутренний deliveryCount равен 1, затем 2
- Test: `DlqTest.RecoverIncrementsDeliveryCount`

#### Scenario: CountMonotonicAcrossRecoverAndRollbackPaths

- **WHEN** две persistent записи одной очереди проходят независимые recover и rollback
- **THEN** каждая попытка увеличивает счётчик своей записи ровно один раз
- Test: `DlqTest.CountMonotonicAcrossRecoverAndRollbackPaths`

#### Scenario: RedeliveryOrderPreservedWithoutBackoff

- **WHEN** три записи получены CLIENT-сессией, затем вызывается recover без backoff
- **THEN** выжившие доступные записи возвращаются в исходном относительном порядке
- Test: `DlqTest.RedeliveryOrderPreservedWithoutBackoff`

### Requirement: Default redelivery policy

Локальный C++ брокер SHALL выполнять следующий контракт восстановления и политики.
Для persistent сообщения обычной очереди и durable-подписки подтверждённая запись
не возвращается, а неподтверждённая после отказа возвращается с признаками редоставки,
если не истекла или не завершилась явным переходом политики. Non-persistent сообщения
могут быть потеряны при отказе, а подтверждённые не воспроизводятся. Это не меняет
квитирование между сессиями. По умолчанию повтор не ограничен, без backoff и DLQ.
Явная политика расширения tiny-mq сохраняет предел, абсолютный nextRedeliveryAt,
счётчик, идентичность DLQ и выбранный исход переноса для persistent доставки.
Порог maxRedeliveries=N разрешает N повторных доставок после первой. При превышении
переносится одна копия в настроенную DLQ или запись удаляется по явно установленному
пределу без DLQ. Политика назначения, применимая к уже запланированной попытке,
берётся из сохранённого результата планирования. Просроченная запись исключается
обычным TTL. Origin и DLQ фиксируются одним идемпотентным решением с ключом исходной
записи и идентичностью её подписки; копия не наследует счётчик другой подписки.
Начисление повторной доставки относится к одной попытке. При аварии вокруг передачи
сохранённый номер может включить уже зарегистрированную попытку, которую приложение
не успело увидеть. Раздельные JMSDeliveryTime и nextRedeliveryAt, переживающая сбой
метаинформация и отсутствие предела по умолчанию заменяют исторический контракт
message-redelivery и SDD 46. Эти правила заменяют поглощённые legacy-требования этой дельты.

RedeliveryPolicy{} имеет maxRedeliveries=-1, backoffMs=0, maxBackoffMs=60000,
deadLetterQueue=nullptr. Отсутствие DLQ означает drop только при явном конечном пределе.
При backoffMs>0 повтор доступен не раньше now + min(backoffMs * 2^(count-1),
maxBackoffMs); абсолютный срок вычисляется с насыщением. Это nextRedeliveryAt,
а не новый JMSDeliveryTime. Правило применяется также к rollback при закрытии сессии;
close не подавляет backoff. Без backoff повтор готов немедленно с учётом REQ-72-1.
Порядок выживших повторов без backoff определяет REQ-72-5.

DLQ-копия сохраняет тело, messageId, timestamp, expiration, priority, replyTo,
correlationId, type, пользовательские свойства, исходный JMSDeliveryTime и финальные
внутренний deliveryCount/redelivered. Добавляется строка JMSXDeadLetterReason со значением
"maxRedeliveries exceeded". Сбрасывается только внутренний nextRedeliveryAt.
Надёжность копии совпадает с исходной. Persistent DLQ доступна после рестарта и
consumer, созданному после переноса; исходная запись удаляется из своей очереди или
storage конкретной durable-подписки. Истечение TTL остаётся основанием исключения DLQ-копии.
Изменённые заголовки и кешированные байты согласованы по ADR-0008.

ID: `REQ-72-7`
Источник: `repo:openspec/specs/message-redelivery/spec.md@9f7983c0054636a6686f9928831c8c53dfbd3679#requirements`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 3.5.11, 6.2.9–6.2.11, 7.7, 9.1; расширение tiny-mq.

#### Scenario: CrashAndDeadLetterMatrix

- **WHEN** брокер принудительно останавливается до и после ack, при запланированной редоставке,
  до и после решения о переносе и в replay; проверяются очередь и durable fan-out, положительный
  лимит, отсутствие лимита, истечение TTL и старые/новые записи
- **THEN** не теряется persistent запись в пределах её гарантии, подтверждённая исчезает,
  сохранённый backoff применяется к той же копии, публичное время не меняется, DLQ получает
  ровно одну копию при завершённом переносе даже после повторного старта
- Test: `DeliveryContractTest.CrashAndDeadLetterMatrix`

#### Scenario: DefaultPolicyAllowsMoreThanSixRedeliveries

- **WHEN** назначению не задают явную политику и выполняют семь rollback
- **THEN** запись остаётся доступной с JMSXDeliveryCount=8; default не удаляет её после шести повторов
- Test: `DlqTest.DefaultPolicyAllowsMoreThanSixRedeliveries`

#### Scenario: RestartPreservesCounterAndBackoff

- **WHEN** persistent запись с maxRedeliveries=2/backoffMs=5000 дважды возвращена; Exchange пересоздан в том же каталоге
- **THEN** до сохранённого срока записи нет, в sentAt+15000 она имеет count=2, redelivered=true и прежний deliveryTime=sentAt
- Test: `DlqTest.RestartPreservesCounterAndBackoff`

#### Scenario: SessionCloseRequeuesWithBackoff

- **WHEN** TRANSACTED-сессия получает persistent вход с backoffMs=5000 и явно закрывается без commit
- **THEN** вход недоступен до срока, затем возвращается с внутренним count=1, flag=true и прежним deliveryTime
- Test: `DlqTest.SessionCloseRequeuesWithBackoff`

#### Scenario: DurableRetryFlagAndCountAfterCrash

- **WHEN** новый процесс открывает каталог после crash между durable claim и ack, затем подтверждает повтор и запускается снова
- **THEN** одновременно redelivered=true и JMSXDeliveryCount=2; после ack следующего появления нет
- Test: `DeliveryContractTest.CrashAndDeadLetterMatrix`

#### Scenario: LandsInDlqAfterMaxRedeliveries

- **WHEN** persistent TextMessage получен и трижды возвращён с maxRedeliveries=2 и DLQ
- **THEN** origin пуст; одна DLQ-копия сохраняет body/deliveryTime, имеет внутренний count=3, redelivered=true и причину превышения
- Test: `DlqTest.LandsInDlqAfterMaxRedeliveries`

#### Scenario: NullDlqDropsMessage

- **WHEN** явный maxRedeliveries=1 без DLQ превышается двумя rollback
- **THEN** origin пуст и после пересоздания Exchange; запись удалена
- Test: `DlqTest.NullDlqDropsMessage`

#### Scenario: UnlimitedRedeliveriesWhenNegative

- **WHEN** явный maxRedeliveries=-1 и десять rollback
- **THEN** вход доступен с внутренним count=10 без DLQ
- Test: `DlqTest.UnlimitedRedeliveriesWhenNegative`

#### Scenario: DeadLetteringOnSessionCloseRollback

- **WHEN** получен вход в TRANSACTED с maxRedeliveries=0 и настроенной DLQ; сессия закрывается без commit
- **THEN** вход перемещается в DLQ по политике close-rollback
- Test: `DlqTest.DeadLetteringOnSessionCloseRollback`

#### Scenario: DlqPreservesHeadersAndProperties

- **WHEN** сообщение с correlationId/replyTo/type/priority=7 и int/string свойствами перемещается в DLQ
- **THEN** перечисленные поля и свойства сохраняются
- Test: `DlqTest.DlqPreservesHeadersAndProperties`

#### Scenario: DlqCopySurvivesRestart

- **WHEN** persistent сообщение перенесено в DLQ, Exchange пересоздан
- **THEN** DLQ consumer получает копию, origin consumer не получает её
- Test: `DlqTest.DlqCopySurvivesRestart`

#### Scenario: DlqConsumerCreatedAfterDeadLettering

- **WHEN** DLQ consumer создаётся после превышения лимита
- **THEN** ему доступна сохранённая DLQ-копия
- Test: `DlqTest.DlqConsumerCreatedAfterDeadLettering`

#### Scenario: DurableSubscriberDeadLetterRemovesFromDurableStorage

- **WHEN** durable-подписка получает persistent сообщение с maxRedeliveries=0 и выполняет recover, затем Exchange пересоздан
- **THEN** origin данной подписки пуст, DLQ содержит копию
- Test: `DlqTest.DurableSubscriberDeadLetterRemovesFromDurableStorage`

#### Scenario: BackoffDelaysRedelivery

- **WHEN** настроен backoffMs=200, затем выполняются первый и второй rollback
- **THEN** повторы недоступны раньше 200 и 400 мс соответственно; публичный deliveryTime остаётся исходным
- Test: `DlqTest.BackoffDelaysRedelivery`

#### Scenario: BackoffCappedByMaxBackoff

- **WHEN** backoffMs=200, maxBackoffMs=300, выполняется третья попытка повтора
- **THEN** backoff ограничен 300 мс вместо 800 мс; измеренное ожидание не превышает 600 мс с допуском теста
- Test: `DlqTest.BackoffCappedByMaxBackoff`

#### Scenario: PersistentBackoffPreservesDeliveryTime

- **WHEN** persistent сообщение повторно получается с backoffMs=100
- **THEN** публичный deliveryTime сохранён, внутренний deliveryCount увеличен, redelivered=true
- Test: `DlqTest.PersistentBackoffPreservesDeliveryTime`

#### Scenario: TopicSubscriberDeadLetteredIndependently

- **WHEN** две CLIENT-подписки получают копии persistent публикации и первая превышает maxRedeliveries=1
- **THEN** одна DLQ-копия относится к первой подписке; вторая сохраняет собственный счётчик и доставку
- Test: `DlqTest.TopicSubscriberDeadLetteredIndependently`

#### Scenario: DlqCopyKeepsExpiration

- **WHEN** копия перенесена в DLQ с исходным TTL=2 с
- **THEN** expiration сохранён, копия доступна до его истечения
- Test: `DlqTest.DlqCopyKeepsExpiration`

## REMOVED Requirements

### Requirement: Redelivery limit routes to dead-letter

Поглощено REQ-72-7: явный предел, DLQ/drop и close.

### Requirement: Dead-letter copy preserves payload and headers

Поглощено REQ-72-7: сохранение полей и надёжности копии, неизменяемый JMSDeliveryTime.

### Requirement: Exponential backoff delays redelivery

Поглощено REQ-72-7: отдельный nextRedeliveryAt и backoff также при close.

### Requirement: Redelivery order preserved without backoff

Поглощено REQ-72-5: относительный порядок выживших доступных записей сессии.

### Requirement: Per-subscriber limit and DLQ for topics

Поглощено REQ-72-7: независимый экземпляр доставки и ключ поколения подписки.

### Requirement: Dead-letter copy keeps expiration

Поглощено REQ-72-7: исходный expiration и обычный TTL.

### Requirement: Counter and backoff not persisted across restart

Поглощено REQ-72-7: сохранённые counter, backoff и исход политики; прежний сброс отменён.
