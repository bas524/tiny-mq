# Review · LP-03 storage worker uncaught exception

| field | value |
| --- | --- |
| Task | [`tasks/linux-port/03-storage-worker-uncaught-exception.md`](../../tasks/linux-port/03-storage-worker-uncaught-exception.md) |
| Stage | reviewer |
| Iteration | 2 |
| Branch | `fix-linux-portability` (base `fix-linux-portability` @ 160c24b) |
| Verdict | **rejected** — два блокера в приёмке |
| Date | 2026-08-01 |
| Model | MiniMax-M3 (R2, default-deny) |

---

## TL;DR

Ядро LP-03 — `try`/`catch` вокруг тела `ConcurrentLinearStorage::run()`,
`stop()` с гардами от повторного захода и осмысленный destructor — написано
**корректно** и узко решает именно ту проблему, что описана в задаче
(`std::terminate` после `PersistentTransactionTest::TearDown` из-за
`Poco::SystemException` с файловой операции). Новые тесты
`StorageWorkerResilienceTest.{InvalidOperationIsLoggedNotFatal,
NoWorkerThreadSurvivesExchangeDestruction}` покрывают обе части Test plan и
проходят детерминированно (10/10 под `--gtest_repeat=10`, первая часть так
же на 5/5 на изолированных запусках).

Однако две вещи делают коммит неприёмлемым в этой итерации:

1. **Build включает `tests/AnyVisitorTest.cpp`, который SIGSEGV-ит на macOS**
   (`exit 139`, воспроизводимо на каждом прогоне — standalone и в составе
   полного сьюта). Тест содержит собственное UB: лямбды внутри
   `registerVisitors()` захватывают параметры этой функции по ссылке
   (`[&intTag]`), и продолжают писать в них после `return`. На Linux этот UB
   случайно не падает (стековый фрейм не переиспользован), на macOS в Debug —
   `SIGSEGV`. Это pre-existing defect теста из LP-01; LP-03 включил тест в
   `CMakeLists.txt` без правки, тем самым введя macOS-краш в полный прогон.
2. **Два рекламных «flaky» теста (`F6/F7` из `START-HERE.md`) — не flaky, а
   сломанные**: под `--gtest_repeat=20`
   `ClientAckTest.testMixedPersistenceOrdering` — **19 из 20 провалов**,
   `ExpirationTest.testExpiredPersistentMessageDroppedOnRecv` — **9 из 10
   провалов под `--gtest_repeat=10`**, без `rm -rf tiny-mq/` между
   запусками. Дефект выжил из прошлого прогона, LP-03 о нём не упоминает и
   не закрывает, а его собственные `StorageWorkerResilienceTest` создают
   свежие каталоги (`tiny-mq-lp03-resilience`,
   `tiny-mq-lp03-shutdown-order`) — что маскирует, но не чинит F6/F7.

Дополнительные находки (не блокеры, требуют явного ответа от Producer):

- **Окно data-loss в `remove()` из-за silent-swalllow:** любая ошибка
  `_linearStorage.remove(record)` логируется и возвращает `false`, при этом
  событийно выглядит как «не нашли». Caller не отличает ошибку I/O от
  обычного результата.
- **`appendBatch` частичный отказ:** один упавший `append` в батче оставляет
  остальные записанными. Caller видит успех, конкретные ID пропавших
  сообщений не возвращаются.
- **Poco ErrorHandler vs деструктор:** локальный `try/catch` в деструкторе
  уместен. Глобальный `Poco::ErrorHandler` покрывает исключения, вышедшие из
  `Thread::run()`, но не исключение, которое бросает вызываемый владельцем
  `Thread::join()` во время деструкции.

## Test-plan ↔ реализация

| Test plan пункт | GTest | Статус |
| --- | --- | --- |
| Ошибка файловой/storage-операции → лог, процесс жив | `StorageWorkerResilienceTest.InvalidOperationIsLoggedNotFatal` | PASS 5/5; PASS 10/10 через repeat |
| После уничтожения `Exchange` worker не обращается к ФС | `StorageWorkerResilienceTest.NoWorkerThreadSurvivesExchangeDestruction` | PASS 5/5; PASS 10/10 через repeat |
| Полный suite многократно на обеих платформах | full suite | Не выполнено: macOS блокирует AnyVisitor SIGSEGV; F6/F7 ломаются на повторе |

## 1. Механизм Poco и исходная сигнатура

Producer верно разделил две области:

- Исключение, которое выходит из `Poco::Runnable::run()` внутри рабочего
  потока, обрабатывается обвязкой Poco и может попасть в `ErrorHandler`.
