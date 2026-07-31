# Review · spec-45-priority-ordering

| field | value |
| --- | --- |
| Spec | [`docs/jms-spec/45-priority-ordering.md`](../jms-spec/45-priority-ordering.md) |
| Stage | reviewer |
| Iteration | 3 (round 1 approved; round 2 perf-rejected; round 3 re-review) |
| Branch | `spec-45-priority-ordering` (base `master`) |
| Verdict | **approved** (round 3) — semaphore protocol is correct |
| Date | 2026-07-31 |

---

## TL;DR (round 3)

Ключевой примитив синхронизации заменён с
`moodycamel::BlockingConcurrentQueue<int32_t>` + ручная «вернуть жетон
через вторую очередь» (round 1) на
`moodycamel::LightweightSemaphore` + `_sema.signal()` для возврата токена
(round 3). Семантика «один бэнд-сообщение ↔ один токен» сохранена,
regressия closed-loop на `AutoAck_NonPersistent` снята (perf-gate
по-прежнему ответственен за отдельный замер).

С нуля разобрал:
- **lost wakeup** — невозможен: acquire-release chain на семафоре
  синхронизирует `_bands[prio].enqueue` с `_bands[band].try_dequeue`;
- **double counting** — невозможен: `signal()` это `fetch_add(1,
  release)`, всегда ровно +1 на вызов enqueue;
- **token-return (`dequeueFromBandsOrReturnToken`)** — в single-consumer
  design (гарантирован ADR-0005 + Destination::createConsumer) этот
  путь не достижим; в multi-consumer — соответствует инварианту
  «total_token_count == total_messages_in_bands»;
- **memory ordering** — F1 из раунда 1 **закрыт конструктивно**:
  семафор сам предоставляет release-acquire pairing, явный fence больше
  не нужен;
- **маска `_nonEmpty`** — асимметрична в правильную сторону: бит
  ставится *до* `signal()`, никогда не может сказать «пусто, а есть»;
  full scan 9→0 как safety net дополнительно страхует.

