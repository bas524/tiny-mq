# START HERE — tiny-mq

Точка входа для новой сессии Claude Code без предыдущего контекста. Проект и
архитектура — в [CLAUDE.md](CLAUDE.md); план фич — в [tasks/UNIFIED-PLAN.md](tasks/UNIFIED-PLAN.md).

## Актуальные команды (CMake-пресеты — verified)

```
cmake --preset user-debug                       # конфигурирование (project-local vcpkg)
cmake --build --preset debug --parallel         # сборка (-Werror)
./cmake-build-debug/tiny_mq --gtest_filter='ExpirationTest.*'   # прогон набора тестов

cmake --preset user-release && cmake --build --preset release --parallel
./cmake-build-releasewithdebuginfo/tiny_mq --gbench --benchmark_min_time=0.5s   # бенчи
```

⚠️ `./cmake-build-debug/tiny_mq` **без аргументов падает в SIGSEGV** (`main.cpp:207`
разыменовывает `argv[1]` при `argc==1`) — гоняй тесты только с `--gtest_filter=`/`--gbench`.
Преэкзистентный баг, к фичам отношения не имеет. `CLAUDE.md` в разделе Build ещё описывает
старый `ninja`-путь — верь командам отсюда.

## Где мы сейчас

- **M0** закрыт. **M1 (семантика доставки)** в работе.
- **Спека 44 (expiration sweep)** — ✅ закрыта (recv-drop + фоновый sweeper), прошла
  кросс-модельное ревью. См. [docs/reviews/44-message-expiration-sweep.review.md](docs/reviews/44-message-expiration-sweep.review.md).
- **Следующий шаг — спека 45 (priority ordering):** 10 бэндов приоритета, polling 9→0,
  бенчмаркать uniform-кейс. Источник: `docs/jms-spec/45-*.md`; статус в UNIFIED-PLAN / CONTINUE-HERE.

## AEF-harness (`.claude/`)

Проект ведётся по Agentic Engineering Framework: роли-агенты + скиллы + событийная цепочка.

**Агенты** (`.claude/agents/`, каждый со своей моделью через ai-proxy):

| Агент | Роль | Модель (`claude-<fn>`) |
|---|---|---|
| jms-orchestrator | Orchestrator | claude-claude-opus-4-8 |
| jms-producer | Producer | claude-claude-sonnet-4-6 |
| jms-reviewer | Reviewer (кросс-модель, S12) | **claude-minimax-m3** |
| perf-specialist / security-specialist | Specialist | claude-deepseek-reasoner |
| conformance-specialist | Specialist | claude-glm-5-2 |
| platform-agent | Platform | claude-qwen3-coder-plus |
| doc-writer | Knowledge (docs фич, S5/6) | claude-glm-5-2 |
| knowledge-gardener | Knowledge | claude-claude-opus-4-8 |

**Скиллы** (`.claude/skills/`): `jms-spec-implement`, `cpp-verify`, `perf-check`,
`cross-model-review`, `security-review`, `doc-write`, `adr-write`, `milestone-status`.

**Протокол:** Producer → Reviewer (на другой модели!) → Specialist gate → Orchestrator;
после `approved` — Doc-writer (`docs/features/<NN>-*.md`) → рубеж человека (milestone + commit).
Handoff-контракт и разрешение конфликтов — `.claude/chain/HANDOFF.md` (default-deny, N=2 → человек).
**Автономия:** R2 по умолчанию; R1 (подтверждение) на `git` / `CMakeLists` / vcpkg;
блокирующий gate — только на необратимом рубеже (коммит в master).

## Событийная цепочка (`.claude/chain/`)

Каждая стадия пишет `handoffs/<spec>/<stage>.json` → хук `Stop`/`SubagentStop`
(`.claude/settings.json`) запускает `route.sh`, который по `status` дёргает следующего
агента на нужной модели. **По умолчанию выключено** (`CHAIN_EXEC=0`). Включить —
`CHAIN_EXEC: "1"` в `.claude/settings.json`. Контракт и предохранители — `.claude/chain/HANDOFF.md`.

Первый боевой прогон цепочки ещё не делался: логика роутера проверена в dry-run, но
реальный headless-хоп (`zsh -ic "claude-minimax-m3 -p …"`) не тестировался — если не
поднимется, скорее всего hook-shell не подхватывает `~/.zshrc`; поправить `zsh -ic` на
явный путь к rc.

## Как закрыть спеку 45 (по harness)

1. Прочитать `docs/jms-spec/45-*.md` — раздел «Test plan» = критерии приёмки.
2. `jms-spec-implement` (Producer, sonnet): 10 бэндов приоритета, polling 9→0; тесты по Test plan.
3. `cpp-verify` (сборка+тесты, `-Werror`) + `perf-check` (горячий путь routing/delivery — **бенчмаркать uniform-кейс**, см. UNIFIED-PLAN M1).
4. `cross-model-review` на **claude-minimax-m3** (отдельная сессия или включённая цепочка).
5. На `approved` — `milestone-status`: UNIFIED-PLAN 45 → ✅ done, CONTINUE-HERE → следующая спека (13 delivery delay).

## Follow-up / долги (не блокеры)

- Спека 44: **n1** — `Tom::dataPrefix` глотает ошибку чтения без `clear()` → свип по тому
  может тихо и навсегда встать; **m5** — гард `0x02` в свипе можно ужесточить; **n3** —
  формулировку спеки 44 уточнить (реклейм стал eventually-consistent). Детали — в review-файле.
- `main.cpp:207` — SIGSEGV при `argc==1` (см. выше).
- `CLAUDE.md`/`tasks/CONTINUE-HERE.md` местами описывают старый `ninja`-путь и неверно
  утверждают, что `--gtest_filter` не поддерживается (поддерживается). Кандидат на gardener.