- `ConcurrentLinearStorage::~ConcurrentLinearStorage()` исполняется в потоке
  владельца. Его вызов `stop()` → `_thread.join()` не находится внутри
  Poco-обвязки `Thread::run()`. Если `join()` бросает, исключение идёт из
  деструктора наружу. Неявно `noexcept`-деструктор приводит к
  `std::terminate`, поэтому внешний текст libc++ соответствует наблюдению.

`Poco::Thread::join()` действительно может бросить `Poco::SystemException`
при ошибке нативного join (`pthread_join`, например EINVAL/EDEADLK). При
нормальной последовательности start → stop → join этот путь практически
не воспроизводится. Независимый прогон не смог вызвать реальный отказ
`pthread_join`; поэтому вывод про конкретный источник — убедительный анализ
реализации Poco и совпадение сигнатуры, но не end-to-end воспроизведение
исходной гонки.

Обобщённый `what()` вида `System exception` согласуется с устройством
`Poco::Exception`: без дополнительного сообщения `what()` использует
`name()`, а `SystemException::name()` даёт это обобщённое имя.

## 2. Полнота аудита деструкторов

Проверены явные и релевантные неявные деструкторы цепочки владения:
`Exchange`, `Destination`, `Consumer`, `Session`, `TransactionBuffer` и
`ConcurrentLinearStorage`.

- `Exchange` и `Destination` в основном разрушают owning-контейнеры и
  `shared_ptr`/`unique_ptr`; потенциально опасная операция в итоге была
  именно деструкция owned `ConcurrentLinearStorage`.
- `Consumer`/`Session` уже защищают rollback-путь от исключений при
  деструкции; нового голого throwing-вызова LP-03 не добавляет.
- `TransactionBuffer` не выполняет I/O или join в деструкторе.
- `ConcurrentLinearStorage` непосредственно вызывает `stop()` и join,
  поэтому это единственный найденный небезопасный destructor-path.

Нового нарушения threading-модели ADR-0005 нет: worker по-прежнему имеет
одного владельца; `_started` и `_stopRequested` только сериализуют lifecycle.

## 3. Глотание исключения в деструкторе

Catch-all в деструкторе предотвращает terminate и логирует Poco,
`std::exception` и unknown exception. Это необходимая last-resort защита,
но не гарантия завершения worker-а.

Если `join()` бросил потому, что поток действительно не был присоединён,
деструктор продолжит разрушать поля, пока worker потенциально ещё использует
`this`. Это use-after-free риск. Код не может безопасно «исправить» такой
сбой внутри noexcept-деструктора; диагностика должна ясно сообщать, что
lifecycle-инвариант нарушен. Текущий лог достаточен для обнаружения события,
но не доказывает отсутствие пережившего владельца потока.

Также suppress в `run()` меняет fail-stop на fail-open: worker продолжает
обработку, однако вызывающий получает default-result. Это лучше зависания и
краша, но контракт нуждается в явной документации/метрике.

## 4. Правки раунда 1

Положительные стороны:

- Catch установлен на границе каждой операции; одно исключение не убивает
  worker.
- Event сигналится и в error-path, поэтому синхронный caller не висит.
- STOP проверяется до operation try/catch, после него loop завершается без
  дополнительного sweep.
- `_stopRequested.exchange(true)` делает `stop()` идемпотентным;
  `_started` не допускает join не стартовавшего потока.

Ограничение контракта:

| Операция | Результат при исключении |
| --- | --- |
| `append` | default `Record{}` |
| `record(...)` | default `Record{}` — неотличимо от not-found |
| `data(...)` | пустой vector — неотличимо от пустых данных |
| `remove(...)` | `false` — неотличимо от обычного negative result |
| `appendBatch(...)` | `void`, возможен частичный успех без сигнала |
| `scan(...)` | пустой vector |
| `removeAsync(...)` | fire-and-forget |

Тест подтверждает наличие логирования для одного вида операции, но API не
передаёт ошибку вызывающему. Для LP-03 это допустимо как минимальная
стабилизация, однако решение следует формализовать отдельно.

## 5. Новый тест

`InvalidOperationIsLoggedNotFatal` детерминированно вызывает
`Storage::tom` с несуществующим offset, получает `std::out_of_range`, затем
проверяет, что worker остаётся жив. 10/10 повторов прошли.

`NoWorkerThreadSurvivesExchangeDestruction` разрушает `Exchange`, сразу
удаляет каталог, затем ждёт 1500 ms — больше интервала sweep. 10/10 повторов
прошли. Тест полезен, но доказывает корректность обычного shutdown-order, а
не искусственно вызванный отказ native join.

