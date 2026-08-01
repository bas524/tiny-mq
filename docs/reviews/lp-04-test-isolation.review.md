# Review · LP-04 test storage isolation

| field | value |
| --- | --- |
| Task | [`tasks/linux-port/04-test-storage-isolation.md`](../../tasks/linux-port/04-test-storage-isolation.md) |
| Stage | reviewer |
| Iteration | 1 |
| Branch | `fix-test-storage-isolation` (base `main` @ 4472394) |
| Verdict | **approved** — задача закрыта, остаточные риски документированы |
| Date | 2026-08-01 |
| Model | MiniMax-M3 (R2, default-deny) |

---

## TL;DR

Изменение Producer-а узко закрывает именно ту задачу, что в спеке: ни один
`TEST_F`-набор, конструирующий `Exchange` или `ConcurrentLinearStorage`, после
коммита не пишет в общий `./tiny-mq`. Семантика тестов не тронута — диф
содержит только SetUp/TearDown баз-path plumbing и одно переименование
локальных `basePath` внутри `ExpirationTest`. Build чистый, репозиторий не
замусорен.

Остаточные наблюдения — не блокеры, оформлены ниже как forward-looking notes,
а не как новые долги этой задачи:

1. **Параллельные процессы** (две одновременные копии бинаря) будут писать в
   один и тот же каталог — нет pid-суффикса. Pre-existing risk (ничего в
   `ConcurrentLinearStorage` не ставит `flock`/`O_EXCL`). Допустимо для
   закрытия этой задачи; фиксируется ниже как «вне скоупа, но в будущем
   стоит рассмотреть».
2. **SetUp-side wipe** действительно затирает улики предыдущего failed run,
   но это явный design choice, документирован в комментарии хелпера
   («defensive, in case a previous run was killed mid-test»).
3. **Один тест вне обычной изоляции** — `StorageWorkerResilienceTest` —
   оставлен с хардкод-путями по spec-design (TEST, не TEST_F; каждое
   `TEST` имеет pre/post cleanup на месте), что соответствует
   «suites_left_untouched_already_isolated» в Producer handoff.

Никаких подгоночных правок ожидаемых значений, чисел сообщений или таймингов
в диффе нет — проверено покаждой строке.

## Test plan ↔ реализация

| Test plan пункт | Реализация | Статус |
| --- | --- | --- |
| `--gtest_repeat=20` без предварительной чистки — 20/20 PASSED | SetUp/TearDown чистят `./tiny-mq-test-storage/<SuiteName>` | Проверено человеком независимо (см. `producer.json.evidence`); spot-check `--gtest_repeat=3` на `ClientAckTest.*` и единичный `ClientAckTest.testMixedPersistenceOrdering` — PASS, каталоги пусты |
| `--gtest_shuffle --gtest_repeat=5` чисто | Смоук выходит за рамки ревью (см. prompt: «это НЕ нужно перепроверять») | Producer доказал, не воспроизвожу |
| Общие каталоги после прогона не создаются | `./tiny-mq` и `./bench-mq` отсутствуют; `./tiny-mq-test-storage/` остаётся пустой parent dir | Проверено: после `--gtest_filter='ClientAckTest.*' --gtest_repeat=3` parent пуст |
| Пройти по всем наборам, использующим Exchange/сторадж | Полный обход — см. раздел «Полнота охвата» | Покрыто |

## 1. Полнота охвата

Покаждой: `grep -rE '\./tiny-mq|\./bench-mq|"tiny-mq"|"bench-mq"|kBaseDir|basePath\s*=' tests/`.

