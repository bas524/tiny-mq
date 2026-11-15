# Независимое ревью реализации SDD 70
Общий вердикт раунда 3 — **approved**. Все три раунда выполнены через профиль
`deepseek-pro` с прямым подключением к `api.deepseek.com`.

[Прогоны раунда 1](../../handoffs/current/70/logs/reviewer-r1),
[прогоны раунда 2](../../handoffs/current/70/logs/reviewer-r2) и
[прогоны раунда 3](../../handoffs/current/70/logs/reviewer-r3).

---

# Ревью SDD 70 — сообщения, свойства и селекторы (независимый рецензент)

Вердикт: **rejected**

Дата: 2026-11-15
Рецензент: независимая сессия с чистым контекстом (модель ≠ автор).

## Что проделано собственными прогонами

Сборку и проверки запускал сам командой `sh handoffs/current/70/tools/verify-review.sh`
из чистой рабочей копии. Результаты совпадают с логами автора, фабрикации не обнаружено:

- Debug и RelWithDebInfo собрались без ошибок (`build-debug.log`, `build-release.log`).
- C++ GoogleTest: 165/165 passed, включая 8 тестов `MessageContractTest` (`cpp.log`).
- Локальный Java runner: 6 сценариев `MessageContractTest` passed (`java.log`).
- `trace.py tests docs/jms-spec/70-jakarta-message-contract.md tests`: ошибок 0 (`trace.log`).
- Отпечаток дерева до и после прогона совпал: `tree:4ff115dbf141a33434321efe65d0450e72f126efd5c745243103a0723cce0fd1`.

Дифф (`git diff HEAD` + `git ls-files --others --exclude-standard`) лежит внутри
объявленного `handoffs/current/70/target-files`: 30 файлов, расхождений с набором нет.

## Карта покрытия девяти REQ-70

| Требование | Тест | Runner | Покрытие |
|---|---|---|---|
| REQ-70-1 | MessageContractTest.CrossLanguageEnvelope | C++ и Java | есть |
| REQ-70-2 | MessageContractTest.BodyRoundTrip | C++ и Java | есть |
| REQ-70-3 | MessageContractTest.BodyAccessMatrix | Java | есть |
| REQ-70-4 | MessageContractTest.PropertyAccessMatrix | C++ и Java | есть |
| REQ-70-5 | MessageContractTest.SelectorGrammarMatrix | C++ | есть |
| REQ-70-6 | NfrGateTest.HotPathBudget | NFR-шлюз отдельно | заготовка есть, шлюз ещё не дал чистую серию |
| REQ-70-7 | MessageContractTest.GroupProperties | C++ и Java | есть |
| REQ-70-8 | MessageContractTest.BinaryCorrelationId | C++ и Java | есть |
| REQ-70-9 | MessageContractTest.StorageV02Compatibility | C++ | есть |

Тесты содержательные: селекторная матрица проверяет трёхзначную логику, числовое
продвижение, точность long, BETWEEN/IN/LIKE/ESCAPE/IS NULL и синтаксические ошибки;
конверт и тела проверяются независимо в C++ и Java; v0x02 читается из эталонного
hex-вектора исходной редакции.

## Блокер (воспроизведён)

### B1. Новый формат хранения 0x03 не читается свипом истечения

Место: [ConcurrentLinearStorage.cpp](/private/tmp/tiny-mq-review70.ma8ppf/ConcurrentLinearStorage.cpp:255)
`if (static_cast<uint8_t>(prefix[kMagicOffset]) != 0x02) continue;`.

`Message::toBytes()` ([Message.cpp](/private/tmp/tiny-mq-review70.ma8ppf/Message.cpp:181))
теперь пишет `0x03` для записей с расширенным состоянием (null-свойство, бинарный/пустой
correlation ID, пустой JMSType, null-текст, базовый `Message`, тело ObjectMessage с
присутствующим пустым массивом, непустой `destination`). Поле `JMSExpiration` лежит по тому
же смещению 35 и в 0x02, и в 0x03, но свип пропускает все записи с magic, отличным от 0x02.

