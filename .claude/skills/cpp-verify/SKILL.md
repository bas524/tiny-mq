---
name: cpp-verify
description: Проектный verify для tiny-mq — собрать и прогнать все тесты, убедиться в чистоте -Werror. Используй перед сдачей любого изменения кода и как оракул «сделано». Триггеры: «проверь сборку», «прогони тесты», «verify», перед ревью/коммитом.
---

# cpp-verify

Оракул «сделано» для контура Verification. Собирает через штатный vcpkg+ninja и гоняет весь GTest-набор.

## Процедура

0. **Куда писать вывод.** Все прогоны сохраняются в файл — `evidence` в handoff-пакете
   это **путь к логу, а не проза о нём** (Standard 15). Заведи каталог:
   ```
   mkdir -p handoffs/<spec>/logs
   ```

1. **Сборка** (директория преднастроена):
   ```
   cd cmake-build-debug && ninja 2>&1 | tee ../handoffs/<spec>/logs/build.log
   ```
   Если конфигурируешь с нуля — только с vcpkg-toolchain:
   ```
   cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -G Ninja ...
   ```
   Без toolchain зависимости (Poco, GTest, benchmark, parallel-hashmap, span) не находятся — **не** откатывайся на голый `cmake --build`.

2. **Все тесты:**
   ```
   ./cmake-build-debug/tiny_mq --gtest_filter='-*Bench*' 2>&1 | tee handoffs/<spec>/logs/cpp-verify.log
   ```
   Один набор по фильтру:
   ```
   ./cmake-build-debug/tiny_mq --gtest_filter=<Suite>.*
   ```
   Фильтр обязателен: бинарь без аргументов падает в SIGSEGV (`main.cpp:207` разыменовывает
   `argv[1]` при `argc==1`) — пустой запуск даёт не «упавшие тесты», а отсутствие прогона.

3. **Warnings-as-errors.** Сборка обязана быть чистой по `-Wall -Werror -Wextra -Wshadow`. Любое предупреждение = падение сборки = не пройдено. Чини причину, не подавляй.

4. **Release-сборка тоже обязательна** (`cmake --preset user-release && cmake --build --preset release --parallel`,
   вывод в `handoffs/<spec>/logs/build-release.log`). В release `poco_trace`/`poco_debug`
   компилируются в ничто (`POCO_LOG_DEBUG` выключен), и переменная, живущая только ради лога,
   становится `-Werror=unused-variable` — debug этого не видит. Так упала сборка дистрибутива
   в PR #7 (спека 24). Приём для таких переменных — `[[maybe_unused]]`, как `requeued` в
   `Consumer::recover()`.

## Критерий прохождения
- debug **и release** собрались без ошибок и предупреждений;
- `./cmake-build-debug/tiny_mq` — все тесты зелёные (0 failed).

## Что возвращать (Standard 15)

В `evidence` — **пути к логам**: `handoffs/<spec>/logs/build.log`,
`handoffs/<spec>/logs/cpp-verify.log`. Роутер проверяет, что файлы существуют и непусты;
их отсутствие трактуется как сфабрикованная проверка и останавливает цепочку.

Строку итогов (`[  PASSED  ] N tests`, при падении — `[  FAILED  ]` + имя теста) клади в
`evidence_summary` — поле для чтения человеком, **без доказательной силы**. Не объявляй
«done» без зелёного прогона и не пересказывай лог вместо того, чтобы на него сослаться.
