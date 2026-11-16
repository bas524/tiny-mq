# Ревью SDD 71 — очереди, топики, подписки и временные назначения

- Класс: K4 (свидетельство, замороженное во времени)
- Стадия: reviewer (AEF, Стандарт 13, независимое ревью другой моделью)
- Модель рецензента: deepseek-v4-pro (автор — gpt-6.1-sol; оси независимости соблюдены)
- Дата: 2026-11-16
- Spec: 71, iteration: 1
- Вердикт: **rejected**

## Прогон рецензента (собственный, не принятый на веру)

Сборка и тесты выполнены из отдельного каталога `handoffs/current/71/build/reviewer-r1`
(Debug, Ninja, vcpkg arm64-osx, macos-expat.cmake). Отпечаток дерева до прогона и после
последнего прогона совпал: `tree:0ac319b7aa1719988b175a8251790de7db879ebae2c2f774019808d71c0ffa3b`
(совпадает и с `evidence_tree` пакета автора — входы за время проверки не менялись).

- configure Debug: exit 0 → `handoffs/current/71/logs/reviewer-r1-configure-debug.log`
- build --parallel 8: exit 0, 52 цели, без предупреждений (-Wall -Werror -Wextra -Wshadow) → `handoffs/current/71/logs/reviewer-r1-build.log`
- ./tiny_mq --gtest_filter='-*Bench*': 171/171 PASSED (28 suites) → `handoffs/current/71/logs/reviewer-r1-verify.log`
- trace.py tests ... --id-pattern 'REQ-[0-9]{2}-[0-9]+': ошибок 0 → `handoffs/current/71/logs/trace-tests.log`

Сверка с логами автора: configure/build exit 0, verify 171/171, trace 0 ошибок. Расхождений
нет — факт прогона автора подтверждён моим собственным прогоном; признаков «сфабрикованной
проверки» нет. Оценка 6 тестов `DestinationContractTest` совпадает с `test-plan.log` автора
(6/6; разброс таймингов — ожидаемый шум машины).

## Карта «сценарий → тест» (трассируемость, Стандарт 3)

- SelectiveQueueClaim — REQ-71-1 — `DestinationContractTest.SelectiveQueueClaim` — PASSED
- SubscriptionFanout — REQ-71-2 — `DestinationContractTest.SubscriptionFanout` — PASSED
- DurableIdentityMatrix — REQ-71-3 — `DestinationContractTest.DurableIdentityMatrix` — PASSED
- NoLocalMatrix — REQ-71-4 — `DestinationContractTest.NoLocalMatrix` — PASSED
- BrowseWithoutClaim — REQ-71-5 — `DestinationContractTest.BrowseWithoutClaim` — PASSED
- TemporaryReplyLifecycle — REQ-71-6 — `DestinationContractTest.TemporaryReplyLifecycle` — PASSED
- DestinationHotPathBudget — REQ-71-7 — `NfrGateTest.DestinationHotPathBudget` → `tests/nfr71.py` — см. Находку 2

Тесты проверяют заявленную семантику, а не проходят вхолостую: конкурентный claim двух
независимых сессий через `std::barrier`/`std::jthread` (REQ-71-1), restart с восстановлением
неподтверждённой persistent записи, fan-out с независимыми копиями и offline durable через
перезапуск (REQ-71-2), матрица ключей durable/legacy и пограничных имён (REQ-71-3), noLocal
по идентичности соединения и client ID (REQ-71-4), снимок браузера, не зависящий от
send/claim/ack (REQ-71-5), request/reply через JMSReplyTo/JMSCorrelationID и границы
владения временного назначения (REQ-71-6). Требований без теста и тестов-сирот trace не нашёл.

## Объём задачи и целевой набор (Стандарт 18)

Дифф лежит внутри объявленного `target_files` (18 изменённых + 5 новых файлов — все в наборе).
Изменений «мимо SDD 71» нет. Публичный API изменён только аддитивно: существующие сигнатуры
`Consumer`, `Producer`, `Destination`, `Session` сохранены.

## Инварианты и ADR

- ADR-0009 (атомарный claim, владение, реестр durable): соблюдён. `DeliveryState::claim` атомарен
  под `DestinationMutex`; `publish` фиксирует состав подписок в точке публикации и раздаёт копии;
  реестр `Exchange` связывает ключ подписки с поколением и переживает перезапуск.
