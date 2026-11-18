# Удаление метаданных попытки через псевдоним корня хранилища (M-01)

- **Класс:** сопровождение, дефект хранения.
- **Статус:** в работе.
- **Autonomy Level:** R2, слияние R1.
- **Заведено:** 2026-11-18 по проверке кандидата SDD 72.
- **Owner:** Alexander B.

## Symptom

На macOS штатный псевдоним `/tmp` указывает на `/private/tmp`. Создание брокера
через `/tmp/<каталог>/` и повтор persistent сообщения с backoff дают один файл
`attempt-72/<uuid>`. После повторной выдачи и успешного ack в журнал попадает
`delivery-72 attempt cleanup: Bad data format`, а файл остаётся до следующего
открытия хранилища. При запуске через физический путь той же временной области
последовательность удаляет файл. Воспроизведение на `d8c60b2` использует
`--delivery-test-child DIR retry-crash` с `TINY_MQ_TEST_CRASH=retry:scheduled`
и затем `--delivery-test-child DIR retry-check`.

## Spec ref

`tiny-mq/72:REQ-72-7@d8c60b25e358aa1b0665a0d5a6d78fd529167344`.
[ADR-0010](../arch/0010-atomic-local-delivery.md) ограничивает объём метаданных
активными persistent записями и описывает удаление внешнего кадра после ack
и защиту операций внутри каталога брокера.

## Cause

Путь, переданный на вход Exchange, может содержать допустимый псевдоним своего
корня. Журнал открывает канонический корень и использует его для чтения и записи
метаданных. Путь очистки после ack повторно открывает компоненты исходного
абсолютного пути со строгим запретом симлинков. На macOS он останавливается на
компоненте `tmp` ещё до открытия хранилища.

## Done criteria

- `DeliveryContractTest.CrashAndDeadLetterMatrix` проверяет, что persistent
  запись с внешними метаданными успешно квитируется и файл исчезает при запуске
  через псевдоним корня. Проверка работает на POSIX с временным симлинком.
- Существующие защиты `decision` и `attempt-72` сохраняются при попытках
  подмены вложенного пути. Внешние файлы сохраняют содержимое.
- Полный Debug и `clang-tidy`, матрица Release и Broker hot paths на
  публикуемой редакции PR №12.

## Scope-lock

`tasks/M-01-attempt-cleanup-root-alias.md`, `server/src/Exchange.cpp`,
`server/src/LocalTransactionJournal.cpp`, `server/src/LocalTransactionJournal.h`,
`server/src/LinearStorage.cpp`, `server/src/LinearStorage.h`,
`tests/DeliveryContractTest.cpp`.

## Evidence

- `handoffs/current/M-01/logs/repro-before.log`
