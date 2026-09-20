# Redelivery counter & Dead Letter Queue

> Спека — это **SDD** (AEF Standard 2), а не план работ: она описывает *что и почему*,
> а не *как*. И это **замкнутая модель** (Standard 5): всё, что фича вводит, перечислено
> поимённо в `Entity inventory`. Формулировки открытого множества запрещены.
>
> Класс документа: **K1 — Executable** (Standard 6) — читает агент во время работы.

## Owner

- Alexander B

## JMS reference
- JMS 2.0 § 3.5.9 `JMSXDeliveryCount` (JMS-defined property; provider sets it).
- JMS 2.0 § 3.4.7 `JMSRedelivered`, § 3.4.10 `JMSDeliveryTime`.
- JMS 2.0 § 4.4 (transacted session: rollback → redelivery), § 8.4.8 (`Session.recover()`).
- Dead letter queue is provider-specific; JMS mandates only the counter and the flag.
  Behavioural model mirrors ActiveMQ's `RedeliveryPolicy` (maximumRedeliveries,
  initialRedeliveryDelay, exponential backoff) without its broker-wide defaults.

## Current state in tiny-mq
- `Message::Headers::deliveryCount` (`Message.h:186`) и `redelivered` (`:187`) есть с
  спеки 23. Оба сериализуются в формат `0x02` (`Message.cpp:153`, `:190`).
- **Семантика счётчика — число повторных доставок**: при первой доставке `deliveryCount == 0`,
  после первого `recover()` — `1` (`tests/RecoverTest.cpp:62`, `:82`). Это решение спеки 23 и
  здесь **наследуется**: JMS-ное «1 при первой доставке» не вводится.
- `Consumer::recover()` (`Consumer.cpp:297`) инкрементирует `deliveryCount`, ставит
  `redelivered`, обновляет кэш байт (`refreshCachedStorageBytes`, ADR-0008) и кладёт
  сообщение обратно в `_queue` без задержки.
- `Consumer::rollback()` (`Consumer.cpp:275`, SESSION_TRANSACTED) переливает `_transactQueue`
  в `_queue` **без** инкремента счётчика и без флага `redelivered` — пробел, который эта
  спека закрывает.
- Ни `recover()`, ни `rollback()` не пишут счётчик на диск: после рестарта брокера
  реплей даёт `deliveryCount == 0` (замерено ревью спеки 23).
- Нет ни лимита повторов, ни DLQ: отравленное сообщение крутится бесконечно.
- `Destination` не имеет публичных мутаторов (`Destination.h:53-63`); все операции —
  через `friend` (`Session`, `Consumer`, `Producer`, `Exchange`).
- У каждой `Destination` есть `_defaultConsumer`, который держит `_queue` и `_storage`, поэтому
  очередь без прикладных консьюмеров всё равно принимает и хранит сообщения.
- `ConnectionMetaData::jmsxPropertyNames` пуст с пометкой «до спеки 24»
  (`ConnectionMetaData.h:29-31`).
- Механизм отложенной видимости есть: `Destination::enqueueOrSchedule` +
  `DeliveryScheduler` (спека 13, ADR-0007).

## Proposed API

```cpp
// RedeliveryPolicy.h
namespace tiny_mq {
struct RedeliveryPolicy {
  int32_t          maxRedeliveries = 6;      // повторов сверх первой доставки; < 0 = без лимита
  int64_t          backoffMs       = 0;      // задержка перед 1-й повторной доставкой; 0 = сразу
  int64_t          maxBackoffMs    = 60000;  // потолок экспоненты
  Destination::Ptr deadLetterQueue;          // nullptr = удалить сообщение (с warning в лог)
};
}
```

```cpp
// Destination.h — публичные (единственные публичные мутаторы Destination)
void            setRedeliveryPolicy(RedeliveryPolicy policy);   // только queue family
RedeliveryPolicy redeliveryPolicy() const;
```

Контракты модулей (Standard 9):

