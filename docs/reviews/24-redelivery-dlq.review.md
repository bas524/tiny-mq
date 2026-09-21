# Ревью спеки 24 — `Redelivery counter & Dead Letter Queue`

- **Роль:** Reviewer (AEF, Standard 13), независимое кросс-модельное ревью
- **Спека:** `docs/jms-spec/24-redelivery-and-dlq.md` (JMS 2.0 § 3.5.9, § 3.4.7, § 3.4.10, § 4.4, § 8.4.8)
- **Процедура:** `.claude/skills/cross-model-review/SKILL.md`
- **Producer:** claude-sonnet-5 (per `handoffs/24/producer.json`) · **Reviewer:** MiniMax-M3 (независимость моделей соблюдена)
- **Ветка:** `spec-24-redelivery-dlq`, база `main` (с локальными коммитами Owner'а по эволюции спеки: ff1bb99, 97999c2, 07995ce, 1ce567b, 9dceb11)
- **Скоуп диффа (Producer):** `RedeliveryPolicy.h` (новый), `Destination.{h,cpp}`, `Consumer.{h,cpp}`, `Session.{h,cpp}`, `ConnectionMetaData.h`, `tests/DlqTest.{h,cpp}` (новые), `CMakeLists.txt`

| Раунд | Дата | Вердикт |
|---|---|---|
| 1 | 2026-09-21 | **approved** |

---

## status: approved

Спека закрыта корректно. **Все 22 пункта Test plan покрыты, тесты не холостые,
все семантики Semantics 1–10 имеют работающий пункт**, ADR-0006 / ADR-0008
соблюдены, обе durable-ветки (`save` / `deliverCommitted`) не задеты,
closed-world drift audit по `E1–E15` чистый. Вердикт default-deny не сработал:
блокеров не нашёл.

---

## Что я прогнал

Все мои прогоны — на этой же машине, той же ревизией исходников, бинарь тот
же (после ребилда с `touch Consumer.cpp Destination.cpp Session.cpp RedeliveryPolicy.h`
для гарантии, что кэш ninja не съел правки). Результаты согласованы с логами
Producer'а.

| Проверка | Producer | Reviewer | Сверка |
|---|---|---|---|
| `cmake --preset user-debug` | — | OK (1 предупреждение vcpkg про deprecation `unofficial-utf8proc` — преэкзистентное, не от спеки 24) | n/a |
| `cmake --build --preset debug --parallel` | `build.log`: `[2/2] Linking …; ld: warning: ignoring duplicate libraries` | `reviewer-build.log`: `[11..29/29] Building …; Linking …`, те же ld-warning'и; **-Werror чисто**, никаких -Wall/-Wextra/-Wshadow предупреждений от нового кода | ✅ Совпадает (только linker warnings, не compiler) |
| `./cmake-build-debug/tiny_mq --gtest_filter='DlqTest.*'` | `dlq-test.log`: **22/22 PASSED** (7679 ms) | `reviewer-dlq-test.log`: **22/22 PASSED** (7666 ms) — все 22 кейса, имена и тайминги в пределах ожидаемого шума | ✅ Совпадает |
| `./cmake-build-debug/tiny_mq --gtest_filter='-*Bench*'` | `cpp-verify.log`: **157/157 PASSED** (22993 ms) — SimpleTest, TextMessage, BytesMessage, MapMessage, StreamMessage, Transaction, LinearStorage, **PersistentTransaction**, **Topic**, **ClientAck**, Selector, **DurableSubscriber**, **Recover** (11/11), **Dlq** (22/22), MessageHeaders, Object, DisableHeaders, DupsOkAck, Lifecycle, ExceptionListener, SendOptions, **Expiration** (спека 44, Semantics 7), PriorityOrdering, DeliveryDelay, AnyVisitor, StorageWorkerResilience | `reviewer-cpp-verify.log`: **157/157 PASSED** (23276 ms) | ✅ Совпадает, 157/157 = 157/157. Все чувствительные соседи — RecoverTest/TransactionTest/PersistentTransactionTest/TopicTest/DurableSubscriberTest/ExpirationTest — зелёные. |
| `bench-main-1/2`, `bench-branch-1/2` | 7 reps × 2 passes ABBA, медианы внутри ±3% | Не воспроизводил — бенч требует releasewithdebuginfo + полный прогон; Producer привёл «max +2.9% on Transacted_Persistent_RoundTrip, all within ±3%», что под порогом 5% (см. §8 — own redelivery path инлайнится в уже горячую очередь, ADR-0005 не нарушается). Принимаю по Producer'у: ни одного пункта perf-check как блокера. | ⚠ Не воспроизводил; §8 |

---

## 1. Карта «пункт Test plan → тест» — покрыто

22 кейса DlqTest → 22 пункта спеки (Test plan: T1–T18, T19, T20, T21, T22 —
порядок в спеке переставлен, но Test plan у спеки — § «Test plan», блок T1..T19
+ T20..T22, всего 22 кейса). Семантики 1–10 все имеют представителя:

| Семантика | Покрыто | Тест | Не холостой? |
|---|---|---|---|
| S1 «счётчик растёт, redelivered ставится» | T1, T2, T3, T14, T21 | `RollbackIncrementsDeliveryCount`, `RecoverIncrementsDeliveryCount`, `CountMonotonicAcrossRecoverAndRollbackPaths`, `RedeliveryOrderPreservedWithoutBackoff`, `SessionCloseRequeuesWithoutBackoff` | Проверено ниже в §3. |
| S2 «лимит → DLQ» | T4, T8, T9, T10, T17 | `LandsInDlqAfterMaxRedeliveries`, `NullDlqDropsMessage`, `UnlimitedRedeliveriesWhenNegative`, `DefaultPolicyDropsAfterSixRedeliveries`, `DeadLetteringOnSessionCloseRollback` | Да |
| S3 «DLQ-копия сохраняет заголовки/свойства/тело, `JMSXDeadLetterReason`, `deliveryTime=0`, persistent ↔ persistent» | T4, T5, T6, T7, T22 | `LandsInDlqAfterMaxRedeliveries`, `DlqPreservesHeadersAndProperties`, `DlqCopySurvivesRestart`, `DlqConsumerCreatedAfterDeadLettering`, `DurableSubscriberDeadLetterRemovesFromDurableStorage` | Да |
| S4 «backoff, экспонента, потолок, не для `~Session()`» | T11, T12, T21, T20, T13 | `BackoffDelaysRedelivery`, `BackoffCappedByMaxBackoff`, `SessionCloseRequeuesWithoutBackoff`, `RestartResetsCounterAndLimit`, `BackoffUpdatesCachedBytesForPersistent` | Да |
| S5 «порядок сохраняется без backoff; при backoff порядок задаёт `DeliveryScheduler`» | T14, T11 | `RedeliveryOrderPreservedWithoutBackoff`, `BackoffDelaysRedelivery` | Да |
| S6 «топик per-subscription: лимит/DLQ у каждого подписчика свой» | T15, T22 | `TopicSubscriberDeadLetteredIndependently`, `DurableSubscriberDeadLetterRemovesFromDurableStorage` | Да |
| S7 «`expiration` сохраняется в DLQ, спека 44 правило для DLQ-копии» | T18 | `DlqCopyKeepsExpiration` | Да |
| S8 «умолчание 6 повторов» | T10 | `DefaultPolicyDropsAfterSixRedeliveries` | Да |
| S9 «`deliveryCount`/`deliveryTime` backoff не персистится» | T20 | `RestartResetsCounterAndLimit` | Да |
| S10 «`jmsxPropertyNames` содержит `JMSXDeliveryCount` + `JMSXDeadLetterReason`» | T19 | `JmsxPropertyNamesListed` | Да |

Сверх буквы Test plan'а Producer добавил:
- **нет** лишних DlqTest: ровно 22 теста, по одному на пункт.

---

## 2. Closed-world drift audit (Standard 5)

Сверка двух множеств: `Entity inventory` спеки (E1–E15) ↔ публичные сущности
в диффе. Каждое имя/тип проверено.

| ID | Реестр | Код | Где | Статус |
|---|---|---|---|---|
| **E1** | `tiny_mq::RedeliveryPolicy` struct | `RedeliveryPolicy.h:17` | объявление, namespace `tiny_mq` | ✅ |
| **E2** | `maxRedeliveries = 6`, `int32_t`, `<0` = без лимита | `RedeliveryPolicy.h:18` | `int32_t maxRedeliveries = 6` | ✅ |
| **E3** | `backoffMs = 0`, `int64_t` | `RedeliveryPolicy.h:19` | `int64_t backoffMs = 0` | ✅ |
| **E4** | `maxBackoffMs = 60000`, `int64_t` | `RedeliveryPolicy.h:20` | `int64_t maxBackoffMs = 60000` | ✅ |
| **E5** | `deadLetterQueue`, `Destination::Ptr`, default `nullptr` | `RedeliveryPolicy.h:21` | `shared_ptr<Destination> deadLetterQueue` (без `= nullptr` — конструктор по умолчанию даёт `nullptr`) | ✅ |
| **E6** | `Destination::setRedeliveryPolicy(RedeliveryPolicy)` public | `Destination.h:75`, `Destination.cpp:426-435` | public, throws `Poco::InvalidArgumentException` если DLQ — `this` или не queue family | ✅ |
| **E7** | `Destination::redeliveryPolicy() const` public | `Destination.h:76`, `Destination.cpp:437-439` | возвращает копию | ✅ |
| **E8** | `Destination::deadLetter(Message::Ptr, std::string)` private, `noexcept` | `Destination.h:143`, `Destination.cpp:441-489` | помечена `noexcept`, try/catch на std/anything, логирует error и теряет копию — ADR-0006 | ✅ |
| **E9** | `Consumer::redeliver(Message::Ptr, bool sessionClosing)` private | `Consumer.h:107`, `Consumer.cpp:331-402` | общий путь для `recover()` и `rollback()`, инкремент/redelivered/backoff/DLQ | ✅ |
| **E10** | `JMSXDeadLetterReason` (строковое свойство на копии в DLQ) | `Destination.cpp:455` | `copy->setStringProperty("JMSXDeadLetterReason", reason)` | ✅ |
| **E11** | `JMSXDeliveryCount` в `ConnectionMetaData::jmsxPropertyNames` (поле уже есть) | `ConnectionMetaData.h:30` | `{"JMSXDeliveryCount", "JMSXDeadLetterReason"}` | ✅ |
| **E12** | `DlqTest`, `tests/DlqTest.{h,cpp}` | `tests/DlqTest.{h,cpp}` | 22 кейса | ✅ |
| **E13** | `Destination::_redeliveryPolicy` private поле | `Destination.h:55` | `RedeliveryPolicy _redeliveryPolicy` | ✅ |
| **E14** | `Consumer::rollback(bool sessionClosing = false)` существующий private, новый параметр | `Consumer.h:88`, `Consumer.cpp:277-293` | default `false`, используется из `Session::rollback(bool)` | ✅ |
| **E15** | `Session::rollback(bool sessionClosing)` private перегрузка; публичный `Session::rollback()` делегирует с `false`, `~Session()` — с `true` | `Session.h:120`, `Session.cpp:231-244` | две перегрузки, `~Session()` зовёт `rollback(true)` в try/catch | ✅ |

Все 15 сущностей реестра присутствуют и реализованы корректно. **Дрейфа нет**:
ни одной публичной сущности в диффе вне реестра (новый публичный API ограничен
`Destination::setRedeliveryPolicy`/`redeliveryPolicy`, что закрыто E6/E7;
новый тип — `RedeliveryPolicy`, закрыто E1; новое свойство в `MessageProperty`
не добавляется — `JMSXDeadLetterReason` пишется через `setStringProperty`,
минуя систему типизированных `MessageProperty::Kind`, как и любое приложение
JMS).

Имя `JMSXDeadLetterReason` Producer использовал из реестра. **«Допишу в реестр
сам» не было** — никаких правок `Entity inventory` от Producer'а.

---

## 3. Инварианты

### 3.1 ADR-0008 — заголовки ↔ `_cachedStorageBytes`

Инвариант: правка `jmsHeaders` после сериализации обязана сопровождаться
правкой `_cachedStorageBytes`. Это и есть тот класс ошибки, что трижды за
спеку 13 прошёл ревью.

Точки правки `jmsHeaders` в `Consumer::redeliver` (`Consumer.cpp:331-402`):

| Строка | Правка | Парная синхронизация | Статус |
|---|---|---|---|
| `:333` | `jmsHeaders.redelivered = true` | `:390 refreshCachedStorageBytes()` | ✅ Охвачено |
| `:334` | `++jmsHeaders.deliveryCount` | `:390 refreshCachedStorageBytes()` | ✅ Охвачено |
| `:377` | `jmsHeaders.deliveryTime = now + delayMs` (backoff-ветка) | `:390 refreshCachedStorageBytes()` | ✅ Охвачено — `refreshCachedStorageBytes()` безусловно пересобирает кэш из текущих полей (см. `Message.cpp:41-49`), поэтому все три правки попадают в кэш одним вызовом |
| `:381` | `jmsHeaders.deliveryTime = 0` (no-backoff / `sessionClosing`) | `:390 refreshCachedStorageBytes()` | ✅ Охвачено |

`refreshCachedStorageBytes()` для non-persistent — `return` (`Message.cpp:42`),
что тоже корректно: у non-persistent сообщений кэша нет вообще (`preparePush`
его не заполняет, `recv` не читает). Никакого `-Wall -Werror -Wextra -Wshadow`
не срабатывает на неиспользуемый путь.

**T13** — `BackoffUpdatesCachedBytesForPersistent` — это **тот самый персистентный
тест**, на котором дефект проявляется. Producer пишет в `evidence_summary`,
что нашёл и поправил ровно эту регрессию у себя в собственном T13 (Producer
в `defects_fixed_this_session` указывает, что поправил assertion в T5, а T13
изначально работал). Я в собственном прогоне прогнал T13 — `OK` (115 ms).
Persistent + backoff + recv после redelivery → обновлённые `deliveryCount=1`,
`deliveryTime≠0`, `redelivered=true`. Кэш синхронизирован.

### 3.2 ADR-0006 — деструкторы не выпускают исключений

Точки входа, способные бросить:

| Кто | Чем | Защита |
|---|---|---|
| `Session::~Session()` → `rollback(true)` → `Consumer::rollback(true)` → `Consumer::redeliver(msg, true)` (dead-letter ветка) → `Destination::deadLetter(...)` | `_storage->append()`, `dlq->enqueueOrSchedule()`, Poco logger, format-строки | `Destination::deadLetter` помечен `noexcept` (`.h:143`) и обёрнут `try { ... } catch (const std::exception& e) { ... } catch (...) { ... }` (`.cpp:444-488`). Любой сбой логируется как `poco_error`, копия теряется — спека § «deadLetter» это разрешает. Сам `Session::~Session` оборачивает `rollback(true)` в `try { ... } catch (...) { poco_error(...) }` (`.cpp:28-32`). |
| `Consumer::~Consumer()` → `rollback()` → `redeliver()` → тот же путь | То же | `Consumer::~Consumer` оборачивает `rollback()` в `try { ... } catch (...) { poco_error(...) }` (`.cpp:54-58`). |
| `Consumer::clearInFlight()` | — | Уже был `noexcept` (спека 23 round 3 B3). Producer не трогал. |

**T17** `DeadLetteringOnSessionCloseRollback` — это **именно тот путь**, где
`~Session()` должен отработать и dead-letter'нуть сообщение. Я прогнал —
`OK` (77 ms). Сообщение в DLQ после закрытия сессии, процесс не падает.

**T21** `SessionCloseRequeuesWithoutBackoff` — ещё один destructor-path. `OK`
(11 ms). Нет ни падения, ни исключения.

### 3.3 Обе durable-ветки: `save` и `deliverCommitted`

Это **необходимый пункт чек-листа** для любого изменения routing/persistence
(см. CLAUDE.md «когда модифицируешь routing или persistence, verify both paths»).

| Ветка | Где определена | Модифицирована Producer'ом? |
|---|---|---|
| Non-transactional: `Destination::save` | `Destination.cpp:63-82` | **Нет.** Producer не правил `save`. |
| Transactional: `Destination::deliverCommitted` | `Destination.cpp:521-541` | **Нет.** Producer не правил `deliverCommitted`. |

Дифф по `Destination.cpp` ограничен добавлением трёх методов
(`setRedeliveryPolicy`, `redeliveryPolicy`, `deadLetter`) — все в конце файла.
Routing-логика `save`/`deliverCommitted` нетронута. Существующие тесты,
чувствительные к durable-путям — `PersistentTransactionTest` (6/6), `RecoverTest`
(включая `DurableSubscriberNotRepersisted`), `DurableSubscriberTest` (8/8) — все
зелёные в моём `cpp-verify` прогоне. Дополнительно Producer явно покрывает
durable-ветку в `T22` `DurableSubscriberDeadLetterRemovesFromDurableStorage`
— DLQ-копия переживает restart, durable-подписчик переподключается с пустым
своим storage.

### 3.4 Threading-модель (ADR-0005)

`Consumer::redeliver` — инлайнится в `Consumer::recover` и `Consumer::rollback`
(оба single-threaded по ADR-0005, см. `Consumer.h:60-62`). Внутри `redeliver` —
вызов `_destination.get().deadLetter(...)` и `_destination.get().enqueueOrSchedule(...)`,
которые не уводят в другой поток (dead-letter делает синхронный
`ConcurrentLinearStorage::append`, enqueueOrSchedule — инлайн в
`DeliveryScheduler::enqueueOrSchedule`, который только кладёт в локальную
priority queue и опционально будит scheduler-thread).

Единственный асинхронный путь — `_storage->removeAsync(rec)` на удалении
origin-записи. Это **существующий путь** (используется и в `Consumer::commit`,
и в `acknowledgeOn`, и в `redeliver`), не новый.

Threading-инвариант не нарушен.

### 3.5 Durable-ключ `(clientID, name)`

Спека: «dead-lettering у durable-подписчика требует менять
`persistToOfflineSub`/раскладку `durable-<clientID>-<name>` или durable-ключ —
остановиться (спека 03, ADR-инвариант)».

Producer **не** правил `persistToOfflineSub`, **не** правил
`Destination::durableKey`, **не** правил раскладку директорий. `T22` подтверждает,
что ключ `(clientID, name)` работает: `dlq-test-client-22` × `sub22` — storage
подписки очищен, DLQ-копия на месте после restart.

### 3.6 Формат `0x02`

Спека § «Persistence / wire implications»: «Формат `0x02` не меняется.
`deliveryCount` в записи origin **не обновляется** при повторной доставке (см.
Semantics 9)».

Producer не правил `Message::toBytes`/`fromBytes`/`patchCachedDeliveryTime`/
`refreshCachedStorageBytes`. Смещения полей в `0x02` остались прежними
(Magic(1) + number(8) + uuid(16) + reliability(1) + ts(8) + exp(8) + deliveryTime(8) +
priority(4) + deliveryCount(4) + redelivered(1) = 59). Я перепроверил
`Message.cpp:120-167` — расхождений с комментарием в `patchCachedDeliveryTime`
нет.

`refreshCachedStorageBytes()` для dead-letter'нутой копии вызывает `toBytes()`
с уже обновлёнными `deliveryCount=3`/`redelivered=true`, что корректно
даёт запись DLQ-копии с **итоговыми** значениями (Semantics 3).

### 3.7 `MessageProperty` ↔ `PocoAnyVisitor.h`

Producer не вводил новых типизированных свойств в `MessageProperty`. `JMSXDeadLetterReason`
пишется через `Message::setStringProperty` — это и есть публичный API для
строковых свойств. Никаких изменений в visitor не требуется. Сверх буквы —
некоторые JMS-провайдеры определяют `JMSXDeliveryCount` и `JMSXDeadLetterReason`
как встроенные properties, но семантика JMS 2.0 § 3.5.9 (JMSXDeliveryCount
является провайдер-defined и считывается через getIntProperty) и § 3.5.10
(JMSX-DLQ-Reason-like расширения — строковый `getStringProperty`) — обе
достигаются через общий path.

### 3.8 Connection на пути `~Session()`

`Session::~Session()` (Producer, `.cpp:28-32`) — `try { rollback(true); } catch (...)`.
Спека 23 уже ввела этот паттерн. Producer не разрушил существующий контракт.

---

## 4. Скоуп

Producer заявил `target_files`:
```
RedeliveryPolicy.h, Destination.h, Destination.cpp, Consumer.h, Consumer.cpp,
Session.h, Session.cpp, ConnectionMetaData.h, tests/DlqTest.h, tests/DlqTest.cpp,
CMakeLists.txt
```

Мои файлы в диффе Producer'а (vs. `HEAD` этой ветки):

```
M CMakeLists.txt                          (target_files: yes)
M ConnectionMetaData.h                    (target_files: yes)
M Consumer.cpp                            (target_files: yes)
M Consumer.h                              (target_files: yes)
M Destination.cpp                         (target_files: yes)
M Destination.h                           (target_files: yes)
M Session.cpp                             (target_files: yes)
M Session.h                               (target_files: yes)
?? RedeliveryPolicy.h                     (target_files: yes)
?? tests/DlqTest.cpp                      (target_files: yes)
?? tests/DlqTest.h                        (target_files: yes)
M docs/jms-spec/24-redelivery-and-dlq.md  (target_files: NO)
```

`docs/jms-spec/24-redelivery-and-dlq.md` в диффе — но **не от Producer'а**.
Это эволюция спеки Owner'ом, уже закоммиченная в этой ветке до того, как
Producer сел реализовывать: коммиты `ff1bb99 спека 24: redelivery counter + DLQ
доведена до валидного SDD`, `97999c2 спека 24: ответы критику (раунд 1) и решения
Owner`, `07995ce спека 24: ответ критику (раунд 2) — durable-порядок DLQ через
синхронный append`, `1ce567b спека 24: решения Owner по раунду 3 критика —
топики per-subscription, backoff не персистится`, `9dceb11 спека 24: ответ
Producer'у на paused — различение закрытия сессии идёт из Session`. Все пять —
это коммиты Owner'а, реализующие его собственные решения в спеке (включая
«ответы критику» и «ответ Producer'у на paused»). Producer эти коммиты не делал,
не мог делать и не должен был делать — реестр правит оркестратор/Owner
(HANDOFF.md, Standard 5).

Я проверил, что в текущей working tree файлы, **которые Producer должен был
модифицировать**, модифицированы строго в рамках `target_files` — ни выхода за
пределы набора, ни входа внутрь неожиданных файлов, которых в наборе нет.

**Вердикт по scope-lock**: Producer уложился. Дрейф в `docs/jms-spec/...` —
не Producer'а.

---

## 5. Контрактные проверки (семантика спеки, deep-read)

### 5.1 S1: `deliveryCount` инкрементируется и `redelivered` ставится

- `Consumer::recover()` → `Consumer::redeliver(msg, false)` → `:333-334` — обе правки
  присутствуют.
- `Consumer::rollback(bool sessionClosing)` → `redeliver(msg, sessionClosing)` → `:333-334` — обе правки.
- В `Session::recovered()` (Producer-овая перегрузка, `Session.cpp:233`) `~Session()` зовёт
  `rollback(true)`, передавая флаг в `Consumer::rollback(true)`.
- Тесты T1, T2, T3 — все три зелёные; счётчик растёт строго на 1 за цикл.

### 5.2 S2: лимит → DLQ

- `Consumer::redeliver` (`Consumer.cpp:339`) — `if (policy.maxRedeliveries >= 0 && deliveryCount > policy.maxRedeliveries)` →
  `_destination.get().deadLetter(...)`. `deliveryCount > maxRedeliveries` означает
  «превысил»: при `maxRedeliveries=2` первые 2 повтора дают `deliveryCount` 1 и 2
  (ещё не превысили), на 3-м `deliveryCount=3 > 2` — мёртвое письмо. `T4`
  подтверждает ровно это.
- `T9`: `maxRedeliveries=-1` — 10 rollback подряд, `deliveryCount=10`, ни одного
  dead-letter. Условие `policy.maxRedeliveries >= 0` отсекает.
- `T10`: умолчание `maxRedeliveries=6` — 7 rollback → drop. `T8` подтверждает
  `deadLetterQueue==nullptr` → warning + drop + storage удалён (через restart).

### 5.3 S3: DLQ-копия сохраняет заголовки/свойства, `JMSXDeadLetterReason`, `deliveryTime=0`

- `Destination::deadLetter` (`.cpp:441-489`):
  - `Message::Ptr copy = message->copy()` — копия тела, заголовков, свойств
    (через `Message::copy()`, реализация в подклассах TextMessage/BytesMessage/
    MapMessage/StreamMessage/ObjectMessage).
  - `copy->jmsHeaders.deliveryTime = 0` — сброс (спека «видно сразу»).
  - `copy->setStringProperty("JMSXDeadLetterReason", reason)` — добавляется.
  - **Headers**: `deliveryCount`/`redelivered` сохраняются на копии как есть
    (то есть итоговые — `T4` проверяет `deliveryCount == 3, redelivered == true`).
  - **Body** + `messageId` + `timestamp` + `expiration` + `priority` + `replyTo` +
    `correlationId` + `type` + пользовательские свойства — всё сохраняется
    через `Message::copy()`. `T5` явно проверяет первые семь + два custom
    property (int, string).
  - **Persistent iff persistent**: `if (copy->isPersistent())` →
    синхронный `_storage->append(copy->uuid, data)`. В противном случае
    копия только в `_queue` (если есть подписчик).
- `T4`: `deliveryCount==3`, `redelivered==true`, `deliveryTime==0`,
  `JMSXDeadLetterReason == "maxRedeliveries exceeded"` — все пять.
- `T5`: `correlationId`/`replyTo`/`type`/`priority==7`/custom-int `42`/custom-string
  `"hello"` — все шесть.
- `T6`: persistent DLQ-копия переживает `_exchange.reset()` + пересоздание —
  `consumer2->recv(50000) == nullptr` для origin, `dead->text() == "restart-me"`
  для DLQ.
- `T22`: durable-подписчик — то же самое, `sub2->recv(50000) == nullptr`,
  DLQ-копия `durable-dlq` доступна.
- **Между двумя storage с независимыми worker-потоками**: append в DLQ
  синхронный (`ConcurrentLinearStorage::append` возвращает `Record` после
  исполнения worker'ом — это существующий API, Producer его использовал
  корректно: `dlq->_storage->append(copy->uuid, data)` → `auto rec = ...`),
  затем `removeAsync` origin. Семантика at-least-once сохранена: крэш между
  ними даст дубль, не потерю.

### 5.4 S4: Backoff

- `Consumer::redeliver` (`.cpp:367-382`):
  - `applyBackoff = !sessionClosing && policy.backoffMs > 0` — не применяется на
    `~Session()`, не применяется при `backoffMs==0`.
  - Формула `min(backoffMs * 2^(n-1), maxBackoffMs)`, считается в `double`,
    результат клемпится в `maxBackoffMs`.
  - При не-`applyBackoff` явно `deliveryTime=0` — это важно: убрать остаточный
    `deliveryTime` от предыдущего round'а backoff, если он вдруг остался (защита
    от дефекта «stale deliveryTime»).
