# Jakarta Session Delivery

## Purpose

Локальный C++ контракт сессии: сериализованная обработка MessageListener и
CompletionListener, завершение отправок, атомарная фиксация потреблённых входов
и опубликованных выходов локальной транзакции, порядок отправок одной сессии.
Применяется к очередям и подпискам в пределах гарантий SDD72; lifecycle соединения,
заголовки, маршрутизация и политика повторов определены соседними capabilities.

## Requirements

### Requirement: Listener и граница сессии
Consumer::setMessageListener SHALL регистрировать callback с аргументом Message::Ptr или снимает
его пустым callback. Сессия исполняет один callback MessageListener за раз для всех её
consumer. При остановленной connection новые callbacks не стартуют. Внешний stop и close consumer, session и connection
ждут соответствующие активные callbacks. Сервис connection и session доступен выполняющемуся
listener до его выхода. Listener может закрыть свой consumer: текущий callback завершается
и его ack обрабатывается по обычному правилу. Вызов stop или close собственной connection
либо close собственной session из MessageListener отклоняется IllegalStateException.
Исключение listener немедленно открывает ту же доставку для повторного вызова в AUTO/DUPS_OK
с признаками редоставки. Явно настроенная политика tiny-mq действует на этот повтор. В CLIENT
запись остаётся неподтверждённой до ack/recover, а в TRANSACTED до commit/rollback. В обоих
режимах само исключение допускает следующую доставку.

ID: `REQ-72-3`
Источник: `repo:docs/jms-spec/20-message-listener.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.5–6.2.6, 6.2.13, 8.7–8.8.

#### Scenario: ListenerModeMatrix

- **WHEN** два consumer одной сессии конкурируют, listener бросает runtime-ошибку в каждом режиме, закрытие вызывается из допустимых потоков
- **THEN** callbacks одной сессии не перекрываются, stop ждёт открытый callback, consumer self-close
  завершает текущую доставку, self-stop и self-close session/connection дают IllegalStateException,
  а таблица режимов подтверждает исходы как для обычного возврата, так и для исключения
- Test: `DeliveryContractTest.ListenerModeMatrix`

#### Scenario: ClientAndTransactedListenerFailureRetainsInput

- **WHEN** первый listener бросает в CLIENT или TRANSACTED для обеих надёжностей; следующая и side-доставка завершаются, listener снимается и создаётся новый consumer
- **THEN** failed input недоступен до явного recover/rollback, затем приходит с исходным телом, reliability и парой redelivered=true/JMSXDeliveryCount=2; ack/commit происходит после этой проверки
- Test: `DeliveryContractTest.ListenerModeMatrix`

### Requirement: Завершение асинхронной отправки
Producer::sendAsync SHALL принимать сообщение и CompletionListener, с обычными настройками либо
явными SendOptions. CompletionListener принимает SendCompletion. C++ async send проверяет
аргументы и состояние до принятия. Ошибка до принятия возвращается
вызывающему потоку без callback. Для принятой операции ровно один callback получает success
и снимок заголовков либо ошибку операции. Success означает достижение той же границы
надёжности, что у sync send. Для транзакции это принятие отправки в текущую транзакцию,
успех commit наблюдается через сам commit. Исторический запрет SDD 21 на транзакционные async send заменяется правилом §7.3.4.
Контракт ограничен локальной транзакцией C++; Java EE относится к SDD 76.
Сессия вызывает не более одного callback
завершения одновременно и на потоке, отличном от вызова send. Порядок callbacks каждого
producer совпадает с порядком принятых async send. Порядок смешанных send задан REQ-72-8.
Исключение из пользовательского callback не превращает уже сообщённый исход в новый callback
и не препятствует завершению барьера. Попытка из callback закрыть свой producer/session/connection
или вызвать commit/rollback своей session даёт IllegalStateException. Внешние close producer,
session и connection и commit/rollback session ждут подходящие незавершённые операции и возврат
их callbacks. Они не удерживают блокировку, нужную этому callback для завершения.

ID: `REQ-72-4`
Источник: `repo:docs/jms-spec/21-completion-listener.md@9f7983c0054636a6686f9928831c8c53dfbd3679#semantics`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 7.2–7.3.