| Находка | Файл | Решение | Где живёт | Оценка |
| --- | --- | --- | --- | --- |
| 14 наборов с `tiny_mq::Exchange("./tiny-mq")` | `ClientAckTest`, `ConnectionLifecycleTest` (2 фикстуры), `DisableHeadersTest`, `DupsOkAckTest`, `DurableSubscriberTest`, `ExpirationTest`, `MapMessageTest`, `ObjectMessageTest`, `SelectorTest`, `SendOptionsTest`, `SimpleTest`, `TopicTest`, `TransactionTest`, `PriorityOrderingTest` | Переведены на `CurrentTestSuiteStorageDir()` + `RemoveTestStorageDir()` в SetUp и TearDown | `tests/<Suite>Test.cpp:18-...` | OK |
| `LinearStorageTest` использует `Poco::Path("./tiny-mq/storage-test")` | `tests/LinearStorageTest.cpp:35` | Переведён на `Poco::Path(CurrentTestSuiteStorageDir())`, SetUp добавлена defensive wipe | `tests/LinearStorageTest.cpp:38-44` | OK |
| `ExpirationTest::testExpiredMessageSweptFromStorage` использует `./tiny-mq/expiration-sweep` | `tests/ExpirationTest.cpp:69` (бывшая строка 66) | Переименовано в `./tiny-mq-test-storage/ExpirationTest-sweep`; inline pre/post cleanup сохранён | `tests/ExpirationTest.cpp:69,103-104` | OK — путь лежит рядом с suite-каталогом (sibling, не child), но не пересекается по namespace, поэтому фикстура его не трогает; pre-wipe в начале тела нет — но это поведение НЕ изменилось относительно до-PR (было то же) |
| `ExpirationTest::testSweepFiresUnderContinuousLoad` то же | `tests/ExpirationTest.cpp:134` | Переименовано в `./tiny-mq-test-storage/ExpirationTest-load`; pre/post cleanup сохранён | `tests/ExpirationTest.cpp:134,164-165` | OK (см. выше) |
| `PersistentTransactionTest` использует `./tiny-mq-persistent-test` | `tests/PersistentTransactionTest.cpp:21,243,284` | Оставлен без изменений. Уже имел per-fixture cleanup, явно отмечен в Producer handoff как «pre-existing working sample» | — | OK — действительно изолирован на уровне фикстуры, SetUp/TearDown чистят |
| `StorageWorkerResilienceTest::InvalidOperationIsLoggedNotFatal` использует `./tiny-mq-lp03-resilience` | `tests/StorageWorkerResilienceTest.cpp:61` | Оставлен. Это TEST (не TEST_F), inline pre/post cleanup внутри каждого TEST. Явно отмечен в handoff как «already isolated per-test» | — | OK — pre-existing isolation, TEST-уровень чище suite-уровня |
| `StorageWorkerResilienceTest::NoWorkerThreadSurvivesExchangeDestruction` то же | `tests/StorageWorkerResilienceTest.cpp:110` | Оставлен по тем же основаниям | — | OK |
| `BenchmarkFixture` использует `./bench-mq` | `tests/BenchmarkTest.cpp:45,55-57` | Оставлен. Task plan явно исключает `*Bench*` из repeat/shuffle run-ов. Producer отметил как «out of scope per task Test plan» | — | OK — task plan действительно исключает |
| `ConnectionLifecycleTest.cpp:81,88` — `EXPECT_EQ("tiny-mq", md.providerName)` | Это строковый литерал имени провайдера, **не путь** | — | — | НЕ finding, ложное срабатывание grep-а |

**Полнота покрытия:** все наборы, перечисленные в спеке как примеры
(`ClientAckTest.testMixedPersistenceOrdering`, `ExpirationTest.testExpiredPersistentMessageDroppedOnRecv`),
а также все смежные — переведены. Сторонние хардкод-пути либо осмысленно
оставлены (как у `PersistentTransactionTest`), либо находятся вне скоупа
(`BenchmarkTest`), либо уже per-test изолированы (`StorageWorkerResilienceTest`).

## 2. Ожидаемые значения, границы, тайминги — не подогнаны

Покаждой: `git diff <test> | grep -E '^[+-]' | grep -v '^[+-]{3}'`.

В КАЖДОМ из 16 модифицированных файлов диффы содержат ТОЛЬКО:

- `#include "TestHelper.h"` (LinearStorageTest, MapMessageTest) — там, где раньше хелпер не подключали.
- Переписывание `SetUp`/`TearDown` в multi-line форму с wipe-ом.
- Перевод базового пути `Exchange` c `"./tiny-mq"` / `kBaseDir` / `Poco::Path(...)` на `CurrentTestSuiteStorageDir()`.
- Внутри `PriorityOrderingTest::testRestartPreservesBanding`: одна строка переименования `dir` (`./tiny-mq/restart-priority-test` → `<suiteDir>/restart-priority-test`) и удаление inline pre-wipe, ставшим redundant благодаря фикстуре.

**Ни одна** строка из диффа не затрагивает:

- `EXPECT_*` / `ASSERT_*` с числами/таймингами
- `kLow`, `kHigh`, `constexpr int kX` значения
- `Poco::Thread::sleep(...)` аргументы
- числа в `nowMs() ± 1000`, `setSweepIntervalMicros(...)`, `recv(...)` timeout-ы
- `kMaxMessages`, размеры payload-ов, `producer->send(...)` квантити

Семантика тестов сохранена полностью. Если какой-то набор сейчас падает
после изоляции — это будет находка, а не повод править константу. По
producer evidence (`new_failures_after_isolation: "None"`) таких находок нет,
и перепроверять не нужно (это часть критерия, который человек гонял
отдельно и подтвердил: 20/20 PASSED на full suite).

