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

**Агенты** (`.claude/agents/`). Модель фиксируется в двух местах, и они означают разное:
frontmatter `model:` — для вызова субагентом (только алиасы `opus`/`sonnet`), обёртка
`claude-<fn>` — для процессного запуска цепочки. Единственный источник истины по привязке —
**`.claude/chain/CALIBRATION.md`**; таблица ниже — краткая выжимка.

| Агент | Роль | Субагент | Процесс (`claude-<fn>`) |
|---|---|---|---|
| jms-orchestrator | Orchestrator | opus | — |
| **spec-critic** | **Критик намерения (V IV §5.3)** | **opus** | **claude-glm-5-2** |
| jms-producer | Producer | sonnet | claude-claude-sonnet-5 |
| jms-reviewer | Reviewer (кросс-модель, S13) | opus | **claude-minimax-m3** |
| perf-specialist / security-specialist | Specialist | opus | claude-deepseek-reasoner |
| conformance-specialist | Specialist | opus | claude-glm-5-2 |
| platform-agent | Platform | sonnet | — (R1) |
| doc-writer | Knowledge (docs фич, S6/7) | sonnet | claude-glm-5-2 |
| knowledge-gardener | Knowledge | sonnet | — (рубеж человека) |

**Скиллы** (`.claude/skills/`): `spec-critique`, `jms-spec-implement`, `cpp-verify`,
`perf-check`, `cross-model-review`, `security-review`, `doc-write`, `adr-write`,
`milestone-status`.

**Протокол:** Критик намерения → Producer → Reviewer (на другой модели **и с чистым
контекстом**) → Specialist gate → Orchestrator; после `approved` — Doc-writer
(`docs/features/<NN>-*.md`) → рубеж человека (milestone + commit).
Handoff-контракт и разрешение конфликтов — `.claude/chain/HANDOFF.md`
(default-deny, **N=5** → человек; пороги — `CALIBRATION.md`).

**Три правила, на которых стоит цепочка** (AEF Std 5/15/18, добавлены после сверки с
фреймворком от 2026-08-11; номера — по изданию от 2026-09-19, см. ниже):

1. `evidence` — **массив путей к логам прогонов**, не проза. Отчёт исполнителя о своей
   проверке — заявление, а не свидетельство; роутер проверяет существование файлов.
2. Проверяющий **прогоняет проверки заново** и не получает на вход разбор производителя
   (две оси независимости: модель + контекст).
3. `target_files` — scope-lock: правка вне объявленного набора **останавливает** цепочку.
   Не теоретическое: `.cache/` и `cmake-build-asan/` (2613 файлов) уже уезжали в коммит.

Плюс `status: paused` — остановка по `Stop-conditions` спеки. Это **не ошибка**: работа
корректна и возобновляема, в отличие от `rejected` (есть дефект) и сбоя (нужен чек-пойнт).
Решение на `paused` — за **`Owner`** спеки (конкретный человек, поле Standard 2); роутер
рядом печатает **подсказку о консультации** — роль из замкнутой таблицы Том II §1.2
(локальное соответствие — `CALIBRATION.md`), событие `CONSULT` в `chain.log`.
**Автономия:** R2 по умолчанию; R1 (подтверждение) на `git` / `CMakeLists` / vcpkg;
блокирующий gate — только на необратимом рубеже (мерж в main).

**Сверка с AEF от 2026-09-19** (коммит `a21c2bf` фреймворка). Что изменилось в harness:
- **Стандарты перенумерованы сплошняком 1–23** — все ссылки в `.claude/`, шаблоне спеки,
  ADR обновлены. Соответствие старых → новых: 22→**5** (Closed-world), 23→**15**
  (Verification Integrity), 5–13 → +1, 14–21 → +2. `docs/reviews/*` не тронуты — это
  evidence-артефакты, зафиксированные во времени, там номера старого издания.
- **`Owner` в спеке** — конкретный человек в роли Engineer, не роль и не автор; без него
  спека невалидна. Добавлен в `_template.md`; **у 32 существующих спек поля нет** — заполнить
  при следующем касании каждой (кандидат на gardener).
