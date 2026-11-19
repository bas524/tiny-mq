# Шлюзы SDD 72

Действующий Conformance: iteration 4, переход 37, REQ-72-1…8 approved; REQ-72-9 отложен Owner.
Предшествующие разделы относятся к указанным в них историческим деревьям.

## Conformance iteration 6

**Rejected.** SDD 72 v0.4, локальное ядро C++, профиль аварийной остановки
процесса в том же каталоге данных. Самостоятельный прогон Debug на macOS arm64
дал 8/8 для `DeliveryContractTest`; `trace sdd/input/tests` завершились с кодом 0.
Дополнительный прогон через публичный API подтвердил нарушение REQ-72-2.
Отпечатки до и после совпали с входом итерации и NFR6
`tree:d958854b4a907c0a22bfd03469e7fa407b511f3de8e165448781786775306515`.
[Матрицы](../../handoffs/current/72/logs/conformance-r6-matrices.log) ·
[Трассировка, воспроизведение и отпечатки](../../handoffs/current/72/logs/conformance-r6-checks.log)

### Основания вердикта

1. **REQ-72-2.** В двух AUTO-сессиях на общей очереди предикат
   `receiveChecked` удерживал блокировку готовых сообщений. Во второй сессии
   `receiveNoWait()` ждал освобождения предиката 155 мс и вернул пустой результат.
   Контракт требует немедленного пустого результата. `receiveNoWait` использует
   `recv(0)`, а при неудачном `try_lock` тот выполняет блокирующий `lock`.
   Проверка конкурентного входа в матрице использует один consumer и ожидает
   `IllegalStateException`. [Прогон](../../handoffs/current/72/logs/conformance-r6-checks.log#L79) ·
   [Consumer.cpp](../../server/src/Consumer.cpp#L32) ·
   [Destination.h](../../server/src/Destination.h#L79) ·
   [DeliveryContractTest.cpp](../../tests/DeliveryContractTest.cpp#L293)
2. **Стандарт 5.** Публичные добавления `Consumer`, `Producer` и `Session`
   совпадают с E5, E8–E9, E11 и E17–E28. E2 реализован как вложенный
   `LocalTransactionJournal::DeliveryAttempt`. В реестре отсутствуют новое
   поле дискового заголовка `linear_storage::Header::attempt` и маркер тома
   `<номер>.delivery-72`. ADR описывает оба элемента, от которых зависят чтение
   старых томов и откат. [Entity inventory](../jms-spec/72-jakarta-delivery.md#entity-inventory) ·
   [LinearStorage.h](../../server/src/LinearStorage.h#L25) ·
   [ADR-0010](../../arch/0010-atomic-local-delivery.md#L61)
3. **REQ-72-3–5.** `ListenerModeMatrix` покрывает исключение с политикой по
   умолчанию, а повтор слушателя с явно заданной политикой отсутствует.
   `AsyncSendBarriers` передаёт явные `SendOptions` только в негативном тесте
   с priority=-1. Успешный вызов этой перегрузки и его снимок заголовков
   не проверяются. `AcknowledgeAndRecoverMatrix` выполняет recover/rollback
   без аварийной остановки. Отдельный дочерний сценарий сбоя вокруг ack
   использует AUTO, оставляя DUPS_OK, CLIENT и TRANSACTED без требуемых проверок
   сбоя между выдачей и подтверждением для этих режимов.
   [Listener](../../tests/DeliveryContractTest.cpp#L334) ·
   [Async send](../../tests/DeliveryContractTest.cpp#L454) ·
   [Recover](../../tests/DeliveryContractTest.cpp#L575) ·
   [Ack crash](../../tests/DeliveryContractTest.cpp#L895)

### Карта прогона

| Требование | Критерий | Установлено |
|---|---|---|
| REQ-72-1 | `DeliveryContractTest.SendClockMatrix` | PASS. Persistent и non-persistent × send/commit, границы TTL/delay, насыщение, открытие 0x02/0x03 и новой записи. |
| REQ-72-2 | `DeliveryContractTest.ReceiveLifecycle` | PASS. Start/stop, тайм-ауты, close, старый recv. Дополнительный прогон выявил блокировку receiveNoWait. |
| REQ-72-3 | `DeliveryContractTest.ListenerModeMatrix` | PASS. Четыре режима, два consumer, stop/close и исключения callback. Пробел в настроенном повторе. |
| REQ-72-4 | `DeliveryContractTest.AsyncSendBarriers` | PASS. Sync/async, ошибка до и после принятия, поток и порядок callbacks, close/commit/rollback. Пробел в успешной отправке с явными опциями. |
| REQ-72-5 | `DeliveryContractTest.AcknowledgeAndRecoverMatrix` | PASS. Четыре режима × обе надёжности, несовместимое тело, порядок двух consumer, изоляция ack, TTL, приоритет и счётчик. Пробел в аварийных ветвях. |
| REQ-72-6 | `DeliveryContractTest.AtomicConsumeAndSend` | PASS. Persistent, non-persistent и смешанный commit/rollback, видимость для claim/browser, сбой до решения и replay после него, durable fan-out и повторное открытие после ack. |
| REQ-72-7 | `DeliveryContractTest.CrashAndDeadLetterMatrix` | PASS. Persistent очередь/durable до и после ack, сохранённый backoff, DLQ до и после решения и повторный replay, TTL legacy, живые повторы обеих надёжностей. |
| REQ-72-8 | `DeliveryContractTest.SessionSendOrder` | PASS. Два producer и обе надёжности × send/commit, sync/async, priority, delay, селектор и независимые durable-копии. |
| REQ-72-9 | `NfrGateTest.DeliveryHotPathBudget` | Трассировка runner прошла. Отдельный шлюз NFR6 approved для этого дерева, Linux 44271216781 / CI 44271216836 success. Повторный замер в этой стадии не выполнялся. |

Основание семантики — закреплённый Jakarta Messaging 3.1 §§3.4, 3.5.11,
6.1, 6.2, 7.3, 7.7–7.9, 8.4–8.8 и 9.1 и Javadoc 3.1 для Connection, Session,
MessageConsumer, MessageProducer, CompletionListener и Message. Для состава
подписок SDD 71/72 и ADR-0010 закрепляют выбор при commit в локальном C++ ядре.
В §7.9 состав привязан ко времени отправки. Этот стык важен при отображении
ядра на Java. Сеть, Java SE, EE, XA и HA относятся к последующим SDD.
[Закреплённые источники](../../handoffs/current/72/inputs/) ·
[INT-01](../../requirements/intents/INT-01-jakarta-messaging.md) ·
[SDD 71](../jms-spec/71-jakarta-destinations.md#architecture) ·
[NFR6](72-jakarta-delivery.nfr.md)

## Security iteration 2

**Rejected.** DeepSeek V4 Pro, независимое воспроизведение на собственном scratch.
Журнал локальной транзакции `delivery-72/decision` открывается без `O_NOFOLLOW`
и следует симлинку как на файл, так и на каталог, из-за чего перезапуск брокера
пишет байт состояния `0x00` в произвольный файл за пределами корня данных и,
при размере цели более 1 МиБ, усекает его до одного байта. Sidecar `attempt-72`
имеет тот же класс дефекта на уровне каталога. Это нарушает
[ADR-0010](../../arch/0010-atomic-local-delivery.md) «Журнал под каталогом данных
Exchange» и «Контракт отказа этой SDD — остановка процесса в том же каталоге
данных», а также REQ-72-6 (журнал под каталогом данных Exchange). Системное
решение оформляется ADR, код не правился.

Воспроизведение (все пути внутри созданного scratch, сеть и Docker не использовались):

```
TINY_MQ_TEST_CRASH=commit:after-decision ./cmake-build-debug/tiny_mq --delivery-test-child <scratch>/broker/ tx-crash
# exit 72; <scratch>/broker/delivery-72/decision имеет байт состояния 0x01 и кадр TMQ72W01
mv <scratch>/broker/delivery-72/decision <scratch>/outside/decision
ln -s <scratch>/outside/decision <scratch>/broker/delivery-72/decision
./cmake-build-debug/tiny_mq --delivery-test-child <scratch>/broker/ tx-check-commit
# exit 0; первый байт <scratch>/outside/decision: 0x01 -> 0x00
```

Механизм. `LocalTransactionJournal::openDecision()` открывает
`delivery-72/decision` вызовом `::open(path, O_RDWR|O_CREAT|O_CLOEXEC, 0600)`
([LocalTransactionJournal.cpp](../../server/src/LocalTransactionJournal.cpp#L165)),
`recover()` читает кадр через `std::ifstream` и `fs::file_size`
([LocalTransactionJournal.cpp](../../server/src/LocalTransactionJournal.cpp#L220)),
а `retireDecision()` пишет байт 0 и при крупном кадре вызывает `ftruncate`
([LocalTransactionJournal.cpp](../../server/src/LocalTransactionJournal.cpp#L197)).
Ни открытие, ни чтение, ни запись не проверяют, что `decision` — обычный файл
внутри корня данных. Дескриптор разрешает симлинк, поэтому писать можно в любой
доступный процессу файл, на который указывает ссылка.

Границы дефекта, проверенные независимо:

- `decision` — симлинк на файл: подтверждён write-escape, первый байт внешнего
  файла `0x01` → `0x00` после `tx-check-commit` (exit 0), размер 821 байта не
  изменился (порог усечения 1 МиБ не достигнут).
- `delivery-72` — симлинк на каталог: подтверждён тот же write-escape, первый
  байт внешнего `decision` `0x01` → `0x00`.
- `attempt-72` — симлинк на каталог: подтверждён delete-escape. `removeAttempt()`
  ([LocalTransactionJournal.cpp](../../server/src/LocalTransactionJournal.cpp#L405))
  вызывает `fs::remove` по пути, который проходит через каталог-симлинк: после
  `retry-check` внешний файл попытки удалён (число файлов 1 → 0). Тот же путь
  `saveAttempt()` использует для записи кадра попытки.
- `attempt-72/<uuid>` — симлинк на файл: escape не подтверждён. Запись идёт
  атомарным `fs::rename` соседнего `.tmp`
  ([LocalTransactionJournal.cpp](../../server/src/LocalTransactionJournal.cpp#L387)),
  а удаление `fs::remove` действует на сам линк, поэтому внешний файл попытки
  (101 байт, `TMQ72A01`) после `retry-check` не изменился.

Затронутые критерии: REQ-72-6 (атомарная локальная транзакция, журнал в каталоге
данных Exchange), ADR-0010 (журнал под каталогом данных, контракт отказа в том же
каталоге). В терминах роли — деструктивная операция и граница хранилища без
проверки принадлежности пути. [Журнал воспроизведения](../../handoffs/current/72/logs/security-journal-symlink.log) ·
[Отпечаток до](../../handoffs/current/72/logs/security-fingerprint-before.log) ·
[Отпечаток после](../../handoffs/current/72/logs/security-fingerprint-after.log)

Отпечатки совпали: `tree:371d1b332f7c5901f17975a97b3f6d533d368f4032c80d65ea420780d460967b`.

## Conformance iteration 4 — 2026-11-19, переход 37

**Approved только в функциональной области REQ-72-1…8, SDD 72 v0.5.**
Непокрытых функциональных утверждений не осталось. **REQ-72-9 не подтверждён и отложен
по прямому решению Owner**, `approval_basis=owner-approved-performance-deferral`.
Технический `performance_verdict=rejected` сохраняется: NFR i4 сообщает устойчивые
+6,59% CPU AutoAck_NonPersistent_RoundTrip при бюджете +5%; остальные 14 путей в бюджете.
Это результат предыдущего независимого NFR-шлюза, а не мой повторный замер.
NFR-пакет принят маршрутизатором 19.11 в 07:41:37Z с `approval_basis=owner-approved-deferral`.
Owner в 07:15:21Z разрешил «Влить по обычному CI, perf — отдельная задача»;
решение не делает девятый критерий выполненным. Задача M-05 остаётся открытой,
её перенос в `tasks/` выполняется при закрытии, не этой стадией.
[NFR-пакет](../../handoffs/current/72/nfr.json) ·
[Прямое решение и журнал перехода](../../handoffs/current/72/chain.log) ·
[M-05](../../handoffs/current/72/logs/M-05-local-nightly-performance.md)

Формальная стадия возобновлена в **той же независимой OpenAI-сессии**, которая выполнила
предварительный прогон. Свежий собственный Debug-прогон `DeliveryContractTest.*` из нового
временного cwd: **8/8 passed**, 3207 ms, exit 0. Все три свежих `trace sdd/input/tests`
завершились exit 0; tests использует `--id-pattern 'REQ-[0-9]{2}-[0-9]+'`.
Предыдущие **109/109** смежных тестов, 14 suites, exit 0, выполнены мной в этой же сессии;
SHA-256 бинарника, SDD, Dependencies, INT-01, ADR и закреплённых Jakarta-входов не изменились.
Полные команды, среда, временные cwd, вывод и child exit codes находятся в моих журналах.
Бинарник Debug: AppleClang 21.0.0.21000401, macOS 27.2.1 arm64,
SHA-256 `9690151de17fc295fabefc7ca210c44935778eca838f3bc6212a46df8375c95c`.
[Формальные матрицы](../../handoffs/current/72/logs/conformance-formal-delivery.log) ·
[Мои смежные тесты](../../handoffs/current/72/logs/conformance-preflight-neighbors.log) ·
[Неизменность входов и граница NFR](../../handoffs/current/72/logs/conformance-formal-inputs.log)

Свежие отпечатки до и сразу после последнего прогона совпали с обоими предварительными
и NFR i4: `tree:30773ebf309bde6232825bb32089394ddaf73ed33bb45d24e372a40d12597d76`.
Продуктовые исходники и тесты сохраняют функциональную редакцию `5a934ff`; прежнее
побайтовое сравнение 118 functional files с `5a934ff` и `2b3c536` и свежая неизменность
входов позволяют повторно использовать собственное смежное evidence.
Отдельное GitHub-only продолжение CI cache M-02 (`4e85e33`) не включается в этот
продуктовый вердикт и не объявляется проверенным мной.
[До](../../handoffs/current/72/logs/conformance-formal-fingerprint-before.log) ·
[После](../../handoffs/current/72/logs/conformance-formal-fingerprint-after.log) ·
[Исходный аудит](../../handoffs/current/72/logs/conformance-preflight-audit.log)

### Карта конечного Test plan

| Требование | Критерий и мой результат | Что подтвердили утверждения |
|---|---|---|
| REQ-72-1 | `DeliveryContractTest.SendClockMatrix`, PASS | Persistent/nonpersistent × send/commit; send clock для timestamp/expiration/deliveryTime, commit до/после delay, точные TTL/delay границы, нулевые значения, насыщение, replay 0x02/0x03 и новой записи, legacy padding без метки. Соседи `DeliveryDelayTest.*`, `ExpirationTest.*`, `PriorityOrderingTest.*`. Jakarta §3.4.4, §3.4.9–13, §7.6–9. |
| REQ-72-2 | `DeliveryContractTest.ReceiveLifecycle`, PASS | Stopped/start/stop/close, steady timeout, receive()/receive(0)/receiveNoWait и старый recv; закрытие блокированного и готового receive, закрытые объекты, запрет sync при listener, body reservation без блокирования независимого receiveNoWait/timeout, конкурентное close и пробуждение после исключения. Jakarta §6.1.4–5, §6.1.8, §6.2.5, §8.4–8.8; Javadoc MessageConsumer. |
| REQ-72-3 | `DeliveryContractTest.ListenerModeMatrix`, PASS | Четыре режима, два consumer без перекрытия callbacks; stop/external close ждут callback, consumer self-close, запреты self-stop/self-close session/connection. AUTO/DUPS_OK retry с настроенным backoff для обеих надёжностей (+49/+50, true/2). CLIENT/TRANSACTED для обеих надёжностей: failed input удерживается, следующая доставка разрешена, явный recover/rollback возвращает failed input с true/2 до acknowledge/commit. Jakarta §6.2.13, §8.7–8.8. |
| REQ-72-4 | `DeliveryContractTest.AsyncSendBarriers`, PASS | Persistent/nonpersistent × обычная/транзакционная отправка, два producer, sync/async; ранняя ошибка без callback и поздняя с error, поток и session serialization, порядок и единственный исход. Явные SendOptions перекрывают defaults, snapshot сохраняется после мутации исходника. Callback reentry запрещён, close producer/session/connection и commit/rollback ждут callback; rollback не публикует выход. Jakarta §7.3.1–9 и Javadoc MessageProducer/CompletionListener. C++ snapshot не меняет Java-ограничение доступа к исходному Message до callback. |
| REQ-72-5 | `DeliveryContractTest.AcknowledgeAndRecoverMatrix`, PASS | Несовместимое тело в четырёх режимах × две надёжности; совместимое/пустое receiveChecked, recover/rollback через двух consumer, TTL/приоритет и относительный порядок, session ack после consumer close, изоляция чужой сессии, count 1/2 и насыщение. CLIENT/DUPS_OK/TRANSACTED crash после claim: exit 72, новый процесс получает только неподтверждённую persistent запись true/2, следующий restart пуст после ack. Соседи `ClientAckTest.*`, `DupsOkAckTest.*`, `RecoverTest.*`. Jakarta §3.4.7, §3.5.11, §6.2.10–11, §8.6, §9.1. |
| REQ-72-6 | `DeliveryContractTest.AtomicConsumeAndSend`, PASS | Commit/rollback/активный session.close для persistent, nonpersistent и mixed; закрытие producer/consumer сохраняет транзакцию. Queue и durable выходы исчезают после close, вход возвращается. Подписки и selectors выбираются именно на commit. Claim/browser/fan-out невидимы при подготовке; pre-decision error, crash до/после решения и apply, повторный crash replay, restart после ack без дублей. Соседи `TransactionTest.*`, `PersistentTransactionTest.*`, `DestinationContractTest.SubscriptionFanout`. Jakarta §6.2.7, §6.2.15, §9.1; ADR-0009/0010. |
| REQ-72-7 | `DeliveryContractTest.CrashAndDeadLetterMatrix`, PASS | Queue/durable до/после ack, durable restart одновременно redelivered=true/count=2, повторный restart без воскрешения. Сохранённый backoff и policy snapshot, неизменный public deliveryTime; queue/durable DLQ до/после решения и в replay, ровно одна копия, независимый sibling count; TTL legacy, unlimited обеих надёжностей и explicit drop. `DlqTest.*` включает `SessionCloseRequeuesWithBackoff` и `DeadLetteringOnSessionCloseRollback`. Jakarta §3.4.7, §3.5.11, §6.2.11, §7.7, §9.1 и расширение tiny-mq. |
| REQ-72-8 | `DeliveryContractTest.SessionSendOrder`, PASS | Два producer одной сессии, обе надёжности × send/commit, sync/async FIFO; разрешённые priority/delay исключения, selector сохраняет unmatched, независимые durable copies сохраняют подходящий порядок. Соседи `PriorityOrderingTest.*`, `TopicTest.*`, `SelectorTest.*`. Jakarta §6.2.9, §7.3.3. |
| REQ-72-9 | `NfrGateTest.DeliveryHotPathBudget`, **DEFERRED, не выполнено** | Runner существует и трассируется; NFR i4 технически rejected (+6,59% AutoAck). Мой функциональный прогон не является измерением CPU. Owner разрешил отложить требование в открытую M-05; `deferred_requirement_ids=[REQ-72-9]`. |

### Закрытие прежних четырёх пробелов

- **REQ-72-3:** `tests/DeliveryContractTest.cpp:550` — CLIENT/TRANSACTED для persistent/nonpersistent;
  перед recover/rollback failed input недоступен, после — проверяются его исходное тело,
  надёжность, redelivered=true и count=2; затем принимаются следующие входы и делается ack/commit.
- **REQ-72-6, close:** `tests/DeliveryContractTest.cpp:1113` — transaction.close после stage;
  `check(false)` требует ровно один вернувшийся вход и отсутствие queue/durable выходов во всех трёх
  сочетаниях надёжности. Отдельная политика close подтверждена моими двумя указанными Dlq-тестами.
- **REQ-72-6, commit snapshot:** `tests/DeliveryContractTest.cpp:1099` — после send до commit
  создаются durable `copies=2` и `copies=3`; первая получает ровно одну копию после commit,
  вторая и matching-подписка, созданная после commit, остаются пустыми.
- **REQ-72-7:** `tests/DeliveryContractTest.cpp:1334` — `ack-durable-retry` в новом процессе
  требует одновременно count=2 и redelivered=true; родитель проверяет exit 0, следующий
  `ack-empty` — отсутствие копии после ack. Все эти утверждения выполнены в свежих 8/8.

### Продукции, inventory и нормативная граница

Мои 109 смежных тестов полностью включают `ClientAckTest`, `DupsOkAckTest`, `RecoverTest`,
`TransactionTest`, `PersistentTransactionTest`, `DeliveryDelayTest`, `DlqTest`,
`PriorityOrderingTest`, `ExpirationTest`, `TopicTest`, `DurableSubscriberTest`,
`SelectorTest`, `DestinationContractTest`, `MessageContractTest`.
Грамматика и полные TRUE/FALSE/UNKNOWN AND/OR/NOT таблицы —
`MessageContractTest.SelectorGrammarMatrix` и `SelectorTest.*`;
четыре стандартных ack-режима — delivery-матрицы, индивидуальный ack — C++-расширение в соседних
`RecoverTest`/`DlqTest`. Все четыре вида подписок, offline durable и независимые copies —
`DestinationContractTest.SubscriptionFanout`, `DurableIdentityMatrix`, `NoLocalMatrix`,
`DurableSubscriberTest.*`. Sync/listener и sync/async-send проверены непосредственно.
Java API mapping/SE, Jakarta EE, XA, сеть и HA относятся к SDD 73–78; здесь N/A,
совместимость Java/TCK не заявлена. Новых требований в SDD не добавлено.

Inventory E1–E30 повторно соотнесён с ранее проверенным неизменным кодом:
clock/dispatch/journal, вложенный DeliveryAttempt, nextRedeliveryAt, SendCompletion,
ReceiveBodyResult/Disposition, API lifecycle/async/ack, session send order, durable keys/decision,
inline attempt и volume marker E29/E30. Незаявленных самостоятельных доменных сущностей
не обнаружено. Legacy/новые данные и повторный replay входят в собственный Test plan;
гарантия ограничена остановкой процесса в том же каталоге по ADR-0010.

Сохранённое уникальное свидетельство прежней iteration 2 (история, не мой новый прогон):

E29/E30 совпадают с абзацем о заголовке тома ADR-0010. После `ack:before-ack` в помеченном томе очереди байт `attempt` по смещению 25 стал 1 при `deleted=0`. У durable-копии, которую ещё не получали, `attempt=0`. Два последующих запуска подтвердили восстановление и отсутствие повтора после ack. `SendClockMatrix` прочитала legacy-том со значением 42 в прежнем выравнивании и без метки, счётчик остался 1. [ADR-0010](../../arch/0010-atomic-local-delivery.md#решение), [журнал чтения тома](../../handoffs/current/72/logs/conformance-i2-20261118T114546Z-volume.log), [legacy fixture](../../tests/DeliveryContractTest.cpp#L94).

Нормативная сверка этой сессии использует полностью прочитанные SDD 72/70/71, INT-01,
ADR-0005/0006/0008/0009/0010 и закреплённый [Jakarta Messaging 3.1](../../handoffs/current/72/inputs/jakarta-3.1-delivery.txt),
§§3.4, 3.5.11, 6.1, 6.2, 7.3, 7.7–7.9, 8.4–8.8, 9.1,
Javadoc [MessageConsumer](../../handoffs/current/72/inputs/javadoc-MessageConsumer.html),
[MessageProducer](../../handoffs/current/72/inputs/javadoc-MessageProducer.html),
[CompletionListener](../../handoffs/current/72/inputs/javadoc-CompletionListener.html),
[Connection](../../handoffs/current/72/inputs/javadoc-Connection.html),
[Session](../../handoffs/current/72/inputs/javadoc-Session.html) и
[Message](../../handoffs/current/72/inputs/javadoc-Message.html).
Выбор подписок именно на commit проверен как локальный C++ контракт SDD 71/72 и ADR-0009;
формулировка Jakarta §7.9 про время send остаётся стыком будущего Java mapping, а не новым
требованием этой стадии. Исторические delay-from-commit, запрет transacted async и прежние
retry/backoff/default правила заменены только в заявленных границах SDD 72.

[Trace SDD](../../handoffs/current/72/logs/conformance-formal-trace-sdd.log) ·
[Trace input](../../handoffs/current/72/logs/conformance-formal-trace-input.log) ·
[Trace tests](../../handoffs/current/72/logs/conformance-formal-trace-tests.log) ·
[Canonical пакет](../../handoffs/current/72/conformance.json).
Маршрутизатор этой сессией не запускался; дальнейший переход выполняет оператор.

## Security (iteration 4, formal transition 38)

Одобрено собственным прогоном на неизменном дереве `tree:30773ebf309bde6232825bb32089394ddaf73ed33bb45d24e372a40d12597d76`.
Подмена `decision` и `attempt-72` символическими ссылками (файл и каталог) отклоняется exit 74 без
изменения внешнего файла и его SHA-256; FIFO/нерегулярный `decision` отклоняется; допустимый псевдоним
доверенного корня после ack оставляет ноль attempt-кадров; коррупция журнала блокирует запуск с
диагностикой, пустой кадр — безопасный rollback; replay идемпотентен (tx/ack/dlq, нет воскрешения и
дублей). CI: performance read-only, release `contents: write` за гейтом тегов, vcpkg по пину, jar по
SHA-256, heredoc квотирован, trap чистит только созданный mktemp; M02 — только ключ кэша. Пакет
[security.json](../../handoffs/current/72/security.json), логи `handoffs/current/72/logs/security-formal-*.log`.
