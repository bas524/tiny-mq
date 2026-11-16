# Контракты локального C++ API

Архитектурное решение описано в [ADR-0009](../../../../arch/0009-destination-claim-and-ownership.md).
Сигнатуры принятых методов приведены по [Session.h](../../../../server/src/Session.h),
[QueueBrowser.h](../../../../server/src/QueueBrowser.h) и [Destination.h](../../../../server/src/Destination.h).

```cpp
// Session
Consumer::Ptr createConsumer(const Destination::Ptr& destination,
                             const std::string& selector, bool noLocal);
Consumer::Ptr createDurableConsumer(const Destination::Ptr& topic, const std::string& name,
                                    const std::string& selector, bool noLocal);
Consumer::Ptr createSharedConsumer(const Destination::Ptr& topic, const std::string& name,
                                   const std::string& selector = {});
Consumer::Ptr createSharedDurableConsumer(const Destination::Ptr& topic, const std::string& name,
                                          const std::string& selector = {});
void unsubscribe(const std::string& name);
QueueBrowser createQueueBrowser(const Destination::Ptr& queue, const std::string& selector = {});
TemporaryDestination::Ptr createTemporaryQueue();
TemporaryDestination::Ptr createTemporaryTopic();

// QueueBrowser
std::vector<Message::Ptr> enumerate() const;

// TemporaryDestination
void remove(Connection& connection);
```

Пустой селектор снимает фильтр. Стандартная unshared durable использует
четырёхаргументную перегрузку, прежние перегрузки с двумя или тремя аргументами
открывают legacy-подписку. Семантика QueueBrowser из SDD выражена в C++ вызовом
`enumerate()`. В принятом локальном контракте граница отправки во временное
назначение определяется принадлежностью одному брокеру. Сетевая авторизация
относится к [SDD 73](../../../../docs/jms-spec/73-jakarta-broker-transport.md), а Java
`QueueRequestor` и `TopicRequestor` относятся к [SDD 74](../../../../docs/jms-spec/74-jakarta-java-se-provider.md).
