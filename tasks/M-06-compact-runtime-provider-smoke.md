# M-06 — реальное исполнение provider smoke сокращённой оснастки

Статус: OPEN. Задача сопровождения по независимым review/security и реальным
операторским проверкам change79. Продуктовый код не затрагивается.

## Symptom
Реальный Runtime.provider_probe(79-demo, reviewer) останавливается до API:
сгенерированный guard.py содержит незакрытую строку в print, line26.
Отдельный native negative integration PASS не покрывает эту генерацию.

## Spec ref
 tiny-mq/79:REQ-79-4@ba86aa9; REQ-79-2 (наблюдаемое завершение ошибки).

## Cause
Шаблон строкового Python-скрипта не сохраняет закрывающую кавычку marker.
После исправления дополнительно проверить настоящие локальные DeepSeek
профили: реальная авторизация через уже настроенный command/args helper,
DEEPSEEK_API_KEY в окружении отсутствует. Совместимость пока не доказана.

## Done criteria
- Сгенерированный guard компилируется, настоящий native guard проходит;
  отрицательные проверки symlink/atomic replace/credentials сохраняются.
- Реальные OpenAI и direct DeepSeek Pro выполняют именно защищённый guard;
  observer сохраняет команды, exit, JSONL и hash в стабильной области.
- Ошибка не оставляет ложного running либо свежего разрешения; checkpoint
  допускает только проверенное восстановление. Все попытки сохранены.
- Пройдены sealed criteria и независимые review/security. Критерии не ослаблять.

## Scope-lock
.claude/chain/workflow_runtime.py, по найденным связям workflow_policy.py/aef.py
в пределах утверждённой реализации79. Критерии и контракт sealed неизменны.

## Evidence
handoffs/current/79/logs/operator-native-smoke.log — native negative PASS.
handoffs/current/79/logs/operator-provider-profile-result.json — rejected.
Сохранённая fixture в logs/operator-provider-fixtures, raw native JSON и
сгенерированный guard.py дают SyntaxError до запуска API. Secret bytes не читались.

Второй независимый операторский repro CLI-конфигурации (без регистрации receipt)
показал ещё более ранний отказ: wire_api="chat" не поддерживается Codex0.170.0.
Exit1 до API. Свидетельство logs/operator-provider-auth-compat.json.
Существующий прямой профиль работает с другим wire_api; авторизация helper,
а env_key-совместимость остаётся неподтверждённой. Использовать реальный профиль,
не подставлять устаревший провайдерный формат/новую переменную API-ключа.

Оператор воспроизвёл ещё один красный mechanical gate:
REQ_ID_PATTERN='REQ-[0-9]{2}-[0-9]+' python3 -B .claude/chain/trace.py tests
openspec/changes/79-aef-journey-harness/proposal.md .claude/chain/tests
даёт exit1: легитимный REQ-72-9 объявлен сиротой. Source trace должен учитывать
канонические legacy SDD/факты, а не удалять ссылку из защищённого теста.
REQ79-1/7; evidence logs/operator-native-trace-tests.log. Дополнительный scope
trace.py уже входит в утверждённые восемь файлов. Это не waiver красного gate.

Независимая безопасность: SEC-F1 rejected (профиль); SEC-F2/3 — необходимые
внешние native/provider свидетельства; SEC-F4 — недостижимое диагностическое
сообщение legacy archive guard. Полный пакет history/security-r1/output/result.json.
Reviewer approved относится к коду и не отменяет перечисленные красные проверки.
После исправления нужны свежие read-only review/security и полный real smoke.

## Дополнительный дефект control plane
Оператор воспроизвёл выполнение изменяемого workflow_runtime.py как host exit
observer между модельными стадиями. На безопасной fixture замена объявленного
source target записала marker вне reviewer output до появления exit-receipt.
Модели/секреты/реальный пользовательский root не затрагивались; API calls=0.
Evidence handoffs/current/79/logs/operator-mutable-observer-repro.json.
REQ79-4: waiter обязан быть неизменным снимком control plane, недоступным модели,
не импортировать изменяемые project-модули и сохранять подлинные exit/cleanup.
Нужен постоянный регрессионный тест до исправления и независимая критика критерия.
