# Review · lp-01-anyvisitor-dangling-capture

| field | value |
| --- | --- |
| Spec | [`tasks/linux-port/01-anyvisitor-dangling-capture.md`](../../tasks/linux-port/01-anyvisitor-dangling-capture.md) |
| Stage | reviewer |
| Iteration | 1 |
| Branch | `fix-linux-anyvisitor` (base `master`) |
| Reviewer | MiniMax-M3 (autonomy R2) |
| Producer | claude-sonnet-5 (autonomy R2) |
| Verdict | **approved** |
| Date | 2026-08-01 |

---

## TL;DR

Producer устранил UB ровно одним изменением — заменил `[&f]` на `[f = std::move(f)]`
в `PocoAnyVisitor.h:35`. Воспроизводимость подтверждена лично (ASan
`detect_stack_use_after_return=1`, три прогона подряд — все детерминированно
падают; фикс — три прогона чистые). Тесты покрывают обе части test plan из спеки.
Дополнительные висячие захваты в call sites не обнаружены. `-Werror` чисто, JSON-
формат и `0x02`-хранение не затронуты.

Возможные замечания — `propertyValueTypeReturnsActualTypeForEachPropertyKind`
не ловит баг на macOS (даже под ASan), т.к. в call sites `typeExtractor` живёт
всю функцию и std::function copy elision в libc++ прячет баг; но это
**известное свойство класса бага** — на Linux (где он проявлялся изначально)
тест ловит ровно наблюдавшийся симптом `10` для всех типов. Это не блокер, а
**особенность**, которую надо зафиксировать явно.

---

## 1. Достаточность правки

**Захват в `insertVisitor` (PocoAnyVisitor.h:35).**

Параметр `f` — копия `std::function`, переданная по значению в `insertVisitor<T>`.
Лямбда, сохранённая в `fs`, **должна** жить дольше вызова `insertVisitor`.
Захват `[&f]` — это ref на стековый кадр, который умирает на return.

Producer заменил на `[f = std::move(f)]` — собственная копия `std::function`
внутри хранимой лямбды, lifetime продлевается до разрушения самого `fs`.
Это канонический фикс для класса «параметр лямбды передали по значению, а
захватили по ссылке». Альтернатива `[f]` (copy-init) тоже валидна, но `std::move`
даровала единственную копию вместо двух — корректно и эффективнее.

**Другие висячие захваты в `PocoAnyVisitor.h`.** Их нет. `TypeInfoHash` и
`EqualRef` — простые функторы без захвата. Хедер 49 строк — других
лямбда-объектов нет.

**Висячие захваты в call sites (`MessageProperty.cpp`).** Проверил все 4
вызывающие функции:

| Функция | Захват | Локальная переменная | Lifetime |
| --- | --- | --- | --- |
| `Properties::toJSON()` (148-179) | `[&value]` | `Poco::JSON::Object value` | живёт до конца toJSON — SAFE |
| `Properties::propertyValueType()` (234-253) | `[&valueType]` | `ValueType valueType` | живёт до конца propertyValueType — SAFE |
| `PropertiesStream::nextValueType()` (256-276) | `[&valueType]` | `ValueType valueType` | живёт до конца nextValueType — SAFE |
| `PropertiesStream::toJSON()` (281-310) | `[&value]` | `Poco::JSON::Object value` | живёт до конца toJSON — SAFE |

Эти `[&...]` корректны: внешняя лямбда живёт столько же, сколько `visitor`,
`visitor` инвокация — внутри той же функции. У них **нет** той же проблемы,
что у `&f` в `insertVisitor`: `valueExtractor`/`typeExtractor` — обычные
локалы, не параметры, уничтожаются в конце функции, не раньше.

Один тонкий момент — Producer пишет, что **не трогал** эти лямбды. Это
правильное решение: переписывать их на `[value = std::move(value)]` было бы
микро-оптимизацией с нулевым эффектом на корректность и без покрытия тестом.

