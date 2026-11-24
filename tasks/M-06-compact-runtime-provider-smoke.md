# M-06 — реальное исполнение provider smoke сокращённой оснастки

Статус — профиль C++ принят к активации. Задача сопровождения по review/security
и реальным операторским проверкам change 79.

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
  persistent/non-persistent, обычная отправка и commit. Расписание локальных
  замеров и данные см. в [M-05](M-05-local-nightly-performance.md). GitHub perf
  отключён. Для профиля нужны валидные замеры в пределах бюджета, waiver SDD72
  действует только для PR №12.
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


## Актуальный технический итог на 2026-11-21

Код `5f87b6b8fd67f9753faf74064bdc0abbeea5f67d`; текущий planned-v2
`dd1997d47da08e63b77cbaec8aa1efa0262b70bd759068b3989125ee3bdfec76`.
Предыдущие разделы сохраняют последовательность отказов и исправлений.
Старые owner-review a62…, acceptance/patch99a… и readiness от 20 ноября
заменены текущим предметом проверки; текущей Owner-приёмкой они не являются.

Author6 действительно остановился на observer fixture: разрешённая область
не включала существующие harness targets. Координатор исправил только input
coverage fixture; production code и assertions не менялись. Модель Author6
не выдавала completed: raw paused/failed evidence сохранены, нового author
iteration/модельного запуска для механического завершения не было.
[Поправка](../handoffs/current/79/fixture-correction.json),
[raw Author6](../handoffs/current/79/author-r6-output/implementation-report.json),
[механическое завершение](../handoffs/current/79/author6-mechanical-completion.json),
[операторские argv/exit/logs](../handoffs/current/79/logs/operator-r6/checks.json)
различают исходный failed probe и исправленный fixture setup без смены assertions.
Независимый [Reviewer](../handoffs/current/79/reviewer-r5-output/report.md)
одобрил эту поправку и повторил проверки самостоятельно.

Текущие [NFR](../handoffs/current/79/nfr-r3-output/report.md),
[Conformance](../handoffs/current/79/conformance-r4-output/report.md) и
[Security5](../handoffs/current/79/security-r5-output/report.md) approved,
findings пусты. NFR — scope not_applicable, без продуктового измерения.
Security raw approved сохранён; в каноническом пакете только пустой успешный
git-diff log заменён раскрытой
[structured receipt](../handoffs/current/79/logs/operator-r6/security-evidence-normalization.json),
оригинальные raw файлы не переписаны. Opt-in skips не считаются native pass.
Реальные OpenAI/direct DeepSeek Pro guard smoke подтверждены отдельно:
[result](../handoffs/current/79/logs/real-probes-20261121T092325Z/result.json),
passed, source_unchanged=true, private_contexts_remaining=[]. Это синтетический
transport/isolation QA, не production archive и не продуктовый C++ прогон.

Runtime проверяет подлинный user source по thread/message/turn/точному тексту,
редакции и provenance, а не по заявлению модели. Автор и Reviewer независимы
по провайдерам; проверяющие имеют чистый контекст и защищённые inputs.
Неизменный observer сохраняет фактический exit, command/binary/log hashes и cleanup.
Control и audit разделены по hash worktree root внутри git-common-dir;
общий change в двух worktrees не получает общий namespace.

[Closing preflight](../handoffs/current/79/logs/operator-r6/closing-preflight.json):
native state79 отсутствует; legacy native archive отказывает по дизайну.
Bootstrap использовал legacy guarded roles, receipts не импортированы.
Полный production native archive journey не заявлен. DocWriter3 получил stale r2
контекст и отказал до записи: это инфраструктурный резерв, не docs approval.
M-06 остаётся OPEN до актуальной Owner-приёмки, внешней проверки raw audit,
применимого archive и отдельного переключения Root. Существующие подлинные
Owner sources сохранены; новые approval не сочиняются. Глобальные лимиты и
COMPACT_RUNTIME здесь не меняются. C++ остаётся unavailable по плану выше;
M-05 не закрыта, её waiver не переносится. Новых performance jobs/расписаний
и подтверждённой числовой экономии нет.

## Закрытие миграции 79

