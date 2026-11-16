# Шлюзы SDD 71

## Conformance

**Rejected.** Итерация 2, [SDD v0.5](../jms-spec/71-jakarta-destinations.md). В отдельной Debug-сборке прошли 14/14 целевых тестов и 172/172 тестов без бенчмарков. `trace sdd`, `trace input` и `trace tests` завершились с кодом 0. Вердикт учитывает полноту утверждений тестов и дрейф реестра сущностей. [Целевой прогон](../../handoffs/current/71/logs/conformance-test-plan.log) · [Полный прогон](../../handoffs/current/71/logs/conformance-verify.log) · [Trace](../../handoffs/current/71/logs/conformance-trace-tests.log)

SDD 71 проверяет локальный C++ claim и его физическое квитирование, доставку по selector, видимость по заданным датам, владение и восстановление persistent-записей при локальном перезапуске. Грамматику и трёхзначную логику проверяет запущенный `MessageContractTest.SelectorGrammarMatrix` из SDD 70. Политика дат, режимов ack, асинхронной доставки и восстановления после отказа относится к SDD 72, транспорт и авторизация — к SDD 73, Java API и отображение исключений — к SDD 74. [SDD 70](../jms-spec/70-jakarta-message-contract.md#requirement-компилятор-и-трёхзначное-вычисление) · [SDD 72](../jms-spec/72-jakarta-delivery.md) · [SDD 73](../jms-spec/73-jakarta-broker-transport.md) · [SDD 74](../jms-spec/74-jakarta-java-se-provider.md)

Критерии сверены с указанными разделами закреплённой копии Jakarta Messaging 3.1 Final Release. § 4.1.6 оставляет провайдеру выбор видимости конкурентных изменений, а SDD 71 фиксирует политику снимка. § 4.2.4 задаёт обязательный набор для переносимых имён и поддержку длины до 128 символов, а SDD уточняет классификацию Java 21 и предел в кодовых точках UTF-8. Независимое сравнение таблицы категорий с Unicode 15.0.0 нашло 0 расхождений. [Копия Jakarta Messaging 3.1](../../handoffs/current/71/jakarta-messaging-3.1.html) · [Сверка разделов](../../handoffs/current/71/logs/conformance-source-audit.log) · [Сравнение Unicode](../../handoffs/current/71/logs/conformance-unicode.log)

| Требование и разделы Jakarta Messaging 3.1 | Тест из прогона | Покрытие |
|---|---|---|
| REQ-71-1 · § 4.1.2, 6.2.9–6.2.10, 8.2, 8.8 | [DestinationContractTest.SelectiveQueueClaim](../../tests/DestinationContractTest.cpp#L37) | Перекрывающиеся селекторы на двух потоках дают уникальные claim при смешанной persistent и non-persistent отправке. Unmatched доступна другой сессии. Закрытие consumer оставляет claim сессии. Rollback, recover и закрытие сессии освобождают запись, устаревший handle её не квитирует, а после рестарта видна лишь неподтверждённая persistent-запись. Queue commit с обоими режимами также пройден в `PersistentTransactionTest.testMixedPersistentNonPersistentInTransaction`. |
| REQ-71-2 · § 4.2.2, 6.2.7, 8.3.1–8.3.4 | [DestinationContractTest.SubscriptionFanout](../../tests/DestinationContractTest.cpp#L143), [DestinationContractTest.NoLocalMatrix](../../tests/DestinationContractTest.cpp#L380) | Проверены оба режима сохранности на send и commit, включая задержку. Подписки выбираются при публикации. Копии изолированы, последний shared nondurable consumer сбрасывает backlog, offline shared durable получает persistent backlog при открытии после рестарта. Параллельный claim внутри shared не проверен. |
| REQ-71-3 · § 4.2.4, 6.1.2, 8.3.3–8.3.4 | [DestinationContractTest.DurableIdentityMatrix](../../tests/DestinationContractTest.cpp#L241) | Проверены требования к client ID, пространство shared nondurable, конфликты unshared durable, запрет замены и удаления с pending claim, trim при повторном открытии, замена поколения, unsubscribe shared с ID и без него, ошибки имени, рестарт и отказ при повреждённых метаданных. Legacy-данные доступны по прежнему адресу и изолированы от нового ключа. Покрытие shared durable и проверки старой сборкой неполное. |
| REQ-71-4 · § 4.2.2, 8.3.1, 8.3.3–8.3.4, A.3.20 | [DestinationContractTest.NoLocalMatrix](../../tests/DestinationContractTest.cpp#L380) | Своя публикация отсечена у обоих unshared. Другое соединение с тем же client ID проходит для nondurable и фильтруется для durable, включая offline. Чужой commit доставлен, shared принимает свою публикацию, durable noLocal без client ID отклонён. |
| REQ-71-5 · § 4.1.6 | [DestinationContractTest.BrowseWithoutClaim](../../tests/DestinationContractTest.cpp#L430) | Снимок исключает claimed, unmatched, просроченные и будущие записи. Проверены приоритет, FIFO внутри полосы, копирование и доступность сообщений для дальнейшего receive. Покрытие неизменности снимка и отсутствия регистрации доставки неполное. |
| REQ-71-6 · § 2.16, 4.1.5–4.1.7, 4.2.7, 4.2.10, 6.2.2 | [DestinationContractTest.TemporaryReplyLifecycle](../../tests/DestinationContractTest.cpp#L496) | Для обоих типов пройдён request/reply с JMSReplyTo и JMSCorrelationID, запрет чужого потребления и удаления, запрет удаления с активным consumer и проверка границы брокера. Для очереди также проверены запрет чужого browse, явное удаление и новая ссылка. Для топика проверены запрет durable и удаление при закрытии владельца. Отмена отложенных доставок напрямую не проверена. |
| REQ-71-7 | `NfrGateTest.DestinationHotPathBudget` | Профильный пакет `approved`, итерация 2, отпечаток совпадает с этим прогоном. В решающей серии ABBA из семи повторов все три пути в пределах бюджета, максимальный CV 4,42%. Отдельно измерены concurrent claim и fan-out на 1, 2 и 4 подписки. [Пакет NFR](../../handoffs/current/71/nfr.json) · [Отчёт NFR](71-jakarta-destinations.nfr.md#решающая-полная-серия-nfr-r2-warmed-2) |

### Блокеры

- **REQ-71-2.** Пары shared nondurable и shared durable читают по очереди. Требование о единственном claim при конкурирующих активных участниках проверено для обычной очереди и не наблюдается для двух видов shared-подписки. [Nondurable](../../tests/DestinationContractTest.cpp#L175) · [Durable](../../tests/DestinationContractTest.cpp#L229)
- **REQ-71-3.** Активный конфликт по топику и по селектору проверен у shared nondurable. У стандартной shared durable отсутствуют проверки этих двух конфликтов, запрета unsubscribe и замены при неподтверждённом claim, повторного открытия с эквивалентным selector и разрешённой замены с удалением backlog. Замена unshared меняет топик, selector и noLocal одновременно, поэтому не обнаружит реализацию, которая игнорирует одно из полей при замене. Тест моделирует остаток старого поколения после переключения реестра. Исход с сохранением старого поколения при прерывании до переключения отсутствует. В блоке legacy тест создаёт запись и читает копию через `linear_storage::Storage` внутри бинарника кандидата. Проверки с запуском старой сборки на копии, как требует `DurableIdentityMatrix`, нет. [Матрица ключей](../../tests/DestinationContractTest.cpp#L259) · [Смена поколения](../../tests/DestinationContractTest.cpp#L293) · [Копия legacy](../../tests/DestinationContractTest.cpp#L328)
- **REQ-71-5.** Совместное выполнение send, claim и ack проверяет размер второго снимка 3 или 4, без сверки допустимого полного состава. После recover и последующих receive проверяется только размер первого снимка. Переход записи через expiration после снятия снимка и отсутствие claim браузера в его сессии отдельными утверждениями не наблюдаются. [Снимок под конкуренцией](../../tests/DestinationContractTest.cpp#L464)
- **REQ-71-6.** Для временной очереди пройдено явное удаление, для временного топика — закрытие владельца. Обратные ветви для обоих типов отсутствуют. Тест планирует persistent-доставку с задержкой 30 секунд и затем удаляет назначение, проверяя недействительность ссылки. Отмена самой доставки напрямую не наблюдается. [Ветки удаления](../../tests/DestinationContractTest.cpp#L531)
- **Дрейф Entity inventory.** E24 описывает `DeliveryState` с ожидающими получателями, однако отдельный новый тип `DeliveryState::Waiter` отсутствует в E1–E32. Тип держит selector и состояние ожидания и используется при claim и адресном пробуждении. [Реестр SDD](../jms-spec/71-jakarta-destinations.md#entity-inventory) · [Объявление типа](../../server/src/Destination.h#L64) · [Операция claim](../../server/src/Destination.cpp#L79)

Отпечатки совпали. `tree:287bfa69d990bd878ff03385c83757a4f1d5c3e985af6b0a86535e7dd6d3f7b0`. [До](../../handoffs/current/71/logs/conformance-fingerprint-before.log) · [После](../../handoffs/current/71/logs/conformance-fingerprint-after.log)

## Conformance · итерация 3

**Approved**, OpenAI GPT-6.1-Sol. Закрыты конкурентный claim обоих shared-режимов,
сохранение и замена durable-конфигурации, прерывания до и после переключения,
состав и неизменность снимка, явное и автоматическое закрытие временных назначений,
освобождение копии отложенной доставки. Реестр v0.6 включает `DeliveryState::Waiter`
и `ScheduledMessageProbe`. После правки GCC прошли 172 теста, трассировка требований,
экспорт durable backlog и чтение его копии C++-брокером из `7770feb`.
[Тесты](../../tests/DestinationContractTest.cpp) ·
[Прогон](../../handoffs/current/71/logs/conformance-r3-gcc-verify.log) ·
[Старый читатель](../../handoffs/current/71/logs/conformance-r3-gcc-old-reader.log) ·
[REQ-71-7](71-jakarta-destinations.nfr.md)

## Security · итерация 3

**Approved**, DeepSeek V4 Pro напрямую. Шлюз проверил имена и UTF-8, UUID-пути
поколений durable, отказ при повреждённых метаданных, владение временными
назначениями и отмену отложенной доставки при их закрытии. После правки GCC под
AddressSanitizer прошли все 172 теста. Сетевая аутентификация, TLS и ACL описаны
в SDD 73. [Прогон ASan](../../handoffs/current/71/logs/security-r3-gcc-asan.log) ·
[SDD 73](../jms-spec/73-jakarta-broker-transport.md)

Оба прогона привязаны к `tree:68d748ff22964393813504c7bd537db2c89bf5045546371cfd4f4811865651c1`.
