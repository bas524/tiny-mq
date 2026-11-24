# Baseline бенчей tiny-mq

Зафиксированные значения main на девелоперской машине. Предназначение — сравнивать регрессии
последующих спек (routing/delivery/storage/ack/transaction) с эталоном, а не с «предыдущим
запуском ветки».

## Baseline main от 24.11.2026

Owner принял таблицу из локального коммита `bef6548` 24.11.2026
ответом «принимаю» в чате. Замерен `main` `2c3e80355bca795976b914f414dbc56037c36a5f`
в паре с pre-M‑07 `0dc8995f107ad8746d72cd0aa5555da235817e23` на Mac16,8 (arm64), macOS 27.2.1,
Apple clang version 21.0.0 (clang-2100.4.1.1), RelWithDebInfo. vcpkg `cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3`.
SHA-256 бинарника main `2da430f6c3ea967ec7659894e0f58e80b42bd86a9c2ff91a274228dc3d599090`.

На каждую сторону приходится девять повторов ABBA. CPU включает работу storage
workers для persistent-путей. Дневной прогон выполнен от сети в ручном режиме.
Штатный прогон M‑07 сохранил `rejected` из-за CV у `Transacted_Persistent_RoundTrip/process_time`.
Для этого пути в таблицу взят отдельный ABBA-замер с минимумом 12 секунд на
повтор. Независимый пересчёт подтвердил бюджет +5% и CPU CV ≤5% на всех
выбранных сериях.

| Путь | CPU p50, нс/доставку | Real p50, нс/доставку | CPU CV, % | Δ CPU к pre-M‑07, % |
|---|---:|---:|---:|---:|
| `AutoAck_NonPersistent_RoundTrip` | 58.58 | 58.59 | 4.45 | -0.02 |
| `Topic_AutoAck_NonPersistent_RoundTrip` | 64.05 | 64.07 | 1.37 | +0.14 |
| `AutoAck_Persistent_RoundTrip/process_time` | 3802.41 | 3809.71 | 2.75 | -1.11 |
| `ClientAck_Persistent_RoundTrip/process_time` | 3842.12 | 3848.99 | 2.92 | +0.37 |
| `ClientAck_Batch_NonPersistent/1` | 72.67 | 72.69 | 1.96 | +1.25 |
| `ClientAck_Batch_NonPersistent/100` | 99.28 | 99.30 | 1.03 | +1.98 |
| `ClientAck_Batch_NonPersistent/1000` | 102.57 | 102.60 | 3.13 | +2.11 |
| `Transacted_NonPersistent_RoundTrip` | 16509.49 | 16514.00 | 2.19 | -0.04 |
| `Transacted_Persistent_RoundTrip/process_time` | 57893.87 | 69162.12 | 1.63 | -3.04 |
| `Transacted_Batch_NonPersistent/10` | 1799.90 | 1800.12 | 4.14 | -0.23 |
| `Transacted_Batch_NonPersistent/100` | 286.67 | 286.69 | 1.75 | +0.24 |
| `Transacted_Batch_NonPersistent/1000` | 122.15 | 122.16 | 0.77 | +0.54 |
| `Transacted_Batch_Persistent/10/process_time` | 9308.58 | 10421.82 | 2.01 | +0.09 |
| `Transacted_Batch_Persistent/100/process_time` | 4077.37 | 4224.55 | 2.70 | -3.82 |
| `Transacted_Batch_Persistent/1000/process_time` | 3810.13 | 3826.67 | 2.97 | -0.81 |

Сырые данные и проверка находятся в основном checkout под `handoffs/current/M-05/manual/2026-11-24-hu3xsb2l/`.
SHA-256 `verification.json` — `98c6605c73d8514c51c2577c9c5b89420eb3948509c2879261b44fafa4e2d86f`.
SHA-256 `m07/long-roundtrip.json` — `346bf01dc0d195dd2e66b8622140439b1b3012b2a3fa66bec2de22cb9fa64bad`.

## Исторические измерения от 31.07.2026

### Методика

- **Сборка:** `cmake --preset user-release && cmake --build --preset release --parallel`
  (RelWithDebInfo, vcpkg toolchain).
- **Бинарь:** `./cmake-build-releasewithdebuginfo/tiny_mq --gbench`.
- **Флаги:** `--benchmark_repetitions=7..9 --benchmark_report_aggregates_only=true --benchmark_min_time=1.2s`.
- **Метрика:** `cpu_mean_ns` (wall time под нагрузкой шумит сильнее). Верь значениям с CV ≤ ~5%;
  выше — перепрогнать.
- **Машина:** Apple M2, 8 ядер (4P+4E), macOS (Darwin 25.5.0), 2026-07-31. Машина **шумная**
  (общая, L.A. колеблется 3–55); сравнение — только интерливированные прогоны в одном окне.

### Таблица main

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

1. Для допуска сравнивай точные baseline и candidate в одном окне на одной машине.
2. Регрессия CPU свыше 5% при CV обеих сторон ≤5% блокирует допуск. Серия с CV
   свыше 5% требует повтора по методике NFR.
3. При смене toolchain или машины пересними эталон.
4. Для горячего пути без измерения добавь сценарий по [nfr-gate](../.claude/skills/nfr-gate/SKILL.md).