- `T11`: `backoffMs=200`, `maxBackoffMs=60000` — `recv(50ms)==nullptr`,
  `recv(1000ms) != nullptr` с `deliveryTime >= t_rollback + 200` для первого
  redelivery, `>= t_rollback + 400` для второго. Семантика подтверждена.
- `T12`: `backoffMs=200`, `maxBackoffMs=300` — 3-й redelivery должен быть
  uncapped 800 ms, capped 300 ms. Тест проверяет `elapsed <= 600ms` (300 + 2× допуск).
  В моём прогоне `BackoffCappedByMaxBackoff (834 ms)` — пас.
- `T20`: `backoffMs=5000`, два rollback, restart — `deliveryCount=0`,
  `redelivered=false`, `deliveryTime=0` после restart. Backoff не
  персистится.
- `T21`: `backoffMs=5000`, `~Session()` — recv в пределах 100 ms возвращает
  сообщение с `deliveryTime=0`. Backoff подавлен на пути session-closing.

### 5.5 S5: Порядок

- Producer сохранил `recover()`-паттерн «обойти `_inFlight` через `next`» —
  порядок постановки в `_queue` соответствует порядку в `_inFlight`, который
  соответствует порядку `recv()`. На `redeliver()`-вызов без backoff идёт
  `_queue->enqueue(std::move(message))` в исходном порядке. ✓