## 3. Изоляция по набору vs по тесту

`CurrentTestSuiteStorageDir()` возвращает `./tiny-mq-test-storage/<SuiteName>`.
`SuiteName` одинаков для всех `TEST_F(X, ...)` внутри одного набора.

**Оценка достаточности:** достаточно для симптома из спеки.

- Внутри suite SetUp wipe-ает suite-dir перед КАЖДЫМ тестом. Это значит, что
  тест A, идущий после теста B в suite X, не видит остатков B.
- GTest по умолчанию запускает тесты в source order, но `--gtest_shuffle`
  перемешивает. Перемешивание не страшно: SetUp wipe-ает всегда.
- Destination имена в тестах берутся из `CurrentTestName`, что уже гарантирует
  per-test namespace внутри suite-level storage.
- Два теста в `ExpirationTest` используют собственные `basePath`-ы
  (`./tiny-mq-test-storage/ExpirationTest-sweep`,
  `./tiny-mq-test-storage/ExpirationTest-load`), которые являются sibling-ами
  suite-dir (НЕ children). Это означает, что фикстура их НЕ трогает
  автоматически, но это поведение унаследовано от до-ЛП-04 реализации
  (раньше они лежали под `./tiny-mq/` как sibling-и ./tiny-mq-root и точно
  так же имели inline pre/post cleanup). Net effect: без изменений.

**Слабое место (но не блокер):** если бы внутри suite X один тест
рассчитывал на сообщения или persistent state, оставленные другим тестом
этого же suite, — это был бы долг. Я просмотрел все 16 diff-нутых файлов и
не нашёл такого: каждый тест создаёт своих Producer-ов/Consumer-ов на
`CurrentTestName`-destination и стартует с пустого состояния благодаря
SetUp-wipe.

## 4. Параллельные прогоны

Риск реален: `./tiny-mq-test-storage/<SuiteName>` идентичен для любого
процесса, запустившего тот же suite. В `ConcurrentLinearStorage.{h,cpp}` нет
`flock`/`O_EXCL`/`Poco::File::createFile` (последнее только создаёт пустой
файл, не лочит). Так что:

- Два CI-job-а одновременно на одном workspace → гонка на уровне файлов.
- Локальный запуск во время CI job на shared workspace → то же.
- Будущая параллелизация (`--gtest_repeat` с разнесением по воркерам или
  gtest-parallel) усугубит.

**Pre-existing risk:** тот же риск был до ЛП-04 — каталог `./tiny-mq` тоже
был глобальным. ЛП-04 не ухудшает и не улучшает ситуацию для concurrent.

**Решение для будущего (вне скоупа этого тикета):** модифицировать
`CurrentTestSuiteStorageDir()` так, чтобы включать `getpid()` или,
предпочтительнее, использовать `mkdtemp`/`Poco::TemporaryFile` для
уникальной tmpdir. Стоимость — одна строка в `TestHelper.h`. Ничего не
меняется в семантике тестов. Это скорее архитектурная задача (T2-tier),
чем блокер ЛП-04.

Для текущей задачи (изоляция от replay `--gtest_repeat`) suite-level path
достаточен и не противоречит Test plan.

## 5. SetUp + TearDown wipe — диагностика и живучесть

**Defensive wipe в SetUp:** реализация

```cpp
void FooTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
```

Делает wipe ДО того, как Exchange начинает писать. Это гарантирует, что после
жёсткого падения (SIGKILL/segfault/SIGTERM от CI shutdown) следующая итерация
начнёт с чистого каталога. Без этого `--gtest_repeat` в текущей реализации
test discovery снова бы натыкался на stale state — потому что GTest вызывает
TearDown, но НЕ гарантирует вызов TearDown при process-crash-ах (а они
неизбежны в стресс-тестах вроде LongRunningTest, который появится позже).

**Маскировка ликов:** да, частичная. Если тест A падает с crash в production
path, и НИ SetUp ни TearDown не успели записать улики (например, корка
процесса при попытке `Poco::File::remove(true)`), следующий SetUp B
молча сотрёт. Но это та же trade-off, что и в production storage recovery —
на crash ты не можешь полагаться на удачу, ты должен полагаться на log lines
и/или core dump, что и происходит.

Возможное улучшение (вне скоупа): при `GTEST_SKIP_KEEP_STORAGE=1`
сохранять каталог упавшего теста для посмертной диагностики. Не критично —
текущее поведение совпадает с тем, как падают unit-тесты во многих проектах,
и не мешает репродукции через `--gtest_repeat=N` (любой N ≥ 2 теперь
воспроизводимо PASS-ит).

