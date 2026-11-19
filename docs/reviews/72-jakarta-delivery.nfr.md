# Производительность SDD 72 — вердикт НФТ-шлюза (итерация 4, Intel Xeon 6975P-C, Clang 18 / LLD 18)

REQ-72-9 сравнивает RelWithDebInfo сборки `main` (база `230eefe8`) и merge-кандидата PR №12
на одном GitHub раннере. Порог — рост медианы CPU time/op не более 5% на каждом согласованном
горячем пути при CPU CV ≤ 5%. Серия с CV выше порога повторяется один раз; сохраняющаяся
нестабильность даёт отрицательный результат. Persistent-бенчмарки учитывают CPU процесса целиком
(`MeasureProcessCPUTime`), а обе сборки и их worker threads наследуют одну маску CPU до запуска
subprocess.
[SDD](../jms-spec/72-jakarta-delivery.md#requirement-бюджет-горячего-пути-доставки) ·
[Исторический workflow](https://github.com/bas524/tiny-mq/blob/2b3c53623734947c47b43c311465b841439f80fb/.github/workflows/performance.yml) · [Runner](../../tests/nfr72.py)

Итерация 4 (machine iteration 4) — шлюз после Reviewer i4, SDD v0.5. Запуск `44475262358`,
попытка 1, head PR `2b3c53623734947c47b43c311465b841439f80fb`, base `main`
`230eefe8321f5d9a956a9fb89b09af1dab84cbad`, synthetic merge
`35fac1e4131169f0c166f8dc6b60e43b6531420d`.

## Проверка входов

Артефакт `broker-performance` проверен: SHA-256
`d2ffbba3ee8c52f36b266aae864f2f9de5c64adfdc9be872e0196d5d08e02425`
совпадает со значением, записанным в `github-perf-44475262358-attempt1.log`.
`environment.txt`: `Baseline 230eefe8321f5d9a956a9fb89b09af1dab84cbad`,
`Candidate 35fac1e4131169f0c166f8dc6b60e43b6531420d`, `CC=clang-18` / `CXX=clang++-18`
(Ubuntu Clang 18.1.3), `LDFLAGS=-fuse-ld=lld-18` (Ubuntu LLD 18.1.3),
`Intel(R) Xeon(R) 6975P-C`, 4 CPU (0–3), 2 threads/core, runner `ubuntu24`.
`summary.log` содержит `CPU affinity: inherited=[0, 1, 2, 3] effective=[0, 1]` — одна строка
маски до начала ABBA; маска наследуется обеими сборками и их worker threads.

## Собственный пересчёт попытки 1

Девять повторов на вариант, чередование ABBA: база 1/4 (4+5), кандидат 2/3 (4+5).
Пересчёт из сырых JSON прямым чтением через `zipfile.ZipFile.read` (без распаковки).
Batch-пути нормированы на размер батча в нс/доставку; медианы — CPU time/op,
CV — выборочное стандартное отклонение. Число итерационных строк проверено
(4/4/5/5, 9 на вариант), имя регистрации одинаково в четырёх файлах каждой серии.
Повторов (`-repeat`) в этой попытке нет: ни один путь не превысил CV 5%.

| Путь | CPU было → стало, нс/доставку | Δ CPU | CV база / кандидат | Исход |
|---|---:|---:|---:|---|
| AutoAck_NonPersistent_RoundTrip | 106,6 → 113,6 | **+6,59%** | 0,57% / 1,73% | **регрессия > 5%** |
| Topic_AutoAck_NonPersistent_RoundTrip | 117,2 → 118,0 | +0,60% | 2,66% / 0,41% | в бюджете |
| AutoAck_Persistent_RoundTrip/process_time | 1336,5 → 880,0 | −34,16% | 0,90% / 0,96% | в бюджете |
| ClientAck_Persistent_RoundTrip/process_time | 1354,0 → 886,2 | −34,55% | 0,25% / 0,41% | в бюджете |
| ClientAck_Batch_NonPersistent/1 | 140,1 → 132,7 | −5,31% | 0,61% / 2,63% | в бюджете |
| ClientAck_Batch_NonPersistent/100 | 211,4 → 219,2 | +3,68% | 0,38% / 0,56% | в бюджете |
| ClientAck_Batch_NonPersistent/1000 | 265,2 → 241,5 | −8,93% | 0,23% / 1,24% | в бюджете |
| Transacted_NonPersistent_RoundTrip | 3959,5 → 3893,1 | −1,68% | 0,16% / 0,85% | в бюджете |
| Transacted_Persistent_RoundTrip/process_time | 69584,0 → 12912,4 | −81,44% | 0,73% / 0,07% | в бюджете |
| Transacted_Batch_NonPersistent/10 | 583,6 → 555,7 | −4,78% | 0,84% / 0,87% | в бюджете |
| Transacted_Batch_NonPersistent/100 | 264,0 → 238,8 | −9,56% | 0,28% / 3,67% | в бюджете |
| Transacted_Batch_NonPersistent/1000 | 285,1 → 226,3 | −20,63% | 0,80% / 2,58% | в бюджете |
| Transacted_Batch_Persistent/10/process_time | 13822,2 → 2946,1 | −78,69% | 1,93% / 0,62% | в бюджете |
| Transacted_Batch_Persistent/100/process_time | 11252,3 → 1973,7 | −82,46% | 1,21% / 1,17% | в бюджете |
| Transacted_Batch_Persistent/1000/process_time | 10983,9 → 2084,0 | −81,03% | 1,24% / 1,08% | в бюджете |

[Пересчёт](../../handoffs/current/72/logs/nfr-i4-recalc.log) ·
[Скрипт пересчёта](../../handoffs/current/72/logs/nfr-i4-recalc.py) ·
[Сырые выборки](../../handoffs/current/72/logs/github-perf-44475262358-attempt1.zip)

## ТЕХНИЧЕСКИЙ ВЕРДИКТ — rejected

По собственным данным шлюза, `AutoAck_NonPersistent_RoundTrip` даёт стабильный рост
медианы CPU time/op **106,6 → 113,6 нс/доставку, +6,59%**, при CV базы 0,57% и кандидата
1,73% — оба значения далеко ниже порога 5%, то есть это не шум, а воспроизводимое
превышение бюджета +5%. Путь входит в действующий набор сравнений SDD 71, который
REQ-72-9 явно сохраняет, поэтому нарушение бюджета на нём является отрицательным
результатом измерения. Остальные 14 путей стабильны и в бюджете (преимущественно
существенное ускорение кандидата). Повтор серии не требуется и не выполнялся: CV
в пределах порога. Измерение пригодно для отрицательного вердикта — регрессия
подтверждена данными, а не нестабильностью базовой серии.

Этот результат отличается от итерации 3: на AMD EPYC 7763 два persistent-батча шумели
по базе, а AutoAck был быстрее; на Intel Xeon 6975P-C с Clang 18 картина иная —
persistent-пути стабильны и быстры, а AutoAck стабильно медленнее базы на 6,59%.

## РЕШЕНИЕ О ВЫПУСКЕ — Owner deferral, не performance pass

Owner 19.11 принял два решения, зафиксированные в `chain.log` как `HUMAN-DECISION`:
при очередном неуспехе GitHub perf убрать perf-прогоны из GitHub и настраивать ночные
прогоны на этой машине; на уточнение о приёмке PR — «Влить по обычному CI, perf —
отдельная задача». Условие наступило: GitHub Performance `44475262358` — `failure`,
ordinary CI `44475262476` — `success`. Workflow `performance.yml` уже
`disabled_manually`; для `main` обязательны все 6 ordinary CI-проверок,
strict+admins сохранены.

Это явное исключение Owner из блокирующего НФТ-гейта для продолжения выпуска. Оно
**не** означает, что REQ-72-9 подтверждён или что технический результат стал зелёным:
`performance_verdict=rejected` при подтверждённых данных. НФТ не называется пройденным,
горячий путь не помечается N/A, стабильные числа не объявляются шумом. Отложенное
требование — `REQ-72-9`; открытая отдельная задача —
[M-05-local-nightly-performance](../../tasks/M-05-local-nightly-performance.md)
(статус открыта, ночной job не настроен, время Owner не задано).

Отпечаток дерева до и после прогонов совпал:
`tree:30773ebf309bde6232825bb32089394ddaf73ed33bb45d24e372a40d12597d76`.