Нарушенный критерий: `INHERITED_CONSTRAINTS` PROJECT.md («совместимость сохранённых записей»),
парное изменение «кодер ↔ декодер»/все читатели формата, и существующее поведение JMSExpiration
(spec 44): истёкшие persistent-записи должны забираться фоновым свипом. Новая версия формата
введена, а этот читатель формата не обновлён.

Воспроизведение (лог `handoffs/current/70/reviewer/sweep-repro.log`): две persistent
`TextMessage` с одинаковым прошедшим `expiration`; у второй выставлено null-свойство, из-за
чего `toBytes()` даёт magic 0x03. После `ConcurrentLinearStorage::start()` и интервала свипа:

```text
v02 record magic=2 expiration(past)=...
v03 record magic=3 expiration(past)=...
before sweep: 2 records
after sweep: 1 records
surviving record magic=3
```

Запись 0x02 удалена, запись 0x03 с тем же истёкшим сроком осталась в хранилище. Минимальный
случай подтверждён на собственной сборке.

Важно: исправление требует правки `ConcurrentLinearStorage.cpp`, которого нет в объявленном
`target_files`. То есть либо целевой набор неполон для введённого изменения формата, либо
схему версии надо пересмотреть так, чтобы не ломать существующих читателей. Это решение
оркестратора; код рецензент не правит.

## Прочие наблюдения (не основание вердикта)

- N1. Предварительный NFR-прогон автора `logs/bench-preliminary.log` завершился `FAIL`:
  `AutoAck_Persistent_RoundTrip` имеет CV 5.19%/5.39% (> 5%), то есть серия шумная.
  `MessageSelector` показывает −98.81% (база снималась со старым Selector, где LIKE
  пересобирал `std::regex` на каждом совпадении, поэтому ускорение объяснимо). REQ-70-6
  закрывает профильный шлюз NFR; ему нужна чистая серия из девяти повторов — это открытый
  пробел, а не блокер моего ревью.
- N2. `MessageContractTest.V03DeliveryAndTransaction` не содержит комментария с ID
  требования, хотя Test plan SDD требует «каждый тест в коде и логе указывает свой ID».
  Тест полезный (покрывает доставку persistent 0x03-записей), но без ID нарушает план.

## Вывод

Семантика конверта, обе части (C++ и Java), правила Jakarta 3.1 для тел/свойств/селектора
и точность селектора проверены и на собственных прогонах зелёные. Однако введённая версия
хранения 0x03 ломает существующий свип истечения для persistent-записей нового формата —
воспроизведено, значит допуск невозможен: **rejected**.

---

# Ревью SDD 70 — раунд 2 (независимый рецензент)

Вердикт: **approved**

Дата: 2026-11-15
Рецензент: независимая сессия, чистый контекст; код не менял.

## Собственные прогоны

`sh handoffs/current/70/tools/verify-review.sh` выполнен в этой копии:

- Debug и RelWithDebInfo собрались без ошибок (`build-debug.log`, `build-release.log`).
- C++ GoogleTest: 165/165 passed, включая `ExpirationTest.testExpiredMessageSweptFromStorage`
  и 8 сценариев `MessageContractTest` (`cpp.log`).
- Java runner: 6/6 `MessageContractTest` passed (`java.log`).
- `trace.py tests` — ошибок 0 (`trace.log`).
- Отпечаток до и после совпал: `tree:29b3ff5621275b4e237e03f19e8e69ddc0c637e4e10a9d350ca49c1d6e677ba6`.

## Три изменения после раунда 1

1. `ConcurrentLinearStorage.cpp` — свип читает 0x02 и 0x03 через
   `tiny_mq::hasStorageHeaders`; тот же предикат применён в
   `Message::patchCachedDeliveryTime`.
2. `tests/ExpirationTest.cpp` — тест создаёт expired/live для v0x02 и v0x03, проверяет magic
   записи и по остатку сверяет UUID с множеством живых ID, а не только размер.
