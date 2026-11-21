# Выполнение миграции

## Autonomy Level
R1 — отдельные решения Owner о постановке, архитектуре, результате и активации.
Подготовка локального контракта, критериев и изолированных проверок разрешена
поручением Owner. Новая архитектура и окончательный результат принимаются
Owner отдельно. Commit черновика не приёмка. Публикация/merge требуют
применимого отдельного решения; направление из другого чата их не разрешает.

## Stop-conditions
Нет решения о бизнес-смысле/архитектуре, независимого разбора критериев,
подтверждённой изоляции или точного источника — зависимая реализация закрыта.
429, no_progress, штатные лимиты, необходимость расширить разрешённые сервисы
или ослабить шлюз — остановка с сохранением checkpoint. Нельзя запускать M05
и следующую продуктовую SDD на старой цепочке.

## Evidence
До реализации: точная Git-редакция proposal, независимый разбор полного комплекта
и критериев, решения Owner, закреплённые до реализации тесты и red baseline.
После: старые regression tests + новые unit/integration/negative tests; native
sandbox и реальные прямые профили; независимое ревью и безопасность; сквозной
проход CLI без пропуска зарезервированных решений; полный путь к факту/аудиту.
Полные логи/пакеты — handoffs/current/79; в MR только связный контракт и код.
Длительный аудит после приёмки — стабильная копия handoffs/archive, не только
каталог временной рабочей копии.

## Work
- [x] Изучить предложение и актуальное состояние Timeline; реализация замены там ещё не принята.
- [x] Создать чистую ветку tiny-mq, закрепить черновой proposal до технических критериев.
- [x] Подготовить исполняемые критерии и red baseline, независимо проверить контракт.
- [x] Получить отдельные решения Owner о смысле/архитектуре и дополнении C1–C6.
- [ ] Реализовать native adapter/runtime и точный допуск исключений, сохранив старые проверки.
- [ ] Исправить ссылки/исторические пометки SDD72 и проверить archive.
- [ ] Пройти независимые review/security, реальные sandbox/provider smoke и сквозной маршрут.
- [ ] Получить Owner-приёмку, переключить вход, зафиксировать полные расходы/вмешательства.

## Owner decisions
```json
[
  {
    "kind": "architecture",
    "status": "approved",
    "source": "handoffs/current/79/owner-architecture-decision.json",
    "subject": "original architecture d7997ddf"
  },
  {
    "kind": "architecture",
    "status": "approved",
    "source": "handoffs/current/79/owner-amendment-decision.json",
    "subject": "C1-C6 and task-only36/7/6"
  },
  {
    "kind": "acceptance",
    "status": "pending",
    "source": null,
    "subject": "result after C1-C6 fixes"
  },
  {
    "kind": "archive",
    "status": "pending",
    "source": null,
    "subject": "separate archive/activation"
  }
]
```

## Results
```json
{
  "status": "blocked",
  "code_head": "5683847c258a1042850b3d6a561f4b361d4882fc",
  "evidence": [
    "handoffs/current/79/logs/delegation-probe/result.json"
  ]
}
```
