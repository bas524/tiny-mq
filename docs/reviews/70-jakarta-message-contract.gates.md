# NFR SDD 70 — REQ-70-6 (HotPathBudget)

Вердикт: **approved**

Сравнение main `9f7983c` (baseline, A) и кандидата (B), RelWithDebInfo, ABBA,
9 повторов на сборку, медиана CPU time/op в наносекундах. Фильтр — минимальный
`BenchmarkFixture/AutoAck_(NonPersistent|Persistent)_RoundTrip$` плюс изолированный
профильный `MessageSelector` (изменённый горячий путь селектора).

| Бенчмарк | main, ns | кандидат, ns | Δ | CV main / кандидат |
|---|---|---|---|---|
| AutoAck_NonPersistent_RoundTrip | 177.50 | 178.66 | +0.66% | 3.81% / 3.32% |
| AutoAck_Persistent_RoundTrip | 6917.04 | 6865.03 | −0.75% | 1.08% / 2.21% |
| MessageSelector | 3798.83 | 44.60 | −98.83% | 0.75% / 0.99% |

Все CV ≤ 5%, серия не шумная — повтор не требовался. Регрессия по обоим
RoundTrip-фильтрам в пределах порога 5%; селектор даёт значимое ускорение.

Отпечаток дерева (до = после): `tree:29b3ff5621275b4e237e03f19e8e69ddc0c637e4e10a9d350ca49c1d6e677ba6`

Привязка входов:

- baseline A: `/tmp/tiny-mq-head-baseline.tpr9w7/release/tiny_mq`, ref `9f7983c0054636a6686f9928831c8c53dfbd3679`, sha256 `f6e67e07f448b088c71b1d4ddb3ec9865bdad54736a6b244697b7904a850daf4`
- кандидат B: `handoffs/current/70/reviewer/release/tiny_mq`, ref `tree:29b3ff5621275b4e237e03f19e8e69ddc0c637e4e10a9d350ca49c1d6e677ba6`, sha256 `960667a527bf02924dff606e6372e6e569396adcf95ee1b8829ce9b3ab1bc4d9`
- платформа: `macOS-27.2-arm64-arm-64bit-Mach-O`, cpu_count 12

Свидетельства:

- сводка: `handoffs/current/70/nfr/summary.log`
- сырые данные (платформа, команды, sha256, stdout/stderr, 9×3 сэмплов): `handoffs/current/70/nfr/raw.jsonl`
- запуск: `sh handoffs/current/70/tools/verify-nfr.sh`

Примечание к `benchmarks/baseline.md`: таблица зафиксирована 2026-07-31 на другой
машине (Apple M2 8 ядер, Darwin 25.5.0), поэтому её абсолютные значения расходятся с
текущим окном; критерий REQ-70-6 сравнивает кандидата с main в одном интерливированном
окне ABBA, что и выполнено.

---

# Conformance AEF · SDD 70

**Вердикт `rejected`.** Селектор нарушает эквивалентность `NOT BETWEEN` из Jakarta Messaging 3.1 §3.8.1.1. В матрицах остаются непроверенные случаи.

**Отпечаток до и после прогонов** `tree:29b3ff5621275b4e237e03f19e8e69ddc0c637e4e10a9d350ca49c1d6e677ba6`.

В изолированной копии `sh handoffs/current/70/tools/verify-conformance.sh` завершился с кодом 0. C++ прошёл 9/9 на предоставленной Debug-сборке, Java — 6/6 на локальном API 3.1.0, trace — 0 ошибок. Дополнительный прогон C++ без бенчмарков прошёл 165/165. [C++](cpp.log) · [Java](java.log) · [trace](trace.log) · [полный C++](cpp-full.log).

## REQ → тест

