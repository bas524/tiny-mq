# Явный toolchain сравнительного Linux-замера (M-04)

- **Класс:** сопровождение, сборочная оснастка.
- **Статус:** исправление подготовлено, CI и независимая проверка ожидаются.
- **Autonomy Level:** подготовка R2, слияние R1.
- **Заведено:** 2026-11-18 по PR №12.
- **Owner:** Alexander B.

## Symptom

[Broker hot paths на Ubuntu 24.04](https://github.com/bas524/tiny-mq/actions/runs/44364616253)
не дошёл до замеров: линковка baseline `230eefe` с GCC 13 и LTO
остановилась на `-Werror=maybe-uninitialized` в `std::optional` внутри
тестов MessageContractTest. Полный журнал сохранён в
`handoffs/current/72/logs/github-ubuntu24-job-135014904620.log`.

## Spec ref

`tiny-mq/72:REQ-72-9@0798bdf9166144728477d18bf4447a8d8acfddc8`,
[бюджет и сопоставимость](../docs/jms-spec/72-jakarta-delivery.md#requirement-бюджет-горячего-пути-доставки).

## Cause

Workflow выбирает образ, но компилятор остаётся системным умолчанием.
При смене стандартного образа это GCC 13 вместо ранее применявшегося GCC 15.
Это наблюдаемая ошибка сборки, не доказательство ошибки памяти в библиотеке.

## Done criteria

Обе версии в Broker hot paths явно используют штатные `gcc-14` и `g++-14`
одного образа Ubuntu 24.04. Согласно [инвентарю GitHub runner-images](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md),
GCC 14 предустановлен. База и кандидат должны собраться с неизменными LTO,
`-Wall -Werror -Wextra -Wshadow`, затем пройти прежние 15 сравнений.
Границы регрессии и CV остаются 5%, ABBA, девять повторов на сторону.
Новые внешние библиотеки и источники пакетов не добавляются.

## Scope-lock

`.github/workflows/performance.yml` — только переменные CC/CXX у broker job.
`tasks/M-04-paired-perf-toolchain.md` — постановка и результаты.

## Evidence

- Ссылка на исходный сбой приведена выше. Исправление пока не принято.