Owner принял `889d1be` и разрешил разовое архивирование,
активацию и публикацию. [Архив](../openspec/changes/archive/2026-11-23-79-aef-journey-harness/acceptance.md) и
[факт OpenSpec](../openspec/specs/aef-workflow/spec.md) фиксируют результат. Исходные свидетельства и
проверяемый источник решения хранятся в общем Git-каталоге `aef-audit/bootstrap-79/`.
Новый Runtime активирован для `harness`. Остаток M‑06 — проверка профиля C++.

## Кандидат профиля C++

Профиль `cpp` выбирается при `aef.py prepare <change> --profile cpp` и
сохраняется в защищённом состоянии задачи. По умолчанию остаётся `harness`.
До решения об активации кандидат числился в `unavailable_profiles`.
Область записи профиля — объявленные C/C++ файлы в `server/src`.
Критерии, сборочные файлы и состояние Runtime защищены.

Вход сборки — заранее установленные зависимости и исходники vcpkg
`2026.06.24` под `handoffs/toolchains/cpp/{vcpkg,installed}`. Можно задать
`AEF_CPP_SDK` для другого каталога внутри `handoffs/` той же рабочей копии.
Нативная проверка использует чистую временную область для CMake, Ninja,
clang-tidy, логов процесса и данных тестов. Подготовка зависимостей выполняется
до проверки. Сборка в песочнице читает готовый SDK и работает без сети.

Reviewer выполняет Debug, clang-tidy, тесты и Release. Conformance повторяет
Debug и тесты, Security выполняет ASan. Это локальные проверки. При публикации
действуют все шесть обязательных GitHub CI checks из PROJECT.md.

В отсутствие сопоставимого замера Runtime останавливает NFR до вызова
модели со статусом `unobserved`. Подключение парного замера по M‑05 остаётся
условием активации профиля. До активации `cpp` задачи SDD 73 остаются закрыты.

### Результат проверки на macOS

Изолированная база `0dc8995`. Чистые Debug, Release, clang-tidy и ASan прошли
с vcpkg `2026.06.24` (`cd61e1e`). В Debug и ASan прошли по 180 тестов.
Профиль macOS внутри Codex отдельно подтвердил запись только в объявленные
существующий и новый исходники автора. Проверяющий пишет в свой scratch.
Попытки записи вне области, через симлинк и atomic replace отклонены.

На исходном коде нативный прогон отклонил 137 тестов, которым требуется журнал.
Обход абсолютного пути в `LocalTransactionJournal.cpp` открывает промежуточный
каталог с `O_RDONLY`. На macOS это требует права чтения вне рабочей копии.
Нативный контроль увидел `EPERM` на `/Users`. `O_SEARCH` позволяет пройти к
разрешённому каталогу с той же политикой. В одноразовой копии прототип использует
`O_SEARCH` для промежуточных компонентов, сохраняет `O_NOFOLLOW` и открывает
конечный каталог с `O_RDONLY`. С прототипом прошли Reviewer, ASan и
Conformance. По 180 тестов в Debug и ASan. Код брокера в ветке
`codex/m-06-cpp-profile` остаётся в исходной редакции.

Команды, логи, прототип и точные хеши сохранены в `handoffs/current/M-06/`.
NFR по-прежнему `unobserved`. Для активации требуются отдельное решение
о правке журнала, проверка её безопасности, подключение сопоставимого
NFR-замера и решение Owner об активации профиля.


### Проверка на слитой M‑07

На основе `2c3e80355bca795976b914f414dbc56037c36a5f` 24.11.2026 в
тестовой изолированной копии прошли команды Reviewer (Debug, clang-tidy,
180 тестов и Release), Security (ASan, 180 тестов) и Conformance (Debug,
180 тестов). Проверены write scope автора и изоляция проверяющего.
Дополнительно прошли 20 тестов Runtime. Обход каталога взят из main.

[Нативные проверки](../handoffs/current/M-06/logs/native-cpp/merged-m07-2026-11-24/result.json)
и [тесты Runtime](../handoffs/current/M-06/logs/native-cpp/merged-m07-2026-11-24/runtime-tests.log).
Записи из тестовой копии имеют `test_only:true`. NFR в Runtime остаётся
`unobserved` до подключения парного замера к продуктовой стадии.


### Дневной NFR и предмет приёмки

Baseline main `2c3e80355bca795976b914f414dbc56037c36a5f` подготовлен в
`benchmarks/baseline.md` ветки `codex/m-05-nightly-perf`. В выбранных
парных сериях все 15 путей в бюджете. Для `Transacted_Persistent_RoundTrip/process_time` использована отдельная
серия с минимумом 12 секунд, прошедшая независимый пересчёт. История
штатного `rejected` и дополнительное наблюдение сохранены в основном
checkout под `handoffs/current/M-05/manual/2026-11-24-hu3xsb2l/`.