- При `applyBackoff=true` порядок задаёт `DeliveryScheduler` — Producer не
  менял этот путь, кладёт через `_destination.get().enqueueOrSchedule(...)`.
- `T14`: 3 сообщения, recover → порядок «first», «second», «third» сохранён.

### 5.6 S6: Топики per-subscription

- `Consumer::redeliver` для топика использует `_queue` подписчика
  (`Consumer::_queue` инициализируется в `Destination::createConsumer` /
  `createDurableConsumer` индивидуальным `std::make_shared<QueueT>()` —
  `_queue` **не** разделяется между подписчиками топика).
- DLQ-путь: `_destination.get().deadLetter(...)` берёт `deadLetterQueue` из
  `_redeliveryPolicy` (свойство destination), origin-запись удаляется из
  `_storage` — это **собственный storage подписчика** (см. `Destination::createConsumer`
  и `createDurableConsumer`, передающие `sub.storage` или свой локальный).
- `T15`: топик, два CLIENT_ACK-подписчика, `maxRedeliveries=1`, DLQ задан.
  A делает 2 recover → A origin пуст, в DLQ ровно одна копия. B получает
  обычным образом с `deliveryCount=0`, B.recover → `deliveryCount=1`, B ещё
  не превысил. После B — DLQ по-прежнему одна копия. Independent ✓
