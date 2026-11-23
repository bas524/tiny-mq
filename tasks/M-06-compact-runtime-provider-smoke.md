# M-06 — реальное исполнение provider smoke сокращённой оснастки

Статус: OPEN. Задача сопровождения по независимым review/security и реальным
операторским проверкам change79. Продуктовый код не затрагивается.

## Проверка evidence наблюдённого результата

Сверка с общим китом `10b5db8` выявила, что хеш итогового JSON подтверждает
его байты, но Runtime принимал указанные моделью пути без проверки файлов.
Исправление распространяется на обычное завершение и восстановление результата
того же исполнения после прерывания координатора.

Если роль указывает `evidence`, каждый путь должен вести к непустому обычному
файлу в её каталоге вывода. Допустим путь от этого каталога или абсолютный путь
внутри него. Ссылки и выход из каталога запрещены. Неверный путь блокирует
переход до формирования карточки следующего решения. После `resume` проверка
повторяется перед использованием сохранённого результата.

Контроли: `ObservedEvidenceTest.test_observed_role_evidence` и
`ObservedEvidenceTest.test_recovered_role_evidence`. Восстановление валидного
результата сохраняет одну исходную попытку. Используются локальные фикстуры.

## Дополнение 2026-11-20: дефекты перед активацией

Проверенная вершина: `5683847c258a1042850b3d6a561f4b361d4882fc`.
Поручение из книжного чата — источник замечаний, не новое решение Owner о классе,
архитектуре, приёмке или активации. Прежний запрос приёмки снимка `a62ce29…`
отозван координатором до устранения находок. Технические вердикты по прежнему
снимку сохраняются как история; новым исправлениям не приписываются.

Воспроизведение: `handoffs/current/79/delegation-probe.py`; полный вывод
`handoffs/current/79/logs/delegation-probe-run.log`; отдельные trace/legacy логи
и машиночитаемые результаты в `handoffs/current/79/logs/delegation-probe/`.
Ни одного вызова модели, реального решения Owner или изменения production-кода.
Все данные — отдельные временные Git-репозитории, удалённые после проверки.

| ID | Причина и фактическое наблюдение | Предлагаемое исправление / контроль |
|---|---|---|
| M06-C1 | `field()` проверяет только пустоту и `{{`. Независимые случаи R9, Architecture=TODO и произвольный Entity inventory принимаются reader, trace sdd/input и чистым production prepare. | Явный R1/R2/R3; замкнутые заглушки и структурный разбор с учётом fenced code/комментариев; реестр по локальной таблице «Сущность / Kind / Роль» и ENTITY_KINDS из PROJECT. Не копировать Timeline-значения и его формат таблицы. |
| M06-C2 | `resolve()` и `read_text()` читают symlink design.md вне репозитория; reader/trace/prepare принимают его. | До разыменования проверять принадлежность входов корню и отсутствие ссылок во всех компонентах пути; также для sources, specs и metadata. Запретить незаявленные/пропущенные delta-файлы, дубликаты полей/сущностей, неизвестные Kind. Негативные случаи для ссылки файла, каталога, исходника и выхода за корень. |
| M06-C3 | `contract_hash=digest(raw file hashes)`; [ ]→[x] и Owner decisions меняют контракт, `_fresh` блокирует. Поздний acceptance уже не меняет contract_hash. | Версионированный отпечаток постановки отдельно от точных материалов. Только в Work нормализовать три маркера [ ], [x], [X]; Results/Owner decisions выделить в строго проверяемые исполнительные данные. Текст задач, поля прав/границ/критериев/источников и неизвестные разделы остаются значимыми. Никакой семантической эвристики. |
| M06-C4 | Обновление статусов tasks/design отложено, поэтому там всё ещё нет реально полученного решения об архитектуре и результатов работы. | После согласованного разделения отпечатков обновить существующие tasks/design по точным источникам и логам: архитектура утверждена, gates пройдены на старом снимке, новые дефекты OPEN, приёмка/активация pending. Не объявлять завершённым неисполненное и не дублировать ручную сводку. |
| M06-C5 | Production registry содержит только harness, но чистый prepare допускает server/product.cpp; NFR остаётся not_applicable. Owner-граница всё ещё закрыта. | Явно блокировать targets вне проверенной области профиля. C++ профиль до собственной проверки unavailable, без автоматического fallback на harness. Сейчас подготовить только план команд и ограничений, продуктовый код не менять. |
| M06-C6 | В файловой archive-фикстуре rebase сохраняет адресаты Markdown-ссылок и исходный Git source, но старый task_ref отсутствует, load_contract архива падает invalid schema/task_ref. | Различать активный допуск и историческое чтение; сохранять оригинальные байты и Git-редакции до переноса, отдельно проверять перенесённые ссылки/metadata. Историческое чтение не открывает prepare/исполнение/новый допуск. Нужны отдельные отрицательные контроли на испорченный origin/manifest/source и настоящий archive guard. |

### Исправление данных observer-фикстуры — 2026-11-21
Author6 остановился честным paused: C5 отвергает три контроллера .claude/chain,
которые старый положительный observer-тест копирует в fixture, поскольку её
code_paths их не покрывает. Координатор добавил существующий каталог
.claude/chain в профиль только WorkflowFixture; production profile/C5 не
ослаблены. Все методы критериев, их assertions и код кандидата неизменны.
Raw author package и его failed log сохраняются; поправка данных не выдаётся
за новый модельный produced. Механическое завершение проверок фиксируется
отдельно; независимый Reviewer обязан проверить поправку и собственный прогон.
Основание — согласованное поведение C5 и положительного observer-контроля,
а не изменение требований. Evidence: handoffs/current/79/fixture-correction.json
и fixture-correction.patch. Дополнительный запуск модели/итерация автора не нужны.