3. `tests/java/MessageContractTest.java` — `conversionMatrix` проверяет полную матрицу
   10×10 одновременно для Map- и Stream-читателей.

## Сверка с отчётом раунда 1

- B1 устранён: свип больше не пропускает v0x03; регрессия покрывает именно путь
  `sweepExpired` и прошла в `cpp.log`.
- N2 устранён: `V03DeliveryAndTransaction` теперь помечен `// REQ-70-1, REQ-70-9`.
- NFR: первая повторная серия `logs/bench-repeat.log` — PASS; финальную независимую серию
  снимает отдельный NFR-шлюз после этого ревью.

## Набор файлов и отпечаток

Полный diff (отслеживаемые + неотслеживаемые) совпадает с
`handoffs/current/70/target-files`: 32/32, расхождений нет.

Логи: `handoffs/current/70/reviewer/build-debug.log`,
`handoffs/current/70/reviewer/build-release.log`,
`handoffs/current/70/reviewer/cpp.log`, `handoffs/current/70/reviewer/java.log`,
`handoffs/current/70/reviewer/trace.log`.

---

# Ревью SDD 70 — раунд 3 (независимый рецензент)

Вердикт: **approved**

Дата: 2026-11-15
Рецензент: независимая сессия; код не менял.

## Собственные прогоны

`sh handoffs/current/70/tools/verify-review.sh` выполнен в изолированной копии:

- Debug и RelWithDebInfo собрались без ошибок (`handoffs/current/70/reviewer/r3/build-debug.log`,
  `handoffs/current/70/reviewer/r3/build-release.log`).
- C++ GoogleTest: 165/165 passed (`handoffs/current/70/reviewer/r3/cpp.log`).
- Java runner: 6/6 `MessageContractTest` passed (`handoffs/current/70/reviewer/r3/java.log`).
- `trace.py tests` — ошибок 0 (`handoffs/current/70/reviewer/r3/trace.log`).
- Отпечаток до и после прогона совпал:
  `tree:79fcf40c9897b96c40ef871dce97dc94cde4e64961fc26cc549e428312073f3e`.

## Проверка дельты после раунда 2

Изменение ограничено тремя файлами, правки которых появились после отчёта раунда 2
и conformance-r2: `Selector.cpp`, `tests/MessageContractTest.cpp`,
`tests/java/MessageContractTest.java`.

- `Selector.cpp`: `NOT BETWEEN` разворачивается в `value < low OR value > high`
  (`negated ? T::Lt : T::Ge` / `negated ? T::Gt : T::Le`), а не в отрицание `>= AND <=`.
- `tests/MessageContractTest.cpp`: полная таблица трёхзначной логики SQL — 9 пар AND,
  9 пар OR и NOT для TRUE/FALSE/UNKNOWN, плюс случаи NaN и String в BETWEEN/NOT BETWEEN.
- `tests/java/MessageContractTest.java`: пограничные чтения null/отсутствующего поля тела
  (Map/Stream) и null/отсутствующего свойства примитивными getters, недопустимые имена
  свойств, включая имя с дефисом.

## Усиленная сверка дефекта conformance-r2

Собственный probe на Debug-сборке (`handoffs/current/70/reviewer/r3/selector-probe.log`):

- String `"2"`: `p NOT BETWEEN 5 AND 20` = FALSE, `p < 5 OR p > 20` = FALSE.
- Double `NaN`: `p NOT BETWEEN 5 AND 20` = FALSE, `p < 5 OR p > 20` = FALSE.

Эквивалентность из Jakarta Messaging 3.1 §3.8.1.1 восстановлена; прежнее расхождение
`TRUE` против `FALSE` устранено.

## Вывод

Дефект conformance-r2 устранён, регрессий в трёх изменённых файлах и в полном наборе
C++/Java не обнаружено. Прочие REQ подтверждены раундом 2; NFR-шлюз — после этого ревью.
**approved**.