- **`Destination::setRedeliveryPolicy`.** Предусловие: `isQueueFamily()`; иначе бросает
  `Poco::InvalidAccessException` и политику не меняет. `policy.deadLetterQueue`, если задан,
  обязан быть queue family и не совпадать с `this`; иначе `Poco::InvalidArgumentException`.
  Постусловие: следующая повторная доставка по любому консьюмеру этой destination использует
  новую политику. Потокобезопасность — как у `Destination` в целом: вызов до старта
  консьюмеров или из потока с affinity к destination (ADR-0005); конкурентная смена
  политики во время `recover()` не поддерживается и не тестируется.
- **`Consumer::redeliver(Message::Ptr)`** (private, общий для `recover()` и `rollback()`).
  Предусловие: сообщение получено этим консьюмером и не подтверждено. Постусловие — ровно
  одно из двух: (а) сообщение снова в `_queue` (у queue family очередь принадлежит
  `Destination` и разделяется всеми её консьюмерами, поэтому переживает закрытие сессии) —
  сразу или через `DeliveryScheduler`, с `redelivered == true`, `deliveryCount` на 1 больше;
  на пути `~Consumer()` — только сразу, без scheduler; (б) сообщение
  удалено из origin (для persistent — запись storage удалена, как при ack) и передано
  в `Destination::deadLetter`. Инвариант: `deliveryCount` монотонно растёт для данного
  `uuid` в пределах жизни процесса.
- **`Destination::deadLetter(Message::Ptr, std::string reason)`** (private).
  Постусловие: при `deadLetterQueue != nullptr` в DLQ появляется **копия** сообщения с
  сохранёнными телом и заголовками, `JMSXDeadLetterReason = reason`, `deliveryTime = 0`;
  при persistent origin-сообщении копия записана в storage DLQ (переживает рестарт).
  При `deadLetterQueue == nullptr` сообщение удалено, в лог — `warning` с `uuid`, origin URI
  и `deliveryCount`. Не бросает (вызывается в том числе из `rollback()` на пути
  `~Consumer()`, ADR-0006): ошибка записи в DLQ логируется как `error`, сообщение теряется.

## Semantics

1. **Счётчик.** Каждая повторная доставка — из `Session::recover()` (не-транзакционные
   режимы) **или** `Session::rollback()` (SESSION_TRANSACTED) — инкрементирует
   `jmsHeaders.deliveryCount` и ставит `jmsHeaders.redelivered = true`. Первая доставка
   счётчик не трогает (наследие спеки 23). `rollback()` из `~Consumer()` (закрытие сессии
   без commit) считается повторной доставкой: счётчик и флаг ставятся, лимит проверяется
   (→ DLQ по Semantics 2), **но backoff не применяется** — сообщение возвращается в очередь
   destination немедленно. Так ведёт себя ActiveMQ Classic: redelivery delay — состояние
   консьюмера, при его закрытии неподтверждённые сообщения возвращаются брокеру и
   передиспатчиваются другому консьюмеру сразу, с `JMSRedelivered` и инкрементом счётчика.
2. **Лимит.** Если после инкремента `deliveryCount > maxRedeliveries` (при
   `maxRedeliveries >= 0`), сообщение **не** возвращается в очередь: вызывается
   `Destination::deadLetter` с reason `"maxRedeliveries exceeded"`. При
   `maxRedeliveries < 0` лимита нет (текущее поведение).
3. **DLQ.** Копия в DLQ сохраняет тело, `messageId`, `timestamp`, `expiration`, `priority`,
   `replyTo`, `correlationId`, `type`, все пользовательские свойства и **итоговый**
   `deliveryCount`/`redelivered` (то есть значение, превысившее лимит). Добавляется строковое
   свойство `JMSXDeadLetterReason`. `deliveryTime` сбрасывается в 0 — в DLQ сообщение видно
   сразу. Копия persistent тогда и только тогда, когда persistent оригинал. Origin-запись
   в storage удаляется (как при ack). Сообщение в DLQ доступно консьюмеру DLQ, созданному
   как до, так и после dead-lettering, и — для persistent — после рестарта брокера.
   `deadLetterQueue == nullptr` → сообщение удаляется с `warning` в лог.