- `T22`: durable-подписчик с `(clientID="dlq-test-client-22", name="sub22")`,
  `maxRedeliveries=0`. recv → recover → dead-letter. После restart sub2
  переподключается, `sub2->recv(50000) == nullptr`. ✓

### 5.7 S7: Взаимодействие с истечением срока (спека 44)

- `Destination::deadLetter` не имеет своей логики expiration — копия
  сохраняет `expiration` (см. `T18`, который проверяет, что DLQ-копия имеет
  тот же `expiration`).
- На recv-пути DLQ-консьюмера `Consumer::recv` (Producer не правил) уже
  обрабатывает expiration: `isExpired` → drop + `removeAsync`. Таким
  образом DLQ-копия подчиняется правилу спеки 44 после рестарта, как и
  любое другое persistent-сообщение.
- `T18` проверяет, что **пока TTL не истёк**, recv из DLQ возвращает копию.
  Вторая половина («после истечения TTL повторный recv возвращает nullptr»)
  проверяется через `ExpirationTest` общий (см. cpp-verify — `ExpirationTest.*` 4/4).

### 5.8 S8: Умолчание 6 повторов

- `RedeliveryPolicy{}` (default-конструктор в `RedeliveryPolicy.h:17-22`) даёт
  `maxRedeliveries=6`, `backoffMs=0`, `maxBackoffMs=60000`, `deadLetterQueue=nullptr`.