- ADR-0006 (исключения из деструктора): `Consumer::finishSession() noexcept`, `~Destination`,
  `~Connection`, `~DeliveryScheduler` ловят и логируют исключения; дефект не воспроизводится.
- ADR-0008 (заголовки ↔ кешированные байты): готовая запись полностью материализована (recv не
  читает кеш). `persist` делает `refreshCachedStorageBytes()` → `append` → `clear`;
  `patchCachedDeliveryTime` сохраняется парно там, где меняется заголовок
  (`Producer::prepareCommit`, `DeliveryScheduler` clamp). Регрессии кешированных заголовков нет.
- Парные изменения: `Destination::save ↔ deliverCommitted` переписаны согласованно; кодер↔декодер
  и `MessageProperty ↔ PocoAnyVisitor` не затронуты.
- Сборка чиста по предупреждениям.

## Находка 1 (блокер): дрейф замкнутого мира (Стандарт 5)

Сверка «новые публичные сущности диффа ↔ Entity inventory SDD» выявила расхождения обоих
направлений. По роли рецензента «сущность в коде вне реестра — дрейф, блокер до MR».

Сущности в коде, отсутствующие в реестре (дрейф):

- `Session::createConsumer(const Destination::Ptr&, const std::string& selector, bool noLocal)` —
  новый публичный метод (unshared nondurable с noLocal, REQ-71-4). В inventory нет.
- `Session::createDurableConsumer(const Destination::Ptr&, const std::string&, const std::string&,
  bool noLocal)` — новый публичный метод (unshared durable с noLocal, REQ-71-4). В inventory нет.
- `TemporaryDestination::remove(Connection&)` — новый публичный метод (удаление временного
  назначения, REQ-71-6). В inventory нет (зарегистрирован только тип E6).
- `DestinationMutex` — новый тип (спинлок, `server/src/Destination.h`). В inventory нет.
- `DeliveryState` — новый тип (ready set очереди/подписки, `server/src/Destination.h`). В inventory нет.

Сущности в реестре, отсутствующие в коде (недоделанный объём):

- E1 `DestinationIdentity` — типа в коде нет; реализация использует `SubscriptionIdentity`
  (пара client ID, имя) и `Destination::Ptr` для ссылки на назначение.
- E4 `DeliveryClaim` — типа в коде нет; выборка реализована как `DeliveryState::claim`,
  возвращающая `Message::Ptr`, без отдельного типа claim.

`DestinationMutex` и `DeliveryState` — внутренние синхронизационные типы, вынесенные в публичный
заголовок по необходимости C++; два `Session`-метода с noLocal и `TemporaryDestination::remove`
— полноценная новая публичная поверхность API. Код и тесты в рамках SDD корректны; блокер
касается сверки контракта закрытого мира, а не дефекта исполнения. Реестр правит оркестратор.

## Находка 2 (для шлюза NFR): бюджет горячего пути не подтверждён, сравнение асимметрично

REQ-71-7 требует рост CPU time/op не более 5% на существующем пути одного получателя. В
`evidence` автора и в его smoke (`bench-smoke.log`) зафиксировано:

- `AutoAck_Persistent_RoundTrip`: 4213.5 → 7240.4 ns/op, +71.84% (CV 1.12% / 3.52%) — вне бюджета.
- `AutoAck_NonPersistent_RoundTrip`: −13.60% (CV эталона 6.09% — выше порога 5% методики).
- `Topic_AutoAck_NonPersistent_RoundTrip`: +1.11% — в бюджете.

Причина подтверждена чтением кода: в новом `Consumer::recv` режим `AUTO_ACKNOWLEDGE` выполняет
синхронное подтверждение до возврата из recv — `acknowledge(*message)` → `_storage->remove(...)`
под `storageMutex`. В старой сборке recv в режиме AUTO_ACK запись из хранилища не удалял
(persistent запись «протекала»). Сопоставляемые операции несимметричны: эталон
`AutoAck_Persistent_RoundTrip` из `benchmarks/baseline.md` измерял send+recv без удаления,
кандидат — send+recv+синхронное удаление. Это правка корректности ack, а не деградация той же
операции; решение о приёме бюджета и пересъёме baseline — за шлюзом NFR по цепочке. Рецензент
фиксирует: бюджет REQ-71-7 по persistent очереди не подтверждён, полный прогон шлюза (без
`--smoke`, ≥7 повторов) автором не выполнялся (в `evidence` только smoke).

## Вывод

