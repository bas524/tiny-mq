---
name: perf-check
description: Перф-гейт tiny-mq — прогнать бенчи и сравнить с baseline; регрессия горячего пути > ~5% = блокер. Используй после любого изменения горячего пути (routing, delivery, (de)serialization, storage, ack/transaction, сетевой кодек). Триггеры: «проверь перф», «прогони бенчи», «нет ли регрессии».
---

# perf-check

Гейт контура Quality/Verification. Производительность в tiny-mq — жёсткое требование; регрессия — блокер, а не «потом».

## Процедура

1. **Собери relwithdebinfo** (перф-конфиг, не debug):
   ```
   cd cmake-build-relwithdebinfo && ninja
   ```

2. **Прогони бенчи** (бенчи — в `tests/BenchmarkTest.cpp`):
   ```
   ./cmake-build-relwithdebinfo/tiny_mq --gbench [--benchmark_filter=<regex>]
   ```
   Фильтром сузь до затронутого горячего пути (send, recv, routing, serialize, storage, ack).

3. **Сравни с baseline.** Baseline — зафиксированный эталон (хранится в `bench-mq/`; если файла нет — попроси `platform-agent` завести и заверсионировать его, а текущий прогон прими как первый baseline с пометкой).
   - Метрики: throughput и/или latency по затронутым бенчам.

4. **Вердикт:**
   - регрессия **> ~5%** без обоснования → **блокер** (`status: rejected`), укажи было/стало, метрику и фильтр;
   - в пределах шума или обоснованно (осознанный trade-off, зафиксированный в ADR) → `status: approved`.

## Если бенча нет
Изменение горячего пути без бенча **не наблюдаемо** → добавь бенч в `tests/BenchmarkTest.cpp` до вынесения вердикта. «Нет бенча» ≠ «нет регрессии».

Верни в `evidence`: числа бенчей (baseline vs current) по затронутым путям.
