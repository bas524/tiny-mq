# Бэкфилл `openspec/specs` и поле `Owner` в спеках (HR-02)

> **Это maintenance-задача, а не SDD.** См. `tasks/_maintenance-template.md`.

- **Класс:** maintenance (knowledge / test-infra)
- **Статус:** open
- **Обратимость:** R2
- **Заведено:** 2026-09-21, гибрид AEF × OpenSpec
- **Исполнитель:** `knowledge-gardener`

## Симптом

1. `openspec/specs/` содержит только вклад спеки 24 (`message-redelivery` — 10 требований,
   `connection` — 1). Поведение закрытых спек M0 (01, 03, 10, 11, 14, 22, 33) и M1
   (12, 13, 23, 44, 45) в capability-спеках не описано; их SDD застыли как proposal
   (спека 45 после закрытия говорит «`QueueT` — strict FIFO, no priority awareness»).
   `docs/features/45-priority-ordering.md` — единственный doc старого формата.
   `Purpose` у `connection` явно помечен как частичный.
2. `Owner` (Standard 2, введён сверкой с AEF 2026-09-19) отсутствует в **31 из 32** спек
   `docs/jms-spec/`; роутер на `paused` по такой спеке печатает «Owner не заполнен —
   спека невалидна».

## Причина

Гибрид введён 2026-09-21; всё, что закрыто раньше, через doc-writer не проходило.
`Owner` добавлен в шаблон после написания всех спек; заполнять массово решено не было —
**при касании** (решение в START-HERE). Но спеки, которые никто не коснётся до M4,
остаются невалидными бессрочно.

## Критерии «сделано»

- Для каждой закрытой спеки — change `openspec/changes/<NN-slug>/` с дельтой по
  `docs/features/`, `docs/reviews/` и коду (`Test:` — реальные имена GTest-кейсов),
  `openspec.py validate` — 0 ошибок, `archive` выполнен; capability-спеки: `session`,
  `consumer`, `producer`, `destination`, `message`, `storage`, `durable-subscriptions`,
  `transactions`, `message-redelivery` (дописать recover из 23), `selector`.
- `Purpose` у каждой capability — содержательный (не «TBD»).
- `docs/features/45-priority-ordering.md` переведён в дельту и удалён (или помечен K4).
- `Owner` заполнен во всех спеках с непустым `Test plan`; для не-спек (42, 50, 61) —
  либо переоформление, либо явная пометка «не SDD».
- Сверка `Entity inventory` ↔ код для закрытых спек по Standard 8 выполнена, дрейф-флаги
  заведены (ожидается ≥ 1: у спек до 24 реестра не было — его надо восстановить из кода).

## Границы (scope-lock)

`openspec/**`, `docs/jms-spec/*.md` (только секция `Owner` и `Entity inventory`),
`docs/features/`, `START-HERE.md` (Follow-up). Код не трогать.

## Evidence

`handoffs/<NN>/logs/openspec-validate.log` на каждый change; вывод `openspec.py validate`
после последнего archive; `git log` по `docs/jms-spec/` с добавлением `Owner`.
