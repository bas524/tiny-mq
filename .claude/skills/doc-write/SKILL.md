---
name: doc-write
description: Написать OpenSpec-дельту принятой реализации спеки tiny-mq (гибрид AEF × OpenSpec). Используй после approved-ревью и перф-гейта, до milestone-status. Источник истины — docs/jms-spec/NN, дифф и тесты; вывод — openspec/changes/NN-slug/ (proposal, design, specs/<capability>/spec.md). Триггеры: «задокументируй фичу», «опиши функциональность спеки NN», стадия doc-writer в цепочке.
---

# doc-write

Контур Knowledge (Standard 6 «Documentation Levels» + Standard 7 «Documentation Quality»).
Описывает **что делает фича и как ей пользоваться** — по факту принятой реализации.

## Предусловие
Реализация закрыта Reviewer'ом (`status: approved`) и `cpp-verify` зелёный. Документируем
принятое поведение, а не замысел.

## Проверка на дубликат — до записи (Standard 16 п. 9)

Прежде чем создавать файл, поищи по `docs/` существующий док про эту же фичу:

```
REUSE (уже описано → сошлись) > EXTEND (есть близкое → расширь) > JUSTIFY (новое → обоснуй) > ESCALATE (сомнение → человек)
```

Типичный случай для tiny-mq: фича размазана по нескольким спекам (13 → 24 переиспользует
scheduled delivery; 23 → 24 переиспользует `deliveryCount`). Тогда правильный ход —
`EXTEND` существующего дока со ссылкой, а не второй файл с пересечением на 70%.

## Шаги
1. Прочитай `docs/jms-spec/<NN>.md` (`Semantics`, `Test plan`, `Open questions`, `Dependencies`)
   и дифф реализации из handoff-пакета Producer. Сверь публичный API по коду (сигнатуры реальны).
2. Создай change `openspec/changes/<NN>-<slug>/` (формат и правила — `openspec/README.md`,
   роль — `.claude/agents/doc-writer.md`): `.openspec.yaml`, `proposal.md` (зачем, ссылки на
   SDD/ревью), `design.md` (принятая реализация: API, дефолты, чего НЕ делает),
   `specs/<capability>/spec.md` — дельта `## ADDED/MODIFIED/REMOVED Requirements`:
   `Semantics` N → `### Requirement:` с `SHALL`; `Test plan` T → `#### Scenario:` WHEN/THEN +
   `- Test: \`<Suite>.<Case>\``. Если требование уже есть в `openspec/specs/<capability>/spec.md` —
   MODIFIED с точным именем.
   Прогони `python3 .claude/chain/openspec.py validate <NN>-<slug>` → 0 ошибок, лог в
   `handoffs/<spec>/logs/openspec-validate.log`.
3. Никакого «впрок»: документируй только то, что есть в спеке и коде (Закон 1 — точность важнее полноты).
4. Стиль — как в существующих доках репо; кратко и проверяемо.

## Выход
Handoff-пакет `handoffs/<spec>/docwriter.json` со `status: documented`, ядром
`spec · artifact · evidence · provenance` (`artifact` = каталог change'а, `evidence` = лог
validate, `target_files` = файлы change'а). После этого цепочка останавливается на рубеже
человека: `python3 .claude/chain/openspec.py archive <NN>-<slug>` (merge в
`openspec/specs/`) + `milestone-status` + коммит.

## Границы (что это НЕ)
- Не ADR (архитектурное «почему» → скилл `adr-write`).
- Не сопровождение базы знаний/статуса (→ `knowledge-gardener` / `milestone-status`).
- Не тесты (→ `jms-spec-implement`).