Owner принял baseline `main` `2c3e803` из локального коммита `bef6548`
ответом «принимаю» 24.11.2026. Свидетельства нативной проверки команд M‑06
сохранены в разделе выше.
Продуктовая проверка и её результат приведены ниже. Активация `cpp`
оформляется отдельным решением Owner.

### Регистрация продуктового NFR

Когда C++ задача дошла до `nfr`, оператор запускает
`python3 -B .claude/chain/aef.py verify <change> --actor nfr` в согласованное
окно на машине замера. Команда фиксирует Git-базу задачи и отпечаток её
текущих исходников, отдельно собирает обе версии с одним SDK в RelWithDebInfo
и запускает ABBA в нативной песочнице. У замера свой каталог записи.
Собранные бинарники и манифест сборки доступны ему для чтения.

Координатор сверяет состояние задачи, toolchain, SHA бинарников и сырые серии.
Набор включает 15 путей и девять повторов на каждую сторону. При CPU CV
свыше 5% путь повторяется один раз. Для persistent transaction round trip
и persistent transaction batch минимум на повторе составляет 12 секунд.
Порог роста CPU остаётся +5%. На macOS отслеживается питание от сети.

При валидных данных `next` передаёт независимому NFR-специалисту результат,
выводы и путь к файлам наблюдения. Стабильная регрессия даёт `rejected`.
Шум без подтверждённой регрессии, отсутствующие или изменённые серии дают
`unobserved` и останавливают стадию до вызова модели. Данные копируются в
выход задачи вместе с манифестом SHA-256. Решения Owner и выбор доступных
профилей по-прежнему ведутся отдельно.


### Проверка регистрации NFR в нативной песочнице

На одном Mac проверена пара `d04f2a1f89f19b1bbde86c4c5b3d5732a105c9f8` →
`59131ca176ed0eb02fc56c1c7f69ad5976ebd8ea`. Основа использует C++ исходники принятого
`2c3e803`, кандидат отличается только комментарием в объявленном исходнике
журнала. Точные Git объекты сохранены в [пакете тестовых ревизий](../handoffs/current/M-06/logs/cpp-nfr-native-final/fixture.bundle).

Нативный `verify --actor nfr` получил `passed`. Все 15 путей в пределах
бюджета, максимальный рост CPU +2.39%, максимальный CPU CV 4.65%.
`next` перешёл на `conformance` с хэшем исходного наблюдения.
Свидетельство имеет `test_only:true`. Сохранённые продуктовые серии точных
пар SDD 72 и M‑07 отдельно пересчитаны текущим валидатором, включая длинную
серию M‑07.

- [Наблюдение координатора](../handoffs/current/M-06/logs/cpp-nfr-native-final/receipt.json), [сырой пересчёт](../handoffs/current/M-06/logs/cpp-nfr-native-final/measurement/verification.json)
  и [переход](../handoffs/current/M-06/logs/cpp-nfr-native-final/transition.json).
- [Независимое статическое ревью DeepSeek Pro](../handoffs/current/M-06/logs/cpp-nfr-native-final/deepseek-review.json)
  через прямой профиль дало `approved` с пятью замечаниями. Исправлены
  распаковка на Python 3.11 без tar data filter, привязка пути результата,
  пустой ответ pmset, конкуренция между локальными копиями и проверка SDK.
- [Проверки исправлений](../handoffs/current/M-06/logs/cpp-nfr-native-final/final-tests.log). Runtime и запускатель
  M‑05 используют одну блокировку на пользователя и машину.

На приёмку передаётся кандидат с включённой проверкой NFR в реестре `cpp`.
Доступность профиля определяется отдельным решением Owner.

## Активация профиля C++ от 24.11.2026

Owner принял локальный кандидат `0652c98` и согласовал активацию в ответе
«принимаю, продолжай» на предложенный порядок включения и публикации.
Реестр разрешает явный выбор `cpp` и сохраняет `harness` по умолчанию.
Принятый [baseline M‑05](../benchmarks/baseline.md) и запускатель входят в
ту же поставку. Публикация проходит через обязательные проверки защищённой
`main`.
