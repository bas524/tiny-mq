# Задачи harness (AEF-цепочка, `.claude/`)

Заведены 2026-09-21 по итогам первой полной цепочки на обновлённом фреймворке
(спека 24): критик ×3 → решение Owner → Producer (paused, обрыв, 429) → ревью → перф →
doc-writer (первая OpenSpec-дельта). Шесть дефектов `route.sh` починены по ходу
(см. START-HERE → «Уроки спеки 24»); здесь — то, что осталось открытым.

| Задача | Статус | Суть |
|---|---|---|
| [HR-01](01-route-sh-robustness.md) | ⬜ open | `dispatch` не видит инфра-сбоев (429/400/ENOTFOUND), `NOTREADY` не маркируется, правило «foreground only» не во всех промптах |
| [HR-02](02-openspec-backfill-and-owner.md) | ⬜ open | Бэкфилл `openspec/specs` по закрытым спекам; `Owner` в 31 спеке |
