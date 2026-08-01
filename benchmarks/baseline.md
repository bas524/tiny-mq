# Baseline бенчей tiny-mq

Зафиксированные значения main на девелоперской машине. Предназначение — сравнивать регрессии
последующих спек (routing/delivery/storage/ack/transaction) с эталоном, а не с «предыдущим
запуском ветки».

## Методика

- **Сборка:** `cmake --preset user-release && cmake --build --preset release --parallel`
  (RelWithDebInfo, vcpkg toolchain).
- **Бинарь:** `./cmake-build-releasewithdebuginfo/tiny_mq --gbench`.
- **Флаги:** `--benchmark_repetitions=7..9 --benchmark_report_aggregates_only=true --benchmark_min_time=1.2s`.
- **Метрика:** `cpu_mean_ns` (wall time под нагрузкой шумит сильнее). Верь значениям с CV ≤ ~5%;
  выше — перепрогнать.
- **Машина:** Apple M2, 8 ядер (4P+4E), macOS (Darwin 25.5.0), 2026-07-31. Машина **шумная**
  (общая, L.A. колеблется 3–55); сравнение — только интерливированные прогоны в одном окне.

## Таблица main

| Бенчмарк | CPU mean, ns | CV, % | rep | Примечание |
|---|---|---|---|---|
| AutoAck_NonPersistent_RoundTrip | 159 | 1.2–7.4 | 9 | горячий путь |
| Topic_AutoAck_NonPersistent_RoundTrip | 163 | 1.2–3.8 | 9 | горячий путь |
| AutoAck_Persistent_RoundTrip | 4200 | 0.8–1.0 | 7 | storage-bound |
| ClientAck_Persistent_RoundTrip | 4250 | 3.7–4.8 | 7 | storage-bound |
| Transacted_NonPersistent_RoundTrip | 20900 | 2.1–3.2 | 7 | commit-bound |
| Transacted_Persistent_RoundTrip | 56500 | 0.7–4.3 | 7 | storage+commit |
| Transacted_Batch_NonPersistent/100 | 46400 | 3.1 | 5 | commit amortized |
| Transacted_Batch_NonPersistent/1000 | 251000 | 2.1 | 5 | |
| Transacted_Batch_Persistent/100 | 790000 | 0.3 | 5 | |
| Transacted_Batch_Persistent/1000 | 7.63M | 0.7 | 5 | |

> Pooled-mean по чистым прогонам одного окна (2026-07-31, L.A. 3–5 для горячего пути).
> Ориентиры раунда 1 (2026-07-30, L.A. ~13): AutoAck 150 ns, Topic 172 ns, AutoAck_Persistent 4007 ns,
> Transacted_Persistent 56609 ns — совместимы с этой таблицей в пределах шума машины.

## Правила использования

1. Прогон сравнивается с **этим** baseline, а не с соседним прогоном той же ветки.
2. Регрессия горячего пути (routing/delivery/serialize/storage/ack/transaction) **> ~5%** без
   обоснования = блокер.
3. Если baseline устарел (значимые изменения тулинга/железа) — переснять и обновить эту таблицу.
4. Добавление нового бенча на горячий путь без него — обязательный шаг (см. perf-check).