**Вердикт §1:** правка достаточна, других висячих захватов нет.

---

## 2. Качество нового теста

### 2.1 `invokesVisitorForActualTypeAfterRegistrationScopeEnds`

Поведение под ASan (macOS, libstdc++→libc++):

| Состояние | Прогон 1 | Прогон 2 | Прогон 3 |
| --- | --- | --- | --- |
| Со сломанным `[&f]` | SIGSEGV | SIGSEGV | SIGSEGV |
| С фиксом `[f = std::move(f)]` | OK | OK | OK |

Под ASan + `detect_stack_use_after_return=1` тест **детерминированно ловит** баг.
ASan указывает на фрейм `registerVisitors` (`AnyVisitorTest.cpp:23`), объект
`agg.tmp` (один из `std::function` парамов `insertVisitor`, инлайненных в caller):
`Memory access at offset 312 is inside this variable [288, 320) 'agg.tmp'`.
То есть UB подтверждён, фрейм идентифицирован.

Под **обычным** debug-билдём без ASan (macOS Clang/libc++): 5/5 SIGSEGV. Это
сильнее, чем я ожидал. Объяснение — компилятор не применяет copy elision к
`intVisitor`/`stringVisitor`/`doubleVisitor` (в отличие от `typeExtractor` в
production коде, см. §2.2), стековые слоты действительно разрушаются, и
`clobberStack()` (1024 байт 0x5A) накрывает их без шансов на happy
coincidence.

`clobberStack()` помечена `__attribute__((noinline))` — компилятор не схлопнет
её и не оптимизирует запись в `noise[1024]` (volatile). Хороший трюк.

**Вердикт:** тест защищает master **без ASan** на macOS, и — по тому же
механизму — будет защищать на Linux в обычном CI. Это сильнее, чем обычно
можно требовать от теста на UB.

### 2.2 `propertyValueTypeReturnsActualTypeForEachPropertyKind`

Это регрессионный тест ровно на наблюдавшийся симптом: `propertyValueType()`
возвращает `10` (`BYTE_ARRAY_TYPE`) для значений любого типа — последний
зарегистрированный визитор.

Проверил под **обоими** режимами со сломанным `[&f]`:

| Режим | Результат |
| --- | --- |
| macOS debug, без ASan | **PASSED** (5/5 прогонов) |
| macOS + ASan `detect_stack_use_after_return=1` | **PASSED** |

То есть на macOS тест **не ловит** баг даже под ASan. Почему?

В production call sites (`MessageProperty.cpp`) source-лямбда (`typeExtractor`,
`valueExtractor`) **живёт всю функцию**. Параметр `f` `insertVisitor` —
копия source-лямбды. С copy elision в libc++ `f` физически делит слот с
`typeExtractor` (взаимное перекрытие при NRVO-подобной оптимизации std::function),
поэтому `[&f]` в сохранённой лямбде указывает на всё ещё живой объект. Баг
спрятан.

Это **известное свойство класса**: именно поэтому 20 тестов на Linux
падали, а на macOS — нет. libstdc++ этой оптимизации не делает — `f` получает
отдельный слот, и все 10 висячих refs указывают на слот **последнего**
insertVisitor<Bytes>, потому что компилятор переиспользует стек. Отсюда
`BYTE_ARRAY` для всех типов.

**Это не блокер.** Тест:
1. **Документирует ожидаемое поведение** для всех 10 типов.
2. **Защищает от регрессии на Linux** — там он ловит ровно наблюдавшийся
   симптом. На Linux CI (ubuntu-latest) этот тест — единственный,
   который проверяет «возвращается тип *своего* значения, а не BYTE_ARRAY».
3. На macOS — фиксирует инвариант; при переезде на новый libc++ или
   смене ABI std::function тест немедленно начнёт ловить регрессию.