Код и тесты SDD 71 собираются без предупреждений, 171/171 тестов проходят, трассируемость 0
ошибок, инварианты ADR-0006/0008/0009 соблюдены, объём задачи и целевой набор в границах.
Отказ — по Стандарту 5: реестр сущностей SDD разошёлся с публичной поверхностью реализации
(Находка 1), блокер «до MR», устраняется сверкой `Entity inventory` с кодом (оркестратор), не
правкой кода рецензентом. Находка 2 адресована шлюзу NFR и не является основанием данного
вердикта, но должна быть закрыта до полной приёмки.

---

# Итерация 2 (ревью R2)

- Класс: K4 (свидетельство, замороженное во времени)
- Стадия: reviewer (AEF, Стандарт 13), модель deepseek-v4-pro (автор — gpt-6.1-sol)
- Дата: 2026-11-16, Spec 71, iteration 2
- Состояние R1: stash `ebb38c5402f31ec6d896efeacc8853fab4956dc1` (`reviewed-candidate-stash`)
- Вердикт: **approved**

## Область итерации 2

Сверка против состояния R1 по scoped diff затронула: `ConcurrentLinearStorage.{h,cpp}`,
`Consumer::recv/acknowledge`, `Destination::persist`, `tests/StorageWorkerResilienceTest.cpp`,
`tests/nfr71.py`, плюс повторная сверка `Entity inventory` SDD v0.5. Прочие части, уже принятые
в R1 (REQ-71-1…6, трассируемость, ADR-0006/0008/0009, парные изменения), заново не
разбираются; прогон тестов повторён целиком.

## Собственный прогон (Стандарт 15)

Сборка из отдельного каталога `handoffs/current/71/build/reviewer-r2` (Debug, Ninja,
`CMAKE_PREFIX_PATH=…/cmake-build-debug/vcpkg_installed/arm64-osx`, macos-expat.cmake).
Отпечаток дерева до и после совпал и равен `evidence_tree` автора:
`tree:287bfa69d990bd878ff03385c83757a4f1d5c3e985af6b0a86535e7dd6d3f7b0` — входы за время
проверки не менялись.

- configure Debug: exit 0 → `handoffs/current/71/logs/reviewer-r2-configure-debug.log`
- build --parallel 8: exit 0, 52 цели, без предупреждений → `handoffs/current/71/logs/reviewer-r2-build.log`
- ./tiny_mq --gtest_filter='-*Bench*': 172/172 PASSED (28 suites) → `handoffs/current/71/logs/reviewer-r2-verify.log`
- trace.py tests … --id-pattern 'REQ-[0-9]{2}-[0-9]+': ошибок 0 → `handoffs/current/71/logs/trace-tests.log`
- stress: `StorageWorkerResilienceTest.*` 20/20, `DestinationContractTest.*` 5/5, без флаков

Сверка с логами автора: build exit 0, verify 172/172, trace 0 ошибок. Расхождений нет —
факт прогона автора подтверждён; признаков «сфабрикованной проверки» нет.

## Находка 1 итерации 1 — закрыта (Стандарт 5)

SDD v0.5 выровняла `Entity inventory` с кодом. Все 32 сущности E1–E32 присутствуют в коде:
E20 `Session::createConsumer(…,noLocal)`, E21 `Session::createDurableConsumer(…,noLocal)`,
E22 `TemporaryDestination::remove`, E24 `DeliveryState`, E25 `DestinationMutex`, E26
`Destination::Subscription`, E27 `Exchange::SubscriptionDefinition`, E28
`Exchange::RegisteredSubscription`, E29 `generation`, E30 `_storageId`, E31/E32
`ConcurrentLinearStorage::appendDelivery/removeDelivery` — все сверены по заголовкам.
Прежние «лишние» пункты реестра исправлены: E1 теперь `Destination` (а не `DestinationIdentity`),
E4 — метод `DeliveryState::claim` (а не отдельный тип `DeliveryClaim`). Новые публичные
сущности итерации 2 — только E31/E32, они в реестре. Дрейфа вне реестра нет; недоделанного
объёма (реестр → код) нет.

## Хранилище: гонки start/stop и параллельных запросов

