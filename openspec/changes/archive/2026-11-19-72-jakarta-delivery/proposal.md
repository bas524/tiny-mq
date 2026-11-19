# 72 Jakarta delivery

Класс K4. Исторический proposal дельты SDD72 v0.5: REQ-72-1…8.

Архивная пометка 19.11.2026: изменение принято в
[PR №12](https://github.com/bas524/tiny-mq/pull/12), факт перенесён в
[openspec/specs](../../../../openspec/specs). ADR-0010 теперь accepted,
а M-05 остаётся открытой. Указания ниже о proposed/R1 и ещё не выполненном
archive описывают состояние подготовки дельты, а не текущее состояние проекта.
Основание — функциональный source/tests `2b3c536`, Reviewer и шлюзы iteration 4.
Conformance подтверждает восемь функциональных требований, Security approved.
NFR допущен по `approval_basis=owner-approved-deferral`; технический
`performance_verdict=rejected`. REQ-72-9 не входит в требования этой дельты.

Изменение закрепляет время send, lifecycle, callbacks, ack, порядок и атомарные
локальные транзакции. Используются существующие connection, jakarta-message-contract,
jakarta-destinations и message-redelivery. Единственный новый capability —
jakarta-session-delivery: сессия владеет сериализацией callbacks, порядком отправок
и решением consume/send, отдельной областью существующей объектной модели.

REUSE > EXTEND: время заголовков расширяет message contract; lifecycle — connection;
состав подписок при commit уточнён через MODIFIED REQ-71-2 с сохранением ID.
Два legacy-требования message-redelivery без ID расширены до REQ-72-5 и REQ-72-7;
семь поглощённых требований удаляются точными ключами, актуальные сценарии перенесены.
Прежние правила default=6, сброса состояния при рестарте, подавления backoff при close
и подмены JMSDeliveryTime отменяются. Действующие ID не меняются.

Основания: [SDD72](../../../../docs/jms-spec/72-jakarta-delivery.md), зависимости SDD70/71,
четыре openspec/specs и [ADR-0010](../../../../arch/0010-atomic-local-delivery.md)
(статус proposed сохраняется). Приёмка:
[review](../../../../docs/reviews/72-jakarta-delivery.review.md),
[gates](../../../../docs/reviews/72-jakarta-delivery.gates.md),
[NFR](../../../../docs/reviews/72-jakarta-delivery.nfr.md).
Формальные пакеты: `handoffs/current/72/{reviewer,nfr,conformance,security}.json`.

Новых продуктовых KB-доменных сущностей нет: `product_delta=null`.
Сеть, Java mapping/API, XA, EE и кластер остаются будущими SDD73–78.
Archive остаётся рубежом человека; эта дельта не изменяет openspec/specs.

## Исключение Owner

[Performance 44475262358](https://github.com/bas524/tiny-mq/actions/runs/44475262358)
завершился failure: AutoAck_NonPersistent_RoundTrip, CPU median 106,6 → 113,6 ns/op,
+6,59%, CV 0,57% / 1,73%. Это устойчивое превышение бюджета 5%.
[Обычный CI 44475262476](https://github.com/bas524/tiny-mq/actions/runs/44475262476)
зелёный. Решение Owner: «Влить по обычному CI, perf — отдельная задача».
Для этого изменения разрешено исключение из блокирующего perf-гейта при слиянии
по обычному CI. Измерение остаётся отрицательным; бюджет не подтверждён.
GitHub Performance сейчас отключён, локальный ночной job ещё не настроен.
Технический результат и основание допуска сохранены раздельно в NFR-пакете.

## Отложенный долг: бюджет горячего пути доставки

ID: `REQ-72-9`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#сквозные-требования-на-всех-майлстонах`
Статус долга: открыта; результат измерения — failure; выполненным требованием spec-факт не является.
Задача: `M-05-local-nightly-performance`.
Подготовленный документ: [M-05](../../../../tasks/M-05-local-nightly-performance.md).
Путь при финализации: `tasks/M-05-local-nightly-performance.md`; статус останется «открыта».
Test: `NfrGateTest.DeliveryHotPathBudget`; runner `tests/nfr72.py` (исходный критерий долга).

Исходный бюджет SDD72 для изменённых локальных C++ ack, storage и delivery paths —
не более +5% медианного CPU time/op
при сравнении RelWithDebInfo сборок main и merge-кандидата на одном GitHub раннере.
Job Broker hot paths сохраняет действующие три сравнения SDD 71 и охватывает
ClientAck_Persistent_RoundTrip, ClientAck_Batch_NonPersistent,
Transacted_NonPersistent_RoundTrip, Transacted_Persistent_RoundTrip,
Transacted_Batch_NonPersistent и Transacted_Batch_Persistent. Подбираются одинаковые
benchmark registrations и единицы для базы и кандидата, включая CPU storage workers
для путей с persistent I/O. ABBA использует девять повторов на вариант в двух заходах.
Максимальный допустимый CPU CV — 5%. Серия выше порога повторяется один раз и затем
даёт отрицательный результат, если нестабильность сохраняется. Отдельно публикуются
real time/op и профили новых путей listener, async, journal replay и DLQ, если старый
бинарник не имеет сопоставимого benchmark. Они не подменяют проверки регрессии
существующих путей. Методика соответствует nfr-gate и benchmarks/baseline.md.

Это сохранённый целевой бюджет и условия проверки долга, а не подтверждённая
характеристика текущей реализации. Для M-05 предусмотрены локальные ночные
сравнения; исходная методика SDD72 и отрицательный GitHub результат сохранены.
Настройка расписания сама по себе долг не закрывает. M-05 должна получить валидные
сравнительные данные и выполнение бюджета; стадия DocWriter её не создаёт и не закрывает.
