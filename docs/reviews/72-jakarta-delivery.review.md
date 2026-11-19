# Ревью SDD 72 — инфраструктурная правка M-04 «stock Clang 18 + LLD 18» (итерация 4)

Класс K4 (свидетельство, замороженное во времени).

- Спецификация: `docs/jms-spec/72-jakarta-delivery.md` (version 0.5)
- Стадия: reviewer (cross-model-review, AEF, Стандарт 13)
- Модель: deepseek-v4-pro; автономность R2; machine iteration 4
- Дата: 2026-11-19
- База дельты: `ac83145` («Use one explicit GCC 14 toolchain for paired broker benchmarks»)
- Публикация: `2b3c536` («Make paired broker benchmarks reproducible with stock Clang and LLD»)
- Отпечаток дерева до и после прогонов (совпал): `tree:30773ebf309bde6232825bb32089394ddaf73ed33bb45d24e372a40d12597d76`

## Вердикт

**approved** (iteration 4).

Итерация 4 — узкая инфраструктурная правка к результату i4. Она заменяет toolchain
сравнительного Linux-замера `gcc-14/g++-14` (который тоже падал на baseline при LTO) на
штатные `CC=clang-18`, `CXX=clang++-18`, `LDFLAGS=-fuse-ld=lld-18` образа Ubuntu 24.04.
Дельта относительно `ac83145` — ровно два файла: `.github/workflows/performance.yml`
и `tasks/M-04-paired-perf-toolchain.md`. Продуктовый код и тесты не изменялись.

REQ-72-9 (сравнение CPU time/op на GitHub) и десериализационная безопасность остаются за
шлюзами НФТ и security; вердикт рецензента их не подменяет. GitHub-прогон НФТ не запускался
ни автором, ни мной, и не считается пройденным.

## Дельта ac83145 → 2b3c536

`git diff --name-status ac83145 2b3c536` даёт ровно два файла:

| Файл | Изменение |
|---|---|
| `.github/workflows/performance.yml` | stock Clang/LLD toolchain, sanity compile+link+run, ключ vcpkg-кеша, тайм-аут 60, запись версий инструментов в `environment.txt` |
| `tasks/M-04-paired-perf-toolchain.md` | причина, Done criteria, Scope-lock, Evidence |

Рабочие копии обоих файлов побайтово равны публикации `2b3c536` (SHA-256 совпадает).
`git diff --check` пуст (exit 0).

## Проверка дельты по чек-листу

- **Stock Ubuntu 24.04 Clang18/LLD18**: `runs-on: ubuntu-24.04`, job-level
  `env: {CC: clang-18, CXX: clang++-18, LDFLAGS: -fuse-ld=lld-18}`. Верно.
- **Безопасность mktemp**: `probe=$(mktemp -d "$RUNNER_TEMP/perf-toolchain.XXXXXX")` +
  `trap 'rm -rf "$probe"' EXIT`; утечки каталога нет. Верно.
- **Один toolchain для обеих сборок**: env задан на уровне job, наследуется и baseline-subshell,
  и candidate; шаги configure/build его не переопределяют. Подтверждено логом автора
  `producer-i4-clang-ready.log` (4 пробы наследования) и чтением файла. Верно.
- **Причина M-04 — LTO-сбой baseline**: `github-gcc14-job-135032173945.log:1470–1610`
  показывает `-Werror=maybe-uninitialized` в `std::optional/variant`, `server/src/Selector.cpp:257`
  (`propertyValue`), `lto1: all warnings being treated as errors`. Причина указана корректно.
- **Ключ кеша compiler+image+arch**: `vcpkg-${{runner.os}}-${{runner.arch}}-${{env.CXX}}-${{VCPKG_TAG}}-${{steps.toolchain.outputs.image-os}}-${{steps.toolchain.outputs.image-version}}-${{hashFiles('vcpkg.json')}}`;
  fallback ограничен OS/arch/compiler/tag. Нет SHA коммита. Верно.
- **timeout 60**: `timeout-minutes: 60`. Верно.
- **Версии компилятора в environment после первых двух SHA-строк**: шаг «Measure and check
  the CPU budget» пишет `Baseline $BASE_SHA`, `Candidate $(git rev-parse HEAD)`, затем
  `CC`/`CXX`/`LDFLAGS` и `--version` инструментов, затем image/CPU. Порядок соблюдён.

## Никакой отмены порогов и флагов

Дельта не касается `CMakeLists.txt`, `tests/nfr71.py`, `tests/nfr72.py`,
`CMakePresets.json`, `vcpkg.json` (байт-в-байт равны `ac83145`). Независимо сверено:

- `-Wall -Werror -Wextra -Wshadow` задаются неизменённым `CMakeLists.txt:40`;
- 15 сравнений: 3 пути SDD 71 (`nfr71.HOT_PATHS`) + 12 путей SDD 72 (`nfr72.DELIVERY_PATHS`);
- две границы 5 %: CV `max(…) <= 5.0` и регрессия `after * 20 <= before * 21` (+5 %);
- ABBA `(B4,C4,C5,B5)` при 9 повторах на сторону (`reps = (9, "1.2s", 2)`);
- LTO не снимается: sanity-шаг явно компилирует и линкует пробу с `-flto`, а сборки используют
  неизменённые флаги.