| REQ | Сценарий и runner | Результат сверки |
|---|---|---|
| REQ-70-1 | `MessageContractTest.CrossLanguageEnvelope` · C++, Java. `MessageContractTest.V03DeliveryAndTransaction` · C++ | Оба локальных конверта прошли проверку заголовков, четырёх комбинаций подсказок, диапазона времени, ID и пар `false/1`, `true/2`. Дополнительный сценарий прошёл для persistent и non-persistent, при обычной отправке и через commit. |
| REQ-70-2 | `MessageContractTest.BodyRoundTrip` · C++, Java | Все шесть форм, снимок `Serializable` при setter и конструировании, ошибки сериализации и `BytesMessage.writeObject(null)`, копирование массива, восстановление тела и свойств после мутации, непрозрачные байты с обоими состояниями `className` прошли. В Java нет round trip пустых Bytes/Map/Stream и Object с null-телом. |
| REQ-70-3 | `MessageContractTest.BodyAccessMatrix` · Java | Обе таблицы 10×10 для Map и Stream прошли. Отдельно проверены ошибки чтения и повторное чтение, EOF, read-only, очистка, `getBody` и повреждённое Object-тело. Нет граничных проверок чтения null и отсутствующего поля Map примитивными getters. Для Bytes типы, принимаемые `writeObject`, и отклонение постороннего типа остаются вне матрицы. |
| REQ-70-4 | `MessageContractTest.PropertyAccessMatrix` · C++, Java | Java-таблица 8×8, исключения для null и отсутствующего свойства, перечисление, read-only и `clearProperties` прошли. C++ проверил хранение явного null и селектор `$π1`. В Java нет имени с дефисом, прямо указанного в SDD. Проверка регистра в Java использует `propertyExists`, а C++-селектор проверяет только совпадающий регистр. |
| REQ-70-5 | `MessageContractTest.SelectorGrammarMatrix`, `SelectorTest.*` · C++ | Штатные проверки прошли, включая `IS NOT NULL`, кавычки, оба delivery mode, 2^53+1, Java promotion и тип ошибки. Матрица не утверждает результат всех 9 пар каждого из AND и OR, например `UNKNOWN AND UNKNOWN`, `UNKNOWN OR UNKNOWN` и `FALSE OR UNKNOWN`. Дополнительный прогон воспроизвёл расхождение `NOT BETWEEN`. [Воспроизведение](selector-probe.log). |
| REQ-70-6 | `NfrGateTest.HotPathBudget` · отдельный NFR | `approved` для того же отпечатка. ABBA, по 9 повторов, Δ медианы CPU time/op +0,66% для non-persistent, −0,75% для persistent, −98,83% для селектора. [Отчёт NFR](../nfr/report.md) · [сводка](../nfr/summary.log) · [сырые измерения](../nfr/raw.jsonl). |
| REQ-70-7 | `MessageContractTest.GroupProperties` · C++, Java | Оба runner подтвердили значения, типы String/int и отсутствие обоих свойств. |
| REQ-70-8 | `MessageContractTest.BinaryCorrelationId` · C++, Java | Оба runner подтвердили строку, отсутствие, пустой и непустой массив `00ff80`, а также защиту от мутаций. Java подтвердил ошибки несовпадающих getter, C++ — отображение байтового ID в NULL. |
| REQ-70-9 | `MessageContractTest.StorageV02Compatibility`, `MessageContractTest.V03DeliveryAndTransaction`, `ExpirationTest.testExpiredMessageSweptFromStorage` · C++. `MessageHeadersTest.StorageV02Fixture` · старая сборка | Старый writer `9f7983c` побайтно воспроизвёл эталон `0x02`. Кандидат прочитал его и перечитал новый `0x03` с отдельными состояниями null, пустого типа и бинарного ID. Sweep сохранил живые и удалил просроченные записи обоих форматов. Старая сборка успешно открыла отдельную восстановленную копию `0x02`. Этот restore проверен на записи TextMessage. [Откат](rollback.log) · [C++](cpp.log). |

## Воспроизведённое нарушение