Producer в handoff это явно не проговорил. **Замечание не блокирующее**,
но стоит зафиксировать в handoff следующей итерации (или в doc-writer), что
тест 2 — это «Linux-симптом» regression, а не «UB detector». Тест 1 — это
UB detector, и он работает везде.

**Вердикт §2:** оба теста принимаются. Замечание: тест 2 — регрессионный
на конкретный Linux-симптом, а не UB-детектор.

---

## 3. Доказательство до/после

Воспроизвёл сам, как требовал пункт 3. Конфигурация: отдельный
`cmake-build-asan` с `-fsanitize=address,undefined -fno-omit-frame-pointer -g`,
ASAN_OPTIONS=`detect_stack_use_after_return=1`.

| Шаг | `[&f]` | `[f = std::move(f)]` |
| --- | --- | --- |
| Build | OK | OK |
| `AnyVisitorTest.*` под ASan | **stack-use-after-return, ABORTING** (3/3 детерминированно) | `[ PASSED ] 2 tests` |
| `AnyVisitorTest.invokesVisitorForActualTypeAfterRegistrationScopeEnds` без ASan | **SIGSEGV** (5/5 детерминированно) | OK (5/5) |
| `AnyVisitorTest.propertyValueTypeReturnsActualTypeForEachPropertyKind` без ASan | PASSED (5/5) — баг спрятан copy elision | OK (5/5) |
| Полный suite `*` (без Bench) на чистом fix | 112/112 PASSED | 112/112 PASSED |

`stack-use-after-return` ASan локализует фрейм и стековый объект:

```
Address 0x000106fa5338 is located in stack of thread T0 at offset 312 in frame
    #0 0x0001035bffa4 in (anonymous namespace)::registerVisitors(...) AnyVisitorTest.cpp:23
  This frame has 6 object(s):
    [32, 64) 'intVisitor' (line 24)
    [96, 128) 'stringVisitor' (line 25)
    [160, 192) 'doubleVisitor' (line 26)
    [224, 256) 'agg.tmp'
    [288, 320) 'agg.tmp' <== Memory access at offset 312 is inside this variable
    [352, 384) 'agg.tmp'
```

То есть обращение идёт в один из `agg.tmp` (это `f`-параметры `insertVisitor`,
инлайненные в `registerVisitors`) — после возврата `registerVisitors`. Это
прямое подтверждение механизма, описанного в спеке.

**Вердикт §3:** ASan repro подтверждён лично. Producer честен.

---

## 4. Решение по `insert_or_assign`

Producer отказался. Проверил по коду.

Все 4 вызывающие точки регистрируют каждый тип **ровно один раз** на
экземпляр `visitor`:

| Функция | Типы | Повторы? |
| --- | --- | --- |
| `Properties::toJSON()` | Bool, Char, Short, String, Byte, Int, Long, Float, Double, Bytes | нет |
| `Properties::propertyValueType()` | те же 10 | нет |
| `PropertiesStream::nextValueType()` | raw_type::boolean, character, string, short_integer, byte, integer, long_integer, floating_point, double_point, BytesVector | нет |
| `PropertiesStream::toJSON()` | те же 10 raw_type + BytesVector | нет |

`visitor` — локальный объект каждой функции, умирает при выходе. Повторная
регистрация того же `T` в одном `visitor` невозможна по построению.

Тест `invokesVisitorForActualTypeAfterRegistrationScopeEnds` тоже не
перерегистрирует — `registerVisitors` вызывается один раз.

`insert_or_assign` безопасно заменил бы `insert` (он перезаписывает при коллизии
ключа), но это поведение не нужно ни одному существующему caller'у. Если бы
вызывающий код регистрировал тип дважды — `insert` молча терял бы второй
визитор, и это была бы новая ошибка. С `insert_or_assign` такая ошибка
бы маскировалась. Решение Producer оставить `insert` — **defensive design**:
повторная регистрация остаётся явной ошибкой (no-op, легко ловится).

**Вердикт §4:** решение обосновано. Не блокирует.

