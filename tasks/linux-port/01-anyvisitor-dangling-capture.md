# LP-01 — `AnyVisitor::insertVisitor` захватывает висячую ссылку

**Статус:** ⬜ open · **Приоритет:** блокер · **Заведено:** 2026-08-01
**Источник:** первый прогон CI на Linux (GitHub Actions, ubuntu-latest), 20 падающих тестов из 110.

## Симптом

На Linux (GCC/libstdc++) падают 20 тестов, на macOS (Clang/libc++) те же тесты зелёные:

| Группа | Тесты | Проявление |
|---|---|---|
| JSON-дамп | `TextMessageTest.testToJson`, `BytesMessageTest.testToJson`, `MapMessageTest.testToJson`, `StreamMessageTest.testToJson`, `MessageHeadersTest.roundTripPreservesPayloadAndProperties` | `std::bad_function_call` |
| Определение типа | `MapMessageTest.testSendRecvCloneMapMessage`, `StreamMessageTest.testSetAndGet` | `valueType()` возвращает `10` для любого типа |
| Селекторы | все 12 падающих `SelectorTest.*` | ни один селектор не срабатывает |
| Сериализация свойств | `SimpleTest.testSaveAndRestoreMessage` | `unordered_map::at` |

## Причина

`PocoAnyVisitor.h:29-31`:

```cpp
template <typename T>
void insertVisitor(std::function<void(const T&)> f) {          // параметр по значению
  fs.insert(std::make_pair(std::ref(typeid(T)),
            Function([&f](const Poco::Any& x) {                // захват по ССЫЛКЕ
              f(Poco::RefAnyCast<T>(x));
            })));
}
```

`f` — параметр по значению, он уничтожается при выходе из `insertVisitor`. Лямбда,
сохранённая в `fs`, держит ссылку на этот уничтоженный объект. Любой последующий вызов
визитора — обращение к мёртвому объекту, то есть **неопределённое поведение**.

libc++ на macOS оставлял память в состоянии, при котором вызов случайно срабатывал;
libstdc++ на Linux — нет. Это не «баг Linux», а UB, которое до переезда на GitHub просто
никто не наблюдал: проект собирался только под macOS/arm64.

**Подтверждение механизма.** `valueType()` возвращает `10` = `BYTE_ARRAY_TYPE`
(`MessageProperty.h:118`) для значений *любого* типа. `10` — это тип последнего
зарегистрированного визитора: в `Properties::propertyValueType` (`MessageProperty.cpp:239-248`)
последним идёт `insertVisitor<property::Bytes>`. Все десять сохранённых лямбд ссылаются на
один и тот же переиспользуемый слот стека, где после серии вызовов остался объект от
`property::Bytes`. Цифра из лога — прямое следствие, а не совпадение.

## Цепочки до симптомов

- `Properties::toJSON()` (`MessageProperty.cpp:148-179`) — вызов висячего `f` бросает
  `bad_function_call` → падают все `testToJson`.
- `Properties::propertyValueType()` (`MessageProperty.cpp:235-252`) → возвращает `10` →
  `Selector.cpp:165` уходит в ветку `switch` для `BYTE_ARRAY` и не сопоставляет ничего →
  падают все `SelectorTest`.
- `PropertiesStream::nextValueType()` — та же схема → `StreamMessageTest.testSetAndGet`.
- `Message::propertiesAsBytes()` (`Message.cpp:25`) → `propertiesToJSON()` → повреждённый
  дамп → свойство не восстанавливается → `_properties.at(name)` бросает `unordered_map::at`
  → `SimpleTest.testSaveAndRestoreMessage`.

## Что сделать

1. Захватывать `f` **по значению**: `[f](const Poco::Any& x) { f(Poco::RefAnyCast<T>(x)); }`.
   `std::function` копируема, лишняя копия здесь не на горячем пути (визиторы строятся
   при сериализации и определении типа, не в доставке).
2. Рассмотреть приём `f` по `std::function<void(const T&)>` и `std::move` в лямбду —
   но только если это не усложняет вызывающий код.
3. Убедиться, что `fs.insert` не молча игнорирует повторную регистрацию того же типа
   (`insert` не перезаписывает существующий ключ). Если перерегистрация предполагается —
   использовать `insert_or_assign`.

## Test plan

- Тест на сам визитор (сейчас его нет вовсе — в этом и причина, что баг дожил):
  зарегистрировать визиторы для нескольких типов, вызвать визитор **после** выхода из
  области, где создавались исходные функции, и проверить, что вызывается визитор,
  соответствующий фактическому типу значения.
- Тест на `propertyValueType()`: для каждого из 10 типов `property::*` возвращается
  ожидаемый `ValueType`, а не тип последнего зарегистрированного визитора.
  Это регрессионный тест ровно на наблюдавшийся симптом (`10` вместо реального типа).
- Все 20 упавших тестов из таблицы выше должны стать зелёными на Linux.
- macOS не должен деградировать: там тесты и так проходили, но по случайности.

## Границы

- Правка не должна менять формат хранения `0x02` и JSON-представление сообщений —
  ожидаемые строки в `testToJson` уже описывают корректный вывод, менять их нельзя.
- Горячий путь доставки визитор не использует; перф-гейт не требуется, но если
  затрагивается `MessageProperty` на пути (де)сериализации — прогнать бенчи.

## Связанное

- После исправления — LP-02 (разбор остаточных падений на Linux).
- CLAUDE.md требует держать `MessageProperty` и `PocoAnyVisitor.h` синхронными;
  правка не должна нарушить это соответствие.