`ConcurrentLinearStorage` переведён с STOP-операции на два мьютекса (`_lifecycleMutex`,
`_storageMutex`) и синхронные `appendDelivery`/`removeDelivery` под `_storageMutex`.
`_acceptingOperations` пишется под обоими мьютексами, читается под одним из них — гонки на
флаге нет. Порядок блокировок согласован (`DeliveryState::storageMutex → lifecycle → storage`;
`removeDelivery` берёт только storage, никто не берёт storage раньше lifecycle) — инверсии нет.
`stop()` под обоими мьютексами кладёт nullptr-маркер тем же producer-токеном после всей
принятой работы (FIFO) и не ждёт completion от уже завершившегося worker; повторный `stop()`
идемпотентен. `start()`/`stop()` сериализуются на `_lifecycleMutex`; `stop` сразу после
`start` не застревает — подтверждено тестом `StorageWorkerResilienceTest.StopImmediatelyAfterStart`
и 20-кратным прогоном набора.

## Failure/retry и целостность записи

`Consumer::recv` в AUTO_ACK/expired ветке ловит ошибку `acknowledge` и возвращает запись в
готовое состояние (`_queue->enqueue`) перед перебросом исключения — claim при ошибке
синхронного AUTO_ACK не теряется. `removeDelivery` возвращает `false` только на ошибке записи
и не трогает состояние при исключении; `Tom::remove` при сбое записи сбрасывает `deleted=0`.
`Destination::persist` перешёл на `appendDelivery`, который либо бросает (`InvalidArgumentException`
при пустых данных, `RuntimeException` при сбое записи), либо возвращает валидную запись
(`Tom::append` всегда проставляет `tomId = _id`), поэтому прежняя sentinel-проверка
`tomId == max` корректно удалена. `Storage::remove` сохраняет обслуживание B-tree индекса и на
синхронном пути.

## REQ-71-7 (шлюз NFR, не основание вердикта)

Регистрация бенчмарков соответствует SDD v0.5: `AutoAck_Persistent_RoundTrip` через
`BENCHMARK_DEFINE_F` + `BENCHMARK_REGISTER_F(...)->MeasureProcessCPUTime()` (имя
`/process_time`), два non-persistent сценария без изменения регистрации, профили
`Destination_ConcurrentClaim/process_time` и `Destination_Fanout/1,2,4`. Семантика сравнения
симметрична постановке: эталон получает persistent-запись без удаления, кандидат синхронно
отмечает её удалённой до возврата из `receive`. Smoke (2 повтора ABBA) даёт кандидата быстрее
эталона по persistent, но CV эталона и topic-кандидата превышают 5% — это вопрос стабильности
измерения полного прогона (≥7 повторов), а не данного ревью. Полный независимый NFR — за
отдельным шлюзом.

## Вывод итерации 2

Замечание итерации 1 (дрейф реестра) устранено SDD v0.5 и подтверждено сверкой кода; новый
внутренний storage path корректен по гонкам, failure/retry и целостности записи; сборка,
трассируемость и 172/172 тестов воспроизведены собственным прогоном. Вердикт: approved.
Бюджет REQ-71-7 остаётся на шлюзе NFR.

---

# Итерация 3 (ревью R3)

- Класс: K4 (свидетельство, замороженное во времени)
- Стадия: reviewer (AEF, Стандарт 13), модель deepseek-v4-pro (автор — gpt-6.1-sol)
- Дата: 2026-11-16, Spec 71, iteration 3, SDD v0.6
- Состояние R1: stash `2d1041d87c0673dc46db42032c955777c796f153` (`conformance-candidate-stash`)
- Вердикт: **approved**

## Область итерации 3

Итерация 3 отвечает на отказ conformance и меняет только `tests/DestinationContractTest.cpp`
(новые и уточнённые assertions) и реестр сущностей SDD v0.6 (E33, E34; строки Test plan об
историческом читателе). Правок production-кода нет — проверено двумя независимыми способами:

- `git diff --exit-code "$(cat handoffs/current/71/conformance-candidate-stash)" -- server
  CMakeLists.txt tests/BenchmarkTest.cpp` → exit 0;
- полный tracked diff против того же stash: отличается только
  `docs/jms-spec/71-jakarta-destinations.md` (v0.5 → v0.6, правка оркестратора);
- untracked не-тестовые файлы (`server/src/QueueBrowser.cpp/.h`,
  `server/src/SubscriptionRegistry.h`, `tests/nfr71.py`) побайтово совпадают с untracked-родителем
  stash `2d1041d^3` (`git diff --no-index` пуст).

Единственный содержательный дельта — `tests/DestinationContractTest.cpp` (557 → 817 строк),
точечный дифф новых assertions прочитан целиком против предыдущей редакции из `2d1041d^3`.