4. **Backoff.** При `backoffMs > 0` повторная доставка с `deliveryCount == n` (n ≥ 1)
   становится видимой не раньше `now + min(backoffMs · 2^(n−1), maxBackoffMs)`.
   Реализуется через `jmsHeaders.deliveryTime = now + delay` и `Destination::enqueueOrSchedule`
   (спека 13); кэш байт обновляется (ADR-0008). Прикладной код видит обновлённый
   `JMSDeliveryTime` — это момент, с которого сообщение снова eligible for delivery
   (JMS 2.0 § 3.4.10). При `backoffMs == 0` — немедленная повторная доставка (текущее
   поведение `recover()`). Backoff действует только для явных `recover()`/`rollback()`;
   на пути `~Consumer()` он не применяется (Semantics 1).
5. **Порядок.** Среди сообщений, повторно доставляемых одним вызовом `recover()`/`rollback()`
   с `backoffMs == 0`, сохраняется исходный порядок получения (наследие спеки 23). При
   `backoffMs > 0` порядок задаёт `DeliveryScheduler` по `deliveryTime`; равные
   `deliveryTime` — порядок постановки в scheduler.
6. **Область действия.** Политика — свойство queue-family destination. Для topic family
   `setRedeliveryPolicy` бросает; поведение топиков не меняется (бесконечная повторная
   доставка в очередь подписчика). Причина: dead-lettering копии одного подписчика при
   живой durable-записи у другого требует отдельной модели — вынесено в Open questions.
7. **Взаимодействие с истечением срока (спека 44).** Если на момент повторной доставки
   сообщение уже истекло, действует правило спеки 44 (drop на recv-пути / sweeper);
   dead-lettering по лимиту не подменяет истечение и не отменяется им: истёкшее сообщение,
   превысившее лимит, идёт в DLQ с сохранённым `expiration` и там подчиняется спеке 44.
8. **Умолчание.** Destination без вызова `setRedeliveryPolicy` имеет
   `RedeliveryPolicy{}`: 6 повторов, без backoff, без DLQ → 7-я повторная доставка удаляет
   сообщение с `warning`. Это **изменение текущего поведения** (было: бесконечно); выбрано
   потому, что бесконечный цикл отравленного сообщения хуже потери с записью в лог. Прежнее
   поведение доступно явно: `setRedeliveryPolicy({.maxRedeliveries = -1})` (Semantics 2).
   Сверка с ActiveMQ: Classic — лимит 6, затем `ActiveMQ.DLQ`; Artemis — лимит 10, без
   настроенного DLA сообщение отбрасывается. Бесконечно не крутит ни один.
9. **Не переживает рестарт.** `deliveryCount` по-прежнему не пишется на диск при повторной
   доставке: после рестарта реплей даёт `0`, лимит отсчитывается заново. Это осознанное
   ограничение (стоимость записи на горячем пути), см. Open questions.
10. **`ConnectionMetaData::jmsxPropertyNames`** после этой спеки содержит
    `{"JMSXDeliveryCount", "JMSXDeadLetterReason"}`.

## Entity inventory
<!-- Standard 5: замкнутый машиночитаемый реестр. Сущность в коде вне реестра — дрейф,
     блокер до MR. Реестр правит оркестратор через спеку, не producer и не reviewer. -->

| ID | Kind | Name | Note |
|----|------|------|------|
| `E1` | type | `tiny_mq::RedeliveryPolicy` | struct, файл `RedeliveryPolicy.h` |
| `E2` | config | `RedeliveryPolicy::maxRedeliveries` | `int32_t`, default 6, `< 0` = без лимита |
| `E3` | config | `RedeliveryPolicy::backoffMs` | `int64_t`, default 0 |
| `E4` | config | `RedeliveryPolicy::maxBackoffMs` | `int64_t`, default 60000 |
| `E5` | config | `RedeliveryPolicy::deadLetterQueue` | `Destination::Ptr`, default nullptr |
| `E6` | method | `Destination::setRedeliveryPolicy(RedeliveryPolicy)` | public |
| `E7` | method | `Destination::redeliveryPolicy() const` | public |
| `E8` | method | `Destination::deadLetter(Message::Ptr, std::string)` | private, noexcept-контракт |
| `E9` | method | `Consumer::redeliver(Message::Ptr)` | private; общий путь `recover()`/`rollback()` |
| `E10` | header | `JMSXDeadLetterReason` | строковое свойство на копии в DLQ |
| `E11` | header | `JMSXDeliveryCount` | имя в `ConnectionMetaData::jmsxPropertyNames`; поле уже есть |
| `E12` | test-suite | `DlqTest` | `tests/DlqTest.{h,cpp}` |
| `E13` | type | `Destination::_redeliveryPolicy` | private поле-хранилище политики |

