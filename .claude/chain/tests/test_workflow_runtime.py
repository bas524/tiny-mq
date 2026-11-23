"""REQ-79-2/4/5/6/7/8: coordinator semantics with zero provider calls."""
import copy
import importlib
import json
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
        for name in ('server/product.cpp','clients/python/client.py','other.py'):
            with self.subTest(target=name):
                f=WorkflowFixture();self.addCleanup(f.close)
                product=f.root/name;product.parent.mkdir(parents=True,exist_ok=True);product.write_text('synthetic\n')
                registry=f.root/'.claude/chain/workflow-checks.json'
                registry.write_bytes((root/'.claude/chain/workflow-checks.json').read_bytes())
                f.git('add','.');f.git('commit','-qm','unvalidated product profile')
                r=module.Runtime(f.root)
                with self.assertRaisesRegex(ValueError,'profile|scope|unavailable|unchecked'):
                    r.prepare('79-demo',targets=[name])
                self.assertFalse((r._dir('79-demo')/'state.json').exists())
        status=self.runtime.status(self.change)
        self.assertIn('cpp',status['unavailable_profiles'])
        self.assertEqual(self.calls,[])

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