## Собственный прогон (Стандарт 15)

Инкрементальная сборка в `handoffs/current/71/build/reviewer-r2` (Debug, Ninja,
`CMAKE_PREFIX_PATH=…/cmake-build-debug/vcpkg_installed/arm64-osx`): re-run CMake + 47 целей,
exit 0, без предупреждений под `-Wall -Werror -Wextra -Wshadow`.

- build → `handoffs/current/71/logs/reviewer-r3-build.log` (exit 0)
- `./tiny_mq --gtest_filter='-*Bench*'` → 172/172 PASSED, 28 suites → `reviewer-r3-verify.log`
- `--gtest_repeat=10` на четырёх доработанных сценариях → 10×4 PASSED, без флаков →
  `reviewer-r3-repeat.log`
- `trace.py tests … --id-pattern 'REQ-[0-9]{2}-[0-9]+'` → ошибок 0 → `trace-tests.log`
- `git diff --check` + проверка пробелов untracked теста → exit 0 → `reviewer-r3-diff-check.log`
- Rollback: `DestinationContractTest.DurableIdentityMatrix` с
  `TINY_MQ_LEGACY_FIXTURE=$(mktemp -d)` → PASSED, экспорт legacy-backlog в пустой каталог →
  `reviewer-r3-legacy-candidate.log`; затем старый reader из
  `$(cat handoffs/current/71/legacy-baseline.dir)/debug/tiny_mq` с тем же fixture и фильтром
  `LegacyDestinationReader.OpensCandidateBacklog` → PASSED → `reviewer-r3-legacy-reader.log`.

Отпечаток дерева до прогона и сразу после последнего прогона совпал и равен
`tree:b585f790a9dae2032cc4df7e80297c4e4f2ff2cf651ca24b6f3fb605afd7e636`
(равен и `evidence_tree` пакета автора) → `reviewer-r3-fingerprint-before.log` /
`reviewer-r3-fingerprint-after.log`. Входы за время проверки не менялись.

Сверка с логами автора (`producer-r3-*`): build debug/rel exit 0, verify 172/172, repeat
10×4 PASSED, trace 0 ошибок, legacy reader PASSED, diff-check exit 0. Расхождений нет —
факт прогона автора подтверждён моим прогоном; признаков «сфабрикованной проверки» нет.

## E33 / E34 (дрейф замкнутого мира, Стандарт 5)

Прежний блокер conformance «`DeliveryState::Waiter` отсутствует в реестре» закрыт SDD v0.6 и
сверен с кодом: E33 `DeliveryState::Waiter` объявлен в `server/src/Destination.h:64`
(selector + `condition_variable_any`, адресное пробуждение — соответствует формулировке
«внутреннее ожидание с селектором и адресным пробуждением»); E34 `ScheduledMessageProbe`
объявлен в `tests/DestinationContractTest.cpp:712` и служит наблюдаемым объектом освобождения
отложенной копии через `weak_ptr`-маркер. Обе сущности в реестре и в коде; дрейфа в обоих
направлениях нет. Production-код не менялся, поэтому прочая сверка E1–E32 из R2 сохраняется.

## Многопоточная проверка обеих shared-групп (REQ-71-2)

Новый блок `SubscriptionFanout` для `durable ∈ {false, true}` создаёт пару consumer на двух
сессиях (`createSharedConsumer` / `createSharedDurableConsumer`), барьер `std::barrier start(3)`
и два `std::jthread`, 25 раундов, в каждом по одной non-persistent и одной persistent публикации.
Утверждается, что объединённый набор полученных значений равен ожидаемому без дублей
(`combined == expected` после сортировки) и оба consumer после цикла пусты. Это прямое
наблюдение единственного claim при конкурирующих активных участниках для обоих видов shared.

## Rollback: исторический читатель на фиксированном ядре