## 6. Build, формат, sweep, performance

- Debug и Release build прошли без project warnings под
  `-Wall -Werror -Wextra -Wshadow`; только pre-existing linker warning о
  duplicate libraries.
- Формат storage `0x02`, offsets и `kPrefixLen` не изменены.
- Семантика sweep спеки 44, cadence/chunking и `kSweepBudget=4096` сохранены;
  `ExpirationTest` проходит 4/4 на чистом состоянии.
- Durable-ветки `save` и `deliverCommitted` не менялись;
  `DurableSubscriberTest` проходит 8/8.
- Success-path EH имеет zero-cost ABI; новая работа на итерацию — один
  предсказуемый STOP branch. На шумном host (loadavg > 20) storage
  benchmarks были в пределах примерно ±5%, но строгий perf-gate
  неубедителен; нужен повтор на спокойной машине.

Сравнение ключевых storage benchmark:

| benchmark | baseline | LP-03 | delta |
| --- | ---: | ---: | ---: |
| AutoAck_Persistent_RoundTrip | 14261 ns | 14903 ns | +4.5% |
| ClientAck_Persistent_RoundTrip | 15846 ns | 14391 ns | -9% |
| Transacted_Persistent_RoundTrip | 77079 ns | 78946 ns | +2.4% |

## 7. Проверка F6/F7 на baseline

Фактические прогоны подтвердили, что дефект существует и не вызван LP-03,
но термин «flaky» неточен:

```text
ClientAckTest.testMixedPersistenceOrdering --gtest_repeat=20:
  1 PASS / 19 FAIL
ExpirationTest.testExpiredPersistentMessageDroppedOnRecv --gtest_repeat=10:
  1 PASS / 9 FAIL
```

С `rm -rf tiny-mq/` перед каждым отдельным запуском оба теста дают 5/5 PASS.
Причина — leakage persisted state между repetitions. Значит, Producer прав,
что это pre-existing debt, но неверно характеризует его как случайную гонку.
Поскольку Test plan LP-03 требует многократный полный suite, долг либо должен
быть устранён, либо формально вынесен в отдельную tagged-задачу с owner.

## Блокеры

### B1 — AnyVisitorTest SIGSEGV на macOS

```bash
./cmake-build-debug/tiny_mq \
  --gtest_filter=AnyVisitorTest.invokesVisitorForActualTypeAfterRegistrationScopeEnds
# exit 139
```

`tests/AnyVisitorTest.cpp` захватывает параметры `registerVisitors()` по
ссылке и использует их после возврата функции. LP-03 добавляет этот тест в
CMake и тем самым ломает macOS full suite. Исправить test-only UB или убрать
регистрацию до отдельного LP issue.

### B2 — критерий повторного full-suite не выполнен

F6/F7 систематически проваливают repetitions без очистки. Исправить test
isolation либо явно оформить отдельный долг. До этого заявленный Test plan
не закрыт.

## Проверенные команды

```bash
cmake --preset debug \
  -DCMAKE_TOOLCHAIN_FILE=/Users/a.bychuk/coding/pet/tiny-mq/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset debug --parallel

rm -rf tiny-mq/
./cmake-build-debug/tiny_mq \
  --gtest_filter=StorageWorkerResilienceTest.* --gtest_repeat=10

for i in 1 2 3 4 5; do
  rm -rf tiny-mq/
  ./cmake-build-debug/tiny_mq --gtest_filter=PersistentTransactionTest.*
done

rm -rf tiny-mq/
./cmake-build-debug/tiny_mq \
  --gtest_filter=ClientAckTest.testMixedPersistenceOrdering --gtest_repeat=20

rm -rf tiny-mq/
./cmake-build-debug/tiny_mq \
  --gtest_filter=ExpirationTest.testExpiredPersistentMessageDroppedOnRecv \
  --gtest_repeat=10
```

Ограничения evidence: настоящий бросок `Poco::Thread::join()` и Linux/x86_64
лично не воспроизведены; perf measurements получены на перегруженном macOS
host и используются только как отсутствие явного сигнала регрессии.

## Verdict

**Rejected, iteration 2.** Ядро LP-03 принято по существу, но default-deny
остаётся из-за macOS SIGSEGV добавленного в build теста и невыполненного
критерия повторного full-suite. Следующий раунд должен либо исправить оба
блокера, либо явно вынести F6/F7 в tagged debt; AnyVisitor macOS crash должен
быть устранён до зелёного CI.
