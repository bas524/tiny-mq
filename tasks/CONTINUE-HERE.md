# Следующий шаг tiny-mq

Класс K2. Обновлено 2026-11-15.

1. Проверить редакцию [INT-01](../requirements/intents/INT-01-jakarta-messaging.md) и профиль выпуска.
2. [SDD 70](../docs/jms-spec/70-jakarta-message-contract.md) получила
   [`clean` в раунде 3](../docs/reviews/70-jakarta-message-contract.critique.md#раунд-3).
   Следующая стадия — реализация по проверенной редакции 0.3 в чистой рабочей копии.
3. После допуска двигаться по [зависимостям INT-01](../requirements/intents/INT-01-jakarta-messaging.md#декомпозиция).
   До 71 решить изменение инварианта одного consumer на очередь в ADR-0005.
4. При подготовке 72 воспроизвести расхождение send/commit timing из
   [аудита](../docs/reviews/2026-11-15-jakarta-baseline.md#проверенные-расхождения).
5. Для [SDD 78](../docs/jms-spec/78-jakarta-high-availability.md) и
   [SDD 77](../docs/jms-spec/77-jakarta-conformance-release.md) определить испытательные нагрузки,
   абсолютные цели производительности и допустимое время восстановления.

[UNIFIED-PLAN.md](UNIFIED-PLAN.md) хранит старую раскладку JMS 2.0 и закреплённые источники
новых постановок. [Карта Jakarta Messaging 3.1](../docs/jms-spec/README.md) отражает новый объём работ.
Сборка, профильные команды, допуск и следы проверки описаны в [PROJECT.md](../PROJECT.md)
и [AGENTS.md](../AGENTS.md).
