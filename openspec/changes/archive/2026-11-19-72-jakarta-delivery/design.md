# Локальный C++ контракт доставки

Класс K2. Принятая функциональная реализация SDD72 v0.5, REQ-72-1…8,
source/tests `2b3c536`. Формальные шлюзы iteration 4: Conformance и Security approved;
NFR допущен через Owner deferral при техническом rejected. REQ-72-9 — открытый долг.
Исходный fingerprint `30773ebf` сверён перед подготовкой дельты; собственный validate
и отпечаток дельты записываются в официальный пакет DocWriter.
ADR-0010 сохраняет статус proposed. Пути ниже заданы от корня компонента.

Публичные сигнатуры сверены с server/src/{Connection,Consumer,Producer,Session}.h:

```cpp
// Connection, объявления внутри класса:
void start();
void stop();
void close();

// Consumer, объявления внутри класса:
using MessageListener = std::function<void(Message::Ptr)>;
Message::Ptr receive();
Message::Ptr receive(int64_t timeout);
Message::Ptr receiveNoWait();
Message::Ptr recv(int64_t usec_timeout = 10000000);
ReceiveBodyResult receiveChecked(const std::function<bool(const Message&)>& predicate,
                                 int64_t usec_timeout = 10000000);
void setMessageListener(MessageListener listener);
void close();

// Producer, объявления внутри класса:
using CompletionListener = std::function<void(const SendCompletion&)>;
void sendAsync(const Message& message, CompletionListener listener);
void sendAsync(const Message& message, const SendOptions& options,
               CompletionListener listener);
void close();

// Session, объявления внутри класса:
void acknowledge();
void recover();
void commit();
void rollback();
void close();
```

`SendCompletion` содержит `std::optional<Message::Headers> headers` и
`std::exception_ptr error`. `ReceiveBodyResult` содержит disposition со значениями
`empty`, `compatible`, `incompatible` и `Message::Ptr message`.
`receive(timeout)` использует миллисекунды, 0 означает бессрочно, отрицательное
значение отклоняется. Старые recv/receiveChecked используют микросекунды:
0 без ожидания, отрицательное бессрочно, default 10000000. Connection изначально stopped.
`SendOptions{}`: NOT_PERSISTENT, priority=4, TTL=0, delay=0; обычный send без setDefault
сохраняет reliability/priority сообщения. RedeliveryPolicy{}: maxRedeliveries=-1,
backoffMs=0, maxBackoffMs=60000, deadLetterQueue=nullptr.

Wall clock фиксируется при принятии send; timestamp/expiration/deliveryTime не
пересчитываются при commit/replay/retry. Отключение timestamp по REQ-70-1 не меняет
отсчёт delay/TTL. Steady clock задаёт бюджет receive. Backoff хранится отдельно
в nextRedeliveryAt; публичное JMSDeliveryTime сохраняется также у DLQ-копии.
Сериализация listener по сессии и сериализация CompletionListener — отдельные
очереди исполнения; взаимная эксклюзивность этих двух видов callbacks не заявляется.

Exchange координирует одно решение всех входов/выходов локальной транзакции.
Состав топика и selectors выбираются при commit (ADR-0009). Выходы недоступны
claim/browser до общей публикации. До решения ошибка допускает rollback, после
решения replay доводит применение до конца перед обслуживанием каталога.

Детали формата и откат остаются в ADR-0010: delivery-72/decision с одним решением,
attempt-72/<uuid> и идентичность поколения durable, inline attempt и метка тома
*.delivery-72. Сообщения 0x02/0x03 и смещения legacy-полей сохраняются; padding
старого тома без метки не считается attempt. Старые заголовки, включая deliveryTime=0,
читаются как записаны. Гарантия ограничена остановкой процесса и повторным replay
в том же каталоге данных. Потеря устройства/узла этим контрактом не покрывается.

Закрытые четыре пробела закреплены сценариями: CLIENT/TRANSACTED listener exception
удерживает вход до recover/rollback; Session::close возвращает вход и удаляет выходы;
durable snapshot выбирается именно при commit; durable retry после crash требует
одновременно true/2 и отсутствия повторного появления после ack. Test и детали —
в дельтах; наблюдения независимой проверки — logs/conformance-preflight.md внутри
handoffs/current/72/.

## Отложенный бюджет REQ-72-9 и исключение Owner

REQ-72-9 отсутствует в `specs/*/spec.md` этой дельты. Исходный ID, закреплённый
Источник и критерий сохранены в записи долга в proposal. Бюджет остаётся +5% CPU
median, CV≤5%, ABBA, девять повторов суммарно на вариант в двух заходах (4+5),
одинаковые RelWithDebInfo сборки/регистрации/единицы на одной машине; учитывается
CPU storage workers для persistent I/O. Три сравнения SDD71 и шесть семейств
ClientAck/Transacted сохраняются, real time и listener/async/journal replay/DLQ
публикуются отдельно. Неустойчивая серия повторяется один раз, повторная
нестабильность даёт отрицательный результат. Runner: `tests/nfr72.py`,
критерий `NfrGateTest.DeliveryHotPathBudget`; исполнение критерия не заявляется успешным.

Perf [44475262358](https://github.com/bas524/tiny-mq/actions/runs/44475262358) — failure:
AutoAck_NonPersistent_RoundTrip +6,59%, CPU 106,6 → 113,6 ns/op, CV 0,57% / 1,73%.
Это устойчивая регрессия сверх бюджета. Обычный CI
[44475262476](https://github.com/bas524/tiny-mq/actions/runs/44475262476) зелёный.
Решение Owner «Влить по обычному CI, perf — отдельная задача» разрешает исключение
из блокирующего perf-гейта для слияния этого изменения. Требование бюджета остаётся
невыполненным, полного технического одобрения НФТ этим исключением не возникает.

Долг передан в открытую M-05: подготовленный документ
`handoffs/current/72/logs/M-05-local-nightly-performance.md`, будущий путь
`tasks/M-05-local-nightly-performance.md`. Локальные ночные прогоны ещё не настроены;
M-05 закрывается по валидным сравнениям и соблюдению бюджета. ID REQ-72-9 сохраняет
связь с исходной постановкой. GitHub Performance сейчас отключён. Ночные задания ещё не настроены; стадия
DocWriter не создаёт расписание и не повторяет измерения. Источник результата —
формальный NFR-пакет и отчёт iteration 4, источник решения — прямое уточнение Owner.

Формальные пакеты iteration 4 оформлены: NFR status=approved с
approval_basis=owner-approved-deferral и performance_verdict=rejected;
Conformance approved для REQ-72-1…8; Security approved. Исключение Owner
не закрывает M-05 и не подтверждает выполнение REQ-72-9.

Новые продуктовые понятия не вводятся: product_delta=null. Сеть/Java/XA/EE/кластер
относятся к SDD73–78. Собственные validate/fingerprint выполнены при сдаче официального docwriter.json.
Проверки preflight не используются как уже выполненный формальный переход.


## Purpose нового capability при archive

Утверждённый смысл capability записан в `specs/jakarta-session-delivery/spec.md`,
раздел `## Purpose`; новых требований он не добавляет. Текущий
`.claude/chain/openspec.py::merged_text` переносит операции требований, а для нового
capability создаёт общий текст Purpose, игнорируя этот раздел дельты.
На рубеже R1 архивист должен перенести точный текст Purpose из этой дельты в
`openspec/specs/jakarta-session-delivery/spec.md`, заменив общий placeholder,
и проверить итоговую спецификацию. Оставлять placeholder как принятый Purpose нельзя.
DocWriter не изменяет архиватор и не создаёт спецификацию-факт до archive.