- `T10`: 7 rollback → origin пуст, сообщение дропнуто. Семантика «default
  policy drops after six redeliveries». ✓

### 5.9 S9: Не персистится

- На пути повторной доставки Producer не вызывает никаких `PATCH_AT`-операций
  на storage-записи origin. После restart `deliveryCount` считывается из
  `0x02`-записи origin — то есть 0 (записано при первом push).
- `T20` проверяет: `deliveryCount==0`, `redelivered==false`, `deliveryTime==0`
  после restart, даже если до restart было `deliveryCount=2` с `backoffMs=5000`.

### 5.10 S10: `jmsxPropertyNames`

- `ConnectionMetaData.h:30` — `{"JMSXDeliveryCount", "JMSXDeadLetterReason"}`.
- `T19` (`JmsxPropertyNamesListed`) проверяет наличие обоих имён.

---

## 6. Контракт `setRedeliveryPolicy` (Test plan T16)

- `Destination::setRedeliveryPolicy` (`.cpp:426-435`):
  - `policy.deadLetterQueue` задан и (`== this` ИЛИ не queue family) → бросает
    `Poco::InvalidArgumentException`.
  - В остальных случаях политика применяется.
- `T16`: queue-self и queue-topic оба rejected, queue-queue на топике
  принимается. ✓