#### Scenario: AsyncSendBarriers

- **WHEN** два producer одной сессии смешивают sync/async, поздний и немедленный отказы вводятся
  отдельно, callback пробует запрещённый повторный вход и удерживается перед close/commit/rollback.
  Проверяются транзакционный и обычный режимы
- **THEN** проверяются порядок, эксклюзивность на сессию, поток callback, отсутствие callback при ошибке
  вызова, видимость заголовков и барьеры завершения. Успех callback принятого транзакционного send
  предшествует решению commit, а rollback исключает публикацию
- Test: `DeliveryContractTest.AsyncSendBarriers`

### Requirement: Атомарная локальная транзакция
Локальная C++ транзакция сессии SHALL фиксировать одним решением подтверждение всех её потреблённых входов
и публикацию всех её выходов, включая обычные очереди, топик с durable-подписками и смесь
persistent и non-persistent. Граница публикации при живом процессе общая для всего набора:
конкурирующий claim/browser не может получить выход, пока подготовка остальных ещё может
прервать транзакцию. Фильтры и подписчики топика выбираются при commit согласно ADR-0009.
Rollback или close до решения удаляют выходы и возвращают входы по политике повтора.
Ошибка до решения оставляет доступным rollback. После устойчивого решения откат
запрещён, прерванное применение завершается через replay до допуска новых операций.
Успешный commit возвращается после публикации. При аварийном завершении до возврата
ответа вызывающий может не знать исход, но хранилище имеет ровно одно решение.
При рестарте атомарность распространяется на persistent входы и выходы; non-persistent
могут исчезнуть как допускает REQ-72-7. Закрытие consumer/producer по отдельности
не отменяет работу транзакции сессии.

ID: `REQ-72-6`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#снимок-состояния`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.7, 6.2.15, 9.1.

#### Scenario: AtomicConsumeAndSend

- **WHEN** транзакция читает очередь и отправляет в две цели, включая durable fan-out; проверяются
  обычный commit и rollback, инъекция ошибки до решения, остановка процесса до решения, после него
  до применения и между операциями применения, затем повторный рестарт после квитирования
- **THEN** до решения видны только вернувшиеся входы, после решения видны все положенные persistent
  выходы без входов; повторное применение не дублирует доставку и не воскрешает подтверждённое.
  Живая транзакция проверяется также для non-persistent и смеси режимов
- Test: `DeliveryContractTest.AtomicConsumeAndSend`

#### Scenario: CloseActiveTransaction

- **WHEN** транзакция уже потребила вход и подготовила queue и durable выходы; выполняется transaction.close() для persistent/non-persistent/mixed
- **THEN** возвращается ровно один вход, выходы очереди и обеих durable-подписок отсутствуют; лимит и backoff возврата задаёт REQ-72-7
- Test: `DeliveryContractTest.AtomicConsumeAndSend`

### Requirement: Порядок отправок одной сессии
Для одной локальной C++ сессии и цели порядок SHALL иметь общую последовательность принятия send через
разные producer. Она сохраняется между sync/async и в выходах commit. Более высокий
приоритет и более ранняя доступность могут переместить запись вперёд. Гарантия порядка
относится к отправкам одного режима доставки; non-persistent записи могут выпасть при
отказе. Разные сессии и разные назначения не имеют общего гарантированного порядка.
Селекторы и независимые подписки сохраняют порядок подходящих записей.

ID: `REQ-72-8`
Источник: `repo:tasks/UNIFIED-PLAN.md@9f7983c0054636a6686f9928831c8c53dfbd3679#сквозные-требования-на-всех-майлстонах`

Основание поведения — [Jakarta Messaging 3.1](https://jakarta.ee/specifications/messaging/3.1/jakarta-messaging-spec-3.1.html), § 6.2.9, 7.6.

#### Scenario: SessionSendOrder

- **WHEN** два producer одной сессии отправляют в одну цель последовательность с одинаковыми priority, режимом и временем доступности, затем отдельно проверяются допустимые исключения.
- **THEN** порядок для однородной последовательности совпадает с порядком send, перестановки ограничены ветвями §6.2.9.
- Test: `DeliveryContractTest.SessionSendOrder`
