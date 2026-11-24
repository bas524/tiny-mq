"""REQ-79-2/4/5/6/7/8: coordinator semantics with zero provider calls."""
import copy
import importlib
import json
import shutil
import sys
import subprocess
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_workflow_contract import WorkflowFixture


class ObservedEvidenceTest(unittest.TestCase):
    def run_observed_critic(self, case, recover, *, remove_after_resume=False):
        from workflow_runtime import Runtime, sha

        fixture = WorkflowFixture()
        self.addCleanup(fixture.close)
        role = fixture.root / '.claude/agents/spec-critic.md'
        role.parent.mkdir()
        role.write_text('Fixture critic\n')
        change = '79-demo'
        source = {'role': 'user', 'provenance': 'user', 'thread_id': 'fixture-thread',
                  'message_id': 'fixture-owner', 'text': 'Approve fixture business scope'}
        runtime = Runtime(fixture.root, source_reader=lambda ref: source)
        pending = runtime.prepare(change)['pending_decision']
        runtime.record_decision(change, {'kind': pending['kind'], 'subject': pending['subject'],
            'owner': 'Owner', 'source': {**source, 'quote': source['text']}})
        output = Path(runtime.policy(change, 'critic')['output'])
        output.mkdir(parents=True)
        report = output / 'report.log'
        report.write_text('fixture evidence\n')
        empty = output / 'empty.log'
        empty.touch()
        (output / 'file-link').symlink_to(report)
        (output / 'dir-link').symlink_to(output, target_is_directory=True)
        options = {
            'valid': ['report.log', str(report)],
            'missing': ['missing.log'],
            'malformed': 'report.log',
            'null': None,
            'blank': [' '],
            'not-string': [1],
            'empty-file': ['empty.log'],
            'directory': [str(output)],
            'external': [str(fixture.proposal)],
            'traversal': ['../critic/report.log'],
            'file-link': ['file-link'],
            'dir-link': ['dir-link/report.log'],
        }
        claim = {'technical_verdict': 'passed', 'questions': []}
        if case != 'omitted':
            claim['evidence'] = options[case]

        def observe(change, actor, execution, command, env, prompt, raw, last, stderr):
            raw.write_text('{"type":"thread.started"}\n{"type":"turn.completed"}\n')
            last.write_text(json.dumps(claim))
            observed = {'execution': execution, 'subject': runtime._subject(), 'argv': command,
                        'provider': 'deepseek', 'exit_code': 0, 'raw_log': str(raw),
                        'log_sha256': sha(raw), 'binary_sha256': sha(command[0]),
                        'last': str(last), 'last_sha256': sha(last)}
            if recover:
                receipt = runtime._dir(change) / 'observations' / (execution + '.json')
                receipt.parent.mkdir(exist_ok=True)
                receipt.write_text(json.dumps(observed))
                raise InterruptedError('fixture lost coordinator after observation')
            return observed

        valid = case in ('valid', 'omitted')
        with (patch.object(runtime, '_run_observed', side_effect=observe) as execution,
              patch.object(runtime, '_smokes_current', return_value=True),
              patch.object(runtime, '_codex_command', return_value=[sys.executable])):
            state = runtime.next(change)
            if recover:
                self.assertEqual(state['state'], 'interrupted')
                if valid:
                    self.assertEqual(runtime.resume(change, 'Reconcile fixture')['state'], 'ready')
                    if remove_after_resume:
                        report.unlink()
                    state = runtime.next(change)
                else:
                    with self.assertRaisesRegex(ValueError, 'role evidence'):
                        runtime.resume(change, 'Reconcile fixture')
                    state = runtime.next(change)
                    self.assertEqual(state['state'], 'interrupted')
            self.assertEqual(state['dispatches'], 1)
            execution.assert_called_once()
            if valid and not remove_after_resume:
                self.assertEqual(state['pending_decision']['kind'], 'architecture')
            else:
                self.assertIsNone(state['pending_decision'])
                self.assertEqual(state['stage'], 'critic')
                if not recover or remove_after_resume:
                    self.assertIn('role evidence', state['attempts'][-1]['error'])

    def test_observed_role_evidence(self):
        for case in ('valid', 'omitted', 'missing', 'malformed', 'null', 'blank', 'not-string',
                     'empty-file', 'directory', 'external', 'traversal', 'file-link', 'dir-link'):
            with self.subTest(case=case):
                self.run_observed_critic(case, recover=False)

    def test_recovered_role_evidence(self):
        for case in ('valid', 'missing', 'external', 'dir-link'):
            with self.subTest(case=case):
                self.run_observed_critic(case, recover=True)
        self.run_observed_critic('valid', recover=True, remove_after_resume=True)


