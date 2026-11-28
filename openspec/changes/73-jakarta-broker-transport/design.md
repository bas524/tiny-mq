# Проектирование REQ-73-1

## Architecture

Редакция 0.11 от 2026-11-28 (Europe/Moscow), для согласования Owner.
Основа — v0.9 на `68138a2`. Разбор шести замечаний восьмого раунда приведён в tasks.md.
Основания сохранены в источниках REQ-73-1 дельты. Применены INT-01 v0.3,
ADR-0001/0002 от 2026-05-23 и STOMP 1.2, разделы Frames, Connecting,
Heart-beating и ERROR. `server/src/Connection.h` на `6f79fb4` владеет
доменными `Session`. Эти типы не являются сетевым подключением и не меняются.

### Владение и общий интерфейс (Q1, Q5)

`BrokerServer` владеет listener, `Poco::Net::SocketReactor`, worker thread,
принятыми TCP-сокетами, регистрациями, очередями выхода и одним
`unique_ptr<WireConnection>` на сокет. Один reactor обслуживает несколько
подключений без потока на соединение. Протокол не читает и не пишет сокет.
Выбор реализации производится фабрикой один раз при accept по конфигурации
listener. Текущий listener создаёт только `StompConnection`.

Следующие объявления определяют контракт, который доверенная подготовка
перенесёт в `server/src/WireConnection.h`. Все типы находятся в `tiny_mq`.
Заголовок включает только стандартные `<chrono>`, `<cstddef>`, `<cstdint>`,
`<optional>`, `<span>`, `<variant>` и `<vector>`. Он не включает BrokerServer,
StompConnection, Poco, Connection или Session.

```cpp
using WireTime = std::chrono::steady_clock::time_point;
struct WireLimits {
  std::size_t frame_bytes = 1048576;
  std::size_t header_bytes = 8192;
  std::size_t header_count = 64;
  std::chrono::milliseconds handshake{5000};
  std::size_t output_bytes = 1056768;
  std::chrono::milliseconds flush{1000};
  std::chrono::milliseconds heartbeat_send{10000};
  std::chrono::milliseconds heartbeat_receive{10000};
};
struct WireBytes { std::span<const std::byte> bytes; };
struct WireTimer {};
struct WireEof {};
enum class WireFailureKind { read, write, internal };
struct WireFailure { WireFailureKind kind; };
struct WireStop {};
struct WireWritten { std::size_t bytes; };
using WireEvent = std::variant<WireBytes, WireTimer, WireEof,
                               WireFailure, WireStop, WireWritten>;
struct WireInput {
  WireTime now;
  std::size_t output_budget;
  WireEvent event;
};
enum class WireClose { keep, immediate, after_flush };
struct WireResult {
  std::vector<std::vector<std::byte>> output;
  std::optional<WireTime> deadline;
  WireClose close = WireClose::keep;
};
class WireConnection {
 public:
  virtual ~WireConnection() noexcept = default;
  virtual WireResult handle(const WireInput& input) = 0;
};
```

Время абсолютно и монотонно. Интервалы выражены в миллисекундах, число
байтов — в октетах. Результат перемещается транспортному владельцу.
Входной span действителен лишь в течение `handle`, сохранять его нельзя.
Конструктор конкретного протокола получает копию WireLimits и время accept.
Общий интерфейс не содержит кадров STOMP или protobuf и доменных JMS-команд.
`WireConnection` задаёт транспортный жизненный цикл соединения.
`StompCodec` выполняет потоковое кадрирование STOMP внутри
`StompConnection`. Общий кадровый интерфейс `WireCodec` (E2 из SDD 73)
отложен до среза, который связывает STOMP и protobuf с общим back-end
`Connection`/`Session` по ADR-0002. Предложение для Owner разделяет
обязанности двух уровней. `WireConnection` остаётся границей TCP-цикла,
общий кадровый интерфейс используется внутри конкретных протоколов.
Будущий `ProtobufConnection` реализует `WireConnection`, а его
protobuf-кодек — общий кадровый интерфейс. E2 и protobuf-кодек появятся
после отдельного архитектурного решения. Заглушки, Envelope, зависимость
protobuf, клиентский и общий каталоги отложены вместе с protobuf-срезом.
Связь сетевых операций с объектами ядра относится к REQ-73-2.

Точные протокольные входы защищённого теста тоже фиксируются здесь.
В StompConnection.h объявляется `StompConnection(WireLimits, WireTime)`
и override `WireResult handle(const WireInput&)`. Класс final наследует
WireConnection, имеет noexcept destructor и собственный pimpl.
В StompCodec.h объявляется следующая граница прямого тестирования.

