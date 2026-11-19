# Сокращённый процесс AEF tiny-mq

## ADDED Requirements

### Requirement: Единый контракт
Runtime SHALL читать пять файлов без сохранённой копии SDD, проверять пять полей INT, десять полей SDD, источники, сценарии и Test; сохранять отдельный общий INT-01.

ID: `REQ-79-1`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Единый контракт
- **WHEN** prepare получает полный комплект без позднего acceptance либо незаполненное поле/неверную редакцию
- **THEN** полный вход принимается, дефектный блокируется до запуска модели
- Test: `.claude/chain/tests/test_workflow_contract.py::WorkflowContractTest::test_required_fields`
- Test: `.claude/chain/tests/test_workflow_contract.py::WorkflowContractTest::test_sources_and_late_acceptance`
- Test: `.claude/chain/tests/test_workflow_integration.py::LegacyPreservationIntegrationTest::test_native_trace_preserves_sources_and_complete_contract`

### Requirement: Идемпотентное выполнение
Runtime SHALL самостоятельно выполнять подготовку, проверки, переходы и ожидание; повтор/конкурентный next не дублирует execution, interruption и ожидания требуют применимого resume.

ID: `REQ-79-2`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Идемпотентное выполнение
- **WHEN** два next стартуют через общий барьер либо наблюдатель прерван
- **THEN** запуск один; живость/остановка показаны достоверно; checkpoint и история не теряются
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_concurrent_next`
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_interruption_and_waits`
- Test: `.claude/chain/tests/test_workflow_integration.py::WorkflowProcessIntegrationTest::test_real_process_lock_and_duplicate_next`
- Test: `.claude/chain/tests/test_workflow_integration.py::WorkflowProcessIntegrationTest::test_observer_loss_with_live_child`

### Requirement: Проверяемое решение Owner
Runtime SHALL различать технический результат и разрешение перехода, проверять реальное решение, его редакцию, REQ, действие и открытую задачу; исключение NFR не даёт общего обхода блокировок.

ID: `REQ-79-3`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Проверяемое решение Owner
- **WHEN** NFR rejected либо legacy approved/performance_verdict rejected без решения, или связывание исключения изменено
- **THEN** переход закрыт; только узкое действующее решение открывает указанное действие с сохранением rejected
- Test: `.claude/chain/tests/test_gate_decisions.py::GateDecisionTest::test_rejected_is_not_pass`
- Test: `.claude/chain/tests/test_gate_decisions.py::GateDecisionTest::test_exact_exception_binding`
- Test: `.claude/chain/tests/test_gate_decisions.py::GateDecisionTest::test_unwaivable_blockers`
- Test: `.claude/chain/tests/test_workflow_integration.py::DecisionSourceIntegrationTest::test_real_format_and_rejected_origins`
- Test: `.claude/chain/tests/test_workflow_integration.py::DecisionSourceIntegrationTest::test_exact_identity_revision_and_ambiguity`

### Requirement: Изоляция и независимость
Runtime SHALL механически ограничивать запись/чтение стадий; автор не меняет входы/решения/критерии, Reviewer имеет read-only исходники и чистый контекст, модели автора и Reviewer разных провайдеров.

ID: `REQ-79-4`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Изоляция и независимость
- **WHEN** стадия пытается изменить Owner-запись, контрольный файл, чужой выход, воспользоваться symlink или унаследованным MCP
- **THEN** операция блокируется до изменения защищённого содержания; нарушение и фактическая модель отражены в аудите
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_actor_isolation`
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_independence_and_provider_policy`
- Test: `.claude/chain/tests/test_workflow_integration.py::NativeExecutionIntegrationTest::test_real_native_policy_denials`
- Test: `.claude/chain/tests/test_workflow_integration.py::NativeExecutionIntegrationTest::test_codex_exec_profiles_and_guard_observation`

### Requirement: Свежесть свидетельств
Runtime SHALL связывать проверку с содержимым, тестами, зависимостями и окружением; применять собственные проверки проверяющего и повторять затронутые проверки.

ID: `REQ-79-5`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Свежесть свидетельств
- **WHEN** меняются код, критерий, окружение, лог либо только acceptance
- **THEN** затронутый допуск снимается, неизменённый код не перепроверяется из-за текста объяснения, новое объяснение проверяется отдельно
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_receipt_subject_and_environment`
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_acceptance_does_not_invalidate_code`

### Requirement: Факт и доступный архив
Runtime SHALL обновлять факт только по Owner-приёмке, сохранять историю и адресатов относительных ссылок после archive; аудит доступен вне временной рабочей копии.

ID: `REQ-79-6`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Факт и доступный архив
- **WHEN** change перемещается в archive или читатель открывает историческую SDD72
- **THEN** ссылки разрешаются, факт и исторический контракт различимы, M05 остаётся открытой, исходные свидетельства сохранены
- Test: `.claude/chain/tests/test_workflow_contract.py::WorkflowContractTest::test_archive_links_and_history`
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_acceptance_and_audit_required`

### Requirement: Безопасное переключение
Runtime SHALL сохранять прежние механизмы до независимой проверки замены и отдельного решения Owner; новая продуктовая разработка не начинается на старом входе.

ID: `REQ-79-7`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Безопасное переключение
- **WHEN** замена не проверена или Owner её не принял
- **THEN** переключение запрещено, legacy-проверки сохранены, новый продукт ожидает новой оснастки
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_migration_gate_and_legacy_preservation`
- Test: `.claude/chain/tests/test_workflow_integration.py::LegacyPreservationIntegrationTest::test_existing_trace_admission_openspec_and_wrapper`
- Test: `.claude/chain/tests/test_workflow_integration.py::LegacyPreservationIntegrationTest::test_old_entry_refuses_new_product_after_cutover`

### Requirement: Полная наблюдаемость затрат
Runtime SHALL сохранять все попытки, стоимость/неизвестный расход, вмешательства Owner и вид перехода; credits не представляются USD, экономия не объявляется по числу dispatch.

ID: `REQ-79-8`
Источник: repo:openspec/changes/79-aef-journey-harness/proposal.md@4e7f7fc81ca171a1b5762ba28f2c6bd09d5d05e1#outcome

#### Scenario: Полная наблюдаемость затрат
- **WHEN** проверка повторяется из-за дефекта либо среды, или процесс просто ожидается
- **THEN** журнал различает содержательный возврат, инфраструктурный повтор и технический переход; ожидание не вызывает LLM-опрос
- Test: `.claude/chain/tests/test_workflow_runtime.py::WorkflowRuntimeTest::test_accounting_and_transition_kinds`