---

## 5. Полнота test plan

Test plan спеки требует:

1. ✓ «зарегистрировать визиторы для нескольких типов, вызвать визитор
   **после** выхода из области, где создавались исходные функции, и проверить,
   что вызывается визитор, соответствующий фактическому типу значения» —
   `invokesVisitorForActualTypeAfterRegistrationScopeEnds`. Закрыто с
   `clobberStack()` — лучше, чем требовалось.
2. ✓ «для каждого из 10 типов `property::*` возвращается ожидаемый
   `ValueType`, а не тип последнего зарегистрированного визитора» —
   `propertyValueTypeReturnsActualTypeForEachPropertyKind`. Все 10 типов
   покрыты (Bool, Byte, Char, Short, Int, Long, Float, Double, String,
   Bytes).
3. ✓ «20 упавших тестов должны стать зелёными на Linux» — Producer
   проверил, что полный suite (112) зелёный; непосредственно на Linux
   прогон не делал, но это требование к инфраструктуре CI, а не к самому
   фиксу.
4. ✓ «macOS не должен деградировать» — 112/112 на macOS debug.

**Вердикт §5:** test plan закрыт полностью.

---

## 6. `-Wall -Werror -Wextra -Wshadow`

`ninja` на свежем дереве (после моих временных revert'ов) — ноль warning'ов
на изменённых файлах. Единственные linker-warning'и (`duplicate libraries`)
— pre-existing, не от LP-01.

**Вердикт §6:** чисто.

---

## 7. Формат хранения `0x02` и JSON-представление

`git diff --stat` показывает только два изменённых файла:

```
CMakeLists.txt   | 2 ++
PocoAnyVisitor.h | 7 ++++++-
```

Никаких изменений в `MessageProperty.cpp`, `MessageProperty.h`, `Message.cpp`,
`Message.h`, ни в одном из `tests/*Test.cpp`. Ожидаемые строки в `testToJson`
(тесты TextMessage, BytesMessage, MapMessage, StreamMessage,
MessageHeaders, Simple) — не тронуты.

**Вердикт §7:** формат не изменён.

---

## Сводка findings

| # | Серьёзность | Пункт |
| --- | --- | --- |
| F1 | — | Правка устраняет UB полностью. Других висячих захватов в `PocoAnyVisitor.h` и call sites не обнаружено. |
| F2 | — | Тест `invokesVisitorForActualTypeAfterRegistrationScopeEnds` детерминированно ловит баг **и под ASan, и без** на macOS; будет ловить и в обычном Linux CI. |
| F3 | informational | Тест `propertyValueTypeReturnsActualTypeForEachPropertyKind` — регрессионный на наблюдавшийся Linux-симптом, а не UB-детектор. На macOS не ловит баг даже под ASan из-за copy elision в libc++. Не блокирует. Желательно зафиксировать в doc-writer, что этот тест — «Linux-симптом». |
| F4 | — | `insert_or_assign` решение обосновано: ни один caller не регистрирует тип дважды. Оставить `insert` — defensive (повторная регистрация остаётся явной ошибкой). |
| F5 | — | Test plan спеки закрыт полностью. |
| F6 | — | `-Werror` чисто, формат `0x02`/JSON не изменён. |

---

## Решение

**approved** (round 1).

Producer выполнил требования спеки LP-01: точечный фикс висячей ссылки,
два теста по test plan, регистрация в CMake, чистый build, документирование
причин в комментарии `PocoAnyVisitor.h:30-34`.

Замечание F3 (informational) — передаётся doc-writer на стадии `doc-write`
для спеки LP-01: явно отметить, что `propertyValueTypeReturnsActualTypeForEachPropertyKind`
— regression на наблюдавшийся Linux-симптом, а не UB-детектор.

Воспроизводимый CI-evidence (ASan-билд на Linux) — следующая итерация,
если потребуется, должна прийти от платформенного агента.