**Живучесть при падении:** GTest вызывает TearDown при `ASSERT_*` fail и
при test-level exception (через `AssertionResult`-механизм). То есть
падающий ASSERT НЕ оставляет данных на диске: `exchange.reset()` +
`RemoveTestStorageDir()` отрабатывают. И артефакты для разбора ASSERT
failure лежат в GTest-логе, а не в storage (это верно для любого unit-теста
брокера — содержимое storage это и есть то, что тестируется, и оно
воспроизводимо детерминированно с нуля благодаря wipe-у).

## 6. -Werror, чистота репозитория

**Build:** `cmake --build --preset debug --parallel` через `user-debug`
preset (vcpkg toolchain) — clean, единственное диагностическое сообщение
`ld: warning: ignoring duplicate libraries: '…libPocoFoundationd.a',
'…libgtest.a'` — pre-existing в CMake, не из этого PR, не считается -Werror
break. Проверено принудительным touch `tests/TestHelper.h` →
рекомпиляция 16 затронутых `.cpp` (по одному объектнику на файл) — никаких
`-Wall -Werror -Wextra -Wshadow` срабатываний.

**Репозиторий:** единственный новый каталог `./tiny-mq-test-storage/` —
пустой parent после прогона, как и отмечено в Producer handoff. Никаких
`./tiny-mq` или `./bench-mq` на диске не остаётся. `.claude/` нетронут.

## 7. Остаточные мелочи (не блокеры)

- **Пустой parent dir:** `./tiny-mq-test-storage/` остаётся как пустая
  директория после завершения бинаря. Не мусор — доказательство, что cleanup
  корректно прошёл. CI workflow не должен его удалять (это уже не нужно),
  но если хочется чистого дерева — однострочник в `Exchange` main или
  cleanup-task в `EndToEnd` test runner. Сейчас не критично.
- **Orphans от прошлых прогонов:** `./tiny-mq/expiration-sweep`,
  `./tiny-mq/expiration-load`, `./tiny-mq/storage-test` (от старой
  реализации) могли остаться в workspace до этого PR. PR их не убирает,
  но они вне скоупа — на семантику тестов не влияют. CI pre-cleanup их
  мог бы убрать, но это уже не нужно.
- **`PersistentTransactionTest` использует хардкод-путь** вместо helper-а.
  Согласовано в Producer handoff как «pre-existing working sample», но если
  когда-нибудь понадобится расширить helper на подобные тесты — стоит
  рассмотреть. Сейчас это не долг.

## Блокеры

Нет.

## Проверенные команды

```bash
cmake --preset user-debug \
  -DCMAKE_TOOLCHAIN_FILE=/Users/a.bychuk/coding/pet/tiny-mq/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset debug --parallel
# touch tests/TestHelper.h && cd cmake-build-debug && ninja
#   → 17 пересборок, 0 -Werror-срабатываний

cmake-build-debug/tiny_mq \
  --gtest_filter='ClientAckTest.testMixedPersistenceOrdering' --gtest_repeat=3
# 3/3 PASS; ls tiny-mq-test-storage/ → пусто

cmake-build-debug/tiny_mq \
  --gtest_filter='ClientAckTest.*' --gtest_repeat=3
# 4 теста × 3 итерации = 12/12 PASS; ls tiny-mq-test-storage/ → пусто

grep -rn '\./tiny-mq\|\./bench-mq\|"tiny-mq"\|"bench-mq"\|kBaseDir\|basePath\s*=' tests/
# содержит только:
#   - миграции Producer-а на ./tiny-mq-test-storage/<Suite>...
#   - уже изолированные PersistentTransactionTest, StorageWorkerResilienceTest, BenchmarkTest
#   - литерал "tiny-mq" в ConnectionLifecycleTest.cpp:81,88 как provider name
```

Ограничения evidence: полный прогон `--gtest_repeat=20` и
`--gtest_shuffle --gtest_repeat=5` не воспроизвожу, поскольку это уже
подтверждено человеком независимо (см. prompt). Дополнительно проверены
suite-level cleanup после коротких повторов, что перекрывает существенный
фрагмент критерия.

## Verdict

**Approved, iteration 1.** Задача закрыта полностью: полнота охвата
проверена покаждой покаждому `tests/*.cpp`, ожидаемые значения не подогнаны,
билд чист, репозиторий не замусорен. Остаточные наблюдения
(параллельные прогоны, optional diagnostic preservation) явно отмечены как
forward-looking notes и за рамки этой задачи не выходят.
