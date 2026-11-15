# Следующий шаг tiny-mq

Класс K2. Обновлено 2026-11-15.

1. Проверить редакцию [INT-01](../requirements/intents/INT-01-jakarta-messaging.md) и профиль выпуска.
2. [SDD 70](../docs/jms-spec/70-jakarta-message-contract.md) принята на R1.
   Действующий контракт сообщений описан в [OpenSpec](../openspec/specs/jakarta-message-contract/spec.md).
3. Следующий шаг по [зависимостям INT-01](../requirements/intents/INT-01-jakarta-messaging.md#декомпозиция) —
   подготовить [SDD 71](../docs/jms-spec/71-jakarta-destinations.md).
   До реализации сверить требования к конкурирующим получателям с [потоковой моделью сессии](../arch/0005-session-threading-model.md).
4. При подготовке 72 воспроизвести расхождение send/commit timing из
   [аудита](../docs/reviews/2026-11-15-jakarta-baseline.md#проверенные-расхождения).
5. Для [SDD 78](../docs/jms-spec/78-jakarta-high-availability.md) и
   [SDD 77](../docs/jms-spec/77-jakarta-conformance-release.md) определить испытательные нагрузки,
   абсолютные цели производительности и допустимое время восстановления.

[UNIFIED-PLAN.md](UNIFIED-PLAN.md) хранит старую раскладку JMS 2.0 и закреплённые источники
новых постановок. [Карта Jakarta Messaging 3.1](../docs/jms-spec/README.md) отражает новый объём работ.
Сборка, профильные команды, допуск и следы проверки описаны в [PROJECT.md](../PROJECT.md)
и [AGENTS.md](../AGENTS.md).
