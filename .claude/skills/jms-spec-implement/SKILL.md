---
name: jms-spec-implement
description: Spec-first реализация одной JMS-спеки в tiny-mq. Используй, когда нужно реализовать фичу по docs/jms-spec/NN.md — превращает «Test plan» спеки в исполняемые GTest-критерии, реализует минимально достаточный код, прогоняет cpp-verify и perf-check, собирает handoff-пакет для ревью. Триггеры: «реализуй спеку NN», «закрой спеку», «сделай фичу M1/M2…».
---

# jms-spec-implement

Producer-воркфлоу tiny-mq (AEF Standard 2 SDD + Standard 3 исполняемые критерии). Спека — контракт: делаем ровно то, что в ней зафиксировано.

## Шаги

1. **Прочитать спеку.** Открой `docs/jms-spec/<NN>*.md`. Разбери разделы: `Semantics`, `Persistence / wire implications`, `Dependencies`, `Open questions` и — главное — **`Test plan`**. Проверь, что зависимости из `Dependencies` уже закрыты в `tasks/UNIFIED-PLAN.md`; если нет — останови и сообщи оркестратору.

2. **Критерии приёмки → тесты.** Каждый пункт `Test plan` = отдельный GTest-кейс в `tests/…Test.cpp` с именем как в спеке (напр. `ExpirationTest`). Это полный и единственный скоуп «сделано».

3. **Унаследованные ограничения (brownfield).** Явно перечисли, что наследуется без изменений: объектная модель (`Exchange/Session/Destination/Consumer/Producer`), `ConcurrentLinearStorage` и формат хранения `0x02`, threading-модель (ADR-0005), durable-ключ `(clientID, name)`, режимы ack. Отклонение от них — только через ADR (скилл `adr-write`), не тихим выбором.

4. **Реализация.** Минимально достаточный код под критерии. Соблюдай инварианты:
   - обе durable-ветки при правке routing/persistence: `Destination::save` **и** `Destination::deliverCommitted`;
   - `MessageProperty` и `PocoAnyVisitor.h` — вместе;
   - горячий путь → готовься к `perf-check`, при отсутствии бенча добавь его в `tests/BenchmarkTest.cpp`.

5. **Verify.** Прогони скилл **`cpp-verify`** (сборка + все тесты + `-Werror` чисто). Затем, если тронут горячий путь, — **`perf-check`**.

6. **Handoff-пакет** (AEF §5.2) для `jms-reviewer`:
   ```
   artifact:   <дифф + новые тесты>
   evidence:   <вывод tiny_mq GTest; вывод --gbench, если применимо>
   status:     produced
   provenance: model=<producer-model> role=Producer autonomy=R2
   sdd_ref:    docs/jms-spec/<NN>.md
   ```

## Не делать
- Не расширять поведение за пределы спеки (изменение требований → новая дельта/ADR).
- Не коммитить (это R1 — рубеж человека/оркестратора).
- Не подавлять предупреждения компилятора — чинить.
