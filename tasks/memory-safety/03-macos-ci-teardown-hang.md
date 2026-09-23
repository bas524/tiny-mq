# Плавающее зависание полного набора тестов на macOS CI (MS-03)

> **Это maintenance-задача, а не SDD.** См. `tasks/_maintenance-template.md`.

- **Класс:** maintenance (defect-fix, concurrency / teardown)
- **Статус:** open
- **Обратимость:** R2
- **Заведено:** 2026-09-21, PR #7 (спека 24)

## Симптом

GitHub Actions, `macos-latest`, run 35597343807, job «Сборка и тесты (macos-latest)»:
шаг «Конфигурирование и сборка» — 55 с (кэш vcpkg попал), шаг «Тесты» висит с
`12:04:51Z`, последняя строка лога:

```
[ RUN      ] DurableSubscriberTest.testDurableConsumerOnQueueThrows
```

Прогон отменён через 3 ч 18 мин пушем следующего коммита (`cancel-in-progress`).
Тело теста тривиально (`createDurableConsumer` на очереди бросает `Poco::RuntimeException`
до любого изменения состояния) — зависание в SetUp/TearDown или в разрушении
`Connection`/`Session` в конце тела.

Тот же коммит: Ubuntu — 157/157 за 23 с; ASan-гейт (Linux) — зелёный.
Повторный прогон на macOS (run 35618506766, тот же код + фикс use-after-move) — 157/157
за ~20 с. Вчера на `main` (run 35508433313, до спеки 24) macOS — 20 с.

Локально (arm64 macOS, Darwin 25.6): полный набор ×15 с `--gtest_shuffle` и разными
seed'ами — 157/157 каждый раз; `DurableSubscriberTest` ×40 — чисто. **Не воспроизводится.**

## Причина

**Не установлена.** Гонка (проявилась 1 раз из 2 на CI, 0 из 16 локально), не
детерминированный дедлок. Что известно:

- спека 24 в сьютах 1–12 меняет только путь `~Session → Session::rollback(true) →
  Consumer::rollback(bool) → Consumer::redeliver` (`Session.cpp:26-38`, `Consumer.cpp`);
  `DlqTest` бежит 26-м, последним, и на 12-й сьют повлиять не мог;
- в зависшем тесте нет ни консьюмеров приложения, ни сообщений, ни транзакций —
  `redeliver` не вызывается; исполняются только `createDefaultConsumer`, бросок,
  `~Session` (rollback без транзакции — early return), `~Exchange → ~Destination`
  (остановка `ConcurrentLinearStorage`-worker'а, `DeliveryScheduler::stop()`);
- кандидаты: остановка storage-worker'а (LP-03: чинилось исключение, не гонка старта/стопа),
  `DeliveryScheduler::stop()` при незапущенном потоке, `LightweightSemaphore`
  `PriorityQueueT` (спека 45) при разрушении очереди, смена образа `macos-latest`;
- смежное наблюдение: `DlqTest.RestartResetsCounterAndLimit` занимает 5 с при
  `backoffMs=5000` — `_exchange.reset()` ждёт дедлайна scheduler'а вместо мгновенного
  `stop()`. Тот же код teardown; возможно, тот же механизм.

Первый шаг — **получить стек**: в `ci.yml` (PR #7, `c5a838f`) стоит watchdog: через 15 мин
`sample`/`gdb` всех потоков → артефакт `hang-stacks-<os>`, красный шаг. Следующее
проявление даёт evidence; до него любые правки — гадание.

## Критерии «сделано»

- Причина названа по стеку (артефакт `hang-stacks-macos-latest` или локальный `sample`),
  зафиксирована здесь как цитата.
- Есть тест, который воспроизводит гонку детерминированно (например, через
  `--gtest_repeat` + внешняя нагрузка, или прямой тест на `stop()` до старта потока) и
  **падает на текущем коде**.
- 5 последовательных зелёных прогонов «Сборка и тесты (macos-latest)» после фикса;
  `DlqTest.RestartResetsCounterAndLimit` — < 1 с, если механизм общий.

## Границы (scope-lock)

`ConcurrentLinearStorage.{h,cpp}`, `DeliveryScheduler.{h,cpp}`, `Destination.cpp`
(деструктор), `ConcurrentQueueHeader.h`/`PriorityQueueT`, новый тест в
`tests/StorageWorkerResilienceTest.cpp` или `tests/DeliveryDelayTest.cpp`. Семантику
доставки не менять; при смене модели остановки потоков — ADR.

## Evidence

- run 35597343807 (зависание), 35618506766 (зелёный повтор), 35508433313 (main до 24).
- Локальные стресс-прогоны: 15 × 157/157, `--gtest_shuffle` (сессия 2026-09-21).
- Стек зависания — **ещё нет**.