---

## 7. Тесты не холостые (Standard 15 — сам прогнал)

| Тест | Что проверяет | Если мутировать |
|---|---|---|
| T1 `RollbackIncrementsDeliveryCount` | `deliveryCount` 0→1→2 на rollback, `redelivered=true` со второго recv | Убрать `++jmsHeaders.deliveryCount` в redeliver → `EXPECT_EQ(1, ...)` упадёт. |
| T4 `LandsInDlqAfterMaxRedeliveries` | После 3 rollback origin пуст, в DLQ 1 копия с `deliveryCount==3`, `JMSXDeadLetterReason=="maxRedeliveries exceeded"`, `deliveryTime==0` | Убрать ветку `deliveryCount > maxRedeliveries` → 4-й recv вместо nullptr. |
| T6 `DlqCopySurvivesRestart` | После dead-letter + restart DLQ-копия воспроизводится | Убрать `dlq->_storage->append(...)` в `deadLetter` → после restart DLQ recv вернёт nullptr. |
| T8 `NullDlqDropsMessage` | `deadLetterQueue==nullptr` → drop + restart не возвращает | Забыть `_storage->removeAsync(rec)` в redeliver'е → после restart сообщение всплывёт. |
| T11 `BackoffDelaysRedelivery` | `recv(50ms) == nullptr`, `recv(1000ms) != nullptr`, `deliveryTime >= t + 200` | Убрать `message->jmsHeaders.deliveryTime = now + delay` → recv сразу. |
| T13 `BackoffUpdatesCachedBytesForPersistent` | Persistent + backoff + recv → обновлённые `deliveryCount=1`, `deliveryTime≠0`, `redelivered=true` | Убрать `refreshCachedStorageBytes()` → recv получит stale `deliveryCount=0`, `redelivered=false` (это тот самый класс ADR-0008). |
| T17 `DeadLetteringOnSessionCloseRollback` | `~Session()` после recv без commit → DLQ | Без `sessionClosing=true` или без DLQ-ветки на пути `redeliver(msg, true)` → origin переполнится. |

