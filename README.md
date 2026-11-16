# tiny-mq

Экспериментальный C++20 брокер сообщений с локальным хранилищем.
[Начало работы](START-HERE.md) · [Намерение](requirements/intents/INT-01-jakarta-messaging.md) ·
[Спецификации Jakarta Messaging](docs/jms-spec/README.md).

- [`server/`](server/) содержит C++-ядро брокера и его конфигурацию.
- [`clients/java/`](clients/java/) содержит Java-модуль сообщений Jakarta Messaging.
- [`tests/`](tests/) содержит проверки C++ и Java и эталонные данные контракта.