- **Вопросы критика машиночитаемы:** `questions[]` с закрытым множеством `options` + `other`
  и `answer_goes_to`; ответ оркестратор вносит **в спеку**, не в чат. `clean` при непустых
  `questions` роутер понижает до `needs-work`.
- **Садовник сверяет `Entity inventory` с кодом** по коммитам мимо цепочки; расхождение —
  дрейф-флаг в Follow-up (порог `< 5`, CALIBRATION.md).
- Метка класса документа (Standard 6): спека — K1, `docs/features/` — K2, `docs/reviews/` — K4.
- `SEEN` в `chain.log` теперь несёт `model=` — атрибуция harness'ом (Standard 20), для
  критика это запись о разборе критериев по Standard 3.

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
   Спека обязана быть валидным SDD: 10 полей (включая `Stop-conditions` и `Owner`) +
   непустой машиночитаемый `Entity inventory`. Шаблон — `docs/jms-spec/_template.md`.
2. **Критик намерения** (`claude-glm-5-2`): «что понял / что НЕ понял» по тексту спеки,
   до кода. `questions[]` — закрытые варианты + `other`; вердикт не `clean` → оркестратор
   отвечает правкой секций `answer_goes_to` спеки, точечно; лимит раундов 3.
3. **Producer** (`claude-claude-sonnet-5`): реализация + тесты по Test plan.
   Объявляет `target_files`, пишет логи в `handoffs/<spec>/logs/`.
4. **Reviewer** (`claude-minimax-m3`) — другая модель **и чистый контекст**: прогоняет
   тесты сам, сверяет с логами Producer'а, делает closed-world drift audit.
5. **Perf-гейт** (`claude-deepseek-reasoner`), если тронут горячий путь.
6. **Doc-writer** (`claude-glm-5-2`) → `docs/features/NN-*.md`.
7. Рубеж человека: коммит + `milestone-status`.

Журнал роутера `handoffs/<spec>/chain.log` пишет harness — это *свидетельство*; `*.json`
пишет о себе агент — это *заявление*. При расхождении верить журналу (Std 20).

Уроки спеки 45, стоящие дороже всего:
- **Перф мерить только main-vs-ветка на release.** Сравнение двух бенчей внутри одной
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
- **Изоляция тестов по стораджу — ✅ [LP-04](tasks/linux-port/04-test-storage-isolation.md) закрыта.**
  Каждый набор пишет в `./tiny-mq-test-storage/<ИмяНабора>` и чистит за собой
  (`tests/TestHelper.h`). `--gtest_repeat=20` и `--gtest_shuffle` проходят **без** внешней
  чистки, то есть повторные прогоны снова годятся как инструмент поиска гонок.
  Прежняя формулировка долгов F6/F7 как «плавающих падений» была неверна: поведение
  было детерминированным.
- `benchmarks/baseline.md` снят на нагруженной машине (L.A. 3–55) — перепроверить на
  спокойной, прежде чем считать эталоном проекта.
- Спека 44: **n1** — `Tom::dataPrefix` глотает ошибку чтения без `clear()` → свип по тому
  может тихо и навсегда встать; **m5** — гард `0x02` в свипе можно ужесточить; **n3** —
  формулировку спеки 44 уточнить (реклейм стал eventually-consistent). Детали — в review-файле.
- `main.cpp:207` — SIGSEGV при `argc==1` (см. выше).
- `CLAUDE.md`/`tasks/CONTINUE-HERE.md` местами описывают старый `ninja`-путь и неверно
  утверждают, что `--gtest_filter` не поддерживается (поддерживается). Кандидат на gardener.
- **`Owner` отсутствует во всех 32 спеках** `docs/jms-spec/` (поле введено сверкой с AEF
  2026-09-19). Роутер на `paused` по такой спеке печатает «Owner не заполнен — спека
  невалидна». Заполнять при следующем касании спеки, начиная со спеки 13.