По `legacy-baseline-build.log` ядро базовой линии — фиксированный ref
`7770feb6a01c63f23b4997ac709e83f9ec6e29c8` («Separate broker and language client source
directories (#10)»), потомок `0e445705…` (ref методики REQ-71-7), предок `HEAD` и
`remotes/github/main`; поверх него добавлены только `tests/LegacyDestinationReader.cpp` и
`CMake target_sources`. Каталог baseline не содержит ни одного файла SDD 71
(`DestinationContractTest.cpp`, `QueueBrowser.cpp`, `SubscriptionRegistry.h` отсутствуют) —
это публичный main до SDD 71. Читатель открывает прежнюю подписку прежним C++ API
(`createDurableConsumer(topic, "same")`) и получает `"legacy"` из экспортированной копии.
Мой прогон кандидата экспортировал `…/2725311761615597203/durable-same/{1.tom,
fc01f7c4-….linear.storage.index}` в пустой fixture, старый бинарник прочитал его: PASSED.
Пункт Test plan v0.6 («исторический читатель … на отдельной копии данных из новой сборки;
эталон читателя — публичный main до SDD 71 с дополнительным тестовым входом») выполнен.

## Закрытие блокеров conformance (сценарий → новый assertion)

- REQ-71-2: параллельный claim внутри shared → блок «shared nondurable/durable concurrent
  claim yields every publication exactly once» (barrier + jthread, оба вида).
- REQ-71-3 (shared durable): активные конфликты по топику и по селектору, запрет unsubscribe
  и замены при неподтверждённом claim, trim-эквивалентный selector сохраняет backlog,
  разрешённая замена удаляет backlog и принимает новые matches — отдельные assertions.
- REQ-71-3 (unshared durable): замена только топика, только selector, только noLocal — по
  отдельности, каждая сбрасывает backlog; исчисленное старое поколение при прерывании до
  переключения проверено «pre-swap restart» через `registry.pending` + копию нового поколения.
- REQ-71-3 (legacy): запуск старой сборки на копии — `TINY_MQ_LEGACY_FIXTURE` + отдельный
  `LegacyDestinationReader.OpensCandidateBacklog` на бинарнике baseline.
- REQ-71-5: `expectSnapshot` сверяет полный упорядоченный состав и заголовки (не только
  размер 3/4); `browse.recover()` не дублирует и не регистрирует доставку (jmsDeliveryCount=1,
  redelivered=false); expiration после снятия снимка исключает запись из нового снимка и не
  трогает старый; receive-флаги и неизменность первого снимка под recover/ack проверены явно.
- REQ-71-6: матрица 2×2 `queue/topic × explicit delete/owner close`; `ScheduledMessageProbe`
  наблюдает отмену отложенной копии до её deadline (`scheduled.expired()` при активном
  consumer и `recv(0)==nullptr`); уникальность URI временных назначений.

## Трассируемость (Стандарт 3)

`trace.py tests` → 0 ошибок: каждый `Test:` из Test plan имеет исполняемый тест, требований без
теста и тестов-сирот нет. REQ-71-7 остаётся за отдельным runner `NfrGateTest` (шлюз NFR,
approved итерации 2, к данному диффу не относится).

## Вывод итерации 3

Итерация 3 не трогает production-код; все блокеры отказа conformance закрыты точечными
assertions, E33/E34 сверены с реестром, многопоточный claim проверен для обеих shared-групп,
rollback-читатель воспроизведён на фиксированном ядре `7770feb6…`. Сборка, 172/172 тестов,
10×4 повтора, trace 0 ошибок и дифф-check воспроизведены собственным прогоном. Вердикт:
approved.

## Независимая критика спецификации

DeepSeek V4 Pro выдал `needs-work` в первых двух раундах (9 и 3 замечания), затем
`clean` в раундах 3–6. Поправки закрепили время жизни claim, точку снимка браузера,
правила noLocal и имён, ключ shared durable без client ID, границы временных назначений,
наблюдаемость совместимости и измерение полного CPU процесса. Финальная v0.6
прошла раунд 6 без открытых замечаний. Владелец разово разрешил шесть раундов критики
и шестнадцать переходов для этой SDD. Критика и ревью выполнялись через
`codex -p deepseek-pro` напрямую.

## Повтор ревью после диагностики GCC

DeepSeek V4 Pro повторно выдал **approved**. В `a8f901d` три условных вызова
`EXPECT_THROW` получили фигурные скобки. Смысл теста и серверные исходники совпали
с проверенной версией. Независимый прогон `DestinationContractTest.TemporaryReplyLifecycle`
прошёл после пересборки. Вердикт также подтверждает замечания платформенного ревью
по охвату CI, конкуренции запусков, граничному значению 5% и диагностике legacy-эталона.
[Тест](../../tests/DestinationContractTest.cpp#L724) ·
[Свидетельство](../../handoffs/current/71/logs/reviewer-r3-gcc-verdict.md)
