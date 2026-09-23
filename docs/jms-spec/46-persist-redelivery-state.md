# Persist redelivery state across broker restart

> **Заготовка, не валидный SDD.** Заведена 2026-09-21 решением Owner при закрытии спеки 24:
> `deliveryCount` и `deliveryTime` backoff'а не переживают рестарт — осознанное ограничение
> спеки 24 (Semantics 4, 9). Перед передачей агенту довести до `_template.md`: Owner,
> Entity inventory, Stop-conditions, Semantics в форме Requirement/Scenario с `Test:`.
>
> Класс документа: **K1 — Executable** (Standard 6).

## Owner
- `<заполнить при взятии в работу>`

## JMS reference
- JMS 2.0 § 3.5.9 `JMSXDeliveryCount` — «number of delivery attempts»; JMS не требует
  сохранения через рестарт, но провайдеры (ActiveMQ Classic, Artemis) сохраняют.

## Current state in tiny-mq
- Спека 24: `Consumer::redeliver` меняет `deliveryCount`/`redelivered`/`deliveryTime` только в
  памяти и в `_cachedStorageBytes` (ADR-0008); запись `0x02` в storage не обновляется. После
  рестарта реплей даёт `deliveryCount == 0`, лимит `maxRedeliveries` отсчитывается заново,
  backoff теряется (`DlqTest.RestartResetsCounterAndLimit`).
- `ConcurrentLinearStorage` умеет `APPEND / APPEND_BATCH / REMOVE / SCAN / GET`;
  позиционной записи нет.
- `deliveryCount` лежит в записи по фиксированному смещению: `+55` от начала записи
  (type-byte + magic + number + uuid + reliability + timestamp + expiration + deliveryTime +
  priority); `deliveryTime` — `+43`. См. `priorityFromStorageBytes` в `Destination.cpp`.

## Proposed API (набросок)
- Новая операция storage `PATCH_AT` (`OperationId`): позиционная запись N байт по
  `record.offset + fieldOffset`, fire-and-forget через существующий worker
  (`patchAsync(const Record&, size_t fieldOffset, std::span<const char>)`).
- `Consumer::redeliver` для persistent-сообщений после `refreshCachedStorageBytes()`
  патчит `deliveryCount` (и `deliveryTime`, если backoff) в записи origin.
- Формат `0x02` **не меняется**; ADR не требуется, если `PATCH_AT` не меняет инварианты
  append-лога (запись внутри существующей записи, без сдвига).

## Semantics (набросок)
- После рестарта реплей persistent-сообщения даёт `deliveryCount`, равный значению на
  момент последней повторной доставки; лимит продолжает отсчёт, а не начинается заново.
- Отложенное backoff'ом сообщение остаётся отложенным до исходного `deliveryTime`.
- Durable-подписки: патч идёт в storage подписки (`Consumer::_storage`).
- Крэш между `refreshCachedStorageBytes` и патчем: допускается откат счётчика на 1
  (at-least-once по доставке сохраняется).

## Dependencies
- 24 (redelivery), 13 (delivery delay), 23 (recover).

## Test plan (набросок)
- `RedeliveryPersistenceTest.DeliveryCountSurvivesRestart` — инверсия
  `DlqTest.RestartResetsCounterAndLimit`.
- `RedeliveryPersistenceTest.BackoffSurvivesRestart`.
- `RedeliveryPersistenceTest.DurableSubscriberCountSurvivesRestart`.
- Бенч: `Transacted_*` без регрессии — патч не на горячем пути первой доставки.

## Open questions
- Нужен ли `PATCH_AT` в API `Storage` (синхронный) или только async-вариант.
- Порог перф-гейта для пути rollback (не покрыт бенчами — см. отчёт перф-гейта спеки 24).
