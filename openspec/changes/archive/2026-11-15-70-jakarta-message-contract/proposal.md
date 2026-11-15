# Контракт сообщений Jakarta Messaging 3.1

Класс K2.

[SDD 70](../../../../docs/jms-spec/70-jakarta-message-contract.md) закрепляет типы,
заголовки и тела сообщений при подготовке к отправке, хранении и локальной адаптации
Java. C++ вычисляет селекторы по типизированным значениям. Локальный Java-модуль
предоставляет шесть видов сообщений Jakarta Messaging 3.1. Независимые C++ и Java
проверки используют общие типизированные эталоны.

Кандидат прошёл [ревью итерации 3](../../../../docs/reviews/70-jakarta-message-contract.review.md)
и [шлюзы NFR, conformance и security](../../../../docs/reviews/70-jakarta-message-contract.gates.md).
Сетевой транспорт и Java SE провайдер поставляются по
[SDD 73](../../../../docs/jms-spec/73-jakarta-broker-transport.md) и
[SDD 74](../../../../docs/jms-spec/74-jakarta-java-se-provider.md).
