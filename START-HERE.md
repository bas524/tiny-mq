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
- **Спека 45 (priority ordering)** — ✅ закрыта (коммит `a26b5c5`): `PriorityQueueT` —
  10 бэндов + маска непустых бэндов + `LightweightSemaphore` вместо отдельной сигнальной
  очереди. Ревью (MiniMax-M3) и перф-гейт (deepseek-reasoner) approved; перф к master
  −2.2% / +2.5%. См. [docs/features/45-priority-ordering.md](docs/features/45-priority-ordering.md)
  и [docs/reviews/45-priority-ordering.perf.md](docs/reviews/45-priority-ordering.perf.md).
- **Следующий шаг — спека 13 (delivery delay):** min-heap по `deliveryTime`, таймер
  commit-time для транзакций. Источник: `docs/jms-spec/13-delivery-delay.md`; статус
  в UNIFIED-PLAN / CONTINUE-HERE.

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

**Боевой прогон сделан на спеке 45** (коммит `49e2484`). Headless-хоп
`zsh -ic "claude-<model> -p …"` работает. Прогон вскрыл семь дефектов роутера, все
починены: несуществующая обёртка Producer'а, вывод в `/dev/null` (из-за него любая
ошибка запуска выглядела как «ничего не произошло»), отсутствие флагов прав у
headless-агента, инлайн-промпт с ломающимися кавычками, `outdir` мимо каталога пакета,
**отсутствие Specialist-гейта в маршруте** (`approved` вёл сразу в DocWriter) и
непроброс `sdd_ref`. Маршрутизация теперь по паре `stage:status`.

Чего роутер не ловит: стадия может оборваться посреди работы (на спеке 45 —
`API Error: Response stalled mid-stream`), оставив правки в дереве и не записав
handoff-пакет. Цепочка при этом тихо встаёт, потому что ждёт файла, которого не будет.
Признак — свежие изменения в `git status` без нового `*.json` в `handoffs/<spec>/`.

## Как закрыть спеку (по harness)

Порядок, отработанный на спеке 45. Каждая стадия — **отдельный процесс** на своей модели,
обмен через файлы в `handoffs/<spec>/` (каталог в `.gitignore`, эфемерный):

```
zsh -ic 'claude-<model> --permission-mode acceptEdits \
  --allowedTools "Bash,Read,Write,Edit,Grep,Glob" \
  -p "$(cat handoffs/<spec>/<stage>.prompt.md)"'
```

1. Прочитать `docs/jms-spec/NN-*.md` — раздел «Test plan» = критерии приёмки.
2. **Producer** (`claude-claude-sonnet-5`): реализация + тесты по Test plan.
3. **Reviewer** (`claude-minimax-m3`) — обязательно другая модель, чем у Producer.
4. **Perf-гейт** (`claude-deepseek-reasoner`), если тронут горячий путь.
5. **Doc-writer** (`claude-glm-5-2`) → `docs/features/NN-*.md`.
6. Рубеж человека: коммит + `milestone-status`.

Уроки спеки 45, стоящие дороже всего:
- **Перф мерить только master-vs-ветка на release.** Сравнение двух бенчей внутри одной
  ветки стоимость фичи не измеряет — после изменения оба идут по новому коду. На этом
  Producer ошибся, гейт поймал.
- **Дизайн из спеки может не проходить перф-требование.** У 45 прямолинейная реализация
  стоила −13% на горячем пути; потребовалась смена примитива синхронизации.
- **Вердикты специалистов принимать, рекомендации — проверять.** Перф-гейт дал верные
  измерения и при этом небезопасную рекомендацию (fast-path, ломавший главный критерий
  приёмки спеки).
- Сводные поля JSON у агентов бывают устаревшими при верном разборе в `.md` — читать `.md`.

## Follow-up / долги (не блокеры)

- **Спека 26 (shared consumers) — обязательное условие, не пожелание.** Корректность
  `PriorityQueueT` (спека 45) доказана через инвариант ADR-0005 «на одной очереди ровно
  один consumer»: именно на нём стоит отсутствие живой блокировки при возврате жетона
  семафора. Спека 26 сажает нескольких консьюмеров на одну подписку и обязана повторить
  разбор — `docs/reviews/45-priority-ordering.review.md` §R2.
- Спека 45: **F3** — durable-реплей приоритета без отдельного теста (покрыт транзитивно
  общим кодом извлечения приоритета).
- **Изоляция тестов по стораджу (F6/F7).** `ClientAckTest.testMixedPersistenceOrdering` и
  `ExpirationTest.testExpiredPersistentMessageDroppedOnRecv` падают на повторном прогоне
  без `rm -rf tiny-mq/` — оставляют записи в сторадже и спотыкаются о них при реплее.
  Воспроизведено на master. Каждый агент в цепочке спотыкался об это заново.
- `benchmarks/baseline.md` снят на нагруженной машине (L.A. 3–55) — перепроверить на
  спокойной, прежде чем считать эталоном проекта.
- Спека 44: **n1** — `Tom::dataPrefix` глотает ошибку чтения без `clear()` → свип по тому
  может тихо и навсегда встать; **m5** — гард `0x02` в свипе можно ужесточить; **n3** —
  формулировку спеки 44 уточнить (реклейм стал eventually-consistent). Детали — в review-файле.
- `main.cpp:207` — SIGSEGV при `argc==1` (см. выше).
- `CLAUDE.md`/`tasks/CONTINUE-HERE.md` местами описывают старый `ninja`-путь и неверно
  утверждают, что `--gtest_filter` не поддерживается (поддерживается). Кандидат на gardener.
