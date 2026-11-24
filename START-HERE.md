# tiny-mq

Класс K2. Состояние на 2026-11-23.

- [INT-01](requirements/intents/INT-01-jakarta-messaging.md) — семантика JMS для приложений на разных языках.
- [Карта Jakarta Messaging 3.1](docs/jms-spec/README.md) — SDD 70–78, включая критерии выпуска и профиль высокой доступности.
- [Базовый аудит](docs/reviews/2026-11-15-jakarta-baseline.md) — проверенный C++ baseline, известные расхождения и внешний аудит.
- [PROJECT.md](PROJECT.md) — значения проекта, команды и инварианты.
- [Калибровка AEF](.claude/chain/CALIBRATION.md) и [руководство для Codex](AGENTS.md) — роли, проверка и запуск.
- [Следующий шаг](tasks/CONTINUE-HERE.md) — M-05 / SDD 73 по зависимостям после функциональной приёмки 70–72; REQ-72-9 остаётся открытым.

Ядро C++20 собирается в `tiny_mq` и содержит очередь, топик, подписки, локальные транзакции,
хранилище, ack/recover, сериализованные callbacks, атомарный commit/replay и C++ тесты. Сетевой брокер, Java-провайдер, XA, Jakarta EE и кластерный профиль внесены в постановки.
[OpenSpec](openspec/README.md) фиксирует принятые факты. [ADR](arch/README.md) задают
ограничения дизайна и условия пересмотра.

## Оснастка AEF

Единый вход `python3 -B .claude/chain/aef.py <operation> <change>` активирован.
По умолчанию используется `harness`. Профиль `cpp` для `server/src` выбирается явно. [Приёмка M‑06](tasks/M-06-compact-runtime-provider-smoke.md).
[Контракт](openspec/changes/archive/2026-11-23-79-aef-journey-harness/proposal.md) · [Спецификация-факт](openspec/specs/aef-workflow/spec.md).