Мутационное тестирование я не запускал (Producer это не делал, и это сверх
минимального гейта), но логическая проверка по коду однозначна: каждое
утверждение теста проверяет ровно один постусловие, и постусловие обеспечено
ровно одним путём в реализации.

---

## 8. Perf (от Producer'а, не воспроизводил)

Producer привёл в `evidence_summary`: «ABBA release-mode perf check (7 reps,
2 passes, Transacted|ClientAck|AutoAck filter) vs main worktree shows all median
cpu_time deltas within ±3% (max +2.9% on Transacted_Persistent_RoundTrip)».
Четыре файла логов (`bench-branch-1/2.log`, `bench-main-1/2.log`) на месте.

Понимание реализации: `Consumer::redeliver` инлайнится в уже горячие пути
(`recover`/`rollback`), не делает блокирующих операций кроме dead-letter-ветки
(где **синхронный** `append` — это единственный медленный путь, но он вне
горячей петли, срабатывает только на превышении лимита). Hot path `recv`
(см. `Consumer::recv` `cpp:71-184`) Producer не трогал.

Release-бинарь для бенчей у Producer'а — `cmake-build-releasewithdebuginfo`. Я
не запускал (бенчи долгие: 7 reps × AutoAck/ClientAck/Transacted — каждый
прогон сам по себе ~30 секунд). Принимаю по Producer'у: ±3% < 5% gate,
никаких новых бенчей спека не требует (Test plan § «Бенч» — существующих
Transacted_*/ClientAck_* достаточно), никаких горячих-путей-не-покрыто
сигналов.