class WorkflowRuntimeTest(unittest.TestCase):
    def setUp(self):
        module = importlib.import_module('workflow_runtime')
        self.f = WorkflowFixture(); self.addCleanup(self.f.close)
        self.calls = []; self.source = {'provenance': 'user', 'role': 'user', 'text': 'Принимаю указанную редакцию', 'message_id': 'human-1', 'thread_id': 'human-thread'}
        self.runtime = module.Runtime(self.f.root, executor=self.execute, source_reader=lambda ref: self.source,
            check_runner=lambda argv, **kw: subprocess.run(argv, capture_output=True, text=True, **kw))
        self.change = '79-demo'; self.runtime.prepare(self.change)

    def execute(self, stage, request):
        self.calls.append(stage)
        return {'technical_verdict': 'passed', 'summary': 'synthetic fixture', 'questions': []}

    def approve_pending(self):
        pending = self.runtime.status(self.change)['pending_decision']
        self.runtime.record_decision(self.change, {'kind': pending['kind'], 'subject': pending['subject'],
             'owner': 'Owner', 'source': {'thread_id': 'human-thread', 'message_id': 'human-1', 'quote': self.source['text']}})

    def cpp_fixture(self):
        from workflow_runtime import Runtime

        root = self.f.root
        source = root / 'server/src/demo.cpp'
        source.parent.mkdir(parents=True)
        source.write_text('int fixture() { return 0; }\n')
        registry = root / '.claude/chain/workflow-checks.json'
        data = json.loads(registry.read_text())
        data['profiles']['cpp'] = {
            'code_paths': ['server/src'],
            'gates': ['reviewer', 'nfr', 'conformance', 'security'],
            'checks': [{'id': 'cpp-fixture', 'argv': [sys.executable, '-c', 'print("cpp check")'],
                        'actors': ['reviewer']}],
        }
        data['unavailable_profiles'] = {'cpp': 'fixture disabled'}
        registry.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'unavailable'):
            Runtime(root, profile='cpp')
        data['unavailable_profiles'] = {}
        registry.write_text(json.dumps(data))
        return Runtime(root, profile='cpp', executor=self.execute,
                       source_reader=lambda ref: self.source), source

    def test_cpp_profile_is_pinned_and_confined(self):
        from workflow_runtime import Runtime

        runtime, source = self.cpp_fixture()
        # Scope checks run before any provider or business action.
        self.assertEqual(runtime._covered_target('server/src/demo.cpp'), str(source))
        for name in ('code.py', 'server/src/notes.md', 'CMakeLists.txt', 'tests/new.cpp'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                runtime._covered_target(name)
        # Older harness checkpoints remain valid and cannot switch profiles.
        self.assertEqual(Runtime(self.f.root, change=self.change).profile_name, 'harness')
        with self.assertRaisesRegex(ValueError, 'differs from pinned'):
            runtime.status(self.change)
        change = '80-cpp-demo'
        proposal = self.f.change.with_name(change)
        shutil.copytree(self.f.change, proposal)
        metadata = proposal / '.openspec.yaml'
        metadata.write_text(metadata.read_text().replace('79-demo', change))
        state = runtime.prepare(change, targets=['server/src/demo.cpp'])
        self.assertEqual(state['profile'], 'cpp')
        self.assertEqual(state['dispatches'], 0)
        self.assertEqual(Runtime(self.f.root, change=change).status(change)['profile'], 'cpp')

    def test_cpp_missing_nfr_measurement_cannot_advance(self):
        from workflow_runtime import sha

        runtime, source = self.cpp_fixture()
        state = self.runtime._read(self.change)
        state.update(profile='cpp', registry_hash=sha(runtime.registry_path),
                     stage='nfr', pending_decision=None, state='ready')
        state['targets'] = [str(source)]
        runtime._save(self.change,state)
        result = runtime.next(self.change)
        self.assertEqual(self.calls, [])
        self.assertEqual(result['dispatches'], 0)
        self.assertEqual(result['stage'], 'nfr')
        self.assertEqual(result['gate_results']['nfr']['transition'], 'blocked')
        self.assertEqual(result['gate_results']['nfr']['technical_verdict'], 'unobserved')
        with patch('workflow_cpp.toolchain_identity', return_value={'test_only': True}):
            receipt = runtime.verify(self.change, actor='nfr')
        self.assertEqual(receipt['technical_verdict'], 'unobserved')

    def cpp_nfr_fixture(self):
        from workflow_runtime import Runtime, sha

        runtime, source = self.cpp_fixture()
        registry = json.loads(runtime.registry_path.read_text())
        registry['profiles']['cpp']['checks'].append({
            'id': 'cpp-nfr', 'actors': ['nfr'], 'argv': [sys.executable, '-c', 'pass'],
        })
        runtime.registry_path.write_text(json.dumps(registry))
        runtime.git('add', str(runtime.registry_path))
        runtime.git('commit', '-m', 'Install test profile')
        runtime = Runtime(self.f.root, profile='cpp', executor=self.execute)
        state = self.runtime._read(self.change)
        state.update(profile='cpp', registry_hash=sha(runtime.registry_path),
                     base=runtime.git('rev-parse', 'HEAD').strip(),
                     stage='nfr', pending_decision=None, state='waiting', wait_reason='environment',
                     targets=[str(source)],
                     gate_results={'nfr': {'technical_verdict': 'unobserved', 'transition': 'blocked'}})
        runtime._save(self.change, state)
        return runtime, source

    def cpp_nfr_samples(self, mode):
        def command(change, state, check, scratch):
            from workflow_cpp import source_files, source_identity
            from workflow_nfr import ORDER, PATHS, PROFILES, file_hash

            def write(path, names, repetitions, side):
                entries = []
                for name in names:
                    for repetition in range(repetitions):
                        cpu = 100
                        if side == 'candidate' and name == PATHS[0]:
                            if mode == 'regression':
                                cpu = 106
                            elif mode == 'noisy' and repetition == 0:
                                cpu = 145
                        entries.append(dict(run_type='iteration', run_name='BenchmarkFixture/' + name,
                                            repetitions=repetitions, repetition_index=repetition,
                                            cpu_time=cpu, real_time=cpu, time_unit='ns'))
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps({'benchmarks': entries}))

            raw = scratch / 'raw'
            for name in PATHS:
                folders = [name.replace('/', '-')]
                if mode == 'noisy' and name == PATHS[0]:
                    folders.append(folders[0] + '-repeat')
                for folder in folders:
                    for step, (side, count) in enumerate(ORDER, 1):
                        write(raw / folder / f'{step}-{side}.json', (name,), count, side)
            write(raw / 'profiles.json', PROFILES, 9, 'candidate')
            if mode == 'truncated':
                (raw / PATHS[0].replace('/', '-') / '4-baseline.json').write_text('{}')
            binaries = {}
            for side in ('baseline', 'candidate'):
                binary = scratch / (side + '-build') / 'tiny_mq'
                binary.parent.mkdir()
                binary.write_text(side)
                binaries[side] = file_hash(binary)
            identity = source_identity(source_files(self.f.root), self.f.root)
            (scratch / 'nfr-build.json').write_text(json.dumps({
                'base': 'incorrect' if mode == 'wrong-base' else state['base'],
                'candidate_sources': identity, 'baseline_sources': 'fixture-base',
                'binary_sha256': binaries, 'configuration': 'RelWithDebInfo',
            }))
            return {'argv': check['argv'], 'exit': int(mode in ('regression', 'noisy')),
                    'stdout': '', 'stderr': ''}
        return command

    def test_cpp_nfr_audits_and_reuses_registered_observation(self):
        runtime, _ = self.cpp_nfr_fixture()

        def executor(stage, request):
            if stage == 'nfr':
                return self.execute(stage, request)
            return {'technical_verdict': 'unobserved', 'wait_reason': 'environment'}

        runtime.executor = executor
        with (patch('workflow_cpp.toolchain_identity', return_value={'test_only': True}),
              patch.object(runtime, '_cpp_nfr_command', side_effect=self.cpp_nfr_samples('pass')) as run):
            receipt = runtime.verify(self.change, actor='nfr')
            self.assertEqual(receipt['technical_verdict'], 'passed')
            self.assertEqual(runtime.status(self.change)['state'], 'ready')
            result = runtime.next(self.change)
            self.assertEqual(result['stage'], 'conformance')
            self.assertEqual(result['gate_results']['nfr']['receipt_hash'], receipt['receipt_hash'])
            self.assertEqual(self.calls, ['nfr'])
            run.assert_called_once()

    def test_cpp_nfr_stale_or_changed_samples_block_dispatch(self):
        runtime, source = self.cpp_nfr_fixture()
        with (patch('workflow_cpp.toolchain_identity', return_value={'test_only': True}),
              patch.object(runtime, '_cpp_nfr_command', side_effect=self.cpp_nfr_samples('pass'))):
            for invalidation in ('source', 'raw'):
                with self.subTest(invalidation=invalidation):
                    receipt = runtime.verify(self.change, actor='nfr')
                    if invalidation == 'source':
                        source.write_text(source.read_text() + '// changed after measurement\n')
                    else:
                        evidence = self.f.root / receipt['measurement']['path']
                        manifest = json.loads(evidence.read_text())['files']
                        (evidence.parent / next(name for name in manifest if name.startswith('raw/'))).write_text('{}')
                    result = runtime.next(self.change)
                    self.assertEqual(result['technical_verdict'], 'unobserved')
                    self.assertEqual(result['gate_results']['nfr']['transition'], 'blocked')
                    self.assertEqual(self.calls, [])

    def test_cpp_nfr_distinguishes_inconclusive_measurements_from_regression(self):
        runtime, _ = self.cpp_nfr_fixture()
        with patch('workflow_cpp.toolchain_identity', return_value={'test_only': True}):
            for mode in ('wrong-base', 'truncated', 'noisy'):
                with self.subTest(mode=mode), patch.object(
                    runtime, '_cpp_nfr_command', side_effect=self.cpp_nfr_samples(mode),
                ):
                    receipt = runtime.verify(self.change, actor='nfr')
                    self.assertEqual(receipt['technical_verdict'], 'unobserved')
                    self.assertEqual(runtime.next(self.change)['state'], 'waiting')
                    self.assertEqual(self.calls, [])
            with patch.object(runtime, '_cpp_nfr_command', side_effect=self.cpp_nfr_samples('regression')):
                receipt = runtime.verify(self.change, actor='nfr')
                self.assertEqual(receipt['technical_verdict'], 'rejected')
                runtime.executor = lambda stage, request: {
                    'technical_verdict': 'passed', 'performance_verdict': 'passed', 'wait_reason': 'environment',
                }
                result = runtime.next(self.change)
                self.assertEqual(result['gate_results']['nfr']['technical_verdict'], 'rejected')
                self.assertEqual(result['gate_results']['nfr']['performance_verdict'], 'rejected')
                self.assertEqual(result['gate_results']['nfr']['transition'], 'blocked')
                self.assertEqual(result['stage'], 'producer')

    def test_cpp_nfr_archive_works_without_tarfile_data_filter(self):
        import io
        import tarfile
        from workflow_cpp import extract_baseline

        archive = self.f.root / 'baseline.tar'
        output = self.f.root / 'baseline'
        output.mkdir()
        with tarfile.open(archive, 'w') as sources:
            file = tarfile.TarInfo('server/src/sample.cpp')
            value = b'int sample;\n'
            file.size = len(value)
            sources.addfile(file, io.BytesIO(value))
        with patch.object(tarfile, 'data_filter', None, create=True):
            extract_baseline(archive, output)
            self.assertEqual((output / 'server/src/sample.cpp').read_bytes(), value)
            with tarfile.open(archive, 'w') as sources:
                link = tarfile.TarInfo('outside')
                link.type = tarfile.SYMTYPE
                link.linkname = '../outside'
                sources.addfile(link)
            with self.assertRaisesRegex(ValueError, r'invalid C\+\+ baseline archive member'):
                extract_baseline(archive, output)

    def test_cpp_nfr_rejects_empty_power_response(self):
        from workflow_runtime import Runtime

        with (patch('workflow_runtime.sys.platform', 'darwin'),
              patch.object(Runtime, '_tool_env', return_value={}),
              patch.object(Runtime, 'sandbox_command', return_value=['unused']),
              patch('workflow_runtime.subprocess.check_output', return_value=''),
              patch('workflow_runtime.subprocess.Popen') as spawn):
            with self.assertRaisesRegex(ValueError, 'external power'):
                self.runtime._cpp_nfr_command(self.change, {'base': 'fixture'},
                                              {'timeout': 30, 'argv': []}, self.f.root)
            spawn.assert_not_called()

    def test_concurrent_next(self):
        """REQ-79-2: simultaneous continuation launches exactly one critic."""
        self.approve_pending()
        entered = threading.Event(); release = threading.Event(); failures = []
        def blocked(stage, request):
            self.calls.append(stage); entered.set()
            if not release.wait(10): raise RuntimeError('Fixture barrier timeout')
            return {'technical_verdict': 'passed', 'summary': 'fixture', 'questions': []}
        self.runtime.executor = blocked
        def first():
            try: self.runtime.next(self.change)
            except Exception as error: failures.append(error)
        worker = threading.Thread(target=first); worker.start()
        try:
            self.assertTrue(entered.wait(5)); start = time.monotonic()
            result = self.runtime.next(self.change)
            self.assertLess(time.monotonic() - start, 1)
            self.assertEqual(result['state'], 'running'); self.assertEqual(self.calls, ['critic'])
        finally: release.set(); worker.join(5)
        self.assertFalse(worker.is_alive()); self.assertEqual(failures, [])

    def test_interruption_and_waits(self):
        """REQ-79-2: waits survive next; interrupted work is never reported live."""
        for reason in ('owner', 'rate_limited', 'no_progress', 'budget_limit', 'environment'):
            self.runtime.wait(self.change, reason)
            count = len(self.calls)
            for _ in range(2): self.assertEqual(self.runtime.next(self.change)['state'], 'waiting')
            self.assertEqual(len(self.calls), count)
            self.runtime.resume(self.change, reason='Operator reconciled checkpoint')
        self.approve_pending()
        def interrupted(stage, request): raise InterruptedError('fixture observer loss')
        self.runtime.executor = interrupted
        self.runtime.next(self.change)
        state = self.runtime.status(self.change)
        self.assertEqual(state['state'], 'interrupted')
        self.assertEqual(self.runtime.next(self.change)['state'], 'interrupted')
        self.assertTrue(state['checkpoint'])

    def test_actor_isolation(self):
        """REQ-79-4: protected inputs/control and secret reads never become outputs."""
        for actor in ('producer', 'reviewer'):
            policy = self.runtime.policy(self.change, actor)
            self.assertNotIn(str(self.f.proposal), policy['write_paths'])
            self.assertNotIn(str(self.f.root / '.git'), policy['write_paths'])
            self.assertFalse(policy['network_enabled']); self.assertFalse(policy['mcp_enabled'])
        outside = self.f.root.parent / 'outside-fixture'
        symlink = self.f.root / 'code-link.py'; symlink.symlink_to(outside)
        with self.assertRaises(ValueError): self.runtime.prepare(self.change, targets=['code-link.py'])
        with self.assertRaises(ValueError): self.runtime.prepare(self.change, targets=['../escape.py'])
        with self.assertRaises(ValueError): self.runtime.prepare(self.change, targets=['.git/owner.json'])

    def test_independence_and_provider_policy(self):
        """REQ-79-4: role binding and a fresh reviewer are mechanical constraints."""
        a = self.runtime.policy(self.change, 'producer'); r = self.runtime.policy(self.change, 'reviewer')
        self.assertNotEqual(a['provider'], r['provider']); self.assertTrue(r['fresh_session'])
        self.assertEqual(r['write_source_paths'], [])
        for actor in ('critic', 'reviewer', 'security', 'nfr'):
            policy = self.runtime.policy(self.change, actor)
            self.assertEqual(policy['provider'], 'deepseek')
            self.assertEqual(policy['base_url'], 'https://api.deepseek.com/')
            self.assertFalse(policy['proxy_enabled']); self.assertFalse(policy['approval_escalation'])

    def test_receipt_subject_and_environment(self):
        """REQ-79-5: code/test/environment/log changes make applicable evidence stale."""
        receipt = self.runtime.verify(self.change, actor='reviewer')
        self.assertTrue(self.runtime.receipt_current(self.change, receipt))
        code = self.f.root / 'code.py'; original = code.read_text(); code.write_text('value = 2\n')
        self.assertFalse(self.runtime.receipt_current(self.change, receipt)); code.write_text(original)
        changed = copy.deepcopy(receipt); changed['environment_hash'] = 'different'
        self.assertFalse(self.runtime.receipt_current(self.change, changed))
        Path(receipt['log_path']).write_text('forged log\n')
        self.assertFalse(self.runtime.receipt_current(self.change, receipt))

    def test_acceptance_does_not_invalidate_code(self):
        """REQ-79-5: code verification and explanation have separate subjects."""
        receipt = self.runtime.verify(self.change, actor='reviewer')
        first = self.runtime.status(self.change)['contract_hash']
        (self.f.change / 'acceptance.md').write_text('Independent explanation v1\n')
        self.assertTrue(self.runtime.receipt_current(self.change, receipt))
        self.assertEqual(first, self.runtime.status(self.change)['contract_hash'])
        self.assertFalse(self.runtime.status(self.change)['acceptance_verified'])

    def test_acceptance_and_audit_required(self):
        """REQ-79-6: neither files nor a model claim authorize canonical facts/archive."""
        with self.assertRaises(ValueError): self.runtime.archive(self.change)
        (self.f.change / 'acceptance.md').write_text('approved\n')
        with self.assertRaises(ValueError): self.runtime.archive(self.change)
        self.assertTrue(self.runtime.status(self.change)['audit_location'])

    def test_migration_gate_and_legacy_preservation(self):
        """REQ-79-7: replacement and Owner decisions are prerequisites, old controls remain."""
        for _ in range(2): self.runtime.next(self.change)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.runtime.status(self.change)['pending_decision']['kind'], 'business')
        self.approve_pending(); self.runtime.next(self.change)
        self.assertEqual(self.calls, ['critic'])
        self.assertEqual(self.runtime.status(self.change)['pending_decision']['kind'], 'architecture')
        self.runtime.next(self.change); self.assertEqual(self.calls, ['critic'])
        with self.assertRaises(ValueError): self.runtime.record_decision(self.change,
            {'kind': 'architecture', 'owner': 'Owner', 'subject': 'stale', 'source': {'message_id': 'invented'}})

    def test_accounting_and_transition_kinds(self):
        """REQ-79-8: status does not call models; all attempts and unknown costs are visible."""
        before = self.runtime.status(self.change)
        for _ in range(3): self.runtime.status(self.change)
        self.assertEqual(self.calls, [])
        self.assertEqual(before['attempts'], self.runtime.status(self.change)['attempts'])
        self.assertEqual(set(before['transition_kinds']), {'substantive', 'infrastructure', 'mechanical'})
        self.assertIn('unknown_cost', before); self.assertNotEqual(before['gpt_units'], 'USD')

    def test_regression_role_inputs_are_not_outputs(self):
        """REQ-79-4: declared outputs cannot overlap canonical role/skill inputs."""
        module=importlib.import_module('workflow_runtime')
        for name in ('.claude/agents/reviewer.md','.claude/agents/producer.md',
                     '.claude/skills/verify/SKILL.md'):
            with self.subTest(target=name):
                f=WorkflowFixture()
                try:
                    path=f.root/name;path.parent.mkdir(parents=True,exist_ok=True)
                    path.write_text('Trusted control input\n')
                    f.git('add','.');f.git('commit','-qm','pin canonical role input')
                    runtime=module.Runtime(f.root)
                    with self.assertRaisesRegex(ValueError,'protected|canonical|control'):
                        runtime.prepare('79-demo',targets=[name])
                finally:f.close()
        self.assertEqual(self.calls,[])

    def test_regression_next_preserves_rejected_performance(self):
        """REQ-79-3: real next applies the verdict rule, not only evaluate_gate."""
        def contradictory(stage,request):
            self.calls.append(stage)
            if stage=='reviewer':
                return {'technical_verdict':'passed','status':'approved','performance_verdict':'rejected'}
            return {'technical_verdict':'passed','questions':[]}
        self.runtime.executor=contradictory
        self.approve_pending();self.runtime.next(self.change)
        self.approve_pending();state=self.runtime.next(self.change)
        self.assertIn('reviewer',self.calls)
        self.assertFalse(set(self.calls)&{'conformance','security','docwriter','explainer'})
        gate=state['gate_results']['reviewer']
        self.assertEqual(gate['technical_verdict'],'rejected')
        self.assertNotEqual(gate['transition'],'allowed')

    def test_regression_audit_namespace_for_linked_worktrees(self):
        """REQ-79-6: a shared Git common dir cannot alias separate audit bundles."""
        import tempfile
        tmp=tempfile.TemporaryDirectory(prefix='aef-linked-audit-');self.addCleanup(tmp.cleanup)
        linked=Path(tmp.name)/'linked'
        self.f.git('worktree','add','--detach',str(linked),'HEAD')
        module=importlib.import_module('workflow_runtime')
        other=module.Runtime(linked,executor=self.execute,source_reader=lambda ref:self.source)
        other.prepare(self.change)
        first=self.runtime.status(self.change);second=other.status(self.change)
        self.assertNotEqual(self.runtime.control,other.control)
        self.assertNotEqual(first['audit_location'],second['audit_location'])

    def test_regression_debt_accepts_canonical_markdown_status(self):
        """REQ-79-3: canonical maintenance status is parsed conservatively."""
        path=self.f.root/'tasks/M-test.md';path.parent.mkdir(exist_ok=True)
        text='# Debt\n- **Статус:** открыта. Ночной job не настроен.\n## Spec ref\ntiny-mq/79:REQ-79-1@'+self.f.revision+'\n'
        path.write_text(text)
        debt=self.runtime._debt({'path':'tasks/M-test.md'})
        self.assertEqual(debt['status'],'open');self.assertEqual(debt['requirement_ids'],['REQ-79-1'])
        path.write_text(text.replace('открыта.','закрыта.'))
        self.assertNotEqual(self.runtime._debt({'path':'tasks/M-test.md'})['status'],'open')
        path.write_text('# Unknown\n## Spec ref\nREQ-79-1\n')
        self.assertNotEqual(self.runtime._debt({'path':'tasks/M-test.md'})['status'],'open')


    def test_execution_metadata_and_material_cards(self):
        """REQ-79-5: planned freshness survives progress; acceptance binds exact material."""
        from workflow_contract import load_contract
        initial=load_contract(self.f.proposal)
        tasks=self.f.change/'tasks.md';tasks.write_text(tasks.read_text().replace('[ ]','[x]'))
        state=self.runtime._read(self.change)
        self.runtime._fresh(state)
        changed=load_contract(self.f.proposal)
        self.assertEqual(initial['contract_hash'],changed['contract_hash'])
        self.assertNotEqual(initial['material_hash'],changed['material_hash'])
        for kind in ('acceptance','archive'):
            card=self.runtime._pending(state,kind)
            self.assertEqual(card['card']['material_hash'],changed['material_hash'])
        tasks.write_text(tasks.read_text().replace('implement step','implement different behavior'))
        with self.assertRaises(ValueError):self.runtime._fresh(state)
        self.assertEqual(self.calls,[])

    def test_profile_scope_blocks_unchecked_product(self):
        """REQ-79-7 C5: production registry never treats C++ as checked harness."""
        module=importlib.import_module('workflow_runtime')
        root=Path(__file__).resolve().parents[3]
        supported = 'server/src/product.cpp'
        for name in (supported, 'server/product.cpp', 'clients/python/client.py', 'other.py'):
            with self.subTest(target=name):
                fixture = WorkflowFixture()
                self.addCleanup(fixture.close)
                product = fixture.root / name
                product.parent.mkdir(parents=True, exist_ok=True)
                product.write_text('synthetic\n')
                registry = fixture.root / '.claude/chain/workflow-checks.json'
                registry.write_bytes((root / '.claude/chain/workflow-checks.json').read_bytes())
                fixture.git('add', '.')
                fixture.git('commit', '-qm', 'Pin fixture with the active profiles')
                harness = module.Runtime(fixture.root)
                self.assertEqual(harness.profile_name, 'harness')
                with self.assertRaisesRegex(ValueError, 'profile|scope'):
                    harness.prepare('79-demo', targets=[name])
                self.assertFalse((harness._dir('79-demo') / 'state.json').exists())
                cpp = module.Runtime(fixture.root, profile='cpp')
                if name == supported:
                    state = cpp.prepare('79-demo', targets=[name])
                    self.assertEqual(state['profile'], 'cpp')
                    self.assertEqual(state['pending_decision']['kind'], 'business')
                    self.assertEqual(module.Runtime(fixture.root, change='79-demo').profile_name, 'cpp')
                else:
                    with self.assertRaisesRegex(ValueError, 'profile|scope'):
                        cpp.prepare('79-demo', targets=[name])
                    self.assertFalse((cpp._dir('79-demo') / 'state.json').exists())
        self.assertEqual(self.calls, [])

    def test_archive_origin_and_audit_tamper(self):
        """REQ-79-6: exact originals precede relocation; corrupt audit is rejected."""
        from workflow_contract import load_contract,rebase_archive_links
        original={str(p.relative_to(self.f.change)):p.read_bytes() for p in self.f.change.rglob('*') if p.is_file()}
        contract=load_contract(self.f.proposal)
        destination=self.f.change.parent/'archive/2026-11-21-79-demo'
        rebase_archive_links(self.f.change,destination)
        self.assertEqual(original,{str(p.relative_to(self.f.change)):p.read_bytes() for p in self.f.change.rglob('*') if p.is_file()})
        self.assertTrue(contract['material_hash'])
        directory=self.runtime._audit_directory(self.change);directory.mkdir(parents=True)
        from workflow_runtime import sha
        (directory/'raw-input.txt').write_bytes(original['tasks.md'])
        manifest={'raw-input.txt':sha(directory/'raw-input.txt')}
        (directory/'manifest.json').write_text(json.dumps(manifest))
        self.assertEqual(self.runtime.audit(self.change)['technical_verdict'],'passed')
        (directory/'raw-input.txt').write_text('corrupted original\n')
        with self.assertRaises(ValueError):self.runtime.audit(self.change)
        with self.assertRaises(ValueError):self.runtime.archive(self.change)
        self.assertTrue(self.f.change.is_dir());self.assertFalse(destination.exists())

if __name__ == '__main__': unittest.main()
