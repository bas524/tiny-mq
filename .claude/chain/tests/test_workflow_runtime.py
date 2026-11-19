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
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_workflow_contract import WorkflowFixture


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

if __name__ == '__main__': unittest.main()
