# Критика SDD 70

Класс K4. Свидетельства независимых раундов от 2026-11-15.

## Раунд 1

**Вердикт DeepSeek Pro — `needs-work`.** Замысел понятен. Восемь вопросов показывают, где
предложенные тесты допускают несовместимое поведение или оставляют границу ответственности неясной.
Реализация ждёт уточнения постановки и нового раунда критики.

Проверены [SDD 70, редакция 0.1](../jms-spec/70-jakarta-message-contract.md), относящиеся к ней
фрагменты [INT-01, редакция 0.3](../../requirements/intents/INT-01-jakarta-messaging.md),
закреплённые проектные источники, Jakarta Messaging 3.1 §§3.3–3.12 и 7.3.9 и Javadoc
`Message` и `ObjectMessage`. Код реализации критик не получал.
SDD SHA-256 `87cf2b0176678db98ec6b95acfd36c79c1ab7e6a0ce95830aeeb08356022b9c0`.
INT SHA-256 `f8e80f76a6372a9fd10e910d0549970f83986424efc0d1f95f6d448152283132`.

## Вопросы критика

| ID | Требование | Пробел и ожидаемое уточнение |
|---|---|---|
| Q1 | REQ-70-1 | `CrossLanguageEnvelope` не определяет, какие реальные границы пересекает тест. Один кодек на обеих сторонах может скрыть несовместимость Java ↔ C++. Указать границу проверки конверта и отдельно границу сквозного обмена по сети. [Стык INT-01](../../requirements/intents/INT-01-jakarta-messaging.md#стыки), [сквозная проверка SDD 77](../jms-spec/77-jakarta-conformance-release.md#scenario-javaandcpproundtrip) |
| Q2 | REQ-70-1, REQ-70-4 | В конверте и реестре отсутствует `JMSRedelivered`. Уточнить, какая постановка задаёт признак на доставке, а какая проверяет его перенос вместе с `JMSXDeliveryCount`. При `JMSRedelivered=true` счётчик должен быть не меньше 2. [§3.4.7](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#jmsredelivered), [§3.5.11](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#jmsxdeliverycount) |
| Q3 | REQ-70-1 | Сценарий пропускает подсказки отключения ID и timestamp. Указать политику tiny-mq и проверить результат. При принятии соответствующей подсказки значение равно `null` или `0`. Игнорирование подсказки с обычным значением допустимо. [§3.4.3](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#jmsmessageid), [§3.4.4](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#jmstimestamp) |
| Q4 | REQ-70-5 | Матрица не требует проверить, что в селекторе `JMSDeliveryMode` сравнивается со строками `'PERSISTENT'` и `'NON_PERSISTENT'`. Числовое представление могло бы пройти тесты синтаксиса. [§3.8.1.3](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#special-notes) |
| Q5 | REQ-70-5 | Фраза «неявное преобразование строки» не задаёт исход. Если значение свойства — строка `"2"`, числовое сравнение `property > 1` её не преобразует и даёт `FALSE`. [§3.8.1.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#message-selector-syntax) |
| Q6 | REQ-70-4 | Матрица не проверяет ограничения на имена свойств и их связь с грамматикой идентификатора селектора. Отдельно проверить `IllegalArgumentException` при `null` или пустом имени в Java setter. [§3.5.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#property-names), [Javadoc Message](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/Message.html) |
| Q7 | REQ-70-3 | Сценарий не закрепляет повторное чтение того же значения после ошибки преобразования. В `StreamMessage` и `BytesMessage` курсор сохраняется при `MessageFormatException` или `NumberFormatException`, если метод чтения выбросил такое исключение. [§3.11.3](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#conversions-provided-by-streammessage-and-mapmessage) |
| Q8 | REQ-70-2, REQ-70-3 | «Контролируемая ошибка» слишком широка. При сбое десериализации `ObjectMessage` методы `getObject()` и `getBody()` должны выбрасывать `MessageFormatException`, а `isBodyAssignableTo()` возвращает `false`. Указать, какой сценарий проверяет каждый исход. [Javadoc ObjectMessage](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/objectmessage), [Javadoc Message](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/Message.html) |

## Сверка после получения вердикта

- Q1 сохраняется. В таблице стыков INT для конверта указан `MessageContractTest.CrossLanguageEnvelope`. `ReleaseConformanceTest.JavaAndCppRoundTrip` относится к отдельному сквозному сценарию INT-01-INTEROP. [INT-01](../../requirements/intents/INT-01-jakarta-messaging.md)
- Варианты, допускающие исключение `JMSDeliveryMode` из селектора или пропуск восстановления курсора, несовместимы с нормой. Замечание модели о названии runtime-исключения уточнено: `InvalidSelectorRuntimeException` определён стандартом. Отображение ошибки в C++ задаёт контракт проекта. [§3.8](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#message-selection), [§3.11.3](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#conversions-provided-by-streammessage-and-mapmessage), [§10.3](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#standard-exceptions)
- Дополнительный пробел, найденный при сверке Javadoc. `BodyRoundTrip` проверяет мутацию после send, но для `ObjectMessage` нужен отдельный случай мутации между `setObject()` и send. Снимок объекта делается при `setObject()`. [Javadoc ObjectMessage](https://jakarta.ee/specifications/platform/10/apidocs/jakarta/jms/objectmessage)
- Остальные наблюдения модели касаются поведения строкового getter при двоичном ID, примера стороннего `Message`, исторического `className`-hint, выбора фильтра бенчмарка и числового продвижения в смешанных сравнениях. Владельцы вычисления TTL и delay, а также проверки стороннего сообщения уже обозначены в соседних постановках. В SDD 70 нужна явная граница с этими проверками. [SDD 72](../jms-spec/72-jakarta-delivery.md#requirement-send-clock-expiration-и-приоритет), [SDD 74](../jms-spec/74-jakarta-java-se-provider.md#requirement-исключения-и-сторонние-сообщения)

## Свидетельства

Профиль `codex exec -p deepseek-pro`, модель `deepseek-v4-pro`, провайдер `https://api.deepseek.com/`.
Переменные HTTP(S)/ALL proxy удалены из окружения процесса. Сессия эфемерная, в отдельном пустом каталоге,
с песочницей `read-only`. Codex завершился с кодом 0. Проверка AEF `trace.py sdd` и подготовка `trace.py input` прошли.

Пакет `handoffs/current/70/critic-r1.json` хранит вердикт и вопросы модели. Исходный ответ и метаданные
прогона находятся в `handoffs/current/70/logs/critic-round-1.log`. Маршрутизатор обработал пакет на
чистой временной копии проверенной редакции и остановил цепочку на `needs-work`.

## Раунд 2

**Вердикт DeepSeek Pro — `needs-work`, один блокер.** Проверена редакция 0.2, SHA-256
`2686471958686c0c52f29b5e86965141bd53a6bb3eea45d240d4aa4767a2ed5f`.
Критик подтвердил закрытие Q1–Q8 и отдельного замечания о снимке при `setObject()` до send.

**Q10 · REQ-70-7.** Локальный Java-runner мог проверить значения и типы свойств группы, но
у него нет соединения и `ConnectionMetaData`. Объявление имён в метаданных могло остаться
непроверенным при зелёном `GroupProperties`. В редакции 0.3 локальный тест проверяет конверт,
а `ConnectionMetaData.getJMSXPropertyNames()` явно проверяется в
[REQ-74-3](../jms-spec/74-jakarta-java-se-provider.md#requirement-классический-жизненный-цикл).
[Jakarta Messaging §3.5.9](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html#jakarta-messaging-defined-properties)

Замечания о проверке уникальности ID через рестарт и Java-отображении ошибок селектора приняты в
SDD 74. SDD 70 явно проверяет копирование `byte[]` и недопустимое вложенное значение MapMessage.
Для проверки бюджета выделен идентификатор `NfrGateTest.HotPathBudget`.

Использован `codex exec -p deepseek-pro` напрямую, с `read-only` и усилием рассуждения `low` в
завершившейся сессии. Два прерванных запуска завершились с кодом 130 без вердикта. AEF записал
один второй раунд. Исходный ответ сохранён в `handoffs/current/70/logs/critic-round-2.log`.

## Раунд 3

**Вердикт DeepSeek Pro — `clean`.** Критик подтвердил закрытие Q10. Локальный
`GroupProperties` проверяет значения и типы свойств, а [REQ-74-3](../jms-spec/74-jakarta-java-se-provider.md#requirement-классический-жизненный-цикл)
проверяет объявление всех трёх JMSX-имён у реального Java-соединения. Вопросов и новых замечаний нет.

Проверена SDD 70, редакция 0.3, SHA-256 `9f1de027704221843c770339f4b6ea1cca4712f795b6bbb3d63a94c0f972d8d1`,
и относящиеся к стыку фрагменты SDD 74, редакция 0.2, SHA-256
`2ec074163c9981dbe6a10f4f6f73a7765640c29287b11dec21e291e4611d8ec8`.
Использован тот же прямой профиль `codex exec -p deepseek-pro` с `read-only` и усилием `low`.
Две команды критика только подтвердили отсутствие файлов в пустом каталоге сессии.

AEF на чистой временной копии записал `critic.input.json`, подтвердил соответствие входа
и подготовил ручной запуск автора с `exec=0`. Проверки `trace.py sdd` и `trace.py input` прошли
для SDD 70 и 74. Заключение касается постановки и плана тестов.
Действующий пакет — `handoffs/current/70/critic.json`. Пакеты раундов 1 и 2 сохранены рядом
с исходными ответами в `handoffs/current/70/logs/`.