```cpp
struct StompFrame {
  std::string command;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::byte> body;
};
class StompCodec final {
 public:
  explicit StompCodec(WireLimits limits);
  ~StompCodec() noexcept;
  void feed(std::span<const std::byte> bytes,
            const std::function<void(StompFrame&&)>& completed);
  void eof() noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

Заголовок кодека включает WireConnection.h и стандартные string, utility,
functional, memory. completed вызывается синхронно по одному завершённому
кадру, без внутренней очереди. Headers результата содержат только первые
значения в исходном порядке. Синтаксическая ошибка или превышение лимита
вызывает `std::runtime_error` с текстом `syntax` либо `limit`, кодек
после этого не используется. Callback exception распространяется вызывающей
стороне, соединение закрывается. eof отбрасывает остаток. Пустой/неполный
вход не выдаёт кадр. EOL между кадрами не выдаёт StompFrame. Учёт активности
выполняет StompConnection по исходным октетам независимо от callback.

### Порядок событий, буферы и завершение (Q1)

События одного объекта сериализуются reactor. `now` не убывает. На accept
создаётся объект, затем ему подаётся `WireTimer` с временем accept для
публикации исходного срока handshake. `deadline` заменяет предыдущий срок,
`nullopt` снимает его. Ложное или устаревшее пробуждение не является ошибкой.

`BrokerServer` читает порцию не более `frame_bytes`. `handle` принимает её
целиком либо возвращает закрытие и отбрасывает необработанную часть.
Возврата остатка нет. Если DISCONNECT или ошибка завершает протокольную
обработку с закрытием, StompConnection отбрасывает остаток того же WireBytes.
Последующие кадры из этой порции не диспетчеризуются и не добавляют выход,
даже если их байты синтаксически неверны. Только StompConnection/StompCodec хранит незавершённый
кадр, handshake, согласованные интервалы и timestamps конкретного peer.
Декодер обрабатывает кадры по порядку без неограниченной очереди кадров.
Лимиты проверяются до роста буфера. Числовая длина тела проверяется
по остатку бюджета кадра согласно клиентскому профилю.

`output_budget` равен `output_bytes` минус ещё не записанные байты очереди.
Для каждой команды обработчик проверяет размер новых блоков до выделения.
В остатке бюджета учтены блоки, сформированные предыдущими командами
этого же события. Пустые блоки не выдаются. BrokerServer повторно проверяет
сумму при enqueue, сохраняет порядок и владеет блоками до отправки.
Превышение бюджета даёт немедленное
закрытие без дополнительного ERROR. Очередь не принимает превышение.
Бюджет ограничивает логические незаписанные октеты, а не физическую RAM.
При partial write front-блок сохраняет записанный префикс до завершения
блока. Такой префикс возможен только у одного front-блока и не превосходит
output_bytes, так что сумма живых длин буферов очереди не выше
двух output_bytes. Завершённые блоки немедленно освобождаются, их capacity
не кешируется. Capacity vector/container и allocator overhead учитываются
в отдельном memory evidence, не объявляются равными логическому бюджету.
На partial write курсор продвигается только на положительное число реально
записанных байтов. Would-block сохраняет курсор, не вызывает WireWritten
и ждёт следующей writable notification. Нулевой успешный результат записи
трактуется как транспортная ошибка, а не бесконечный retry.

После каждой положительной записи подаётся `WireWritten{n}` с новым
доступным бюджетом и временем завершения записи. `n` не превышает переданную
writer порцию. Протокол ведёт собственные счётчики выданных/записанных
байтов для определения конца CONNECTED. Сообщение WireWritten не несёт
STOMP-маркера. Исключение из handle, фабрики или writer локализуется
немедленным закрытием этого сокета. При исключении фабрики объекта ещё нет.

`keep` означает продолжение и использует возвращённый срок.
`immediate` отбрасывает новый результат и всю очередь, снимает события и
закрывает TCP. `after_flush` принимает допустимый выход, отключает дальнейшее
чтение и вызовы протокола. Срок drain равен `now + flush` и дополнительно
ограничен `deadline` из этого результата, если он задан. Для `after_flush`
это верхняя граница работы транспорта, а не срок повторного вызова.
Предыдущий таймер отменяется. При пустой
очереди, ошибке или достижении срока TCP закрывается. При равенстве сроку
нет новой попытки записи. В состоянии drain успешные записи не вызывают
handle. EOF не генерирует
ERROR, отбрасывает частичный кадр и закрывает немедленно. Read/write failure
и WireStop также закрывают немедленно без гарантии доставки последнего кадра.
Пока соединение принимает события, BrokerServer передаёт ему WireEof,
WireFailure или WireStop один раз. Эти события требуют immediate без
output/deadline. Нарушивший это результат игнорируется с тем же немедленным
закрытием. После входа в drain новых вызовов handle нет. В частности,
`BrokerServer::stop()` завершает drain в транспорте, отбрасывает очередь
и закрывает сокет без передачи WireStop. Повторный stop безопасен.
Сначала снимаются регистрации и таймеры, затем закрывается сокет и
уничтожается объект. Notification несёт поколение соединения и не вызывает
объект после удаления даже при повторном использовании descriptor.
Проверка этой точной защиты выполняется структурным Reviewer review
регистрации/удаления и generation check. WireConnectionBoundary
дополняет его измеримым accept/close stress с живым соседним peer.
TCP и публичные DI-швы не
гарантируют детерминированное воспроизведение descriptor reuse.
Его наблюдение отмечается в stress-логе отдельно, отсутствие наблюдения
не выдаётся за исполненный детерминированный тест устаревшей notification.
Деструкторы noexcept. Ошибка одного peer не завершает reactor и не меняет
состояние соседей. Stop listener запрещает accept, закрывает подключения
и завершает worker с join. `~BrokerServer()` синхронно выполняет тот же
порядок, если worker запущен. К возврату из деструктора listener закрыт,
активные подключения завершены с WireStop, drain отброшен без вызова
протокола, все сокеты закрыты и worker завершён с join. После явного
stop или до запуска деструктор освобождает оставшиеся ресурсы без повторных
событий. Владелец вызывает stop и деструктор вне потока reactor.

### Точный шов внедрения (Q5)

В `BrokerServer.h` после включения WireConnection.h объявляются типы ниже.
Ему нужны стандартные `<functional>`, `<memory>` и заголовок StreamSocket.
Обратных include нет. StompConnection.h включает WireConnection.h,
BrokerServer.cpp подключает конкретный протокол для фабрики по умолчанию.

```cpp
using WireFactory = std::function<std::unique_ptr<WireConnection>(
    const WireLimits&, WireTime)>;
