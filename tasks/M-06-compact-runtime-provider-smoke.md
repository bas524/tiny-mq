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

Дополнительный фактический repro загрузки существующего provider-layer:
logs/operator-provider-layer-compat-r2.json — rejected без API. Доверенный
профиль содержит штатное supports_websockets=false; whitelist/parser ошибочно
его отвергает. Исправить stdlib-разбор/явную безопасную валидацию, сохраняя
этот параметр в effective provider config и существующий auth-helper.
Observer regression усилен по round4 и принят round5 content-clean: реальный
child nonce/exit7, subject/execution/binary/log hash и cleanup private context.
Proposal/design и бизнес-архитектура Owner не изменялись; это критерий исходного
REQ79-4. Критерии фиксируются до зависимого исправления control plane.

## Проверка исправления перед повторными review/security
Кандидат fix3: standalone observer snapshot без mutable imports; stdlib TOML
с supports_websockets=false. Обязательные25 criteria (два платных/native opt-in
в базовом прогоне skipped) и10 дополнительных проверок прошли у автора.
Оператор затем реально выполнил ОБА provider smoke OpenAI/directDeepSeek Pro:
`handoffs/current/79/operator-real-probes-latest.json`, technical_verdict=passed,
source hashes unchanged, private auth contexts remaining=[]; все raw logs и
receipt находятся в сохраняемой fixture. Это не Owner acceptance и не закрытие
M06 до свежих независимых review/security. Порог/модели/источники не ослаблены.

## Возврат после ee33050
Reviewer r2 approved, Security r2 approved; Conformance rejected:
F1 next пропускает performance_verdict=rejected при technical passed,
F2 audit_location совпадает у связанных worktrees одного change.
Репродукции conformance-output/probes.py и evidence/additional-probes.log.
Оператор дополнительно воспроизвёл разрешение producer на запись канонической
.claude/agents/reviewer.md через declared targets: logs/operator-role-input-scope.json.
Это нарушение существующих REQ79-3/4/6, а не новые требования. Нужен ещё один
кодовый проход и свежие review/security/conformance. Регрессионные случаи
добавляются до исправления, исходные25 принятых тестовых методов не меняются.

Совместимость _debt с каноническим markdown-статусом M05 тоже проверяется:
'- **Статус:** открыта.' должен распознаваться как open, closed/unknown не open.
Добавлены четыре программные регрессии реализации при неизменных SDD и телах
25 принятых критериев (proof в regression-extension-proof.json). Никаких новых
требований/изменений архитектуры/повышения лимита критики.

## Исправление 4: операторская проверка
Четыре регрессии исправлены в workflow_runtime.py; исходные25 тестовых методов
и пять канонических файлов change79 побайтно сохранены. Проверки оператора:
workflow26 (два opt-in skipped), policy3 и legacy2 — exit0; trace tests и
OpenSpec validate — exit0. Свидетельства: handoffs/current/79/logs/operator-r4/checks.json.
Реальные защищённые OpenAI/directDeepSeekPro QA повторены для изменённых восьми
source hashes: logs/real-probes-20261119T180733Z/result.json — passed,
source_unchanged=true, private_contexts_remaining=[]. Это отдельные синтетические
проверки транспорта/изоляции; ими не подменяются независимые роли или приёмка.
M06 остаётся OPEN: прежние review/security относятся к предыдущему снимку,
а conformance rejected требует свежей проверки после исправления. Потолок
12 переходов достигнут, запрос разового18 для change79 ожидает Owner.

## Финальный блокер: native trace / legacy admission
На финальном explained маршрутизатор отказал: critic.input.json отсутствует.
Операторская репродукция logs/native-admission-compat-repro.json показывает
дополнительную несовместимость: native trace input.intent имеет пять бизнес-полей,
route._doc ожидает ref/path/pinned и даёт KeyError('ref'). Независимые verdict
по коду4089ae8 сохранены, но цепочка машинно НЕ завершена и пакет НЕ принят.
R5 trace contract_hash и нынешний native contract_hash совпадают, intent тоже
не менялся. Новый разбор требований не подменяет исправление протокола.
Нужен совместимый, проверяемый мост к admission с привязкой полного native
контракта и сохранённого фактического входа критика; нельзя сочинять разрешение,
снимать проверку источника или выдавать content-clean за уже выданный допуск.
Перед кодом закрепить регрессию; исходные29 методов/канонический комплект не
ослаблять. Затем author5 и свежие independent review/security/conformance,
актуальные applicability/docs/brief. Лимит18 исчерпан; новые CLI-стадии до
отдельного Owner-разрешения не запускать.

## Исправление5: проверен механический маршрут
Изменён только route.current_input: native INT5 остаётся в raw trace и
native_context, legacy identity привязана к хэшу полного канонического
контракта; fingerprint охватывает все его файлы. Подмена/устаревание блокируются.
Оператор повторил30 runtime/policy тестов (2 opt-in skipped), legacy2 и
сквозной synthetic CLI-маршрут из30 команд до настоящего рубежа человека,
с отрицательными случаями. Evidence logs/operator-r5/checks.json.
Фикстура не импортируется как допуск. Реальный witness R5 admission:false
остаётся неизменённым. Owner отдельно разрешил6й раунд только79 при общем26;
ожидается новый действительный пакет критика, затем независимые проверки.
