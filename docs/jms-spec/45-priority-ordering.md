# Priority ordering on dequeue

## JMS reference
- JMS 2.0 § 3.4.10 `JMSPriority` — providers should deliver higher-priority messages ahead of lower-priority ones, but strict priority order is not required.

## Current state in tiny-mq
- `QueueT = moodycamel::BlockingConcurrentQueue<Message::Ptr>` — strict FIFO, no priority awareness.

## Proposed API
- No public surface change; internal queue structure changes.

## Semantics
- Partition the queue into **10 priority bands** (0..9). A consumer polls bands 9→0, taking the first non-empty band. Within a band, FIFO.
- Alternative: single priority-queue with heap ordering. Discarded because moodycamel's MPMC lock-freedom is hard to keep under a heap.
- Must preserve ordering guarantees for the common case of uniform priority (= FIFO).

> **Реализовано иначе, чем описано выше — buyer beware.** Прямолинейная реализация
> «10 бэндов + скан 9→0» **не проходит перф-требование проекта**: она дала −13% на
> `AutoAck_NonPersistent` (горячий путь delivery), потому что наивный вариант делает
> **две** операции с очередями на сообщение — enqueue в бэнд плюс enqueue в отдельную
> сигнальную `BlockingConcurrentQueue`, — тогда как прежняя единственная очередь делала
> одну. Попытка вылечить это ускорением выбора бэнда (битовая маска непустых бэндов)
> регрессию **не сняла** — стало −19…−23%, потому что лечила не причину.
>
> Итоговая реализация (`PriorityQueueT` в `Message.h`): бэнды + `std::atomic<uint16_t>`
> маска непустых бэндов + **`moodycamel::LightweightSemaphore` вместо сигнальной
> очереди** (тот же примитив, который `BlockingConcurrentQueue` использует внутри себя).
> Перф относительно master: −2.2% / +2.5%, в пределах шума.
>
> Корректность опирается на инвариант **«на одной `PriorityQueueT` ровно один
> consumer»** (ADR-0005). Спека 26 (shared consumers) этот инвариант нарушает и обязана
> перепроверить семафорный протокол — разбор в `docs/reviews/45-priority-ordering.review.md` §R2.
>
> Подробности: `docs/features/45-priority-ordering.md`, перф-отчёт
> `docs/reviews/45-priority-ordering.perf.md`.

## Persistence / wire implications
- Storage already persists the message; priority is just a header. Replay path must restore messages into the correct band on restart.

## Dependencies
- 10 (headers).

## Test plan
- `PriorityOrderingTest`: interleaved send of p=0 and p=9; receiver sees all p=9 before p=0.
- `PriorityOrderingTest`: uniform-priority workload remains FIFO.
- `PriorityOrderingTest`: restart preserves priority banding.

## Open questions
- ~~Does multi-band polling add unacceptable latency to the default (uniform) case? Benchmark before merging.~~
  **Закрыт (2026-07-31).** Нет — при условии, что сигнальный канал не является второй
  очередью. Цена самого многобэндового поллинга в uniform-случае ≈ **6 нс на сообщение
  (~4%)**; изменение целиком относительно master перф-нейтрально. Измерено интерливированными
  прогонами master-vs-ветка в отдельном worktree, 5 пар, парный t-критерий
  (t = −0.51 и 1.15 при n=5). См. `docs/reviews/45-priority-ordering.perf.md`.