using WireClock = std::function<WireTime()>;
enum class WireWriteStatus { progress, would_block, failed };
struct WireWriteResult {
  WireWriteStatus status;
  std::size_t bytes;
};
using WireWriter = std::function<WireWriteResult(
    Poco::Net::StreamSocket&, std::span<const std::byte>)>;
struct BrokerDependencies {
  WireFactory factory;
  WireClock clock;
  WireWriter writer;
};
class BrokerServer final {
 public:
  explicit BrokerServer(WireLimits limits = {});
  BrokerServer(WireLimits limits, BrokerDependencies dependencies);
  static BrokerDependencies productionDependencies();
  ~BrokerServer() noexcept;
  std::uint16_t startLoopback();
  void notifyClockChanged();
  void stop() noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

Конструктор только с limits делегирует productionDependencies и тем самым
доступен независимому loopback-runner без изменения main.
productionDependencies возвращает фабрику настоящего StompConnection,
steady_clock и реальный Poco writer. Обычные loopback-строки
CapabilityNegotiation и wire benchmark используют production constructor.
В строках с управляемым временем CapabilityNegotiation передаёт
productionDependencies в override constructor, заменяя clock на ManualClock.
Для Q16 дополнительно заменяет writer на ScriptedWriter и сохраняет
фабрику настоящего StompConnection. После наблюдённого would-block либо
записи заданного префикса тест переводит часы на deadline и вызывает
notifyClockChanged. Для детерминированной склейки тот же тест дополнительно
подаёт CONNECT/STOMP и DISCONNECT одним `WireBytes` непосредственно
в настоящий StompConnection и проверяет ограничение drain в результате.
TCP-вариант проверяет байты и закрытие через реальный сокет. Boundary
заменяет все три зависимости и проверяет нейтральный интерфейс.
В `BrokerServer(WireLimits, BrokerDependencies)` пустая `factory`, `clock`
или `writer` вызывает `Poco::InvalidArgumentException` в конструкторе, до создания
listener, сокета или worker и вызова зависимостей. Возврат nullptr или
исключение из непустой фабрики при accept остаются отдельными отказами
соединения.
Защищённый тест задаёт factory до accept. Фабрика вызывается один раз,
её null-результат закрывает сокет. Production-runner передаёт фабрику STOMP,
steady_clock и адаптер неблокирующей записи Poco из BrokerServer.cpp.
startLoopback запускается один раз, возвращает порт ОС на `127.0.0.1`.
Повторный вызов и вызов после stop выбрасывают
Poco::IllegalStateException до изменения состояния. Уже запущенный
listener остаётся на том же порту, worker и открытые соединения продолжают
работу. После stop listener и worker заново не создаются. Конструктор
не запускает worker. Штатный main не включает listener этого среза.

Writer вызывается только production-очередью отдельного сокета.
`progress` требует `0 < bytes <= span.size()`, другие статусы требуют 0.
Недопустимый результат — немедленное локальное закрытие. Тестовый writer
при progress передаёт заданный префикс настоящему сокету, при would_block
и failed возвращает заданный статус. Он не реализует отдельную очередь.
Обычный loopback не используется как гарантия partial write.

notifyClockChanged будит reactor и обеспечивает проверку сроков его
worker. Метод не вызывает протокол из тестового потока. Поддельные часы
потокобезопасны и не убывают. Тест ждёт наблюдаемого события с конечным
wall-clock deadline, а не полагается на sleep. В одной итерации
готовое чтение обрабатывается до таймера, а наступивший таймер — до новой
попытки записи. Для незавершённого рукопожатия `StompConnection` проверяет
абсолютный срок перед разбором любой порции. При `now >= deadline` действуют
правила Q16, даже если в этой итерации доступно чтение.

### Wire-поведение (Q2, Q6–Q10)

Предложение Owner для этой редакции делает весь следующий набор обязательными
конфигурируемыми defaults после утверждения. Невалидные аргументы
конструкторов BrokerServer используют единый тип из `<Poco/Exception.h>`.
Кадр 1 МиБ включая NUL,
строка заголовка 8 КиБ без EOL, 64 физических заголовка, handshake 5 секунд
от accept, очередь выхода `frame_bytes + 8192`, flush 1 секунда,
серверный heartbeat `10000,10000` мс. При изменении frame_bytes значение output_bytes задаётся отдельно.
Defaults равны указанным числам. Default output_bytes вычислен из
default frame_bytes, но runtime overrides этих двух полей независимы,
скрытого пересчёта конфигурации нет.
Оба конструктора BrokerServer до открытия сокета и запуска worker
выбрасывают `Poco::InvalidArgumentException` для нуля в frame_bytes, header_bytes,
header_count или output_bytes, для handshake или flush не больше нуля,
для отрицательных heartbeat_send или heartbeat_receive. Требование также
действует, если handshake, flush, heartbeat_send или удвоенный
heartbeat_receive не представимы в `std::chrono::steady_clock::duration`.
Проверка выполняется до умножения и преобразования, которое могло бы
переполниться. Нулевой heartbeat допустим. Negotiation проверяет также
представимость выбранных интервалов и абсолютных сроков.

### STOMP v1 client profile

Этот раздел задаёт клиентский wire-контракт handshake и кадрирования
по REQ-73-1 для стыка «Сетевой контракт клиентов» из INT-01.
Для потребителей SDD 74, 75 и 77 точка входа — REQ-73-1 и его утверждённый
клиентский профиль.
Набор из CONNECTED, заголовков возможностей, framing-v1 и кодов ERROR
расширяется отдельными срезами по мере появления новых wire-операций.

CONNECT и STOMP согласуют только 1.2. У присутствующего `accept-version`
проверяется весь список. Каждый элемент имеет форму `[0-9]+\.[0-9]+`,
элементы разделены запятыми без пробелов. Пустой заголовок, пустой элемент
или неверная форма дают `ERROR syntax`, даже если в списке есть 1.2.
Из валидного списка выбирается точный токен `1.2`. Валидные неизвестные
версии пропускаются, поэтому `1.2,2.0` согласуется. При отсутствии
заголовка предложение считается равным 1.0. Если точного 1.2 нет,
сервер отвечает `ERROR version` с `version:1.2`.
Непустой host обязателен, пробелы не обрезаются. Используется один
default virtual host для любого непустого значения. Для полностью
декодированного CONNECT/STOMP после проверки версии проверяются наличие
и непустота `host` и формат `heart-beat` (`syntax`), затем списки
возможностей (`capability`, включая ошибки формы списка и обязательные
токены вне поддержанного предложения).
Ошибки кадрирования и лимитов обнаруживаются декодером до этих проверок.
Заголовки вне перечисленных для handshake и действующих общих правил
кадрирования игнорируются после проверки декодером. Это распространяется
на дополнительные заголовки и опциональные login и passcode из STOMP 1.2.
Loopback-профиль среза работает без аутентификации. Значения login и passcode
не сохраняются в состоянии сессии, не попадают в логи и не отражаются в ответах.
Ко всем заголовкам применяются общие проверки UTF-8, синтаксиса, лимитов
и правила escapes команды.
В CONNECTED сервер отправляет version и сконфигурированные
`heartbeat_send,heartbeat_receive` в этом порядке в заголовке
`heart-beat`. Для клиента `20,30` и сервера `10,40` точная строка ответа
`heart-beat:10,40`. В терминах Heart-beating STOMP 1.2 это объявленные
возможности сервера. Интервалы передачи рассчитываются отдельно.
Команды чувствительны к регистру. До handshake допускается
только CONNECT/STOMP и EOL. После него только EOL и DISCONNECT.
Все остальные команды, включая SEND, SUBSCRIBE, UNSUBSCRIBE, ACK, NACK,
BEGIN, COMMIT, ABORT, повторный CONNECT/STOMP, MESSAGE, RECEIPT, ERROR и
неизвестное имя, дают ERROR и закрытие. Это граница среза, не обещание
полного STOMP messaging server.

При успешном CONNECT или STOMP с `receipt` сервер ставит `RECEIPT` после
`CONNECTED` и оставляет сокет открытым. Без заголовка отправляется только
`CONNECTED`. Для STOMP это подтверждение по разделам *Header receipt* и
*RECEIPT* STOMP 1.2. Для CONNECT это явно допустимое в tiny-mq расширение:
стандарт исключает CONNECT из общего правила, а этот профиль обрабатывает
оба варианта рукопожатия одинаково. Значение берётся из первого `receipt`
по правилам декодирования исходной команды, включая буквальный backslash
в CONNECT. Ошибка handshake отправляет только ERROR, если доступен бюджет.

DISCONNECT с `receipt` выдаёт RECEIPT перед drain. Без него закрывается
после сброса ранее выданных байтов. Неподдерживаемые команды
не получают RECEIPT. Во всех успешных ответах receipt-id равен
декодированному значению `receipt`. Присутствующий пустой `receipt:`
даёт `RECEIPT\nreceipt-id:\n\n\0`, ровно 22 октета. К нему применяются
общие правила очереди, бюджета и drain. Сервер заменяет backslash на `\\`,
CR на `\r`, LF на `\n`, colon на `\c`. Остальные UTF-8 октеты сохраняются.
RECEIPT кодируется как `RECEIPT\nreceipt-id:<escaped>\n\n\0`.
NUL внутри `receipt-id` записывается сырым октетом `0x00` и учитывается
как один октет значения. Конец кадра задаёт отдельный NUL после пустой
строки. Для входного значения `a\0b` выход
`RECEIPT\nreceipt-id:a\0b\n\n\0` занимает 25 октетов.
Размер полного выхода вычисляется с проверкой переполнения, включая
escaping, LF и оба вида NUL. Для handshake с receipt учитывается сумма
CONNECTED и RECEIPT. Размер сверяется с остатком бюджета события
до выделения блоков ответа на handshake. При точном равенстве успешный
handshake с receipt возвращает `keep`, полный CONNECTED, затем полный RECEIPT
и срок незавершённой отправки CONNECTED. Превышение возвращает `immediate` с пустым output и без
deadline. Для fixture с heartbeat 0,0, отсутствием возможностей и receipt
`r` ответы зафиксированы как
`CONNECTED\nversion:1.2\nheart-beat:0,0\n\n\0` (39 октетов) и
`RECEIPT\nreceipt-id:r\n\n\0` (23 октета). Для обоих вариантов handshake
полный ответ помещается в 62 октета.

Когда этот CONNECT и DISCONNECT с `receipt:s` поступают одним
`WireBytes` до первого `WireWritten`, результат содержит CONNECTED,
RECEIPT с `receipt-id:r` и RECEIPT с `receipt-id:s` в этом порядке.
В показанной конфигурации размеры равны 39, 23 и 23 октетам.
При бюджете 85 возвращается `after_flush` с исходным handshake deadline,
а транспорт соблюдает `min(now + flush, handshake_deadline)`.
При 84 весь выход этого события отбрасывается. Результат —
`immediate` с пустым `output` и `deadline = nullopt`.
Бюджет проверяется по мере декодирования команд.

Tiny-mq заголовки возможностей используют уникальные непустые токены
`[a-z0-9][a-z0-9-]*`, разделённые запятыми без пробельного padding.
Отсутствующий заголовок задаёт пустое множество. Присутствующий пустой
`x-tinymq-capabilities` или `x-tinymq-required-capabilities` даёт
`ERROR capability`. Required — подмножество предложения. Неизвестная optional возможность
игнорируется, неизвестная required вызывает отказ. CONNECTED возвращает
пересечение с `{framing-v1}` только в `x-tinymq-capabilities`. При пустом
пересечении заголовок отсутствует целиком. `x-tinymq-required-capabilities`
используется во входе. `framing-v1`
гарантирует кадрирование и лимиты текущего контракта STOMP 1.2, включая
частичные кадры, content-length, escapes и heartbeat. Токен не добавляет
новых кадров или JMS-команд и не меняет обычный STOMP handshake.

Декодер принимает LF и CRLF, но отвергает одиночный CR вместо EOL.
Команды и заголовки валидируются как UTF-8, бинарное тело не валидируется.
NUL в строке команды даёт `syntax`. В имени и значении заголовка NUL
сохраняется как кодировка U+0000 по грамматике заголовка STOMP 1.2.
Декодированные строки сравниваются по полной длине. Правила отдельных
заголовков по-прежнему проверяют формат их значений. NUL завершает кадр
в состоянии чтения тела без длины или после ровно content-length октетов.
Некорректный UTF-8, включая overlong, surrogate и незавершённую последовательность
к моменту завершения текстового поля, даёт синтаксическую ошибку.
В CONNECT и CONNECTED escape-последовательности остаются буквальными.
STOMP согласно правилу «все кроме CONNECT/CONNECTED» декодирует escapes.
Для прочих кадров допустимы только `\r`, `\n`, `\c`, `\\`.
Неизвестный escape или завершающий backslash — fatal error. Пробелы в
именах/значениях сохраняются. Повтор заголовка использует первое значение,
но все физические строки считаются в лимите. Header без разделителя `:`
ошибочен, пустое значение сохраняется. `content-length` — непустая
последовательность ASCII цифр. Число десятичное, ведущие нули допустимы.
Строка команды, все физические заголовки с их EOL и пустая строка
занимают часть `frame_bytes`. EOL между кадрами в лимит не входит.
При соблюдении лимитов метаданных декодер сначала проверяет лексическую
форму первого `content-length` целиком. Пустое значение, знак, пробельные
и другие нецифровые октеты дают `syntax`. Для корректной последовательности
цифр резервируется один октет под терминатор. Если места для терминатора
нет или десятичное значение больше остатка после такого резерва, декодер
выдаёт `limit` до разбора или выделения тела. Это охватывает значения,
непредставимые в `std::size_t`. Накопление цифр ограничено остатком кадра
и проверяется до умножения и сложения. При допустимой длине читается ровно
указанное число октетов, включая внутренний NUL. Следующий октет обязан
быть NUL, иначе возвращается `syntax`.
После чтения полного кадра и до вызова callback декодер отвергает
непустое тело у любой команды кроме SEND, MESSAGE и ERROR, включая
неизвестное имя. На сокете это даёт `ERROR syntax` до protocol dispatch.
`content-length:0` и пустое тело доступны любой команде. Корректно
декодированный кадр вне допустимых команд данной фазы даёт
`ERROR unsupported-command`. EOL между кадрами учитывается как heartbeat.

До EOF всякий отказ версии, capability, синтаксиса, лимита, unsupported
command, heartbeat timeout или истечение handshake до постановки
CONNECTED в очередь при доступном бюджете выдаёт один полный ERROR,
затем after_flush. Истечение handshake при ожидающем записи CONNECTED
обрабатывается по Q16. При отправке ERROR после истечения handshake
или входящего heartbeat `after_flush` возвращает `deadline = nullopt`.
BrokerServer даёт ответу собственный интервал flush. Канонический ERROR без тела
имеет LF и байты `ERROR\nmessage:<code>\n[version:1.2\n]\n\0`.
Полный словарь значений `message` приведён в строке STOMP ERROR message
реестра.
version заголовок присутствует только при code=version. Входные данные,
credentials и receipt не отражаются. Отсутствие receipt-id в ERROR при
входном receipt — осознанное отклонение от SHOULD STOMP, а не MUST.
Оно сохраняет короткий ограниченный ответ без отражения недоверенного
ввода. Корреляция успешного DISCONNECT через RECEIPT сохраняется.
При нехватке бюджета, EOF, transport/
write failure или исключении закрытие немедленное без гарантии ERROR.
Сломанный peer и соседний исправный сокет проверяются раздельно.

Heartbeat заголовок содержит ровно два неотрицательных десятичных числа
миллисекунд через запятую. Каждое число состоит из непустой
последовательности ASCII цифр и трактуется в десятичной системе.
Ведущие нули допустимы. Знак, пробельные и другие нецифровые октеты
в компонентах или переполнение chrono дают `syntax`.
Отсутствие эквивалентно 0,0. Для направления с обоими ненулевыми значениями
берётся максимум. Например клиент 20,30 и сервер 10,40 дают входящий
интервал 40 и исходящий 30 мс. Оба таймера запускаются при подтверждении
WireWritten последнего октета CONNECTED строго до абсолютного handshake
deadline. До подтверждения это единственный активный срок. Своевременное
подтверждение снимает срок и устанавливает `last_rx` и `last_tx` в `now`
этого WireWritten. Вход до подтверждения не сокращает интервал после
установления связи. Если CONNECTED ещё не поставлен в очередь,
`now >= handshake_deadline` даёт `ERROR handshake-timeout` при достаточном
бюджете, даже при доступных входных байтах. Если CONNECTED
уже выдан и подтверждение ещё ожидается, `StompConnection` проверяет
`now >= handshake_deadline` до обработки любого события, включая
WireWritten последнего октета CONNECTED. При наступившем сроке результат
равен `immediate` с пустым `output` и `deadline = nullopt`. Фаза heartbeat
не начинается. BrokerServer отбрасывает очередь и закрывает сокет. Peer
может увидеть октеты, которые TCP принял до решения о закрытии, включая
байты записи, начатой до срока и завершившейся на границе или после неё.
После решения о закрытии новых записей нет.

Любой результат `after_flush` до подтверждения последнего октета уже
выданного CONNECTED передаёт абсолютный handshake deadline как верхнюю
границу drain. Это правило одинаково для кадров в одной и разных порциях.
При ошибке входа до этого срока и достаточном бюджете BrokerServer ставит
ERROR после уже выданных байтов и пишет до `min(now + flush, handshake_deadline)`.
На границе срока остаток очереди отбрасывается. Если бюджет исчерпан,
результат `immediate` сохраняет прежнее правило. Если handshake deadline
уже наступил, действует Q16. Для остальных результатов `after_flush`,
включая ERROR по тайм-ауту до выдачи CONNECTED и ERROR по heartbeat
после подтверждённого CONNECTED, `deadline = nullopt`. TCP сохраняет
октеты, принятые вызовом записи, который начался до срока.

Любые положительные входные октеты после handshake, включая часть кадра
и EOL, обновляют last_rx. Положительная запись обновляет last_tx.
Готовность к чтению/записи и would-block активностью не считаются.
При `now >= last_rx + 2 * incoming` наступает timeout, если в ту же
итерацию не приняты положительные байты. При `now >= last_tx + outgoing`
и пустой очереди выдаётся ровно LF `0x0a`. Если выход уже ожидает записи,
новый heartbeat не накапливается. Пока счётчик выданных байтов больше
подтверждённых WireWritten байтов, исходящий deadline снимается, чтобы
истёкший срок не вызывал busy loop. После записи остатка он пересчитывается
от last_tx. При `keep` единственный `WireResult.deadline` равен минимуму
активных абсолютных сроков. До подтверждения это срок handshake. После
подтверждения это `last_rx + 2 * incoming` при активном входящем направлении
или `last_tx + outgoing` при активном исходящем направлении и опустевшем
выходе, либо меньший из двух при их наличии. При отсутствии обоих
сроков возвращается `nullopt`. Если на `WireTimer` одновременно истекли
оба срока и входящая активность не обновила свой срок, входящий timeout
имеет приоритет перед выдачей исходящего heartbeat.
Переполнение вычисления срока отвергается как syntax при handshake.

### Производительность и ресурсы (Q3)

C++20 и существующий Poco достаточны. Новых зависимостей нет.
Бюджет +5% CPU/op сохраняется для 15 существующих путей принятого baseline
по PROJECT, `benchmarks/baseline.md` и nfr-gate. Native cpp-nfr измеряет
именно зарегистрированный набор. Новый wire-путь не сравнивается с
in-process baseline, в котором его нет.

Первый wire-замер обязателен по сценарию
[REQ-73-1/WireBaseline](specs/jakarta-broker-transport/spec.md#scenario-wirebaseline).
Он фиксирует исходный baseline следующего wire-среза. Доверенный runner
в WireContractTest.cpp измеряет 10000 последовательных CONNECT/DISCONNECT с receipt на loopback,
по одному outstanding peer, без heartbeat, после 1000 прогревочных
соединений. Сохраняются точные SHA и команды, toolchain, конфигурация,
process CPU/op, соединения/с, p95/p99 полного round-trip, peak RSS,
число ошибок и повторов. Девять повторов раскрывают разброс при указанных
условиях машины. Сравнительные пороги wire и абсолютные цели выпуска
определяются отдельным решением.

Приёмка требует обоих функциональных тестов, соблюдения лимитов ресурсов,
изоляции отказа, свидетельства WireBaseline и прохождения существующего
шлюза cpp-nfr по профилю cpp из PROJECT.md.
WireConnection не получает диагностического getter ради измерения памяти.
Проверяются ограниченные порции чтения, остаток одного кадра, бюджет
формирования выхода и число незаписанных октетов очереди, которое не
превышает output_bytes. Reviewer отдельно сверяет курсор головного блока,
освобождение завершённых блоков и предел суммы длин живых блоков при
частичной записи в два output_bytes. При 1000 повторных кадров и медленной
записи сохранённые данные не накапливаются между завершёнными кадрами. Reviewer и доверенный оператор
отдельно фиксируют capacity динамических буферов по коду и инструментам,
включая фазу роста, декодированные поля и выход. RSS учитывает allocator
и фиксированное состояние отдельно. Из входного лимита не выводится
необоснованная общая формула peak memory. Эта инструментальная проверка
сохраняется как raw evidence и не расширяет production API.

Имена в первой колонке — устойчивые ключи замкнутого реестра текущего среза.
Формат из трёх колонок обязателен для native reader. Отдельная колонка ID
не добавляется. Реестр включает публичные типы, варианты событий,
параметры конфигурации, расширения tiny-mq и операции, на которые
опираются критерии. Стандартные STOMP-кадры перечислены в wire-матрице,
которая задаёт обязательное поведение для каждого кадра этого среза.
Стандартные заголовки перечислены в правилах соответствующих кадров.
Поля структур и перечислители исчерпывающе заданы в объявлениях типов.
Отдельно зарегистрирован введённый этим срезом словарь wire-кодов ERROR.
Перегрузки одного публичного имени описываются одной строкой. Закрытые типы `Impl`, закрытые методы и деструкторы
не образуют отдельных сущностей. Их условия освобождения и время жизни
зафиксированы в контракте владельца.

## Entity inventory

| Сущность | Kind | Роль |
|---|---|---|
| BrokerServer | type | BrokerServer.h/.cpp, TCP/reactor и lifecycle |
| WireConnection | type | WireConnection.h, нейтральная граница |
| WireConnection::handle | method | Сериализованное событие и владеющий результат |
| BrokerServer::BrokerServer | method | Production constructor и override constructor |
| BrokerServer::productionDependencies | method | Копируемый комплект STOMP/clock/writer |
| BrokerServer::startLoopback | method | Запуск и порт ОС |
| BrokerServer::notifyClockChanged | method | Пробуждение worker |
| BrokerServer::stop | method | Идемпотентное завершение и join |
| StompCodec::StompCodec | method | Новый декодер с заданными лимитами |
| StompConnection::StompConnection | method | Новый протокол с лимитами и временем accept |
| StompConnection::handle | method | Нативная реализация контракта WireConnection |
| StompCodec::feed | method | Потоковые байты и синхронный callback |
| StompCodec::eof | method | Отбрасывание остатка |
| WireTime | type | WireConnection.h, steady_clock time_point |
| WireLimits | type | WireConnection.h, копия конфигурации подключения |
| WireBytes | type | WireConnection.h, все варианты события |
| WireTimer | type | WireConnection.h, все варианты события |
| WireEof | type | WireConnection.h, все варианты события |
| WireFailure | type | WireConnection.h, все варианты события |
| WireStop | type | WireConnection.h, все варианты события |
| WireWritten | type | WireConnection.h, все варианты события |
| WireFailureKind | type | WireConnection.h, полный контракт входа/выхода |
| WireEvent | type | WireConnection.h, полный контракт входа/выхода |
| WireInput | type | WireConnection.h, полный контракт входа/выхода |
| WireClose | type | WireConnection.h, полный контракт входа/выхода |
| WireResult | type | WireConnection.h, полный контракт входа/выхода |
| WireFactory | type | BrokerServer.h, dependency injection |
| WireClock | type | BrokerServer.h, dependency injection |
| WireWriter | type | BrokerServer.h, dependency injection |
| WireWriteStatus | type | BrokerServer.h, результаты записи и зависимости |
| WireWriteResult | type | BrokerServer.h, результаты записи и зависимости |
| BrokerDependencies | type | BrokerServer.h, результаты записи и зависимости |
| StompConnection | type | StompConnection.h/.cpp, handshake и таймеры peer |
| StompCodec | type | StompCodec.h/.cpp, потоковое кадрирование |
| StompFrame | type | StompCodec.h, завершённый кадр прямого decoder-теста |
| ProtobufConnection | concept | Будущая реализация, нет production-файла |
| Connection | type | Существующие доменные типы, вне scope |
| Session | type | Существующие доменные типы, вне scope |
| WireBoundaryDouble | concept | Только tests/WireContractTest.cpp, независимые тестовые реализации |
| ScriptedWriter | concept | Только tests/WireContractTest.cpp, независимые тестовые реализации |
| ManualClock | concept | Только tests/WireContractTest.cpp, независимые тестовые реализации |
| frame_bytes | config | WireLimits, пределы входного кадра |
| header_bytes | config | WireLimits, пределы входного кадра |
| header_count | config | WireLimits, пределы входного кадра |
| handshake | config | WireLimits, время handshake и конечный выход |
| output_bytes | config | WireLimits, время handshake и конечный выход |
| flush | config | WireLimits, время handshake и конечный выход |
| heartbeat_send | config | WireLimits, интервалы listener |
| heartbeat_receive | config | WireLimits, интервалы listener |
| x-tinymq-capabilities | header | Объявленные и обязательные возможности |
| x-tinymq-required-capabilities | header | Объявленные и обязательные возможности |
| framing-v1 | concept | Маркер гарантии framing текущего среза |
| STOMP ERROR message | concept | version, capability, syntax, limit, unsupported-command, handshake-timeout, heartbeat-timeout |
| WireHandshakeProfile | concept | Google Benchmark runner в защищённом WireContractTest.cpp, ручное NFR evidence |
| WireContractTest.CapabilityNegotiation | test-suite | Полная wire-матрица и реальный STOMP loopback |
| WireContractTest.WireConnectionBoundary | test-suite | События, lifecycle, детерминированный writer и ресурсы |

## Rollback

Откатывается сетевой срез и подготовка тестовых целей. Форматы хранения,
ядро JMS и сохранённые данные не меняются.
