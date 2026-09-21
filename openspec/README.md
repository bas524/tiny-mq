# openspec/ — текущая истина о поведении tiny-mq

Класс документа: **K2 — Engineering** (Standard 6).

Гибрид AEF × [OpenSpec](https://github.com/Fission-AI/OpenSpec) (решение Owner, 2026-09-21):

| Артефакт | Где | Роль |
|---|---|---|
| **SDD** — постановка задачи, 10 полей AEF Standard 2 | `docs/jms-spec/NN-*.md` | *предложение* (change-proposal); проходит критика, Producer, Reviewer, гейты |
| **Дельта** — `## ADDED/MODIFIED/REMOVED Requirements` | `openspec/changes/<NN-slug>/specs/<capability>/spec.md` | пишет **doc-writer** после approved-ревью и перф-гейта: описывает *принятую реализацию* |
| **Capability-спека** — `## Purpose` + `## Requirements` | `openspec/specs/<capability>/spec.md` | **единственное место «что система делает сейчас»**; собирается `archive` из дельт |
| Архив change'а | `openspec/changes/archive/<YYYY-MM-DD>-<NN-slug>/` | история; в рабочий контекст агента не индексируется (K4) |

Что **не** переезжает в OpenSpec: `Entity inventory`, `Stop-conditions`, `Owner`, `Autonomy Level`,
`Rollback`, evidence-пакеты и гейты цепочки — это надстройка AEF поверх формата, OpenSpec их не знает.

## Формат (совместим с CLI OpenSpec 1.13)

```
### Requirement: <имя>                       ← устойчивый ключ для MODIFIED/REMOVED
The broker SHALL …                           ← SHALL/MUST обязателен
#### Scenario: <имя>
- **WHEN** …
- **THEN** …
- Test: `DlqTest.LandsInDlqAfterMaxRedeliveries`   ← имя GTest — исполняемый критерий (Standard 3)
```

Каждое требование — ≥1 сценарий с телом. Строка `Test:` — наше расширение: OpenSpec её не
проверяет, наш `spec-critic`/ревьюер — проверяют (сценарий без теста = ручная валидация,
Standard 3 требует её явной пометки).

## Инструмент

Официальный CLI требует npm, который прокси не пропускает. `.claude/chain/openspec.py`
реализует нужное подмножество по правилам их парсера (`validate`, `archive`); раскладка
остаётся читаемой настоящим `openspec`, когда его удастся поставить.

```
python3 .claude/chain/openspec.py validate            # все capability-спеки
python3 .claude/chain/openspec.py validate 24-redelivery-dlq   # + дельты change'а
python3 .claude/chain/openspec.py archive  24-redelivery-dlq   # merge + перенос в archive/ (рубеж человека)
```

## Capabilities (по объектной модели, не по фичам)

`session` · `consumer` · `producer` · `destination` · `message` · `storage` ·
`durable-subscriptions` · `transactions` · `message-redelivery` · `selector`.
Новую capability заводит doc-writer первой дельтой; `Purpose` дописывает садовник при archive.

## Бэкфилл

Закрытые до гибрида спеки (M0, 12, 13, 23, 44, 45) в `openspec/specs` ещё не описаны —
задача садовника: по `docs/features/`, `docs/reviews/` и коду, по одной capability за проход.