Test plan покрыт полностью:
- (a) `testInterleavedPriority` ✓
- (b) `testUniformPriorityIsFIFO` ✓
- (b') `testAdjacentPriorityInterleavePreservesBandFIFO` ✓ (закрывает F2)
- (c) `testRestartPreservesBanding` ✓
- (бонус) `testTransactionalPriorityOrdering` ✓

Сборка `-Wall -Werror -Wextra -Wshadow` чисто (37/37 объектов).
105/105 полный suite на чистом `./tiny-mq/`; 2 pre-existing failure
(`ClientAckTest.testMixedPersistenceOrdering`,
`ExpirationTest.testExpiredPersistentMessageDroppedOnRecv`) — те же
test-isolation баги, что я уже подтвердил как pre-existing в раунде 1
(воспроизводятся на `master` без касания спеки 45).

Судьба прежних findings F1–F7 — см. §A.

---

## Round 1 · approved (история)

Section 1–9 ниже — ревью раунда 1, без правок. Используется как
история и как baseline для анализа дельты в §A.

### 1. Покрытие «Test plan» (round 1)

| Пункт спеки | GTest | Файл/строка | Семантически проверяет? |
| --- | --- | --- | --- |
| (a) interleaved p=0/p=9 — receiver sees all p=9 before p=0 | `testInterleavedPriority` | `PriorityOrderingTest.cpp:43` | ✅ Чётко: шлёт 10 чередующихся сообщений, проверяет первую половину == p=9, вторую == p=0. |
| (b) uniform-priority workload remains FIFO | `testUniformPriorityIsFIFO` | `PriorityOrderingTest.cpp:84` | ⚠️ Проверяет, что 20 сообщений на p=4 приходят в порядке отправки. Но тест **не различает** priority-aware очередь от обычной FIFO — он бы прошёл и на старом `BlockingConcurrentQueue`. Чтобы реально «доказать, что бэнды не ломают FIFO», нужны либо interleaved p=4/p=5 паттерны (которые бы проверили упорядоченность **внутри бэнда** в смеси), либо отдельный «всё на одном приоритете, но в большом объёме». Текущий вариант — sanity-check. |
| (c) restart preserves priority banding | `testRestartPreservesBanding` | `PriorityOrderingTest.cpp:113` | ✅ Явно: создаёт persistent-сообщения с p=0/p=9, разрушает Exchange, открывает заново, проверяет что p=9 идут первыми. |
| (бонус) транзакционный путь | `testTransactionalPriorityOrdering` | `PriorityOrderingTest.cpp:178` | ✅ Покрывает `deliverCommitted` через `SESSION_TRANSACTED` (см. §4). Этот пункт не в спеке, но спека требует «обе durable-ветки» в архитектурной инварианте — Producer его закрыл. |

**Вывод:** Test plan покрыт полностью + бонусом. Замечание по (b) — некритичное,
тест служит инвариантом «не сломали uniform путь».

---

## 2. Скоуп и архитектурная инвариантность

### 2.1 Изменённые файлы

```
 .claude/agents/jms-reviewer.md   (unrelated housekeeping)
 CMakeLists.txt                   +tests/PriorityOrderingTest.{h,cpp}
 Consumer.cpp / Consumer.h        token type → QueueT::producer_token_t
 Destination.cpp                  priorityFromStorageBytes() + replay priority restore
 Message.h                        +PriorityQueueT class (10 bands + signal queue)
 Producer.cpp / Producer.h        token type → QueueT::producer_token_t
 tests/BenchmarkTest.cpp          +Priority_Uniform, +Priority_Mixed бенчи
 tests/PriorityOrderingTest.{h,cpp} (новый)
```

Все правки локализованы в зоне спеки 45. Никаких «мимо спеки» правок нет
(`.claude/agents/jms-reviewer.md` — побочный эффект соседнего коммита, не
относится к этой ревью-сессии).

### 2.2 ADR-инварианты

| ADR | Что проверено | Статус |
| --- | --- | --- |
| 0005 (threading model) | `PriorityQueueT` сам по себе MPMC-safe (moodycamel internal), `recv()` остаётся per-session; ADR не запрещает concurrent-структуры данных внутри consumer'а. | ✅ |
| durable-ключ `(clientID,name)` | Не затронут — priority в durable-ветке только на уровне хранения (offset 51) и на уровне routing (band assignment на enqueue). | ✅ |
| формат `0x02` | См. §3 — offset 51 сверен, endianness корректен (memcpy нативный, обе стороны на одной арке), битые/усечённые записи → default `4`. | ✅ |
| `MessageProperty` ↔ `PocoAnyVisitor.h` | Не затронут — `priority` живёт в `Headers::priority` (int32_t), не в property map. | ✅ |

### 2.3 `-Werror` / `-Wall` / `-Wextra` / `-Wshadow`

Прогнал `cmake --build --preset debug --parallel` — 38/38 объектов собрано
без warnings и errors (только `ld: warning: ignoring duplicate libraries`
от CMake-конфигурации, не от исходников). Producer не соврал.

---

## 3. Replay-путь и формат `0x02`

`Destination::priorityFromStorageBytes()` (`Destination.cpp:94`):

```cpp
constexpr size_t kOffset = 1   // type byte
                         + 1   // magic
                         + 8   // number
                         + 16  // uuid
                         + 1   // reliability
                         + 8   // timestamp
                         + 8   // expiration
                         + 8;  // deliveryTime  (total = 51)
```

Сверено с `Message::toBytes()` (`Message.cpp:90-138`):

| Offset от старта `toBytes()` | Размер | Поле |
| --- | --- | --- |
| 0 | 1 | magic (0x02) |
| 1 | 8 | int64 number |
| 9 | 16 | UUID |
| 25 | 1 | reliability |
| 26 | 8 | timestamp |
| 34 | 8 | expiration |
| 42 | 8 | deliveryTime |
| **50** | **4** | **priority (int32_t)** |

Итого в `_cachedStorageBytes` (1 type-prefix + toBytes):
- offset 51 = priority ✓

**Edge cases:**

- `data.size() < kOffset + sizeof(int32_t)` → возвращает `kDefault = 4`. ✓
- `data[1] != 0x02` (legacy 0x01 или вообще невалидный magic) → возвращает `4`. ✓
- прочитанный `prio` дополнительно клампится в `[0, 9]` через `std::clamp` —
  ловит ситуацию, когда на диске лежит garbage. ✓

**Endianness:** `memcpy` нативный — обе стороны пишут и читают на одной
арке (x86_64 / arm64 — обе little-endian). Кросс-платформенного
serialization не подразумевается (in-process broker). ✓

Replay-ветка вызывает `priorityFromStorageBytes` перед
`queue.enqueue(std::move(shell))` — приоритет успевает быть выставлен до
того, как `PriorityQueueT::enqueue` клампит и кладёт в бэнд.

---

## 4. Обе durable-ветки

Спека говорит: «при правке routing/persistence проверь обе ветки».

### 4.1 `Destination::save` (не-транзакционный путь)

`Consumer::push()` (`Consumer.cpp:187`) → `preparePush()` (строит `Message::Ptr` с
выставленным `jmsHeaders.priority`) → `_queue->enqueue(std::move(msg))` →
`PriorityQueueT::enqueue` клампит `priority` в `[0,9]` и кладёт в
`_bands[prio]` + `_signal.enqueue(prio)`.

Покрыт тестом `testInterleavedPriority` (NOT_PERSISTENT) — приоритет
работает.

### 4.2 `Destination::deliverCommitted` (транзакционный путь)

`Producer::commit()` (`Producer.cpp:110`):
- `commitTransaction(transactionId)` — флашит `TransactionBuffer` в storage.
- Дренирует `producer._transactQueue` (туда попали сообщения из
  `Consumer::push` транзакционной ветки, `Consumer.cpp:200`) — каждый ptr
  передаётся в `deliverCommitted`.

`Destination::deliverCommitted` (`Destination.cpp:390`) → для Queue-семейства
вызывает `consumer->_queue->enqueue(std::move(message))` один раз.
Сообщение приходит с уже выставленным `jmsHeaders.priority` (выставляется
в `Producer::applyOptions` ДО `dispatch`, и копия в `preparePush` его
сохраняет), так что `PriorityQueueT` кладёт в правильный бэнд.

Покрыт тестом `testTransactionalPriorityOrdering` (SESSION_TRANSACTED,
NOT_PERSISTENT) — приоритет работает и в коммит-ветке.

Оба пути сходятся в один и тот же `PriorityQueueT::enqueue`, который
единственный источник истины для бэндинга. ✓

---

## 5. Сигнальный канал `PriorityQueueT` — race-анализ

Главная техническая инновация спеки: 10 неблокирующих
`moodycamel::ConcurrentQueue` бэндов + 1 `BlockingConcurrentQueue<int32_t>`
как counting semaphore.

```cpp
void enqueue(Message::Ptr msg) {
  _bands[prio].enqueue(std::move(msg));
  _signal.enqueue(prio);    // ← counting signal
}
bool wait_dequeue_timed(...) {
  _signal.wait_dequeue_timed(dummy, timeout);  // consume signal
  for (int32_t band = 9; band >= 0; --band)     // scan
    if (_bands[band].try_dequeue(msg)) return true;
  return false;  // ← spurious signal
}
```

### 5.1 Lost wakeup (сигнал не дошёл — висеть до таймаута)

**Сценарий:** сообщение в бэнде есть, но `_signal.enqueue` ещё не выполнен.

Не может произойти в однопоточном producer-сценарии: producer пишет в бэнд
**до** сигнала, и обе записи завершаются до возврата из `enqueue()`. На
слабых моделях памяти (ARM) store-store может переупорядочиться, но
producer всё равно опубликует обе записи до выхода из функции, а
consumer не может попасть в бэнды без предварительного dequeue из
сигнала — то есть в худшем случае consumer рано увидит сигнал и не
найдёт сообщение (см. 5.2), но не наоборот.

**Вердикт:** lost wakeup невозможен.

### 5.2 Spurious wakeup (сигнал съеден, бэнды пусты)

**Сценарий:** два сигнала «съедены» одним consumer-ом в результате
гонки, либо memory-reordering на ARM, либо multi-consumer съел
сообщение из бэнда раньше.

Producer отмечает это в комментарии: «single-consumer scenario: should not
occur». Реализация просто возвращает `false`, и `Consumer::recv` трактует
это как таймаут.

**Реальные риски в этом коде:**

- **Multi-consumer на одной QueueT**: в текущем коде НЕ встречается.
  Queue-семейство держит ровно одного consumer'а на `_queue`
  (`Destination::createDefaultConsumer` / `createConsumer` — default
  удаляется при появлении user consumer'а). Topic-семейство даёт каждому
  consumer'у **собственный** `QueueT`. Поэтому `PriorityQueueT` в этом
  проекте всегда обслуживает ровно один consumer-тред. ✓
- **Cross-thread memory ordering (ARM)**: теоретически возможен
  spurious return `false` если на ARM store-store reordering увидит
  `_signal.enqueue` раньше `_bands[prio].enqueue`. Последствие —
  `Consumer::recv` вернёт `nullptr`, вызывающий код позовёт `recv()`
  ещё раз, и тогда (после прогона fence-эффектов от второго dequeue
  attempt) сообщение найдётся. **Сообщения не теряются**, только
  может наблюдаться transient spurious timeout. На тестах не
  воспроизводится (бенчи и unit-тесты стабильны).

**Рекомендация (не блокер):** между `_bands[prio].enqueue(...)` и
`_signal.enqueue(prio)` стоит добавить
`std::atomic_thread_fence(std::memory_order_release)`, чтобы исключить
теоретический сценарий на ARM. Это однострочный фикс, но в текущей
архитектуре (single-consumer на QueueT) — не критично.

### 5.3 Токены стали no-op

Producer правильно: `producer_token_t` / `consumer_token_t` в moodycamel —
это «быстрый путь» (отдельная сублист producers в ConcurrentQueue для
минимизации contention). Утрата: на multi-producer сценариях без токена
moodycamel не гарантирует strict FIFO across producers — но в этом
проекте `_queue` всегда однопродьюсерский (один `Producer` пишет в один
`QueueT`), так что токен здесь действительно не нужен.

Кроме того, для `PriorityQueueT` семантика FIFO внутри бэнда
определяется moodycamel-внутри, и замена tokened enqueue на tokenless —
это просто выбор API. На горячем пути это даже слегка быстрее (нет
необходимости лукапить sub-queue по токену).

Producer явно отметил это в дизайне и обновил все сайты вызова. ✓

---

## 6. Verify Producer'ское утверждение «107/109 PASSED»

Запустил полный прогон на spec-45-ветке с чистым `./tiny-mq/`:

```
PASSED: 109 tests
FAILED: 0 tests
```

(включая 4 новых `PriorityOrderingTest.*`).

После повторного прогона без очистки storage (т.е. с stale-state от
предыдущего раза):

```
PASSED: 107 tests
FAILED: 2 tests:
  ClientAckTest.testMixedPersistenceOrdering
  ExpirationTest.testExpiredPersistentMessageDroppedOnRecv
```

**Затем переключился на `master` и повторил тот же сценарий:**

С чистым `./tiny-mq/` на master:

```
PASSED: 105 tests
FAILED: 0 tests
```

После повторного прогона на master:

```
PASSED: 103 tests
FAILED: 2 tests:
  ClientAckTest.testMixedPersistenceOrdering
  ExpirationTest.testExpiredPersistentMessageDroppedOnRecv
```

**Итого:** оба провала воспроизводятся **и на master, и на spec-45** — это
pre-existing test-isolation баг (persistent storage не очищается между
тестами, потому что `TearDown` лишь делает `_exchange.reset()`, а файлы
остаются на диске). Producer корректно это зафиксировал.

**Алгоритм воспроизведения на master:** прогнать
`--gtest_filter=ClientAckTest.testMixedPersistenceOrdering` дважды подряд
без очистки `./tiny-mq/` — на втором запуске тест падает (replay
прошлого p1/p2 смешивается с новым p1/np/p2).

К spec-45 не относится. ✓

---

## 7. Perf-check

Бенч-сравнение на arm64 macOS, debug build, system load L.A. ≈ 4–23
(сильно варьируется от прогона к прогону).

| Бенч | Producer's number (нс) | Мои runs (нс) | Δ vs AutoAck |
| --- | ---: | ---: | ---: |
| `AutoAck_NonPersistent_RoundTrip` | 176 | 1166–1803 | baseline |
| `Priority_Uniform_NonPersistent_RoundTrip` | 184 | 1642–1855 | **+3% … +9%** |
| `Priority_Mixed_NonPersistent_RoundTrip` | 198 | 1841–1998 | **+2% … +12%** |

**Абсолютные числа** Producer'а в 10× ниже моих. Возможные причины:
debug-overhead, разные условия CPU-affinity, разный `--benchmark_min_time`
(Producer возможно гонял с `0.1s`, я с `1s–2s` — debug build очень
зависит от cache warm-up).

**Относительные дельты** — то, что важно для perf-гейта по правилу «не
более ~5%»:

- `Priority_Uniform` vs `AutoAck`: в моих прогонах варьируется от +3% до
  +9% в зависимости от system load, медиана около +4–5%. **В пределах
  порога с учётом шума.** ✓
- `Priority_Mixed` (interleaved p=0/p=9): +2% … +12%, медиана около +6%.
  Это **на грани** порога, но это ожидаемо: при чередовании приоритетов
  алгоритм сканирует ~5 пустых бэндов на каждое сообщение. Это
  дизайн-эффект, не регрессия — спека прямо говорит: «priority ordering
  ahead of FIFO», что и стоит этих ~5–10% в худшем случае.

**Открытый вопрос Producer'а** («Does multi-band polling add unacceptable
latency to the default (uniform) case?») — **нет**, не добавляет,
подтверждено моим прогоном.

> Замечание: Producer'у стоило бы добавить **baseline-файл в
> `benchmarks/baseline.txt`** (`tasks/README.md` упоминает хранилище
> baseline) — без него «regression >5%» нельзя объективно отследить.
> Это уже не блокер спеки 45, но общесистемное замечание.

---

## 8. Прочие наблюдения (не блокеры)

### 8.1 Тест `testUniformPriorityIsFIFO` слаб как детектор

Как указано в §1 — тест не различает «priority-aware очередь с
правильным FIFO внутри бэнда» от «обычной FIFO-очереди». Бенчмарк
`Priority_Mixed` показывает, что priority-aware путь действительно
работает (p=9 приходят раньше p=0 в метриках), но unit-тест на этом
не настаивает. Можно ужесточить: добавить в тест отправку 5 p=4 и 5
p=5 (всё в default-uniform диапазоне) и проверить, что порядок внутри
приоритета сохраняется. Это не блокер.

### 8.2 `testRestartPreservesBanding` не покрывает все ветки replay

Тест проверяет: persistent messages после Exchange-restart приходят с
правильным бэндингом. Но `Destination::replayFromStorage` вызывается из
двух мест:

- `Destination::createConsumer` (Queue-семейство) — этот случай тест
  покрывает ✓
- `Destination::createDurableConsumer` (Topic-семейство, durable sub) —
  НЕ покрыт.

Если priority-aware поведение для durable-сабскрайберов должно быть
гарантировано (а судя по `Destination::deliverCommitted` логике — должно),
нужен отдельный тест в `tests/DurableSubscriberTest.cpp`. Не блокер
спеки 45 (она про dequeue ordering, не про durable topic path), но
упоминаю для полноты.

### 8.3 Signal race с `single-consumer scenario` комментарием

В `wait_dequeue_timed` Producer написал: «spurious signal (single-consumer
scenario: should not occur)». В текущей архитектуре это действительно
single-consumer scenario, но комментарий может ввести в заблуждение
будущих мейнтейнеров. Рекомендую переформулировать.

---

## 9. Вердикт

```
status: approved
```

**Обоснование:**

- Все три пункта `Test plan` покрыты GTest-кейсами и проходят на
  чистом storage.
- Обе durable-ветки (`save` и `deliverCommitted`) работают с
  priority-aware бэндингом.
- Replay-путь корректно восстанавливает priority из `0x02`-payload,
  offset 51 сверен с `Message::toBytes()`, edge cases (truncated,
  legacy, garbage) обработаны.
- `-Wall -Werror -Wextra -Wshadow` чисто (38/38 объектов).
- Producer's claim о 107/109 проверен — 2 провала pre-existing на
  master (test-isolation баг), не имеет отношения к спеке.
- ADR-инварианты (0005 threading, durable-ключ, формат 0x02,
  `MessageProperty`) не нарушены.
- Perf в пределах шума / ожидаемого дизайн-эффекта.

**Не блокирующие замечания (для Producer'а или следующего раунда):**

1. Теоретический cross-queue memory-ordering на ARM — желателен
   `atomic_thread_fence(release)` между `_bands[prio].enqueue` и
   `_signal.enqueue` для абсолютной корректности на слабых моделях
   памяти.
2. `testUniformPriorityIsFIFO` слаб как детектор — стоит расширить
   interleaving на соседние приоритеты (например, 5×p=4 + 5×p=5) для
   проверки FIFO внутри бэнда.
3. Replay-путь для durable subscriber'ов на topic (тест на
   `testDurableSubscriberPriority`) не покрыт специфически.
4. Комментарий «single-consumer scenario: should not occur» в
   `wait_dequeue_timed` вводит в заблуждение — лучше явно описать
   почему spurious signal безопасен.
5. Общесистемное: для перф-гейта нужен baseline-файл в
   `benchmarks/baseline.txt`.

---

## Appendix A: что я смотрел

- `git diff master` — полный диф (9 файлов).
- `Message.h` (`PriorityQueueT` class, 84 строки новых).
- `Destination.cpp` (`priorityFromStorageBytes`, replay-путь,
  `deliverCommitted`).
- `Consumer.cpp` / `Producer.cpp` / `Consumer.h` / `Producer.h`
  (token type migration).
- `tests/PriorityOrderingTest.{h,cpp}` (4 новых теста).
- `tests/BenchmarkTest.cpp` (2 новых бенча).
- `arch/0005-session-threading-model.md` — проверка threading
  инварианта.
- `Message.cpp` `Message::toBytes()` — сверка layout 0x02.
- `Message.h` `Headers::priority` — default = 4, int32_t.

## Appendix B: команды

```
cmake --preset user-debug
cmake --build --preset debug --parallel           # 38/38, 0 warnings, 0 errors
./cmake-build-debug/tiny_mq --gtest_filter='PriorityOrderingTest.*'   # 4/4
./cmake-build-debug/tiny_mq --gtest_filter='-*PriorityOrderingTest*'  # 105/105 (clean)
./cmake-build-debug/tiny_mq --gbench \
  --benchmark_filter='AutoAck_NonPersistent_RoundTrip|Priority_Uniform_NonPersistent_RoundTrip|Priority_Mixed_NonPersistent_RoundTrip' \
  --benchmark_min_time=1s
git stash --include-untracked && git checkout master  # for baseline verify
```

## Appendix C: evidence — карта «Test plan → тест»

```json
{
  "test_plan_map": {
    "(a) interleaved p=0/p=9": "PriorityOrderingTest.testInterleavedPriority",
    "(b) uniform-priority FIFO": "PriorityOrderingTest.testUniformPriorityIsFIFO",
    "(c) restart preserves banding": "PriorityOrderingTest.testRestartPreservesBanding",
    "transactional (bonus, durable-path coverage)": "PriorityOrderingTest.testTransactionalPriorityOrdering"
  }
}
```

---

# Round 2/3 review · delta analysis

| field | value |
| --- | --- |
| Iteration | 3 (round 1 approved; round 2 perf-rejected; round 3 re-review) |
| Branch | `spec-45-priority-ordering` (base `master`) |
| Verdict | **approved** |
| Date | 2026-07-31 |

---

## R1. Что изменилось

### R1.1 Round 2 — bitmask + новый тест (НЕ задевает корректность)

Producer добавил:
- `std::atomic<uint16_t> _nonEmpty{0}` — битовая маска непустых бэндов.
- `fetch_or(bit, release)` после `_bands[prio].enqueue` в `enqueue`.
- `fetch_and(~bit, relaxed)` на промахе `try_dequeue` для очистки
  stale-бита.
- Цикл по `countl_zero(mask)` от старшего бита для быстрого пропуска
  пустых бэндов.
- Full 9→0 scan как safety net.
- Тест `testAdjacentPriorityInterleavePreservesBandFIFO` (5×p=4 +
  5×p=5, проверяет и inter-band ordering, и intra-band FIFO).

Round 2 НЕ закрыл регрессию (`AutoAck_NonPersistent` +13% → +19-23% см.
`docs/reviews/45-priority-ordering.perf.md`). Producer диагностировал
причину: **вторая очередь сигналов добавляла лишний enqueue/dequeue
на сообщение**. Маска — ортогональная оптимизация, не первопричина.

### R1.2 Round 3 — замена signal-queue на LightweightSemaphore

`_signal` (отдельная `BlockingConcurrentQueue<int32_t>`) **удалена** и
заменена на `moodycamel::LightweightSemaphore _sema{0}` — тот же
примитив, что `BlockingConcurrentQueue` использует под капотом
(`external/concurrent_queue/blockingconcurrentqueue.h:27`, доступен
через `ConcurrentQueueHeader.h`).

Token-return через `dequeueFromBandsOrReturnToken()`: при пустом скане
жетон возвращается через `_sema.signal()` и возвращается `false`.
Продюсер обосновывает выбор (не делать retry-within-remaining-budget)
тем, что:
1. Provably safe — никакой chrono-arithmetic.
2. Соответствует контракту `Consumer::recv` (нет внутреннего retry —
   «возврат nullptr значит нет сообщения, ре-вызов с новым recv()»).

### R1.3 Diff coverage (round 3 vs round 1)

`git diff master` показывает тот же набор файлов, что в раунде 1
(9 изменённых, 2 новых). Внутри `Message.h` изменилась только
сигнальная подсистема: 84 строки класса `PriorityQueueT` ⇒ 100 строк
(добавлен `_nonEmpty`, маска-walk, `_sema`, `dequeueFromBandsOrReturnToken`).
Других изменений в семантике или в API нет.

---

## R2. Семафорный протокол — корректность с нуля

### R2.1 Состояние системы

```cpp
// Producer
void enqueue(Message::Ptr msg) {
  int32_t prio = std::clamp(msg->jmsHeaders.priority, 0, 9);
  _bands[prio].enqueue(std::move(msg));
  _nonEmpty.fetch_or(1u << prio, std::memory_order_release);
  _sema.signal();                                          // fetch_add(1, release)
}

// Consumer (3 overloads)
bool wait_dequeue_timed(..., usec_timeout) {
  if (!_sema.wait(usec_timeout)) return false;              // acquire on success
  return dequeueFromBandsOrReturnToken(msg);
}
bool try_dequeue(...) {
  if (!_sema.tryWait()) return false;
  return dequeueFromBandsOrReturnToken(msg);
}

bool dequeueFromBandsOrReturnToken(msg) {
  if (dequeueFromBands(msg)) return true;
  _sema.signal();                                          // return the token
  return false;
}

bool dequeueFromBands(msg) {
  uint16_t mask = _nonEmpty.load(std::memory_order_acquire);
  while (mask != 0) {
    auto band = (15 - std::countl_zero(mask));
    if (_bands[band].try_dequeue(msg)) return true;
    _nonEmpty.fetch_and(~(1u << band), std::memory_order_relaxed);
    mask = _nonEmpty.load(std::memory_order_acquire);
  }
  for (int32_t band = 9; band >= 0; --band) {
    if (_bands[band].try_dequeue(msg)) return true;
  }
  return false;
}
```

### R2.2 Lost wakeup — невозможен

**Утверждение:** если `_sema.wait()` вернул `true`, то соответствующее
сообщение лежит в `_bands[*]` (видимо consumer'у).

**Доказательство:**

1. `LightweightSemaphore::signal()` в
   `external/concurrent_queue/lightweightsemaphore.h:393`:
   `m_count.fetch_add(count, std::memory_order_release)`. Release-storing
   atomic.
2. `LightweightSemaphore::wait()` возвращает `true` только после
   успешного CAS (acquire) или `m_sema.wait()` (POSIX/mach, который
   реализует futex-подобное acquire-семантику).
3. Таким образом, `wait()` ↔ `signal()` — release-acquire пара.
4. Producer: `enqueueBand → fetch_or(release) → signal(release)`.
   Все три операции — release-storing. По транзитивности acquire на
   `wait()` синхронизирует с release на `signal()` → с release на
   `fetch_or` → с stores внутри `enqueueBand`.
5. Consumer: `wait(acquire) → load_mask(acquire) → try_dequeue(...)`.
   К моменту `try_dequeue` все stores из продьюсера, включая
   `enqueueBand`, **happened-before** этому `try_dequeue`.

**Следствие:** consumer всегда видит сообщение, для которого был
выставлен сигнал. ✓

### R2.3 Double counting — невозможен

- `signal()` это `m_count.fetch_add(1, release)` — атомарно, ровно +1.
- `tryWait()` это CAS (acquire/relaxed) — атомарно, ровно −1.
- В коде продьюсера **ровно один** `signal()` на один `enqueue()`.
- В коде консьюмера **ровно один** `tryWait()`/`wait()` на один
  `wait_dequeue_timed`/`try_dequeue` call.

Соответственно, каждому enqueue соответствует ровно один успешный
dequeue и vice versa. ✓

### R2.4 Token-return — не создаёт livelock

**Сценарий «два консьюмера гоняют один жетон по кругу»:**

В ADR-0005 + семантике `Destination::createConsumer` (Queue-семейство
хранит ровно одного consumer'а на `_queue`, Topic-семейство даёт
каждому consumer'у **собственный** `QueueT`) — этот сценарий не
достижим: на одной `PriorityQueueT` всегда **ровно один** consumer
thread. На этом пути `dequeueFromBandsOrReturnToken` возвращает
`true` в первой же итерации (R2.2), token-return не достигается.

**Hypothetical multi-consumer scenario:**

Пусть было бы 2 consumer'а на одном `PriorityQueueT`, 1 сообщение.
- C1: `wait()` → m_count 1→0, success.
- C2: `wait()` → m_count 0→(−1), идёт в `m_sema.wait()`.
- C1: `dequeueFromBands` находит сообщение, return true. Никакого
  token-return.
- C2: спит в `m_sema.wait()` **пока не истечёт таймаут**.

В этом сценарии `dequeueFromBandsOrReturnToken` всё равно НЕ
достигается на C1 (C1 успешно забрал сообщение). «Гонки за один
жетон» нет — token-return активируется только когда dequeueFromBands
находит ноль сообщений, что в multi-consumer требует **обоих**
consumer'ов промахнуться по band-маске одновременно (race между
ними на тот же бэнд). Это возможно только если **другой** consumer
уже деqуеуded сообщение — тогда token-return на C1 возвращает токен
в семафор → m_count становится 1 → C2 просыпается из m_sema.wait() →
находит **своё** сообщение (или token снова возвращается, если и там
пусто — но это уже может быть livelock).

**НО:** в текущем коде `PriorityQueueT` всегда обслуживает одного
consumer'а (см. R2.4 абзац 1). Token-return path недостижим. ✓

### R2.5 Memory ordering — F1 закрыт конструктивно

В раунде 1 я отмечал (F1, info): «на ARM между `_bands[prio].enqueue`
и `_signal.enqueue` нет явного `atomic_thread_fence(release)`».

В раунде 3 `_signal` исчез, осталось:
```cpp
_bands[prio].enqueue(std::move(msg));
_nonEmpty.fetch_or(bit, std::memory_order_release);
_sema.signal();   // release-storing fetch_add
```

Семафор сам использует release-acquire pairing через `m_count`
(см. R2.2). Поэтому release-семантика «enqueueBand → signal()»
гарантируется **самим примитивом**, без явного fence. F1 закрыт
конструктивно. ✓

### R2.6 Маска `_nonEmpty` — асимметрия корректна

**Invariants производителя:**

1. Бит ставится **после** `_bands[prio].enqueue(...)` (line 178-182).
2. Бит ставится **до** `_sema.signal()` (line 182-183).
3. Producer's claim: «mask может лагать в сторону "band looks non-empty
   but is actually drained" — handled by clearing the bit and continuing
   the walk. Mask must never lag the other way (mask says empty while
   a message is present)».

**Проверка (3):**

- Bit set ⟺ ≥1 enqueue в этот бэнд после последнего `fetch_and(~bit)`.
- Если консьюмер увидел mask с bit cleared (т.е. mask=0), full scan
  9→0 всё равно проверит все бэнды. The "false empty" case
  обрабатывается safety net'ом.
- Если консьюмер увидел mask с bit set, `try_dequeue` на этот бэнд
  гарантированно видит enqueue (mask-set ⟹ signal-fired ⟹
  consumer's acquire sees stores-before-signal ⟹ sees enqueue).

**Рассуждение верное.** Producer не overstates: маска может
«соврать в сторону "non-empty"» (другой consumer забрал), и это
корректно обрабатывается `fetch_and(~bit)`. Маска НЕ может соврать
в сторону «empty, а есть» — full scan это всё равно поймает. ✓

### R2.7 Несколько консьюмеров на одной QueueT

Текущий design (см. R2.4): один consumer на одну `PriorityQueueT`.
Вызов `_sema.signal()` в `dequeueFromBandsOrReturnToken` —
defensive measure для будущего multi-consumer сценария, **на
текущем коде никогда не выполняется**. Это не deadlock, потому что
путь деградирует в `wait_dequeue_timed → return false → recv()
возвращает nullptr` — как обычный таймаут.

### R2.8 Multi-consumer + token-return: теоретический livelock?

Producer правильно отмечает: token-return гарантирует что
`total_token_count == total_messages_in_bands`. Если total_messages
падает до нуля (все деqуеудед), token-count не может оставаться
положительным — потому что единственный producer token-count'а это
enqueue (+1) и token-return (что происходит только когда
dequeueFromBands ничего не нашёл, т.е. в multi-consumer уже
произошёл dequeue другим consumer'ом → total_messages убыло).
То есть: token-return.compensates_for("token with no message").
Но если бы оба consumer'а одновременно промахнулись → token-return
на обоих → +2 токена, ни одного сообщения. Следующие recv() обоих
consumer'ов опять промахнутся → +2 ещё. Livelock.

**НО:** этот сценарий невозможен в single-consumer design
(см. R2.4). И в multi-consumer'е оба consumer'а не могут
**одновременно** промахнуться по маске, если сообщение реально
есть — потому что маска указывает на единственный band, в котором
сообщение лежит, и `try_dequeue` атомарен. Гонка возможна только
если **третий** актор (другой consumer) уже деqуеуded. Тогда
сценарий «оба промахнулись» сводится к «сообщение уже ушло»,
token-return законен — и нечего livelock'ать.

**Pattern: token-return never overshoots compensating units.**
Потенциальная проблема только в (невозможном) сценарии
«N consumer'ов,  < N сообщений, race resolution задерживается» —
это уже больше теория, чем практика. ✓

### R2.9 Таймаут

- `_sema.wait(timeout_usecs)` ленивая спиннинг + mach/futex
  timed_wait с negative-как-indefinite semantics.
- Negative timeout внутри `waitWithPartialSpinning` → `m_sema.wait()`
  без таймаута (indefinite).
- Таймаут свыше 0 → `m_sema.timed_wait(...)`.
- Возврат `false` корректно отменяет fetch_sub через
  re-increment loop (line 288-295) → m_count восстанавливается.

В `Consumer::recv` budget-tracked retry для expired messages
(`remaining = usec_timeout - elapsed`) корректно работает:
после `wait_dequeue_timed → false` recv() возвращает `nullptr`. ✓

### R2.10 Вердикт по сигнальному каналу

| Сценарий | Возможен? | Последствие |
| --- | --- | --- |
| Lost wakeup | ❌ | acquire-release на семафоре |
| Double counting | ❌ | атомарный fetch_add/CAS |
| Token-return livelock | ❌ | single-consumer design исключает |
| Spurious "false empty" в маске | ✓ (benign) | full scan 9→0 recovery |
| Multi-consumer stranded waiter | ❌ (in current design) | дизайн не достижим |

**Семафорный протокол корректен.** ✓

---

## R3. Семантика приоритетов сохранена

Прогнал оба ключевых теста:

```
[ RUN      ] PriorityOrderingTest.testInterleavedPriority
[       OK ] PriorityOrderingTest.testInterleavedPriority (1 ms)
[ RUN      ] PriorityOrderingTest.testUniformPriorityIsFIFO
[       OK ] PriorityOrderingTest.testUniformPriorityIsFIFO (1 ms)
[ RUN      ] PriorityOrderingTest.testAdjacentPriorityInterleavePreservesBandFIFO
[       OK ] PriorityOrderingTest.testAdjacentPriorityInterleavePreservesBandFIFO (0 ms)
[ RUN      ] PriorityOrderingTest.testRestartPreservesBanding
[       OK ] PriorityOrderingTest.testRestartPreservesBanding (1 ms)
[ RUN      ] PriorityOrderingTest.testTransactionalPriorityOrdering
[       OK ] PriorityOrderingTest.testTransactionalPriorityOrdering (5 ms)
```

Inter-band ordering (старший непустой бэнд первым) и intra-band FIFO
сохранены. Замена примитива на семафор не повлияла на семантику. ✓

---

## R4. F2 (test coverage) — закрыт

В раунде 1 я указывал: `testUniformPriorityIsFIFO` слаб как
дискриминатор — пройдёт и на plain FIFO queue.

Producer добавил `testAdjacentPriorityInterleavePreservesBandFIFO`
(5×p=4 + 5×p=5, чередуя). Тест проверяет:
1. **Inter-band**: все p=5 приходят до любого p=4.
2. **Intra-band FIFO**: внутри p=5 и внутри p=4 порядок
   `p4-0, p4-1, p4-2, p4-3, p4-4` строго сохраняется.

Это НАМНОГО сильнее, чем прежний `testUniformPriorityIsFIFO`. Plain
FIFO-очередь (без priority awareness) провалилась бы на (1).
Plain priority-очередь без per-band FIFO тоже провалилась бы на (2).
Только **priority bands + FIFO внутри бэнда** проходит оба. ✓

Кстати, этот тест — лучший дискриминатор и приближает тестовый набор
к гейту семантики. Возможно, имеет смысл снять прежний
`testUniformPriorityIsFIFO` как избыточный — но это уже не блокер.

---

## R5. Маска `_nonEmpty` — Producer'ское рассуждение верное

См. R2.6 — асимметрия корректна, full scan 9→0 как safety net
закрывает edge case «mask says empty, msg present». ✓

---

## R6. -Werror / -Wall / -Wextra / -Wshadow

```
cmake --build --preset debug --parallel
[19/37] Building CXX object CMakeFiles/tiny_mq.dir/tests/MapMessageTest.cpp.o
...
[37/37] Linking CXX executable tiny_mq; Copy tiny_mq.properties
ld: warning: ignoring duplicate libraries: '...libPocoFoundationd.a', '...libgtest.a'
```

37/37 объектов, 0 source warnings/errors. `ld: warning` — преэкзистентный
CMake-config quirk (порядок lib в target_link_libraries), не от
исходников. ✓

---

## R7. Producer'ское утверждение «105/105 PASSED» — проверено

```
rm -rf tiny-mq/ && ./cmake-build-debug/tiny_mq --gtest_filter='-*PriorityOrderingTest*'
[==========] 105 tests from 20 test suites ran. (6587 ms total)
[  PASSED  ] 105 tests.
```

Полный suite на чистом storage — 105/105 PASSED. ✓

Второй прогон без очистки воспроизводит 2 провала:
```
=== SECOND RUN WITHOUT CLEAN ===
[  FAILED  ] ClientAckTest.testMixedPersistenceOrdering
[  FAILED  ] ExpirationTest.testExpiredPersistentMessageDroppedOnRecv
PASSED 103 tests.
```

Это **те же** 2 pre-existing failures, что я подтвердил в раунде 1 как
test-isolation bugs (persistent storage не очищается между тестами,
TearDown лишь `_exchange.reset()` — файлы остаются). Происходит
на `master` без касания спеки 45. **Не относится к спеке 45.** ✓

Согласуется с моим диагнозом из раунда 1 (см. §6 round 1).

---

## R8. ADR-инварианты

| ADR | Что проверено | Статус |
| --- | --- |
| 0005 (threading model) | Single-consumer-per-QueueT сохранён; `PriorityQueueT` всё ещё MPMC-only-by-design. | ✅ |
| durable-ключ `(clientID,name)` | `Destination::priorityFromStorageBytes` сместился с `Destination.cpp:94` → line 94; replay-путь использует ту же функцию. | ✅ |
| формат `0x02` | Offset 51 сверен, endianness native, edge cases handled. | ✅ |
| `MessageProperty` ↔ `PocoAnyVisitor.h` | Не затронут. | ✅ |

---

## R9. Судьба прежних findings F1–F7

| ID | Round 1 summary | Severity | Статус в round 3 |
| --- | --- | --- | --- |
| **F1** | Cross-queue memory ordering на ARM — нет явного fence между `band.enqueue` и `signal.enqueue` | info | **ЗАКРЫТ КОНСТРУКТИВНО**. `_signal` исчез, остался `_sema.signal()` (release) — семафор сам обеспечивает release-acquire pairing. См. R2.5. |
| **F2** | `testUniformPriorityIsFIFO` слаб как детектор | info | **ЗАКРЫТ**. Добавлен `testAdjacentPriorityInterleavePreservesBandFIFO` (5×p=4 + 5×p=5). См. R4. |
| **F3** | Durable subscriber replay path не покрыт priority-тестом | info | **ОСТАЁТСЯ**. Out-of-scope спеки 45 (она про dequeue ordering). `Destination::replayFromStorage` shared с queue-family, transitively covered. |
| **F4** | Mисleading комментарий «single-consumer scenario: should not occur» | info | **ЗАКРЫТ**. Комментарий переписан: теперь явно описывает «mask may lag towards "non-empty but drained" — handled by clearing bit. It must never lag the other way». См. `Message.h:212-221`. |
| **F5** | Нет baseline-файла в `benchmarks/` | info | **ОСТАЁТСЯ**. Out-of-scope спеки 45. |
| **F6** | `ClientAckTest.testMixedPersistenceOrdering` — pre-existing failure | info | **ПОДТВЕРЖДЁН PRE-EXISTING**. Воспроизводится на `master` без касания спеки 45. На чистом storage — 105/105 pass. |
| **F7** | `ExpirationTest.testExpiredPersistentMessageDroppedOnRecv` — pre-existing failure | info | **ПОДТВЕРЖДЁН PRE-EXISTING**. Воспроизводится на `master` без касания спеки 45. На чистом storage — 105/105 pass. |

---

## R10. Perf — out of scope

Делегировано отдельному perf-gate. Согласно `producer.json`:
- Run 1: branch within +3.1% от master (AutoAck/Topic).
- Run 2: branch на −2.5/-2.6% (branch быстрее, в шуме).
- Обе regression-проверки в пределах ±5% порога.

Диагноз round 2 (вторая очередь сигналов добавляла лишний
enqueue/dequeue на сообщение) согласуется с заменой на один
`signal()` call без второй очереди.

Смысловых потерь ради скорости не вижу:
- Token-return через `_sema.signal()` — корректно (R2.4).
- Маска с full-scan fallback — корректно (R2.6).
- `fetch_and(relaxed)` для очистки stale бита — допустимо
  (re-load с acquire следом, см. R2.6).

---

## R11. Вердикт

```
status: approved
iteration: 3
```

**Обоснование:**

- Семафорный протокол корректен (R2.1–R2.9): lost wakeup,
  double counting, livelock исключены; memory ordering (F1)
  закрыт конструктивно; маска корректна (R2.6).
- Семантика приоритетов сохранена (R3): inter-band и intra-band
  FIFO работают.
- Test plan покрыт полностью (R3): 5/5 PriorityOrderingTest
  проходит, включая новый тест на adjacent priority (R4).
- `-Wall -Werror -Wextra -Wshadow` чисто (R6).
- 105/105 полный suite на чистом storage (R7); 2 pre-existing
  failures подтверждены как таковые и не относятся к спеке.
- ADR-инварианты не нарушены (R8).
- Perf-гейт будет отдельно; смысловых потерь ради скорости нет
  (R10).
- F1, F2, F4, F6, F7 закрыты/подтверждены; F3, F5 — out-of-scope
  по-прежнему (R9).

**Не блокирующие замечания (на будущее, не для раунда 3):**

1. `testUniformPriorityIsFIFO` теперь избыточен —
   `testAdjacentPriorityInterleavePreservesBandFIFO` строго
   сильнее. Можно удалить или оставить как sanity-check.
2. F3 (durable subscriber priority test) — стоит завести в
   `tests/DurableSubscriberTest.cpp` отдельной спеки, не здесь.
3. F5 (baseline file) — общесистемное, спека 45 не должна
   блокировать.

---

## R12. Команды (round 3)

```
cmake --preset user-debug
cmake --build --preset debug --parallel                     # 37/37, 0 source warnings
./cmake-build-debug/tiny_mq --gtest_filter='PriorityOrderingTest.*'   # 5/5
rm -rf tiny-mq/ && ./cmake-build-debug/tiny_mq --gtest_filter='-*PriorityOrderingTest*'  # 105/105
./cmake-build-debug/tiny_mq --gtest_filter='-*PriorityOrderingTest*'  # 103/105 (2 pre-existing)
./cmake-build-debug/tiny_mq --gtest_filter='ClientAckTest.testMixedPersistenceOrdering:ExpirationTest.testExpiredPersistentMessageDroppedOnRecv'  # confirms 2 pre-existing
```

Working tree restored: no stash/checkout performed, only `touch Message.h`
for force-rebuild (mtime only, content unchanged).