Для `p` типа String со значением `"2"` и для `p` типа Double со значением `NaN` выражение `p NOT BETWEEN 5 AND 20` даёт TRUE, а его нормативный эквивалент `p < 5 OR p > 20` даёт FALSE. `Selector.cpp:493` вычисляет отрицание `>= AND <=`. [Jakarta Messaging 3.1 §3.8.1.1–3.8.1.2](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#message-selector-syntax) · [лог](selector-probe.log).

Новые сущности доменного контракта сопоставлены с E1–E21. Вложенный тип callback соответствует E14, а варианты тела и их типовые метки — E2/E3. Идентификаторы REQ в C++-логе восстанавливаются из `Suite.Case` и пометок в исходнике. GoogleTest не печатает их напрямую, как требует Test plan.

Маршрутизатор записал `SCOPE-VIOLATION`. Изолированная копия содержит исходный diff кандидата и текстовый `target-files`, а для наследования этого набора маршрутизатор читает `producer.json`, который в копии отсутствует. [Журнал цепочки](../chain.log).

---

# NFR SDD 70 — REQ-70-6 (HotPathBudget), итерация 3

Вердикт: **approved**

Сравнение main `9f7983c` (baseline, A) и кандидата (B), RelWithDebInfo, ABBA,
9 повторов на сборку, медиана CPU time/op в наносекундах. Фильтр — минимальный
`BenchmarkFixture/AutoAck_(NonPersistent|Persistent)_RoundTrip$` плюс изолированный
профильный `MessageSelector` (изменённый горячий путь селектора).

| Бенчмарк | main, ns | кандидат, ns | Δ | CV main / кандидат |
|---|---|---|---|---|
| AutoAck_NonPersistent_RoundTrip | 173.86 | 178.11 | +2.44% | 2.85% / 3.55% |
| AutoAck_Persistent_RoundTrip | 6850.39 | 6926.62 | +1.11% | 2.37% / 1.09% |
| MessageSelector | 3813.02 | 45.13 | −98.82% | 0.95% / 0.94% |

Все CV ≤ 5%, серия не шумная — повтор не требовался. Регрессия по обоим
RoundTrip-фильтрам в пределах порога 5%; селектор даёт значимое ускорение.

Отпечаток дерева (до = после): `tree:79fcf40c9897b96c40ef871dce97dc94cde4e64961fc26cc549e428312073f3e`

Привязка входов:

- baseline A: `/tmp/tiny-mq-head-baseline.tpr9w7/release/tiny_mq`, ref `9f7983c0054636a6686f9928831c8c53dfbd3679`, sha256 `f6e67e07f448b088c71b1d4ddb3ec9865bdad54736a6b244697b7904a850daf4`
- кандидат B: `handoffs/current/70/reviewer/release/tiny_mq`, ref `tree:79fcf40c9897b96c40ef871dce97dc94cde4e64961fc26cc549e428312073f3e`, sha256 `2d078af6e3374102e5132dee63014abe68a379b489e7b7cb69a05156c2422156`
- платформа: `macOS-27.2-arm64-arm-64bit-Mach-O`, cpu_count 12

Свидетельства:

- сводка: `handoffs/current/70/nfr/summary.log`
- сырые данные (платформа, команды, sha256, stdout/stderr, 9×3 сэмплов): `handoffs/current/70/nfr/raw.jsonl`
- запуск: `sh handoffs/current/70/tools/verify-nfr.sh` (код 0)

Примечание к `benchmarks/baseline.md`: таблица зафиксирована 2026-07-31 на другой
машине (Apple M2 8 ядер, Darwin 25.5.0), поэтому её абсолютные значения расходятся с
текущим окном; критерий REQ-70-6 сравнивает кандидата с main в одном интерливированном
окне ABBA, что и выполнено.

---

# Conformance AEF · SDD 70 · итерация 3

**Вердикт `approved`.** Исправление `NOT BETWEEN` и все случаи, отмеченные в [r2](../conformance-r2/report.md), подтверждены собственным прогоном.

Отпечаток дерева до и после проверок совпал: `tree:79fcf40c9897b96c40ef871dce97dc94cde4e64961fc26cc549e428312073f3e`. [Отпечаток](fingerprint.log).

`sh handoffs/current/70/tools/verify-conformance.sh` завершился с кодом 0. C++ 9/9 на предоставленной Debug-сборке, Java 6/6 с API 3.1.0, trace без ошибок. Дополнительный C++ прогон без бенчмарков прошёл 165/165. [C++](cpp.log) · [Java](java.log) · [Trace](trace.log) · [Полный C++](cpp-full.log).

## Карта требований

| REQ | Тесты | Подтверждённое поведение |
|---|---|---|
| REQ-70-1 | `MessageContractTest.CrossLanguageEnvelope` (C++, Java), `MessageContractTest.V03DeliveryAndTransaction` (C++) | Заголовки, четыре комбинации подсказок, время, ID, пары redelivery/count. Persistent и non-persistent при обычной отправке и commit. |
| REQ-70-2 | `MessageContractTest.BodyRoundTrip` (C++, Java) | Шесть видов тела, снимки сериализуемого объекта и массива, непрозрачные байты с обоими состояниями `className`. В Java подтверждены null и пустой Text, пустые Bytes/Map/Stream, null-тело ObjectMessage. |
| REQ-70-3 | `MessageContractTest.BodyAccessMatrix` (Java) | Матрицы Map и Stream, режимы чтения и записи, повтор чтения после ошибки, EOF и getBody. Map покрывает явный null и отсутствующий элемент. Bytes.writeObject покрывает 10 допустимых типов и отклонение постороннего типа, а BodyRoundTrip проверяет исключение для null. |
| REQ-70-4 | `MessageContractTest.PropertyAccessMatrix` (C++, Java) | Матрица преобразований свойств, null и отсутствие, перечисление и clearProperties. Java отклоняет `with-dash`, селектор C++ различает `$π1` и `$Π1`. |
| REQ-70-5 | `MessageContractTest.SelectorGrammarMatrix`, `SelectorTest.*` (C++) | Грамматика, типизация, числовое продвижение, оба delivery mode и исключение компиляции. По 9 пар AND и OR, три входа NOT. Для String и NaN `NOT BETWEEN` и `< OR >` дают FALSE. [Проба](selector-probe.log) · [Jakarta Messaging 3.1 §3.8.1.1–3.8.1.2](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#message-selector-syntax). |
| REQ-70-6 | `NfrGateTest.HotPathBudget` (NFR) | `approved` для того же отпечатка. ABBA, по 9 повторов, Δ медианы CPU time/op +2,44% non-persistent, +1,11% persistent, −98,82% селектор. [Отчёт NFR](../nfr/report.md) · [Сводка](../nfr/summary.log) · [Измерения](../nfr/raw.jsonl). |
| REQ-70-7 | `MessageContractTest.GroupProperties` (C++, Java) | String/int и отсутствие JMSXGroupID/JMSXGroupSeq. |
| REQ-70-8 | `MessageContractTest.BinaryCorrelationId` (C++, Java) | Строка, отсутствие, пустой массив и `00ff80`, защита от мутаций, ошибки несовпадающих Java-getter и NULL для двоичного ID в селекторе. |
| REQ-70-9 | `MessageContractTest.StorageV02Compatibility`, `MessageContractTest.V03DeliveryAndTransaction`, `ExpirationTest.testExpiredMessageSweptFromStorage` (C++), `MessageHeadersTest.StorageV02Fixture` (старая сборка) | Старый writer побайтно воспроизвёл fixture `0x02`. Кандидат прочитал его и перечитал `0x03`, сохранив null, пустой тип и двоичный ID. Sweep сохранил живые и удалил просроченные записи обоих форматов. Старая сборка прочитала восстановленную копию `0x02` для TextMessage. [Лог восстановления](storage-restore.log). |

Сверка сущностей r2 охватывает E1–E21. В diff итерации 3 для перепроверенных сценариев новых сущностей нет. C++-лог теперь содержит ID, напечатанные тестами MessageContractTest. Правила чтения null в Map сверены с [Javadoc MapMessage](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/mapmessage).

Маршрутизатор остановил передачу с `SCOPE-VIOLATION`. В изолированной копии присутствует diff кандидата и текстовый `target-files`. Для наследования набора маршрутизатору требуется пакет `producer.json`, который в этой копии отсутствует. [Журнал цепочки](../chain.log).

---

# Security AEF · SDD 70 · итерация 3

**Вердикт `approved`.** В изменённых границах конкретного дефекта не воспроизведено.
Границы SDD 70 — непрозрачное хранение Java-байтов в C++, локальный Java-модуль и
стандартная политика `ObjectInputFilter` при десериализации — устойчивы для своей области.
Транспорт и приём внешних записей вынесены в SDD 73/74 и здесь не проверялись.

Отпечаток дерева до и после прогонов совпал:
`tree:79fcf40c9897b96c40ef871dce97dc94cde4e64961fc26cc549e428312073f3e`. [Отпечаток](fingerprint.log).

`sh handoffs/current/70/tools/verify-security.sh` завершился с кодом 0. C++ 5/5 на Debug-сборке,
Java-проба подтвердила отклонение запрещённого класса до вызова `readObject`.
[C++](cpp.log) · [Java-фильтр](java-filter.log).

## Проверенные границы

| Граница | Проверено | Результат |
|---|---|---|
| C++ хранит Java-байты непрозрачно | [ObjectMessage.cpp](../../server/src/ObjectMessage.cpp), [Message.cpp](../../server/src/Message.cpp), `MessageContractTest.BodyRoundTrip`, `ObjectMessageTest` | Тело — opaque-блоб, `className` — length-prefixed метаданные внутри `dataAsBytes`, не заголовок и не свойство; `Selector::parse("className IS NULL")` подтверждает невидимость для стандартного селектора. |
| Хранилище v0x02/v0x03 | `MessageContractTest.StorageV02Compatibility`, `StorageWorkerResilienceTest.*`, код [Message.cpp](../../server/src/Message.cpp) `fromBytes` | v0x02 читается, v0x03 сохраняет null/пустой тип/двоичный ID. `readString` и чтение `propsLen` ограничивают усечение и переполнение. Ошибка операции в worker логируется и не убивает поток; после разрушения Exchange нет утечки worker-потока. |
| Java-модуль локальный | [JavaMessageAdapter.java](../../clients/java/src/main/java/tinymq/jms/JavaMessageAdapter.java), [MessageEnvelope.java](../../clients/java/src/main/java/tinymq/jms/MessageEnvelope.java), Java-runner `bodyRoundTrip` | Массивы копируются на входе и выходе, `setObject` снимает снимок, `forDelivery` строит свежий вид из замороженного конверта. Сетевых компонентов нет. |
| Десериализация Java | [TinyObjectMessage.java](../../clients/java/src/main/java/tinymq/jms/TinyObjectMessage.java), `verify-security.sh` FilterProbe | `ObjectInputStream` работает по стандартной политике JVM (`jdk.serialFilter`); проба подтвердила отказ до `readObject`. |
| Селектор | [Selector.cpp](../../server/src/Selector.cpp), `MessageContractTest.SelectorGrammarMatrix`, `SelectorTest.*` | Лимит 512 токенов; целочисленная арифметика без UB (unsigned wrap + `bit_cast`, защита `min / -1`, деление на 0 → UNKNOWN); границы LIKE/ESCAPE и чисел проверены. |

## Находки

Конкретного дефекта в границах SDD 70 не найдено и не воспроизведено. Остаточные наблюдения
(не блокируют эту стадию, входят в зону SDD 73/74 или будущих стадий):

1. **Стандартный фильтр сериализации.** `TinyObjectMessage.getObject()` полагается на
   общепроцессный `jdk.serialFilter`, а не на per-stream фильтр. Для локального round-trip
   собственных байтов в SDD 70 это корректно; SDD 74 при подключении провайдера к сети
   обязан ставить ограничивающий per-stream фильтр при приёме сторонних ObjectMessage.
2. **Длина селектора.** Лимит 512 токенов ограничивает число токенов, но один длинный строковый
   литерал не ограничен по байтам. Точки внешнего ввода селектора подключаются в SDD 71/74,
   поэтому для SDD 70 это не уязвимость.
3. **Повреждённая локальная запись.** В `Consumer::recv` быстрый путь вызывает `fromBytes` без
   локального перехвата; некорректный JSON свойств в локальном файле привёл бы к исключению.
   Записи создаёт сам брокер, а приём внешних записей — SDD 73/74; в область SDD 70 это не входит.

Секретов, credentials и внешних ключей в изменённом коде не обнаружено.
