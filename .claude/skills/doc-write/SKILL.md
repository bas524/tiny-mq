---
name: doc-write
description: Написать/обновить документацию функциональности tiny-mq по закрытой спеке. Используй после approved-ревью, до milestone-status. Источник истины — docs/jms-spec/NN и её «Test plan»; вывод — docs/features/NN-*.md. Триггеры: «задокументируй фичу», «опиши функциональность спеки NN», стадия doc-writer в цепочке.
---

# doc-write

Контур Knowledge (Standard 5 «Documentation Levels» + Standard 6 «Documentation Quality»).
Описывает **что делает фича и как ей пользоваться** — по факту принятой реализации.

## Предусловие
Реализация закрыта Reviewer'ом (`status: approved`) и `cpp-verify` зелёный. Документируем
принятое поведение, а не замысел.

## Шаги
1. Прочитай `docs/jms-spec/<NN>.md` (`Semantics`, `Test plan`, `Open questions`, `Dependencies`)
   и дифф реализации из handoff-пакета Producer. Сверь публичный API по коду (сигнатуры реальны).
2. Напиши/обнови `docs/features/<NN>-<slug>.md` по разделам:
   **Что делает · Семантика (вкл. чего НЕ делает) · Как пользоваться (пример на публичном API) ·
   Ограничения/конфигурация · Проверяемость (ссылка на спеку + `Test plan` + имена тестов)**.
3. Никакого «впрок»: документируй только то, что есть в спеке и коде (Закон 1 — точность важнее полноты).
4. Стиль — как в существующих доках репо; кратко и проверяемо.

## Выход
Handoff-пакет `handoffs/<spec>/docwriter.json` со `status: documented`, ядром
`spec · artifact · evidence · provenance` (`artifact` = путь к доку). После этого цепочка
останавливается на рубеже человека: `milestone-status` (спека → done) + коммит.

## Границы (что это НЕ)
- Не ADR (архитектурное «почему» → скилл `adr-write`).
- Не сопровождение базы знаний/статуса (→ `knowledge-gardener` / `milestone-status`).
- Не тесты (→ `jms-spec-implement`).
