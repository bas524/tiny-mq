---
name: doc-write
description: Написать дельту принятой реализации SDD tiny-mq в спецификацию-факт (гибрид AEF и OpenSpec). Используй после одобренного ревью и шлюзов, до `milestone-status`. Источник истины — SDD, дифф и тесты, вывод — openspec/changes/<NN>-<slug>/. Триггеры: «задокументируй доработку», «опиши поведение SDD NN», стадия `doc-writer` в цепочке.
---

# doc-write

На partial эта стадия не запускается. После полного допуска укажи в `.openspec.yaml`
`task_ref: <путь исходной постановки>`. Сохраняй рядом с ID полный Источник с редакцией и фрагментом,
сценарии и условия измерений. Подробности — openspec/README.md.

Стадия знания (Стандарты 6 и 7). Описывает, **что система делает сейчас**, по факту
принятой реализации, в спецификации-факте (Том II, §2.2).

## Предусловие

Реализация принята (`approved` от агента-рецензента и всех шлюзов), `verify` зелёный.

## Проверка на дубликат — до записи (Стандарт 16, п. 9)

`REUSE > EXTEND > JUSTIFY > ESCALATE`: требование уже есть в `openspec/specs/<capability>/spec.md`?
→ `MODIFIED` с точным именем, а не второе требование с пересечением.

## Шаги

1. Прочитай `docs/jms-spec/<NN>-*.md` (`Acceptance Criteria`, `Test plan`, `Open questions`, `Dependencies`)
   и дифф реализации. Сверь публичный API по коду (сигнатуры реальны).
2. Создай изменение `openspec/changes/<NN>-<slug>/` (формат — `openspec/README.md`):
   `.openspec.yaml`, `proposal.md`, `design.md`, `specs/<capability>/spec.md` — дельта
   `## ADDED/MODIFIED/REMOVED Requirements`: требование → `### Requirement:` с `SHALL` и строкой
   `ID:` из SDD (при MODIFIED — прежний идентификатор);
   сценарий → `#### Scenario:` с WHEN и THEN и строкой `- Test: <Suite.Case>`.
   Прогони `python3 .claude/chain/openspec.py validate <NN>-<slug>` → 0 ошибок, лог в
   `handoffs/current/<spec>/logs/openspec-validate.log`.
3. Никакого «впрок»: только то, что есть в SDD и коде (Закон 1). Если SDD и код разошлись, опиши
   фактическое поведение и отметь расхождение.
4. Стиль — кратко и проверяемо.
5. Продуктовая дельта (Том IV, §10.6). Изменилась функция продукта или появилось новое понятие доменной модели —
   подготовь `handoffs/current/<spec>/product-delta.md` по `templates/product-delta.md`, с ID функций и понятий,
   ссылкой на SDD и целевым набором для задачи знания. В `product-kb` не пиши: это отдельная задача знания.

## Выход

`handoffs/current/<spec>/docwriter.json` со `status: documented`, ядром (`artifact` — каталог
изменения, `evidence` — лог `validate`, `target_files` — файлы изменения, `product_delta` — путь или `null`).
Дальше рубеж человека: `openspec.py archive <NN>-<slug>`, `milestone-status` и коммит. Если есть продуктовая
дельта, человек вместе с MR компонента заводит задачу знания (`templates/knowledge-task.md`).

## Границы (что это НЕ)

- Не ADR (→ `adr-write`).
- Не сопровождение базы знаний (→ `knowledge-gardener`, `milestone-status`).
- Не запись в знание продукта (→ задача знания, `.claude/chain/kb-task.py`).
- Не тесты (→ `implement`).