### Границы результата воспроизведения

Четыре отрицательных контроля (пустые Architecture/Autonomy/Inventory и
несуществующая редакция source) отклонены reader/trace/prepare/legacy binding.
Для R9/TODO/произвольного inventory и symlink legacy record/check также проходит
**только при явно синтетическом clean-заявлении в временном репозитории**.
Это воспроизводит отсутствие механической проверки, а не независимый вердикт.
На настоящем next после prepare во всех принятых случаях остаётся Owner business
card, dispatches=0. Обход реального Owner или независимого критика не установлен.
Полный production Runtime.archive не выполнялся; показана только файловая
фикстура переноса и отказ исторического читателя, а не повреждение реального архива.

### Конкретный план изменения контракта (утверждён Owner 2026-11-20)

1. Доработать существующие REQ-79-1/5/6/7 и design; бизнес-источник proposal и
   общий INT-01 сохраняются. Уровень автономии обозначить R1 явно, сохранив
   существующие отдельные решения Owner. Не открывать исследовательский маршрут.
2. `contract_hash` сделать версионированной детерминированной проекцией постановки.
   `files` оставить точными хэшами байтов; добавить точный material hash для
   материалов приёмки (включая позднее объяснение и проверяемые ссылки решений).
   Только чекбоксы Work и структурные исполнительные записи исключаются из
   постановки. Изменение текста или перенос критического поля в исключённый
   раздел обязаны отклоняться либо инвалидировать допуск, не исчезать из проверки.
3. Исправить не только load_contract, но и native current_input/critic.input:
   сейчас `sdd_snapshot` тоже хэширует сырые tasks. Новый planned snapshot
   проверять на входе legacy gates, сохраняя исходный сырой снимок критика и
   источники. Старый формат не переинтерпретировать как новый допуск.
4. Runtime._fresh и Owner cards связываются с соответствующими предметами;
   приёмка связывается с точными текущими материалами. Raw receipts/логи не
   переписывать. Проверки, реально читающие изменённые материалы, повторяются;
   обновление статуса само по себе не требует новой критики архитектуры.
5. Сначала закрепить красные критерии для C1–C6, затем критика **полного**
   исправленного контракта/критериев в новой сессии direct DeepSeek Pro.
   Автор — OpenAI; независимые Reviewer/Security direct DeepSeek Pro с
   собственными прогонами, conformance OpenAI, отдельная NFR applicability,
   затем DocWriter/Explainer. Модели и проверки не ослаблять.
6. Повторить prepare→Owner stop, legacy input/admission, точные source/Owner
   negative controls, namespace двух worktrees, защищённый observer и read-only
   стадии. Fixture никогда не импортируется как настоящий допуск. Полный
   production archive/активация остаются за реальными решениями и guards.

Предполагаемый target_files для согласования до реализации: workflow_contract.py,
workflow_runtime.py, route.py, trace.py, openspec.py, workflow-checks.json и
существующие tests/test_workflow_contract.py, test_workflow_runtime.py,
test_workflow_integration.py в `.claude/chain/`; канонические design/tasks/spec
change79 и M-06. Новые зависимости и продуктовые исходники не входят.
Если связь не требует правки файла, не менять его ради списка.

### План C++ профиля — НЕ ГОТОВ

- Проверки: точные BUILD_CMD/BUILD_RELEASE_CMD/TEST_CMD из PROJECT, отдельный
  clang-tidy target при доступном LLVM; Linux/macOS сборка/тесты/дистрибутив,
  ASan и Java contract — все шесть текущих required CI checks. Настроенные
  CI jobs не заменяются Python harness tests. Перед применением сверить команды
  с текущими CMakePresets/CI, toolchain и фиксированным vcpkg.
- Production write scope: только объявленные исходники; inputs, тесты-критерии,
  роли, состояние, .git и credentials защищены. Writable build/log каталоги
  независимы для стадий; CMake outputs должны соответствовать разрешениям.
  Проверить symlink/atomic replace, генераторы и побочные записи configure/build.
- Горячие пути: baseline и candidate на одной локальной машине, один toolchain,
  ABBA, 9 повторов, CPU median/op +5%, CPU CV≤5%, CPU storage workers;
  persistent/non-persistent, обычная отправка и commit. По PROJECT/nfr-gate и
  M-05: тихое окно ещё не согласовано, ночной job не настроен, GitHub perf
  отключён. Waiver SDD72 не переносить. Непригодный/непроведённый замер — не pass.
- До реальных собственных прогонов/изоляционных проверок и Owner-решения
  профиль остаётся unavailable; изменения C++ не запускаются через harness.

### Оставшиеся решения Owner и лимиты

Локальная доработка C1–C6 и разовые пределы 36/7/6 утверждены прямым ответом
Owner msg_01a1f9b3-151d-7ba2-ad3a-5d90c4eade20; точный источник проверен
в handoffs/current/79/owner-amendment-decision.json. Использованы26/36 dispatch,
6/7 critic,5/6 author; итоговая приёмка/активация не даны.
План содержит восемь обязательных стадий: Critic, Producer, Reviewer, NFR,
Conformance, Security, DocWriter, Explainer (до34); два запуска можно разрешить
только как инфраструктурный резерв (до36), без дополнительных итераций автора
или критика. Разрешённые пределы: critic7, author6, dispatch36. Глобальные12/5/5
не меняются; резерв не даёт ещё одной итерации критика или автора.
После исправлений обязательна новая конкретная приёмка результата; нынешняя
передача в приёмку отозвана, итогового merge или факта нет.

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