Kind ∈ `type` · `method` · `header` · `storage-field` · `config` · `test-suite`.
Полей формата `0x02` спека **не добавляет** (`deliveryCount` уже в формате).

## Persistence / wire implications
- DLQ — обычная `Destination` queue family; новых типов storage нет.
- Формат `0x02` не меняется. `deliveryCount` в записи origin **не обновляется** при повторной
  доставке (см. Semantics 9); в записи DLQ-копии сохраняется итоговое значение.
- Dead-lettering persistent сообщения = удаление записи из storage origin + append в storage
  DLQ. Между двумя операциями процесс может упасть. Инвариант — **at-least-once**: допускается
  дублирование (сообщение после рестарта и в origin, и в DLQ), но не потеря. Отсюда порядок:
  сначала append в DLQ, затем удаление из origin. Дубль в origin после рестарта несёт
  `deliveryCount` из записи (см. Semantics 9) и пройдёт цикл заново.
  **Механизм durable-порядка** между двумя storage с независимыми worker-потоками:
  append в storage DLQ выполняется **синхронным** `ConcurrentLinearStorage::append`
  (существующий API: возвращает `Record`, ждёт исполнения worker'ом), и только после его
  возврата сабмитится удаление из origin (`remove`/`removeAsync`). Блокировка на пути
  dead-lettering допустима: это не горячий путь. Новых операций storage не требуется.
- Сетевых кадров нет (M1, in-process).

## Dependencies
- 10 (headers), 13 (delivery delay — backoff), 23 (recover — `deliveryCount`/`redelivered`),
  44 (expiration — взаимодействие в Semantics 7).

## Stop-conditions
<!-- Standard 2, обязательное поле. Producer, столкнувшись с любым из них, пишет
     status=paused и вопрос Owner'у. Молчаливого умолчания не существует. -->

- Для dead-lettering persistent сообщения не удаётся выполнить append в storage DLQ без
  объекта `Producer` (путь `Consumer::push` требует `const Producer&`) и требуется менять
  сигнатуру `push`/`preparePush` или формат `0x02` — остановиться: это касается контракта,
  унаследованного из спеки 10.
- Реализация backoff требует трогать `DeliveryScheduler` иначе, чем через
  `Destination::enqueueOrSchedule` (например, менять его публичный интерфейс) — остановиться:
  ADR-0007.
- `rollback()` на пути `~Consumer()` не может выполнить dead-lettering без риска исключения,
  которое нельзя проглотить корректно (ADR-0006), — остановиться, не «отключать»
  dead-lettering в деструкторе молча.
- Существующий тест (`RecoverTest`, `TransactionTest`, `PersistentTransactionTest`) ломается
  от умолчания Semantics 8 (7 повторов) — остановиться и показать, какой: менять умолчание
  или тест решает Owner.
- Перф-гейт показывает регрессию > 5% на `Transacted_*` или `ClientAck_*` бенчах —
  остановиться с числами; компромисс (например, вынос проверки политики за горячий путь)
  выбирает Owner.

Общие, действующие для любой спеки:
- требуется отклонение от унаследованных ограничений (формат `0x02`, threading-модель
  ADR-0005, объектная модель, durable-ключ) — это ADR, а не решение producer'а;
- нужна сущность, отсутствующая в `Entity inventory`;
- нужен файл вне объявленного `target_files`;
- пункт `Test plan` невозможно выразить как исполняемый GTest-критерий.

## Test plan
Сьют `DlqTest` (`tests/DlqTest.{h,cpp}` по образцу `RecoverTest`; каждый кейс — своя
queue через `CurrentTestName`, DLQ — `CurrentTestName + ".DLQ"`).

1. `RollbackIncrementsDeliveryCount` — SESSION_TRANSACTED, persistent: recv → rollback → recv:
   `deliveryCount == 1`, `redelivered == true`; второй rollback → `2`. (Semantics 1; закрывает
   пробел `rollback()`.)
2. `RecoverIncrementsDeliveryCount` — CLIENT_ACKNOWLEDGE, persistent: recv → recover → recv:
   `deliveryCount == 1`; повтор → `2`. (Semantics 1, регресс спеки 23.)
3. `CountMonotonicAcrossRecoverAndRollbackPaths` — одна persistent-очередь, два сообщения:
   одно гоняется через recover в CLIENT_ACK-сессии, другое через rollback в транзакционной;
   у каждого счётчик растёт строго на 1 за цикл, независимо друг от друга. (Semantics 1.)
4. `LandsInDlqAfterMaxRedeliveries` — `maxRedeliveries = 2`, `backoffMs = 0`, DLQ задан,
   persistent TextMessage: три rollback подряд → origin пуст (`recv(timeout)` = nullptr), в DLQ
   одно сообщение: тот же `messageId`, тело, `JMSXDeadLetterReason == "maxRedeliveries exceeded"`,
   `deliveryCount == 3`, `redelivered == true`, `deliveryTime == 0`. (Semantics 2, 3.)
5. `DlqPreservesHeadersAndProperties` — как 4, но с `correlationId`, `replyTo`, `type`,
   `priority = 7`, пользовательскими свойствами int/string: все сохранены на копии.
   (Semantics 3.)
6. `DlqCopySurvivesRestart` — как 4, persistent; после dead-lettering `_exchange.reset()` и
   пересоздание: консьюмер DLQ получает сообщение, консьюмер origin — нет. (Semantics 3,
   Persistence; **персистентный случай обязателен**, ADR-0008.)
7. `DlqConsumerCreatedAfterDeadLettering` — как 4, консьюмер на DLQ создаётся после
   переполнения лимита: получает сообщение. (Semantics 3, `_defaultConsumer`.)
8. `NullDlqDropsMessage` — `deadLetterQueue = nullptr`, `maxRedeliveries = 1`: два rollback →
   origin пуст, сообщение нигде не появляется; при persistent запись из storage удалена
   (после `_exchange.reset()` и пересоздания origin пуст). (Semantics 3, 8.)
9. `UnlimitedRedeliveriesWhenNegative` — `maxRedeliveries = -1`: 10 rollback подряд, сообщение
   по-прежнему получаемо, `deliveryCount == 10`. (Semantics 2.)
10. `DefaultPolicyDropsAfterSixRedeliveries` — без `setRedeliveryPolicy`: 7 rollback →
    origin пуст. (Semantics 8.)
11. `BackoffDelaysRedelivery` — `backoffMs = 200`, `maxBackoffMs = 60000`: после rollback
    `recv(50ms)` = nullptr, `recv(1s)` возвращает сообщение с `redelivered`, `deliveryTime ≥ t_rollback + 200ms`;
    второй rollback → не раньше 400 ms. (Semantics 4.)
12. `BackoffCappedByMaxBackoff` — `backoffMs = 200`, `maxBackoffMs = 300`: третий повтор
    (2^2·200 = 800) виден через ≤ 300 ms + допуск. (Semantics 4.)
13. `BackoffUpdatesCachedBytesForPersistent` — persistent, `backoffMs = 100`: полученное после
    повторной доставки сообщение несёт обновлённые `deliveryTime` и `deliveryCount` (ADR-0008:
    fast path `recv` читает кэш). (Semantics 4.)
14. `RedeliveryOrderPreservedWithoutBackoff` — CLIENT_ACK, три сообщения, recover: порядок
    тот же (регресс спеки 23 при новом общем пути `redeliver`). (Semantics 5.)
15. `SetPolicyOnTopicThrows` — `setRedeliveryPolicy` на `Topic` → `Poco::InvalidAccessException`;
    `redeliveryPolicy()` не изменился. (Semantics 6.)
16. `SetPolicyRejectsSelfOrTopicAsDlq` — DLQ = сама очередь → `Poco::InvalidArgumentException`;
    DLQ = topic → то же. (контракт `setRedeliveryPolicy`.)
17. `DeadLetteringOnSessionCloseRollback` — SESSION_TRANSACTED, `maxRedeliveries = 0`: recv без
    commit, закрыть сессию → сообщение в DLQ (rollback из teardown считается). (Semantics 1, 2.)
18. `DlqCopyKeepsExpiration` — `maxRedeliveries = 0`, TTL = 2 s, rollback сразу после recv:
    консьюмер DLQ получает копию с `expiration`, равным исходному (наблюдаемо, пока TTL не
    истёк); после истечения TTL повторный recv из DLQ возвращает nullptr (правило спеки 44).
    (Semantics 7.) Ветка «истёк уже к моменту rollback» через `recv` ненаблюдаема (копия
    истекла в момент появления) и отдельным тестом не проверяется — только отсутствием
    сообщения в origin.
20. `RestartResetsCounterAndLimit` — `maxRedeliveries = 2`, persistent: два rollback
    (`deliveryCount == 2`), `_exchange.reset()` и пересоздание: recv из origin даёт
    `deliveryCount == 0`, `redelivered == false`; ещё три rollback нужны до DLQ. (Semantics 9.)
21. `SessionCloseRequeuesWithoutBackoff` — SESSION_TRANSACTED, `backoffMs = 5000`,
    `maxRedeliveries = 6`: recv без commit, закрыть сессию; новый консьюмер origin получает
    сообщение в пределах 100 ms с `redelivered == true`, `deliveryCount == 1`,
    `deliveryTime == 0`. (Semantics 1, 4 — путь `~Consumer()`.)
19. `JmsxPropertyNamesListed` — `ConnectionMetaData::jmsxPropertyNames` содержит ровно
    `JMSXDeliveryCount` и `JMSXDeadLetterReason`. (Semantics 10.)

Каждое утверждение `Semantics` 1–10 имеет пункт здесь (проверяет `spec-critic`, Standard 3);
S9 → T20, путь `~Consumer()` → T17 (лимит) и T21 (без backoff).
Бенч: `tests/BenchmarkTest.cpp` — существующие `Transacted_*`/`ClientAck_*` покрывают горячий
путь `recv`/`rollback`; отдельный бенч на `rollback` с политикой по умолчанию добавить, если
перф-гейт сочтёт существующие недостаточными.

## Rollback
- Удалить `RedeliveryPolicy.h`, `tests/DlqTest.{h,cpp}`, методы E6–E9, поле E13, запись
  в `jmsxPropertyNames`; вернуть `Consumer::rollback()` к прямому переливу без инкремента.
- Формат хранения не менялся — уже записанные данные откат не задевает. Сообщения, попавшие
  в DLQ до отката, остаются в DLQ как обычные сообщения (у них лишь лишнее свойство).

## Autonomy Level
- `R2` (изолированная ветка `spec-24-redelivery-dlq`, приёмка человеком на мерже).
- `R1` не требуется: формат хранения, сборочная среда и публичный API `Session` не меняются;
  новый публичный API ограничен `Destination::setRedeliveryPolicy`/`redeliveryPolicy`.

## Open questions
- Персистентность `deliveryCount` через рестарт (Semantics 9) — **отдельная спека в M5**
  (решение Owner, 2026-09-20). Готовый дизайн: `deliveryCount` лежит в записи `0x02` по
  фиксированному смещению (+55 от начала записи, после `priority`); нужна одна новая
  операция storage `PATCH_AT` (позиционная запись 4 байт, fire-and-forget через worker) и
  вызов из `Consumer::redeliver` для persistent-сообщений. Формат не меняется, ADR не нужен.
  Здесь не делается, чтобы не расширять скоуп 24 на storage.
- Автосоздание DLQ по соглашению (`DLQ.<origin>`) требует доступа `Destination` к `Exchange`;
  отложено до спеки 43 (admin/introspection). Здесь DLQ задаётся явно.
- Политика для topic family (Semantics 6): нужна модель «копия подписчика vs durable-запись»;
  отдельная спека после 26 (shared consumers).
- `JMSXOriginalDestination` на копии в DLQ — не требуется JMS; добавить, если понадобится
  admin-плоскости (спека 43).
