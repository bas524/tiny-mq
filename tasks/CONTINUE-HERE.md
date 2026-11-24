# Следующий шаг tiny-mq

Класс K2. Обновлено 2026-11-19.

1. Проверить редакцию [INT-01](../requirements/intents/INT-01-jakarta-messaging.md) и профиль выпуска.
2. [SDD 70](../docs/jms-spec/70-jakarta-message-contract.md) принята на R1.
   Действующий контракт сообщений описан в [OpenSpec](../openspec/specs/jakarta-message-contract/spec.md).
3. [SDD 71](../docs/jms-spec/71-jakarta-destinations.md) принята на R1.
   Атомарная выборка и владение закреплены в [ADR-0009](../arch/0009-destination-claim-and-ownership.md).
4. [SDD 72](../docs/jms-spec/72-jakarta-delivery.md) функционально принята на R1: REQ-72-1…8,
   `DeliveryContractTest` 8/8; [контракт сессии](../openspec/specs/jakarta-session-delivery/spec.md),
   [ADR-0010](../arch/0010-atomic-local-delivery.md) accepted. Главы 6–8 покрывают текущую
   C++ доставку, ack, локальные tx и callbacks. REQ-72-9 открыт в
   [M-05](M-05-local-nightly-performance.md), технический perf rejected; исключение Owner
   разрешает PR №12 по обычному CI и не означает NFR done.
5. Дальше — валидные ночные замеры по [M-05](M-05-local-nightly-performance.md) и
   [SDD 73](../docs/jms-spec/73-jakarta-broker-transport.md) по зависимостям 70–72.
   Сеть, Java, XA, Jakarta EE и HA остаются будущими SDD 73–78.
6. Для [SDD 78](../docs/jms-spec/78-jakarta-high-availability.md) и
   [SDD 77](../docs/jms-spec/77-jakarta-conformance-release.md) определить испытательные нагрузки,
   абсолютные цели производительности и допустимое время восстановления.

[UNIFIED-PLAN.md](UNIFIED-PLAN.md) хранит старую раскладку JMS 2.0 и закреплённые источники
новых постановок. [Карта Jakarta Messaging 3.1](../docs/jms-spec/README.md) отражает новый объём работ.
Сборка, профильные команды, допуск и следы проверки описаны в [PROJECT.md](../PROJECT.md)
и [AGENTS.md](../AGENTS.md).

Оснастка change 79 активирована для `harness`. История решения и проверок
сохранена в [архиве](../openspec/changes/archive/2026-11-23-79-aef-journey-harness/acceptance.md). Перед продуктовой разработкой на
новой оснастке подготовить и проверить C++ профиль по [M-06](M-06-compact-runtime-provider-smoke.md).
M-05 и требования к продуктовой производительности остаются открытыми.