## Собственные проверки (мой прогон, не отчёт автора)

Прогоны выполнены в этой сессии на дереве `tree:30773ebf…`.

| Проверка | Лог | Результат |
|---|---|---|
| trace tests (Стандарт 3, `--id-pattern 'REQ-[0-9]{2}-[0-9]+'`) | `trace-tests.log` | «ошибок 0», exit 0 |
| trace sdd | `reviewer-clang-retry-trace-sdd.log` | «ошибок 0», exit 0 |
| trace input | `reviewer-clang-retry-trace-input.log` | 8 источников, `pinned == head` |
| diff --check | `reviewer-clang-retry-diff-check.log` | exit 0, пусто |
| Сборка Debug (инкрементально) | `reviewer-clang-retry-build-debug.log` | `ninja: no work to do`, exit 0 |
| clang-tidy target | `reviewer-clang-retry-clang-tidy.log` | `ninja: no work to do`, exit 0 |
| Сборка Release (RelWithDebInfo) | `reviewer-clang-retry-build-release.log` | `ninja: no work to do`, exit 0 |
| Debug: полный прогон `-*Bench*` в изолированном TemporaryDirectory | `reviewer-clang-retry-verify-debug.log` | 180/180 PASS (29 suites) |
| Release: `DeliveryContractTest.*` в изолированном TemporaryDirectory | `reviewer-clang-retry-release-delivery.log` | 8/8 PASS |
| Отпечаток до / после | `reviewer-clang-retry-fingerprint-{before,after}.log` | совпали (`30773ebf…`) |

Сборки инкрементальные (build-каталоги сконфигурированы на это дерево); тесты прогонялись
свежими прогонами из `tempfile.TemporaryDirectory` (изолированный cwd).

## Карта «сценарий → тест» (Стандарт 3)

`trace.py tests` — «ошибок 0»: нет плана без факта, нет требования без теста, нет теста-сироты.

| Требование | Сценарий | Тест | Мой прогон |
|---|---|---|---|
| REQ-72-1 | SendClockMatrix | `DeliveryContractTest.SendClockMatrix` | PASS |
| REQ-72-2 | ReceiveLifecycle | `DeliveryContractTest.ReceiveLifecycle` | PASS |
| REQ-72-3 | ListenerModeMatrix | `DeliveryContractTest.ListenerModeMatrix` | PASS |
| REQ-72-4 | AsyncSendBarriers | `DeliveryContractTest.AsyncSendBarriers` | PASS |
| REQ-72-5 | AcknowledgeAndRecoverMatrix | `DeliveryContractTest.AcknowledgeAndRecoverMatrix` | PASS |
| REQ-72-6 | AtomicConsumeAndSend | `DeliveryContractTest.AtomicConsumeAndSend` | PASS |
| REQ-72-7 | CrashAndDeadLetterMatrix | `DeliveryContractTest.CrashAndDeadLetterMatrix` | PASS |
| REQ-72-8 | SessionSendOrder | `DeliveryContractTest.SessionSendOrder` | PASS |
| REQ-72-9 | HotPathBudget | `NfrGateTest.DeliveryHotPathBudget` (runner `tests/nfr72.py`) | за шлюзом НФТ |

## Инварианты, ADR, дрейф, целевой набор

- Продуктовый код и тесты в дельте не меняются → горячие пути, парные изменения
  (`MessageProperty ↔ PocoAnyVisitor`, `Destination::save ↔ deliverCommitted`), повторявшиеся
  дефекты (ADR-0006, ADR-0008) не затронуты.
- Новых публичных сущностей нет; аудит дрейфа (Стандарт 5) — пусто.
- ADR не нарушены; правка сборочной среды CI не меняет инвариант продукта и оформлена
  сопроводительной задачей M-04.
- Оба файла дельты входят в объявленный `target_files` (Стандарт 18).

## Сверка с логами автора

Мои прогоны совпадают с логами агента-автора: trace sdd/input/tests «ошибок 0», сборки и
clang-tidy «no work to do», Debug 180/180 (29 suites), Release `DeliveryContractTest` 8/8,
`git diff --check` пуст, fingerprint до == после. Расхождений (признаков «сфабрикованной
проверки») не обнаружено.

## Находка (не блокирующая)

В `tasks/M-04-paired-perf-toolchain.md`, секция Symptom, первый абзац (сбой GCC 13) пишет
«в `std::optional` внутри тестов MessageContractTest». Фактические диагностики в
`github-ubuntu24-job-135014904620.log:1467–1473` указывают на `server/src/Selector.cpp:257`
(`propertyValue`), то есть на серверный код, а не на тест — то же место, что и у сбоя GCC 14
(второй абзац указан верно). Суть причины (LTO + `-Werror=maybe-uninitialized`) и сам фикс
от этого не меняются. Рекомендую при следующем касании исправить формулировку на `Selector.cpp`.

## Границы вердикта

Локальная машина — macOS; реальная Linux-сборка и замер на GitHub здесь не выполняются
(как и у автора). Данная правка восстанавливает сопоставимую сборку baseline и candidate
на Ubuntu 24.04 со штатными Clang 18/LLD 18; проверку REQ-72-9 (порог +5 %) выполняет
отдельный обязательный шлюз НФТ, и она не считается пройденной.
