# Явный toolchain сравнительного Linux-замера (M-04)

- **Класс:** сопровождение, сборочная оснастка.
- **Статус:** проверено локально; GitHub не принят.
- **Autonomy Level:** подготовка R2, слияние R1.
- **Заведено:** 2026-11-18 по PR №12.
- **Owner:** Alexander B.

## Symptom

[Broker hot paths на Ubuntu 24.04](https://github.com/bas524/tiny-mq/actions/runs/44364616253)
не дошёл до замеров: линковка baseline `230eefe` с GCC 13 и LTO
остановилась на `-Werror=maybe-uninitialized` в `std::optional` внутри
тестов MessageContractTest. Полный журнал сохранён в
`handoffs/current/72/logs/github-ubuntu24-job-135014904620.log`.

Повтор со штатным GCC 14 также остановил baseline main `230eefe` до измерений:
GNU14 LTO, `Selector.cpp:257`, `std::optional/variant`,
`-Werror=maybe-uninitialized`. Точный журнал:
`handoffs/current/72/logs/github-gcc14-job-135032173945.log:1470–1610`.

## Spec ref

`tiny-mq/72:REQ-72-9@0798bdf9166144728477d18bf4447a8d8acfddc8`,
[бюджет и сопоставимость](../docs/jms-spec/72-jakarta-delivery.md#requirement-бюджет-горячего-пути-доставки).

## Cause

Выбор GCC 14 не устранил наблюдаемый сбой baseline при LTO и предупреждениях
как ошибках. Это инфраструктурный провал до запуска runtime и измерений;
по нему нельзя принять или отклонить бюджет производительности кандидата.

## Done criteria

Обе версии в Broker hot paths явно используют `CC=clang-18`, `CXX=clang++-18`
и `LDFLAGS=-fuse-ld=lld-18` одного штатного образа Ubuntu 24.04.
[toolset-2404.json](https://github.com/actions/runner-images/blob/main/images/ubuntu/toolsets/toolset-2404.json)
включает Clang 18, а [install-clang.sh](https://github.com/actions/runner-images/blob/main/images/ubuntu/scripts/build/install-clang.sh)
устанавливает `clang-V` и `lld-V`. Новые пакеты для этого toolchain не добавляются.
Перед сборками временный исходник компилируется и линкуется обоими драйверами
с warning-as-error и LTO; полученные программы запускаются. Job timeout — 60 минут
для холодного кеша и прежних длинных измерений.
Ключ vcpkg-кеша включает OS, arch, compiler, VCPKG_TAG, ImageOS, ImageVersion
и хеш манифеста, без SHA коммита. Fallback ограничен теми же OS/arch/compiler/tag;
совместимость бинарных пакетов проверяет vcpkg. После первых двух SHA-строк
`environment.txt` сохраняются версии CC/CXX/linker; CPU и affinity сохраняются.
База и кандидат должны собраться с неизменными LTO,
`-Wall -Werror -Wextra -Wshadow`, затем пройти прежние 15 сравнений.
Границы регрессии и CV остаются 5%, ABBA, девять повторов на сторону.
Новые внешние библиотеки и источники пакетов не добавляются.

## Scope-lock

`.github/workflows/performance.yml` — stock toolchain, sanity compile, ключ кеша,
тайм-аут broker job и запись версий инструментов.
`tasks/M-04-paired-perf-toolchain.md` — постановка и результаты.

## Evidence

- Исходные сбои приведены выше. Локальная готовность YAML/shell:
  `handoffs/current/72/logs/producer-i4-clang-ready.log`.
- GitHub не запускался этой стадией; успешная Linux-сборка и результаты НФТ
  ожидаются от оператора и независимого NFR. Независимый reviewer — отдельная стадия.
