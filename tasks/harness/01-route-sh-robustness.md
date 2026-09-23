# Робастность роутера цепочки (HR-01)

> **Это maintenance-задача, а не SDD.** См. `tasks/_maintenance-template.md`.

- **Класс:** maintenance (build-env / harness)
- **Статус:** open
- **Обратимость:** R2 (правки `.claude/chain/route.sh` и промптов; `settings.json` — R1)
- **Заведено:** 2026-09-21, спека 24

## Симптом

Три случая на спеке 24, все — «цепочка тихо встала», `handoffs/24/chain.log`:

1. Producer (`claude-claude-sonnet-5`) запустил сборку `run_in_background` и завершил
   `-p`-сессию с текстом «I'll pick this back up automatically once it finishes» —
   `EXIT=0`, `producer.json` нет, `build.log` пустой. Роутер ждёт файла, которого не будет.
2. `API Error: Request rejected (429) · limit on your personal key has been exceeded`,
   `EXIT=1` — то же: пакета нет, роутер молчит. Аналогично `400 requested model not found`
   (`deepseek-reasoner` исчез из каталога прокси) и `ENOTFOUND`.
3. Legacy-пакет `handoffs/lp-04/producer.json` без ядра: `NOTREADY … missing=artifact`,
   `exit 0` **без маркера** `.routed` → каждый запуск хука упирается в него и не доходит до
   свежих пакетов. Плюс: маркер `.routed` остаётся у старого имени при переименовании
   пакета (`producer.json → producer.paused-1.json`) и «съедает» новый.

Все три записаны в журнал руками оркестратора (`ABORTED`, `INFRA-FAIL`) — то есть
свидетельство писал не harness (нарушение духа Standard 20).

## Причина

`dispatch` в `route.sh` запускает `zsh -ic "claude-<model> … -p …" > log &` и **не
смотрит** ни на код возврата, ни на появление пакета; журнал получает только `DISPATCH`.
`stop`/`NOTREADY` не маркируют пакет. Правило «headless-стадия не имеет права уходить в
фон» добавлено только в промпт docwriter'а (`0487ca6`); в промптах producer/reviewer/perf
его нет — оно дописывалось руками в `handoffs/24/*.prompt.md`.

## Критерии «сделано»

- `dispatch` ждёт завершения процесса (или получает его через `wait` в фоне) и пишет в
  `chain.log` `EXIT stage=… rc=… pkg=present|absent`; при `rc≠0` или отсутствии пакета —
  `INFRA-FAIL` с последней строкой `stdout.log` (регэксп на `API Error|429|400|ENOTFOUND`).
- `NOTREADY` помечает пакет `.notready` и не блокирует сканирование; `.routed` переносится
  вместе с пакетом или ключуется по хэшу содержимого.
- Строка «ты headless-процесс: никаких фоновых команд; сессия завершена только когда
  записан пакет» — во всех пяти промптах `route.sh` (critic, producer ×2, reviewer, perf,
  docwriter), проверяется `grep -c` в тесте роутера.
- Dry-run тест роутера (как в сессии 2026-09-20: синтетические пакеты в scratchpad,
  `PATH` с пустым `git`) покрывает: `rc≠0`, отсутствие пакета, legacy-пакет без ядра.

## Что закрыто переносом harness (2026-09-23)

Синхронизация с эталонным набором закрыла три пункта из четырёх критериев:

- **`NOT-READY` помечает пакет `.notready`** и не блокирует сканирование свежих пакетов;
- **правило «никаких фоновых команд»** — во всех промптах диспатча, проверяется тестом
  (`grep` на строку правила в промпте ревьюера);
- **dry-run тест роутера** заведён: `.claude/chain/tests/route_dryrun.sh`, 25 кейсов, включая
  пакет без ядра, отсутствующий evidence, нарушение scope-lock и его слоение, default-deny.

**Осталось открытым — главное:** `dispatch` по-прежнему не смотрит на код возврата процесса и
не отличает отказ инфраструктуры (429, 400, обрыв) от молчания агента. События `INFRA-FAIL`
и `ABORTED` вписываются в журнал руками, а событие, которое harness мог наблюдать сам, но
записал человек, — дефект harness, а не свидетельство (Standard 20, `HANDOFF.md`).
Критерий остаётся прежним: `EXIT stage=… rc=… pkg=present|absent` в журнале и `INFRA-FAIL`
с последней строкой `stdout.log` при `rc≠0` или отсутствии пакета.

Маркер `.routed` при переименовании пакета (`producer.json` → `producer.paused-1.json`)
тоже не перенесён — ключевать по содержимому.

## Границы (scope-lock)

`.claude/chain/route.sh`, `.claude/chain/HANDOFF.md` (описание новых событий журнала),
`START-HERE.md` (раздел «Событийная цепочка»). Агентов и скиллы не трогать.

## Evidence

`handoffs/24/chain.log` (строки `ABORTED`, `INFRA-FAIL`, `CEILING`, `SCOPEVIOLATION`
от 2026-09-20/21), `handoffs/24/producer.aborted-1.stdout.log`,
`handoffs/24/producer.429-1.stdout.log`, `handoffs/24/perf.400-1.stdout.log`.
