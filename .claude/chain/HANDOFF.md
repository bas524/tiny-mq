# Event-chain handoff contract (AEF §5.2)

Минимальная событийная цепочка агентов tiny-mq. Каждая стадия по завершении пишет
**машиночитаемый handoff-пакет** — это и есть «готовность входного артефакта» для
следующей стадии. Хук (`Stop` / `SubagentStop`) запускает роутер
`.claude/chain/route.sh`, который по `status` дёргает следующего агента.

## Куда писать

```
handoffs/<spec>/<stage>.json     напр. handoffs/44/producer.json
```

`<stage>` ∈ `producer` · `reviewer` · `perf` · `conformance` · `security` · `doc-writer` · `orchestrator`.

## Формат пакета (ядро обязательно — §5.2)

```json
{
  "spec": "44",
  "sdd_ref": "docs/jms-spec/44-message-expiration-sweep.md",
  "stage": "producer",
  "status": "produced",
  "iteration": 1,
  "artifact": "git diff --stat; Consumer.cpp, ConcurrentLinearStorage.{h,cpp}, ...",
  "evidence": "cpp-verify: 105/105, -Werror clean; perf-check: recv 149->142ns",
  "provenance": { "model": "claude-sonnet-4-6", "role": "Producer", "autonomy": "R2" }
}
```

Обязательное ядро: `spec · status · artifact · evidence · provenance`. Пакет без
ядра роутер считает **не готовым** и ничего не запускает (валидация формата границы,
Standard 4).

## Значения `status` → маршрут (роутер, §5.1/§5.3)

| status | что делает роутер |
|---|---|
| `produced` | запускает **Reviewer** (`MiniMax-M3`) по скиллу `cross-model-review` |
| `rejected` | если `iteration < CHAIN_MAX_ITER` (=2) → возвращает **Producer** на правку (iteration+1); иначе **эскалация человеку** |
| `approved` | запускает **Doc-writer** (`glm-5.2`, скилл `doc-write`) — R2, пишет `docs/features/<spec>-*.md` |
| `documented` | **STOP**: рубеж человека/оркестратора (R1) — `milestone-status` + коммит. Не автозапускается (Standard 19) |
| `escalated` | **STOP**: human decision point (§5.3) |
| прочее | **default-deny**: STOP |

`iteration` — счётчик раундов Producer↔Reviewer; Reviewer копирует его из входного
пакета, Producer инкрементирует при правке.

## Как запускается цепочка

1. Оркестратор/человек один раз запускает Producer на спеке → тот пишет `producer.json`.
2. Хук ловит завершение → `route.sh` → Reviewer (MiniMax-M3) → пишет `reviewer.json`.
3. `approved` → Doc-writer (glm-5.2) пишет `docs/features/<spec>-*.md` → `documented` → STOP на рубеже R1 (человек закрывает спеку); `rejected` → назад к Producer (≤2 раунда).

**Диспатч по умолчанию — dry-run** (роутер печатает команду). Включить реальный запуск:
`CHAIN_EXEC=1` (в `.claude/settings.json` → `env`). Предохранители: маркеры `*.routed`
(не диспатчить повторно), лимит раундов `CHAIN_MAX_ITER`, глобальный потолок
`CHAIN_MAX_DISPATCH`, и **STOP на approved/escalated** (необратимое — только через человека).