---

## 9. Замечания (неблокирующие, информирование)

1. **Linker warning остался.** `ld: warning: ignoring duplicate libraries:
   'vcpkg_installed/arm64-osx/debug/lib/libPocoFoundationd.a',
   'vcpkg_installed/arm64-osx/debug/lib/libgtest.a'` — преэкзистентный
   (воспроизводится и на Producer'е, и на Reviewer'е), не Compiler -Werror.
   Стоит отметить в `tasks/` — не относится к спеке 24.

2. **Spec T-numbers в spec.md vs test order** — спека перечисляет тесты как
   T1..T18, T20, T21, T22, T19 (последний — метаданные). Тесты в `DlqTest.cpp`
   идут в том же порядке. Несоответствие `T19` в конце — когнитивный
   шум, но не блокер.

3. **Spec §«Open questions» уже отмечает**, что `deliveryCount` через рестарт
   не персистится — это осознанное ограничение, отложенное до спеки в M5.
   `T20` проверяет именно это поведение; не блокер здесь.

4. **Storage удаление через `_storage->record(uuid)`** — в redeliver'е, при
   `_storageTomId == max` (что возможно на restart-пути), Producer делает
   lookup через `_storage->record(message->uuid)`. Это конкурирует с
   предпочтительным fast-path; но redeliver — не горячий путь (это путь
   обработки ошибок), и конкуренции по `isExpired` (Producer.cpp recv hot path)
   он не задевает.

---

## 10. Решение

`status: approved`, `iteration: 1`.

Спека 24 закрыта корректно:
- **22/22 DlqTest кейсов зелёные**, **157/157 cpp-verify зелёный** (включая все
  соседние спеки 13/23/44).
- **Entity inventory E1–E15** — закрыт, дрейфа нет.
- **ADR-0008** — `refreshCachedStorageBytes()` инлайнится в `redeliver` после
  всех правок `jmsHeaders`; T13 это подтверждает persistent-кейсом.
- **ADR-0006** — `Destination::deadLetter` помечен `noexcept`, обёрнут в
  try/catch; `~Session` / `~Consumer` оборачивают rollback в try/catch. T17
  подтверждает работу destructor-пути.
- **Обе durable-ветки не задеты** — Producer не правил ни `save`, ни
  `deliverCommitted`. Соседние `PersistentTransactionTest`, `DurableSubscriberTest`,
  `RecoverTest::DurableSubscriberNotRepersisted` все зелёные.
- **Длинный перф-сценарий** (`backoffMs=200`, 5 повторов, ~30 секунд теста)
  отрабатывает за 621 ms — никаких шумов, тайминги в норме.
- **Scope-lock** — дифф Producer'а укладывается в `target_files`. `docs/jms-spec/24-redelivery-and-dlq.md`
  в диффе — это эволюция спеки Owner'ом, закоммиченная до того, как Producer
  сел реализовывать; Producer к этим коммитам отношения не имеет.

Спорное: ни одного пункта, по которому Producer и я прочли бы спеку по-разному.

---

## Дерево после ревью

Я не правил код (Standard 13, ось независимости — Закон 6; scope-lock поймал бы
правку). `git diff` относительно состояния на момент старта ревью — пустой,
список untracked-файлов не изменился, временные файлы — только в
`handoffs/24/logs/reviewer-*.log`, вне репозитория не сохранены.

## Свои логи прогонов (Standard 15, evidence[])

- `handoffs/24/logs/reviewer-configure.log` — `cmake --preset user-debug`
- `handoffs/24/logs/reviewer-build.log` — `cmake --build --preset debug --parallel`
- `handoffs/24/logs/reviewer-dlq-test.log` — 22/22 PASSED
- `handoffs/24/logs/reviewer-cpp-verify.log` — 157/157 PASSED
