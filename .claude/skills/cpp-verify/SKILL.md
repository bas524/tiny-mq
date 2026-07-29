---
name: cpp-verify
description: Проектный verify для tiny-mq — собрать и прогнать все тесты, убедиться в чистоте -Werror. Используй перед сдачей любого изменения кода и как оракул «сделано». Триггеры: «проверь сборку», «прогони тесты», «verify», перед ревью/коммитом.
---

# cpp-verify

Оракул «сделано» для контура Verification. Собирает через штатный vcpkg+ninja и гоняет весь GTest-набор.

## Процедура

1. **Сборка** (директория преднастроена):
   ```
   cd cmake-build-debug && ninja
   ```
   Если конфигурируешь с нуля — только с vcpkg-toolchain:
   ```
   cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -G Ninja ...
   ```
   Без toolchain зависимости (Poco, GTest, benchmark, parallel-hashmap, span) не находятся — **не** откатывайся на голый `cmake --build`.

2. **Все тесты:**
   ```
   ./cmake-build-debug/tiny_mq
   ```
   Один набор по фильтру:
   ```
   ./cmake-build-debug/tiny_mq --gtest_filter=<Suite>.*
   ```

3. **Warnings-as-errors.** Сборка обязана быть чистой по `-Wall -Werror -Wextra -Wshadow`. Любое предупреждение = падение сборки = не пройдено. Чини причину, не подавляй.

## Критерий прохождения
- `ninja` собрался без ошибок и предупреждений;
- `./cmake-build-debug/tiny_mq` — все тесты зелёные (0 failed).

Верни в `evidence`: строку итогов GTest (`[  PASSED  ] N tests`) и, при падении, точный `[  FAILED  ]` + имя теста. Не объявляй «done» без зелёного прогона.
